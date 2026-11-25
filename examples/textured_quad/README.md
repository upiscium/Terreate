# Textured, indexed, depth-tested quads

A gap-finding example: two quads at different depths, a generated 64x64 checkerboard
texture, a per-frame tint, drawn with an application-owned frame loop (the one of
`examples/triangle`). It uses Terreate's API for everything the API offers and states
where it had to leave it, so a missing piece shows up here with evidence.

```sh
terreate_example_textured_quad                      # run until the window is closed
terreate_example_textured_quad --frames 90 --resize-at 30 --require-validation --require-resize
```

The scripted form is the CTest `example.textured_quad` (SKIP, never PASS, without a window
system or when the window system does not resize the window; see `examples/triangle`).

## What worked through the API alone

Allocator; vertex, index and uniform Buffers; one `UploadBatch` carrying both Buffers and
an Image; ImageView and Sampler; a descriptor set with a uniform buffer and a combined
image sampler; pipeline vertex input and depth state; a depth attachment created per
Swapchain generation and retired with it; `GraphicsBindingState` proving the indexed draw
fits the bound vertex and index buffers; a per-frame uniform written through the mapped
pointer with one explicit flush.

## Gap report

Native calls the example makes (`vkQueueWaitIdle`, the raw `VkMemoryBarrier2` for the vertex and index data, and `glfwSetWindowSize` on `Window::nativeHandle` for the scripted resize), and API friction found while writing it. The image barriers used to be raw structs too; G1 and G2 record how #437 changed that:

| # | Finding | Kind |
| --- | --- | --- |
| G1 | Every image layout transition used to be a hand-built `VkImageMemoryBarrier2` (eight to twelve lines, raw stage and access constants). **Addressed by #437**: `imageBarrier` and `recordImageBarrier` (`<terreate/graphics/barriers.hpp>`) build or record one explicit transition, and this example uses them. Still raw: the `VkMemoryBarrier2` for the vertex and index data, and the buffer barrier case (no builder yet). | native call, partly addressed |
| G2 | `Image::nativeImage()` returns `VkImage` (while `Image::nativeHandle()` returns `vk::Image`), `SwapchainImage::nativeImage()` returns `vk::Image`. **Addressed by #437**: the barrier builders take either image type, and `SwapchainImage::nativeHandle()` now exists beside `nativeImage()`. | inconsistency, addressed |
| G3 | The depth format was chosen blind: no Terreate query for the format features a device supports; only `createImage` failing would tell, and Vulkan guarantees only `D16_UNORM` and one of `X8_D24_UNORM` and `D32_SFLOAT`. **Addressed by #438**: `queryFormatProperties` and `firstSupportedFormat` (`<terreate/graphics/format_properties.hpp>`); this example asks the device for the first supported depth format of its own list. | missing API, addressed |
| G4 | A host-visible uniform Buffer was requested with the VMA flag `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT`: VMA enums are part of `BufferAllocationDescription`, so saying "the host writes this every frame" needed VMA knowledge (and the example had left out `required_flags = HostVisible`). **Addressed by #439**: `BufferMemoryIntent` and `bufferAllocationFor` (`device_local`, `host_sequential_write`, `host_random_access`); this example names the intent of its three buffers. | ergonomics, addressed |
| G5 | Descriptor writes were a one-element `std::vector` in a `std::variant` built with nested braces and unlabelled numbers, repeating the descriptor type of the layout, and the pool was sized by hand (it silently became too small when a binding was added to a layout). The bindings and pool sizes are named aggregates, so designated initializers already worked: they needed no API (the first version of this report called them positional because the example wrote them so). **Addressed by #440**: `uniformBufferWrite`, `combinedImageSamplerWrite` and the other type-named write builders, and `descriptorPoolDescriptionFor` (the pool sized from the layouts); this example uses them with designated initializers. | ergonomics, addressed |
| G6 | A fenced one-shot submission needed about ten lines of `SubmissionDescription` and `CompletionDescription`, written twice in this example. **Addressed by #441**: `submitFenced` (and the `fenceCompletion` and `timelineCompletion` builders) make it one call; the example uses it. | ergonomics, addressed |
| G9 | The vertex data, the stride, the offsets and the formats were written in four places (a flat `float` array, `5 * sizeof(float)`, `3 * sizeof(float)`, the formats) that agreed only by care. **Addressed by #448**: the application's `Vertex` struct, `vertexLayout` with `vertexAttribute<&Vertex::member>(location, format)` (formats stated, never deduced), `applyTo`, and the typed `uploadItem`; this example uses them. | ergonomics, addressed |
| G7 | `vkQueueWaitIdle` is called wherever no present fence proves the presents are done: a generation dropped behind an `out_of_date` present or on a device without present fences, a pipeline rebuild after a format change, and shutdown (carried over from the triangle; see its README). | native call |
| G8 | A one-shot upload waited on its fence inside a block so the staging memory outlived the GPU use, against the documented pattern; if the wait failed the example returned while the GPU might still read it. **Addressed by #441**: the staging owner is retired into a `RetirementQueue` with the receipt of a submission that has its own command buffer and its own never-reset fence, and the scope guard drains the queue on every exit (the pattern is documented in `docs/architecture/gpu_submission.md`). | addressed |

These findings were gathered without adding library API; G1 and G2 were then addressed separately by #437, which also moved this example to the new builders.

## Regenerating the shaders

The shaders are compiled offline and embedded as `quad_spirv.hpp`; no shader compiler runs
when the example is built.

```sh
cd examples/textured_quad/shaders
glslangValidator -V -g0 --target-env vulkan1.3 -S vert -o quad.vert.spv quad.vert
glslangValidator -V -g0 --target-env vulkan1.3 -S frag -o quad.frag.spv quad.frag
spirv-val --target-env vulkan1.3 quad.vert.spv && spirv-val --target-env vulkan1.3 quad.frag.spv
```

Then regenerate the two arrays in `quad_spirv.hpp` from the `.spv` files and run clang-format (little-endian
32-bit words, for example with `od -An -tx4 -v`) and delete the `.spv` files.

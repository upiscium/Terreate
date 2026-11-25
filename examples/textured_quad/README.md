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

Native calls the example makes (the raw barrier structs, `vkQueueWaitIdle`, and `glfwSetWindowSize` on `Window::nativeHandle` for the scripted resize), and API friction found while writing it:

| # | Finding | Kind |
| --- | --- | --- |
| G1 | Every image layout transition (two for the Swapchain image, one for the depth image, two for the texture) is a hand-built `VkImageMemoryBarrier2` of eight to twelve lines with raw stage and access constants, plus one `VkMemoryBarrier2` for the vertex and index data; `recordBarriers` takes only the raw structs. | native call |
| G2 | `Image::nativeImage()` returns `VkImage` (while `Image::nativeHandle()` returns `vk::Image`), `SwapchainImage::nativeImage()` returns `vk::Image`: filling one barrier struct needs a cast for the second. | inconsistency |
| G3 | The depth format (`D32_SFLOAT`) is chosen blind: no Terreate query for the format features a device supports; only `createImage` failing tells. Vulkan guarantees `D16_UNORM`, and at least one of `X8_D24_UNORM` and `D32_SFLOAT`, so a careful application must ask. The native `vkGetPhysicalDeviceFormatProperties` is what a careful application would call. | missing API |
| G4 | A host-visible uniform Buffer is requested with the VMA flag `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT`; VMA enums are part of `BufferAllocationDescription`, and there is no Terreate-level way to say "written by the host every frame". | ergonomics |
| G5 | Descriptor bindings and writes are positional aggregates (`{0, 0, type, std::vector<...>{{...}}}`): easy to misorder and hard to read. | ergonomics |
| G6 | A fenced one-shot submission needs about ten lines of `SubmissionDescription` and `CompletionDescription` (domain, serial, requested, effective, condition); the example writes it twice. | ergonomics |
| G7 | `vkQueueWaitIdle` is called wherever no present fence proves the presents are done: a generation dropped behind an `out_of_date` present or on a device without present fences, a pipeline rebuild after a format change, and shutdown (carried over from the triangle; see its README). | native call |
| G8 | A one-shot upload is submitted with a fence and waited on inside a block so the staging memory outlives the GPU use. `upload_batch.hpp` recommends handing the staging owner to a `RetirementQueue`; there is no helper for the simple "submit and wait" case, and if the wait fails the example returns while the GPU may still read the staging memory. | ergonomics, error path |

These are findings, not changes: this example adds no library API.

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

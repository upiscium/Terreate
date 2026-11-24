# Triangle

A windowed triangle: the Vulkan objects, recording, submission and presentation
go through Terreate's API. `main.cpp` is the walkthrough (read it top to bottom),
with an application-owned frame loop, one frame in flight, Swapchain recreation
on resize and retirement of the old generation.

```sh
terreate_example_triangle                     # run until the window is closed
terreate_example_triangle --frames 90 --resize-at 30 --require-validation
```

`--resize-at K` must be smaller than `--frames N`.

The scripted form is what the CTest `example.triangle` runs. It reports
`frames=SKIP` (never PASS) without a window system.

## Native calls this example still makes

Terreate leaves these to the application or has no wrapper for them:

- `vkQueueWaitIdle`: dropping a generation whose present fences cannot prove the
  presents are done (no `VK_KHR_swapchain_maintenance1`, or a present that
  returned `out_of_date`), a pipeline rebuild after a format change, and shutdown.
- Raw `VkImageMemoryBarrier2` structs (`recordBarriers` takes them) built from
  `SwapchainImage::nativeImage`, and `Queue::nativeHandle` for the idle call.
- `glfwSetWindowSize` on `Window::nativeHandle` (the scripted resize only).

## Regenerating the shaders

The shaders are compiled offline and embedded as `triangle_spirv.hpp`; no shader
compiler runs when the example is built.

```sh
cd examples/triangle/shaders
glslangValidator -V -g0 --target-env vulkan1.3 -S vert -o triangle.vert.spv triangle.vert
glslangValidator -V -g0 --target-env vulkan1.3 -S frag -o triangle.frag.spv triangle.frag
spirv-val --target-env vulkan1.3 triangle.vert.spv && spirv-val --target-env vulkan1.3 triangle.frag.spv
```

Then rewrite the two arrays in `../triangle_spirv.hpp` from the words of the
two `.spv` files (little-endian 32-bit words, for example with
`od -An -v -t x4 -w32`). Do not commit the `.spv` files.

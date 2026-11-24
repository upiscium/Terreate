# Triangle

A windowed triangle written with Terreate's API only: `main.cpp` is the
walkthrough (read it top to bottom), with an application-owned frame loop, one
frame in flight, Swapchain recreation on resize and retirement of the old
generation.

```sh
terreate_example_triangle                     # run until the window is closed
terreate_example_triangle --frames 90 --resize-at 30 --require-validation
```

The scripted form is what the CTest `example.triangle` runs. It reports
`frames=SKIP` (never PASS) without a window system.

## Native calls this example still makes

`glfwSetWindowSize` (the scripted resize only) and `vkQueueWaitIdle` (the
conservative way to drop an old Swapchain when present fences, the
`VK_KHR_swapchain_maintenance1` feature, are unavailable). Terreate leaves both
to the application.

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

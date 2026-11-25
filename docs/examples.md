# Examples

Both examples are built with `TERREATE_BUILD_EXAMPLES` and run by CTest.

| Example | What it shows | CTest |
| --- | --- | --- |
| `examples/getting_started` | Instance, physical device and Device, headless ([source](getting-started.md)) | `example.getting_started` |
| `examples/triangle` | A window, a Swapchain and an application-owned frame loop that draws a triangle, recreates the Swapchain on resize and reports which paths ran | `example.triangle` |

## The windowed triangle

The header comment of `examples/triangle/main.cpp` states what the file does and which
native Vulkan calls it still makes because Terreate leaves them to the application:

```{literalinclude} ../examples/triangle/main.cpp
:language: cpp
:lines: 1-24
```

The scripted run (`--frames N --resize-at K --require-validation --require-resize`) is
what the CTest executes; without a window system it reports SKIP, never PASS. The
`examples/triangle/README.md` file in the repository explains how the SPIR-V is
regenerated.

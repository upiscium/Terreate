# Getting started

This page tells you what to read and do first. It takes you from an empty directory to a program that
brings Vulkan up through Terreate, then points to the next step for each thing you might want to do.

## What Terreate is (and is not)

Terreate is a C++23 library over Vulkan (through Vulkan-Hpp). It is **not** an engine: your program
keeps `main()`, the frame loop, the frame scheduling and the lifetime of every resource. Three habits
run through the whole API, and knowing them makes every page easier to read:

- **Request, plan, effective.** You describe what you want (a *request*); the library checks it
  against what the machine offers and returns a *plan*; you create from the plan and can read back the
  *effective* state. Nothing is silently changed or chosen for you ([details](architecture/configuration-resolution.md)).
- **Owners are move-only values.** An `Instance`, `Device`, `Buffer` and so on is destroyed when its value
  goes out of scope. A borrowed handle is a separate, non-owning view.
- **You wait, explicitly.** Destroying a C++ object never waits for the GPU. You prove the GPU is done
  (a fence, a timeline value, a retirement queue) before something it uses is destroyed
  ([completion](architecture/gpu_completion.md), [retirement](architecture/gpu_retirement.md)).

Errors are values: functions return `terreate::Result<T>` instead of throwing ([errors](architecture/error-result.md)).

## What you need

| You need | Notes |
| --- | --- |
| Linux, GCC/g++ (C++23), CMake 3.28+, Ninja | Other platforms and compilers are not verified for 0.1 |
| The Vulkan 1.3 headers and loader, a Vulkan 1.3 driver | A software driver (Mesa lavapipe) is enough to run the headless example |
| GLFW 3.4+ | Only for the Platform component and the windowed examples |
| VulkanMemoryAllocator | For Graphics |

The repository ships a pinned [Nix](https://nixos.org/) development shell with all of this:
`nix develop`. Without Nix, install the same packages and see [building](development/building.md).

## 1. Get and build the library

```sh
git clone https://github.com/upiscium/Terreate.git
cd Terreate
nix develop                                   # or: have the dependencies installed
just project::configure
just project::build
```

To use it from your own project, install it and point CMake at the prefix:

```sh
cmake --install .build/default --prefix "$HOME/terreate-install"
```

```cmake
# your CMakeLists.txt
cmake_minimum_required(VERSION 3.28)
project(app CXX)
set(CMAKE_CXX_STANDARD 23)

find_package(Terreate REQUIRED COMPONENTS Graphics)   # also: Core, Platform, IntegrationPlatformGraphics
add_executable(app main.cpp)
target_link_libraries(app PRIVATE Terreate::Graphics)
```

Configure your project with `-DCMAKE_PREFIX_PATH=$HOME/terreate-install`. The package is compatible
within the same `MAJOR.MINOR` version (`0.1.x`). Pick only the components you use: a program that links
`Terreate::Core` does not receive the Vulkan dependency.

## 2. Your first program: bring Vulkan up, headless

The program below is `examples/getting_started/main.cpp`, included unchanged. It is compiled and run by the
CTest `example.getting_started`, and the installed-package smoke test builds the same file as an external
consumer, so this page cannot drift from code that works. Read the numbered comments in order: ask the
loader what exists, resolve a request, create from the plan, repeat for the physical device and the Device.

```{literalinclude} ../examples/getting_started/main.cpp
:language: cpp
:linenos:
```

Run it from the build directory (`terreate_example_getting_started`). It prints `bootstrap=PASS` and the
name of the device it chose. Without a Vulkan device it prints `bootstrap=BLOCKED` and fails, like the
other headless tests (the [test matrix](development/graphics-test-matrix.md) explains PASS, SKIP and BLOCKED).

## 3. Draw something

Continue with the examples, in this order ([all three](examples.md)):

1. **Triangle** (`examples/triangle`): a window, a Swapchain and an application-owned frame loop that
   recreates the Swapchain on resize. Read the header comment first; it lists the native Vulkan calls
   the file still makes.
2. **Textured quad** (`examples/textured_quad`): vertex, index and uniform buffers, an uploaded texture,
   a descriptor set and depth. It also keeps a gap report of what the API could not yet express.

## 4. Where to look next

| I want to... | Read | API reference |
| --- | --- | --- |
| understand the request/plan/effective model | [Configuration](architecture/configuration-resolution.md) | [Graphics](api/graphics.md) |
| handle errors, get diagnostics and validation messages | [Errors](architecture/error-result.md), [Diagnostics](architecture/diagnostics.md), [Debug utils](architecture/debug_utils.md) | [Core](api/core.md) |
| describe geometry on the CPU (`MeshData`, primitive generators) | [Asset](architecture/asset.md) | [Asset](api/asset.md) |
| open a window and poll events | [Platform runtime](architecture/platform_runtime.md) | [Platform](api/platform.md) |
| pick a GPU and create a Device | [Physical device](architecture/physical-device.md), [Logical device](architecture/logical-device.md) | [Graphics](api/graphics.md) |
| create buffers and images | [Allocator](architecture/allocator.md), [Buffer](architecture/buffer.md), [Image](architecture/image.md) | [Graphics](api/graphics.md) |
| upload and read back data | [Upload batch](architecture/upload_batch.md), [Readback batch](architecture/readback_batch.md) | [Graphics](api/graphics.md) |
| describe shaders, descriptors and pipelines | [Shader module](architecture/shader_module.md), [Descriptors](architecture/descriptor_api.md), [Graphics pipeline](architecture/graphics_pipeline.md) | [Graphics](api/graphics.md) |
| record and submit work, know when the GPU is done | [Command lifecycle](architecture/command_lifecycle.md), [Submission](architecture/gpu_submission.md), [Completion](architecture/gpu_completion.md), [Retirement](architecture/gpu_retirement.md) | [Graphics](api/graphics.md) |
| present to a window | [Surface](architecture/surface.md), [Presentation](architecture/presentation.md) | [Integration](api/integration.md) |

The [API reference](api/index.md) lists every public header by topic. Both the architecture pages
(the contract: what holds and why) and the reference (the signatures) are part of the specification.

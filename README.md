# Terreate

Terreate is a code-first C++23/Vulkan rendering-support library, not an application engine.
The application retains its own loop, scheduling, and resource lifetime decisions.
The project uses GCC/g++, CMake, Ninja, CTest, Nix, and Just. KAGARI v0.1.0
is an optional external, isolated Work Unit developer tool rather than a repository-owned agent runtime.

## Architecture documentation

- [Configuration and capability resolution contract](docs/architecture/configuration-resolution.md)
- [Core error and result contract](docs/architecture/error-result.md)
- [Structured diagnostics contract](docs/architecture/diagnostics.md)
- [Graphics allocator integration and ownership](docs/architecture/allocator.md)
- [Graphics validation and integration test matrix](docs/development/graphics-test-matrix.md)

The documentation (architecture, development guide, API reference) is built with
`just project::docs`; its entry point is `docs/index.md`.
The changelog is `CHANGELOG.md`; releases follow `docs/development/release.md`.

## License

Terreate is released under the BSD 3-Clause License: see `LICENSE`. The installed package carries it in the
documentation directory (`share/doc/Terreate`).

## Development shell

Enter the pinned project environment before configuring:

```sh
nix develop
```

The shell selects GCC/g++ through `CC`/`CXX` and provides CMake/Ninja/Just,
`pkg-config`, Vulkan headers, the loader, validation layers, `vulkaninfo`, and
a Mesa software runtime.  It also provides GLFW, VMA, GLM, spdlog, Slang,
shaderc, glslang, SPIR-V Tools, SPIR-V Cross, and the Wayland/X11 development
surface used by GLFW and Vulkan.  The validation layer search path is exported
as `VK_LAYER_PATH`, with the pinned layer directory prepended to any existing
path.  `VULKAN_HEADERS_INCLUDE` is the canonical header interface; `nix
develop` supplies its pinned Vulkan Headers `include` directory value.  The
other `TERREATE_VULKAN_*` variables expose pinned Vulkan inputs for
diagnostics.  CMake requires this explicit variable when Graphics is enabled
for Vulkan-Hpp and does not silently search inherited SDK or host defaults.

Generic GCC/g++, `clangd`, `clang-tidy`, and `clang-format` are editor/build
tooling rather than project-owned dependencies and are expected from the
developer's Home Manager/Neovim toolchain.  The CMake compilation database explicitly
re-exports the GCC implicit include directories discovered at configure time,
so clang-tidy can parse libstdc++ in a Nix shell without a hard-coded store or
compiler-version path.

## Configure, build, and inspect

```sh
just project::configure
just project::build
just project::test
just project::lint
just project::check
vulkaninfo --summary
```

To inspect the canonical headless Graphics runtime check and its marker, run:

```sh
ctest --preset default --output-on-failure --verbose --tests-regex '^graphics\.instance$'
```

The test must emit the exact line `graphics.instance: headless-instance=PASS`.
That marker is also the CTest pass condition, so it proves that the real Vulkan
loader was queried and a native headless instance was created successfully;
synthetic resolver coverage alone cannot satisfy the project check. The
`validation-layer=PASS` line is expected only when
`VK_LAYER_KHRONOS_validation` is available; otherwise its explicit `SKIP` line
is valid. Likewise, `debug-utils-callback=PASS` is emitted when
`VK_EXT_debug_utils` is available, and its explicit `SKIP` line is valid when
that optional extension is unavailable.

The canonical build directory is `.build/default`.  CMake also creates the
ignored root `compile_commands.json` entrypoint as a symlink to that build
database; clangd can therefore use the repository root directly.  The preset
sets `gcc`/`g++`, C++23, Ninja, and `CMAKE_CXX_SCAN_FOR_MODULES=OFF`.

`just project::check` is the canonical verification entrypoint.  Every
invocation enters the pinned `nix develop` shell itself and dispatches the
private check there, so it receives the pinned `VULKAN_HEADERS_INCLUDE` value.
For a top-level Terreate configure, Graphics is fail-closed when
`VULKAN_HEADERS_INCLUDE` is absent; Core-only and Graphics-OFF source trees do
not discover Vulkan.  When Terreate is added with `add_subdirectory`, it does
not impose that top-level environment contract on the parent project: Graphics
uses the surrounding project's Vulkan package discovery, with
`VULKAN_HEADERS_INCLUDE` as an optional explicit include override.  In either
case, an explicitly supplied include path is used rather than silently
searching inherited SDK or host defaults.

The devShell dependencies are not, by themselves, consumer linkage. Core and
Platform remain Vulkan-free and VMA-free. Graphics publicly depends on Vulkan
and VMA through `Vulkan::Vulkan` and `GPUOpen::VulkanMemoryAllocator`, including
their transitive header and linkage requirements. Platform publicly depends on
GLFW 3.4 through `glfw` (#347); Core and Graphics do not. The project still
does not link optional shader backends.

## Components and package exports

The initial component graph exposes only the three architecture targets
`Terreate::Core`, `Terreate::Platform`, and `Terreate::Graphics`.  The
`TERREATE_BUILD_CORE`, `TERREATE_BUILD_PLATFORM`, and
`TERREATE_BUILD_GRAPHICS` options select them independently; Platform and
Graphics require Core but never enable it implicitly.  These `TERREATE_BUILD_*`
options are the sole component configuration API, including when reconfiguring
an existing build tree.

The first optional Integration (#227, #363), `Terreate::IntegrationPlatformGraphics`,
connects Platform and Graphics through their public APIs: it creates a Vulkan
Surface from a Platform Window (see
[Surface ownership and the Window Integration](docs/architecture/surface.md)).
Neither Platform nor Graphics depends on it. Its option,
`TERREATE_BUILD_INTEGRATION_PLATFORM_GRAPHICS`, is on by default only when both
of its dependencies are. Turning it on while Platform or Graphics is off is a
configure error that names the missing option; Terreate does not enable a
component implicitly. Request it with
`find_package(Terreate REQUIRED COMPONENTS IntegrationPlatformGraphics)`, which
also imports Platform and Graphics and their dependencies (GLFW and Vulkan).

The second Integration (#279), `Terreate::IntegrationAssetGraphics`, connects Asset and Graphics: it
plans an Asset `MeshData` as vertex and index buffers, creates them and returns the upload items for
`graphics::prepareUploadBatch`, in three explicit steps that record, submit and wait for nothing (see
[Asset-Graphics Integration](docs/architecture/asset_graphics.md)). Part 2 does the same for an `ImageData`: it plans the
Vulkan format and one copy region per mip and layer, creates the image (no view, no sampler) and returns the upload
items; mips are not generated and no layout transition is recorded (reserved opt-in follow-ups #467 and #468). Asset does not depend on Graphics
and Graphics does not depend on Asset. Its option, `TERREATE_BUILD_INTEGRATION_ASSET_GRAPHICS`, is on by
default only when both `TERREATE_BUILD_ASSET` and `TERREATE_BUILD_GRAPHICS` are, and turning it on without
one of them is a configure error. Request it with
`find_package(Terreate REQUIRED COMPONENTS IntegrationAssetGraphics)`, which imports Asset, Graphics and
Core (and Vulkan, not GLFW).

Graphics can also tell, before a Device exists, what a physical device can present
to a Surface: `<terreate/graphics/presentation.hpp>` (#364) observes it and
resolves the present queue, format, present mode and `VK_KHR_swapchain` into a
Device request (see
[Presentation support and requirements](docs/architecture/presentation.md)).

Install the selected targets and use the same namespace from a consumer:

```cmake
find_package(Terreate REQUIRED COMPONENTS Core Graphics)
target_link_libraries(app PRIVATE Terreate::Graphics)
```

The package reports disabled or unknown requested components through the
standard `<Package>_<Component>_FOUND` and `<Package>_NOT_FOUND_MESSAGE`
variables.  Optional component requests can therefore be inspected without
failing a quiet package lookup, while required requests fail through
`find_package`. Graphics discovers Vulkan and VMA when it is built; package
consumers discover them only when Graphics is requested and available. Core-only
and Graphics-OFF packages do not discover Vulkan or VMA. An optional Graphics
request with unavailable dependencies must leave Core usable rather than force
VMA on Core-only consumers. Only a Platform request discovers GLFW 3.4
(`glfw3` CONFIG); a required Platform request fails when it is missing, and an
optional one leaves Core usable.

Asset (#345) is CPU-only: `TERREATE_BUILD_ASSET` (default ON) builds `Terreate::Asset`, which
depends on Core alone. Like Platform and Graphics it requires Core and never enables it implicitly,
and an `Asset` request searches for no external package, so it never brings Vulkan or GLFW.

`cmake.package_smoke` checks this from outside the build tree (#285). It
installs four packages (all components, Core only, Platform only, Graphics
only), then configures separate consumer projects that use only
`CMAKE_PREFIX_PATH`. It verifies that:

- a required component that was not built fails with a message naming it and
  the `TERREATE_BUILD_*=ON` option, and an optional one leaves Core usable;
- installed package files and the include directories a consumer receives
  (including system include directories) do not refer to the source or build
  tree, and contain no unresolved generator expression;
- Core and Graphics consumers import no GLFW target or link dependency, while a
  Platform consumer receives `glfw` as `Terreate::Platform`'s link dependency
  and runs a Runtime and Window on GLFW's null backend.
- an Integration consumer receives GLFW and Vulkan, imports Platform and
  Graphics with it, and runs the installed Integration on the null backend,
  where it refuses explicitly; every other package either has no Integration or
  reports the disabled-component diagnostic.

Allocator integration is already merged into main. Its public-interface and
installed-consumer contracts retain the same `Terreate::Graphics` target and
transitive dependency visibility; allocator-test linker wrapping is private to
the test executable, not an installed usage requirement. Merged implementation
does not establish a verified package release or formal KAGARI qualification,
and this documentation update claims no newly performed validation.

### Graphics instance ownership

`terreate/graphics/instance.hpp` separates configuration from native
ownership. `resolveInstance` copies its `InstanceDescription` and
`InstanceCapabilities` inputs into a value-owned `InstancePlan`; the inputs
may be changed or destroyed after resolution. `InstancePlan` is copyable and
uses ordinary default move semantics. Its observer functions are zero-copy,
read-only borrowed references, and `createInstance` consumes those references
directly without normalizing or copying the plan before native apply. On
success, `createInstance` stores an independent copy in its move-only
`Instance` owner.

`Instance::nativeHandle()` and `Instance::plan()` are borrowed observers. Do
not retain either result after destroying or moving the owning `Instance`;
reacquire them from the current owner. Plan collection observers follow the
same lifetime rule across plan moves. A `DiagnosticSinkView` passed to
`createInstance` is copied as a view, not as ownership of its target, so the
application-owned sink target must outlive the resulting `Instance`.

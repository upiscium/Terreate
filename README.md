# terreate

Agent-ready C++23/CMake project using GCC/g++, Ninja, CTest, Nix, Just, and
the shared Agent Core.  The repository configuration is a Vulkan 1.3+
development baseline and Graphics exposes the minimal Vulkan instance bootstrap.

## Architecture documentation

- [Configuration and capability resolution contract](docs/architecture/configuration-resolution.md)
- [Core error and result contract](docs/architecture/error-result.md)
- [Structured diagnostics contract](docs/architecture/diagnostics.md)

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

The devShell dependencies are not consumer linkage.  Core and Platform remain
Vulkan-free; Graphics owns the Vulkan loader link and publishes `Vulkan::Vulkan`
to consumers.  The project still does not link GLFW, VMA, or optional shader
backends.

## Components and package exports

The initial component graph exposes only the three architecture targets
`Terreate::Core`, `Terreate::Platform`, and `Terreate::Graphics`.  The
`TERREATE_BUILD_CORE`, `TERREATE_BUILD_PLATFORM`, and
`TERREATE_BUILD_GRAPHICS` options select them independently; Platform and
Graphics require Core but never enable it implicitly.  These `TERREATE_BUILD_*`
options are the sole component configuration API, including when reconfiguring
an existing build tree.

Install the selected targets and use the same namespace from a consumer:

```cmake
find_package(Terreate REQUIRED COMPONENTS Core Graphics)
target_link_libraries(app PRIVATE Terreate::Graphics)
```

The package reports disabled or unknown requested components through the
standard `<Package>_<Component>_FOUND` and `<Package>_NOT_FOUND_MESSAGE`
variables.  Optional component requests can therefore be inspected without
failing a quiet package lookup, while required requests fail through
`find_package`.  Graphics discovers Vulkan only when that component is built and
requested; Core-only and Graphics-OFF packages do not discover Vulkan.  No
component discovers or links GLFW or VMA.

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

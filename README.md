# terreate

Agent-ready C++23/CMake project using GCC/g++, Ninja, CTest, Nix, Just, and
the shared Agent Core.  The repository configuration is a Vulkan 1.3+
development baseline; it does not implement Vulkan production APIs.

## Architecture documentation

- [Configuration and capability resolution contract](docs/architecture/configuration-resolution.md)

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
diagnostics.  CMake requires this explicit variable for Vulkan-Hpp and does
not silently search inherited SDK or host defaults.

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

The canonical build directory is `.build/default`.  CMake also creates the
ignored root `compile_commands.json` entrypoint as a symlink to that build
database; clangd can therefore use the repository root directly.  The preset
sets `gcc`/`g++`, C++23, Ninja, and `CMAKE_CXX_SCAN_FOR_MODULES=OFF`.

`just project::check` is the canonical verification entrypoint.  Every
invocation enters the pinned `nix develop` shell itself and dispatches the
private check there, so it receives the pinned `VULKAN_HEADERS_INCLUDE` value.
Direct `just project::configure` remains fail-closed when
`VULKAN_HEADERS_INCLUDE` is absent; when a caller supplies it explicitly,
CMake trusts that value rather than silently searching inherited SDK or host
defaults.

The devShell dependencies are not consumer linkage.  The project only adds
header/include context for Vulkan-Hpp to the sample targets and deliberately
does not link the Vulkan loader, GLFW, or optional shader backends.  A future
component owns its runtime linkage explicitly.

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
`find_package`.  Component targets do not discover or link Vulkan, GLFW, or
VMA dependencies in this skeleton.

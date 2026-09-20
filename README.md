# terreate

Agent-ready C++23/CMake project using GCC/g++, Ninja, CTest, Nix, Just, and
the shared Agent Core.  The repository configuration is a Vulkan 1.3+
development baseline; it does not implement Vulkan production APIs.

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
as `VK_LAYER_PATH`; `TERREATE_VULKAN_*` variables expose the pinned Vulkan
inputs for diagnostics.  CMake requires `TERREATE_VULKAN_HEADERS` from this
shell for Vulkan-Hpp and never falls back to an inherited SDK or host search
path.

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
private check there, so an ambient `TERREATE_VULKAN_HEADERS` value cannot
bypass the pinned environment.  Direct `just project::configure` remains
fail-closed and requires `TERREATE_VULKAN_HEADERS` from the project shell.

The devShell dependencies are not consumer linkage.  The project only adds
header/include context for Vulkan-Hpp to the sample targets and deliberately
does not link the Vulkan loader, GLFW, or optional shader backends.  A future
component owns its runtime linkage explicitly.

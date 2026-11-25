# Building, testing and documenting

Enter the pinned environment with `nix develop`. It provides GCC/g++, CMake, Ninja,
Just, the Vulkan loader, the validation layers, a Mesa software device, GLFW, Doxygen,
Sphinx and ccache. Generic editor tooling (`clangd`, `clang-tidy`, `clang-format`) is expected
from the developer's own toolchain.

| Command | What it does |
| --- | --- |
| `just project::configure` | Configure with the `default` preset |
| `just project::build` | Build |
| `just project::test` | Run all CTests |
| `just project::docs` | Build the API reference and these pages |
| `just project::check` | The gate: format, lint, build, all tests (includes the documentation tests) |

## Compiler cache

The dev shell provides `ccache` and exports `CMAKE_CXX_COMPILER_LAUNCHER=ccache`, which CMake
reads on every configure, so the preset and the configures started by the CMake-driven tests
(`cmake.component_matrix`, `cmake.package_smoke`) compile through one cache. A warm cache
turns a full rebuild into seconds and the test phase from several minutes into about two.
The cache is capped at 5 GB (`CCACHE_MAXSIZE`) and lives in ccache's default directory
(`CCACHE_DIR` overrides it, for example to a per-clone directory when clones must not share
state); the compiler is hashed by content (`CCACHE_COMPILERCHECK=content`). Set
`CMAKE_CXX_COMPILER_LAUNCHER=` (empty) before `nix develop` to switch the cache off.

CMake stores the launcher in the build directory's `CMakeCache.txt`: if you build an existing
`.build/default` from a shell outside the dev shell, delete that directory and configure
again, or the build looks for a `ccache` that is not on `PATH`.

## Documentation

The documentation is built when `TERREATE_BUILD_DOCS=ON`. The `default` preset sets it;
it is off in a plain configure so a package consumer never needs the tools, and it only
applies when Terreate is the top-level project (not when it is added as a subdirectory). The CTests
`docs.doxygen` and `docs.sphinx` fail on a broken reference, a malformed Doxygen command,
a broken page link or any Sphinx warning.

Write prose with Markdown links, not the `{ref}` or `{cpp:...}` roles: the warnings of
those roles are not checked (see `docs/conf.py`), because the API pages are rendered from
Doxygen XML, which `docs.doxygen` already checks.

`just project::lint` runs one clang-tidy per core, at most 8 by default (each process needs about a
gigabyte on the Vulkan headers); `LINT_JOBS=<n>` overrides the number. A file's output is shown only
when it fails, and the first failure fails the recipe.

The test classes, outcome words and run environments are in the
[test matrix](graphics-test-matrix.md).

# Building, testing and documenting

Enter the pinned environment with `nix develop`. It provides GCC/g++, CMake, Ninja,
Just, the Vulkan loader, the validation layers, a Mesa software device, GLFW, Doxygen
and Sphinx. Generic editor tooling (`clangd`, `clang-tidy`, `clang-format`) is expected
from the developer's own toolchain.

| Command | What it does |
| --- | --- |
| `just project::configure` | Configure with the `default` preset |
| `just project::build` | Build |
| `just project::test` | Run all CTests |
| `just project::docs` | Build the API reference and these pages |
| `just project::check` | The gate: format, lint, build, all tests (includes the documentation tests) |

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

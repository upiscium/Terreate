root := justfile_directory()

mod cmake 'cmake.just'
mod quality 'quality.just'
mod fast 'fast.just'
mod tests 'test.just'
mod? repository 'repository.just'

[working-directory: root]
doctor:
    @test -f CMakeLists.txt
    @command -v cmake >/dev/null
    @command -v ninja >/dev/null
    @command -v gcc >/dev/null
    @command -v g++ >/dev/null
    @compiler="${CXX:-g++}"; command -v "$compiler" >/dev/null
    @command -v clang-format >/dev/null
    @command -v clang-tidy >/dev/null
    @command -v ctest >/dev/null
    @if [ -f CMakePresets.json ]; then cmake --list-presets >/dev/null; fi
    @echo "C++/CMake Project Adapter doctor: PASS"

configure: cmake::configure

format-check: quality::format-check

lint: configure quality::lint

build: configure cmake::build

test: build tests::all

[private]
check-in-dev-shell: doctor format-check lint test
    @echo "C++/CMake Project Adapter check: PASS"

[working-directory: root]
check:
    #!/usr/bin/env sh
    set -eu
    if ! command -v nix >/dev/null 2>&1; then
      printf '%s\n' \
        "Terreate project::check requires Nix to enter the pinned devShell." >&2
      exit 1
    fi
    exec nix develop --command just project::check-in-dev-shell

[private]
check-fast-in-dev-shell: doctor format-check configure fast::lint build fast::tests
    @echo "C++/CMake Project Adapter check-fast: PARTIAL, not the gate (run just project::check before a merge)"

# The development loop: format, changed-file lint, build, and the tests without the heavy
# label (all of them when the change touches CMake, the flake or a public header). Partial
# by design: `check` stays the gate a merge needs.
[working-directory: root]
check-fast:
    #!/usr/bin/env sh
    set -eu
    if ! command -v nix >/dev/null 2>&1; then
      printf '%s\n' \
        "Terreate project::check-fast requires Nix to enter the pinned devShell." >&2
      exit 1
    fi
    exec nix develop --command just project::check-fast-in-dev-shell

# Build the API reference (Doxygen) and the documentation pages (Sphinx) under .build/default/docs/.
[working-directory: root]
docs: configure
    #!/usr/bin/env sh
    set -eu
    if [ -f CMakePresets.json ]; then
      cmake --build --preset "${CMAKE_PRESET:-default}" --target terreate_docs_sphinx
    else
      cmake --build "${CMAKE_BUILD_DIR:-.build/default}" --target terreate_docs_sphinx
    fi

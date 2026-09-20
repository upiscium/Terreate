root := justfile_directory()
bootstrap_script := root / ".automation/bin/project_bootstrap.py"

mod cmake 'cmake.just'
mod quality 'quality.just'
mod tests 'test.just'
mod? repository 'repository.just'

[working-directory: root]
bootstrap name='':
    python3 {{quote(bootstrap_script)}} {{quote(name)}}

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

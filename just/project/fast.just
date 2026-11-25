root := justfile_directory()

# The development loop: PARTIAL by design. It is not the gate (`just project::check`),
# which a merge still needs. Everything here compares against FAST_BASE (default
# origin/main): the committed changes since the merge base, the uncommitted changes and
# the untracked files.

# Print the changed paths, one per line; "ALL" when the base ref does not exist (then every
# check runs in full).
[working-directory: root]
changed:
    #!/usr/bin/env sh
    set -eu
    base="${FAST_BASE:-origin/main}"
    if ! git rev-parse --verify --quiet "$base^{commit}" >/dev/null; then
      echo ALL
      exit 0
    fi
    {
      git diff --name-only --diff-filter=ACMR "$base"...HEAD
      git diff --name-only --diff-filter=ACMR HEAD
      git ls-files --others --exclude-standard
    } | sort -u

# clang-tidy on the changed .cpp files only, in parallel, like `quality::lint`.
[working-directory: root]
lint:
    #!/usr/bin/env sh
    set -eu
    canonical_database=".build/default/compile_commands.json"
    if [ -e compile_commands.json ] || [ -L compile_commands.json ]; then
      if [ ! -L compile_commands.json ] || [ "$(readlink compile_commands.json)" != "$canonical_database" ]; then
        printf '%s\n' "compile_commands.json must be the canonical symlink to $canonical_database" >&2
        exit 1
      fi
      compile_database=.
    else
      compile_database="${CMAKE_BUILD_DIR:-.build/default}"
    fi
    export compile_database
    changed="$(just project::fast::changed)"
    if [ "$changed" = ALL ]; then
      printf '%s\n' "check-fast: no base ref, linting every file" >&2
      exec just project::quality::lint
    fi
    files="$(printf '%s\n' "$changed" | grep -E '\.(cc|cpp|cxx)$' || true)"
    if [ -z "$files" ]; then
      echo "check-fast: no changed .cpp files to lint"
      exit 0
    fi
    echo "check-fast: linting $(printf '%s\n' "$files" | wc -l | tr -d ' ') changed .cpp file(s); headers are not linted on their own"
    # The same job count rules as `quality::lint` (at most 8 by default, LINT_JOBS overrides).
    cores="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
    jobs="${LINT_JOBS:-$((cores > 8 ? 8 : cores))}"
    case "$jobs" in
      ""|*[!0-9]*|0) printf '%s\n' "LINT_JOBS must be a positive integer, got: $jobs" >&2; exit 1 ;;
    esac
    printf '%s\n' "$files" | tr '\n' '\0' |
      xargs -r -0 -n 1 -P "$jobs" sh -c '
        file="$0"
        [ -f "$file" ] || exit 0
        if ! output=$(clang-tidy -p "$compile_database" "./$file" 2>&1); then
          printf "%s\n" "$output" >&2
          printf "lint: clang-tidy failed on %s\n" "$file" >&2
          exit 255
        fi
      '

# The tests without the `heavy` label (cmake.component_matrix, cmake.package_smoke); the
# heavy ones run too when the change touches the build or the package surface (a CMake
# file, a public header, the flake).
[working-directory: root]
tests:
    #!/usr/bin/env sh
    set -eu
    dir="${CMAKE_BUILD_DIR:-.build/default}"
    changed="$(just project::fast::changed)"
    run_heavy=no
    if [ "$changed" = ALL ]; then
      run_heavy=yes
    elif printf '%s\n' "$changed" |
         grep -Eq '(^|/)CMakeLists\.txt$|\.cmake$|^CMakePresets\.json$|^flake\.(nix|lock)$|^tests/cmake/|/include/'; then
      run_heavy=yes
    fi
    if [ "$run_heavy" = yes ]; then
      echo "check-fast: the change touches the build or the package surface: running every test"
      ctest --test-dir "$dir" --output-on-failure
    else
      echo "check-fast: skipping the heavy tests (label heavy): no CMake, flake or public header change"
      ctest --test-dir "$dir" --output-on-failure -LE heavy
    fi

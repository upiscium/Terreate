root := justfile_directory()

# The development loop: PARTIAL by design. It is not the gate (`just project::check`),
# which a merge still needs. Everything here compares against FAST_BASE (default
# origin/main): the committed changes since the merge base, the uncommitted changes and
# the untracked files.

# Print the changed paths, one per line, deleted and renamed ones included (a deleted public
# header or CMake file changes the package surface). Prints "ALL" when the changes cannot be
# determined (no base ref, no merge base): then every check runs in full.
[working-directory: root]
changed:
    #!/usr/bin/env sh
    set -eu
    base="${FAST_BASE:-origin/main}"
    git="git -c core.quotePath=false"
    if ! $git rev-parse --verify --quiet "$base^{commit}" >/dev/null ||
       ! $git merge-base "$base" HEAD >/dev/null 2>&1; then
      echo ALL
      exit 0
    fi
    # Each command is checked: a failure must not leave a shorter list that skips checks.
    committed="$($git diff --name-only --no-renames "$base"...HEAD)" || { echo ALL; exit 0; }
    working="$($git diff --name-only --no-renames HEAD)" || { echo ALL; exit 0; }
    untracked="$($git ls-files --others --exclude-standard)" || { echo ALL; exit 0; }
    printf '%s\n%s\n%s\n' "$committed" "$working" "$untracked" | grep -v '^$' | sort -u || true

# clang-tidy on the changed .cpp files only, with the rules of `quality::lint` (it does the
# work, given the list). A header change does not lint its includers: only the gate does.
[working-directory: root]
lint:
    #!/usr/bin/env sh
    set -eu
    changed="$(just project::fast::changed)"
    if [ "$changed" = ALL ]; then
      printf '%s\n' "check-fast: the changes cannot be determined, linting every file" >&2
      exec just project::quality::lint
    fi
    files="$(printf '%s\n' "$changed" | grep -E '\.(cc|cpp|cxx)$' || true)"
    if printf '%s\n' "$changed" | grep -Eq '\.(h|hh|hpp|hxx)$'; then
      printf '%s\n' "check-fast: a header changed: the .cpp files that include it are not linted here; only the gate lints them" >&2
    fi
    if [ -z "$files" ]; then
      echo "check-fast: no changed .cpp files to lint"
      exit 0
    fi
    echo "check-fast: linting $(printf '%s\n' "$files" | wc -l | tr -d ' ') changed .cpp file(s)"
    list="$(mktemp)"
    trap 'rm -f "$list"' EXIT
    printf '%s\n' "$files" > "$list"
    LINT_LIST="$list" just project::quality::lint

# The tests without the `heavy` label (cmake.component_matrix, cmake.package_smoke); the
# heavy ones run too when the change touches the build or the package surface (a CMake
# file or the package template, a public header, the flake). What only they would catch
# in other changes is found by the gate.
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
         grep -Eq '(^|/)CMakeLists\.txt$|\.cmake$|\.cmake\.in$|^cmake/|^CMakePresets\.json$|^flake\.(nix|lock)$|^tests/cmake/|(^|/)include/'; then
      run_heavy=yes
    fi
    if [ "$run_heavy" = yes ]; then
      echo "check-fast: the change touches the build or the package surface: running every test"
      ctest --test-dir "$dir" --output-on-failure
    else
      echo "check-fast: skipping the heavy tests (label heavy): no CMake, flake or public header change"
      ctest --test-dir "$dir" --output-on-failure -LE heavy
    fi

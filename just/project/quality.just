root := justfile_directory()

[working-directory: root]
format-check:
    #!/usr/bin/env sh
    set -eu
    git ls-files --cached --others --exclude-standard -z -- \
      '*.h' '*.hh' '*.hpp' '*.c' '*.cc' '*.cpp' '*.cxx' |
      xargs -0 sh -c '
        set -eu
        for file do
          [ -f "$file" ] || continue
          clang-format --dry-run --Werror "./$file"
        done
      ' _

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
    # One clang-tidy per core (LINT_JOBS overrides). Each file's output is held back and
    # printed only if that file fails; the first failure (exit 255) stops xargs and
    # makes the recipe fail, as the serial loop did.
    jobs="${LINT_JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
    git ls-files --cached --others --exclude-standard -z -- \
      '*.cc' '*.cpp' '*.cxx' |
      xargs -0 -n 1 -P "$jobs" sh -c '
        file="$0"
        [ -f "$file" ] || exit 0
        if ! output=$(clang-tidy -p "$compile_database" "./$file" 2>&1); then
          printf "%s\n" "$output" >&2
          printf "lint: clang-tidy failed on %s\n" "$file" >&2
          exit 255
        fi
      '

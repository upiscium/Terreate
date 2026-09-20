root := justfile_directory()

[working-directory: root]
format-check:
    #!/usr/bin/env sh
    set -eu
    files=$(git ls-files '*.h' '*.hh' '*.hpp' '*.c' '*.cc' '*.cpp' '*.cxx')
    [ -z "$files" ] || clang-format --dry-run --Werror $files

[working-directory: root]
lint:
    #!/usr/bin/env sh
    set -eu
    files=$(git ls-files '*.cc' '*.cpp' '*.cxx')
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
    [ -z "$files" ] || clang-tidy -p "$compile_database" $files

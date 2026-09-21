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
    git ls-files --cached --others --exclude-standard -z -- \
      '*.cc' '*.cpp' '*.cxx' |
      xargs -0 sh -c '
        set -eu
        for file do
          [ -f "$file" ] || continue
          clang-tidy -p "$compile_database" "./$file"
        done
      ' _

#!/bin/sh
# Build and test Terreate on Debian 13 inside a container, with no Nix.
#
# Usage: tools/verify-non-nix.sh [--no-xvfb] [--build-type TYPE] [--ccache-dir DIR] [--tmpfs-size SIZE] [--log FILE]
#
# It builds tools/non-nix/Dockerfile (network: image pull, apt, and the pinned cgltf
# archive through tools/fetch-deps.sh), copies the working tree (without .git,
# .build and generated files) into the container, then configures, builds and runs
# CTest there as a non-root user and prints a summary. The checkout is never
# written to by the container.
#
# Container tool: docker, or podman if docker is missing. An already set DOCKER_HOST
# is honoured as is (for a rootless daemon: DOCKER_HOST=unix:///run/user/$(id -u)/docker.sock).
#
# Exit status: 0 only when CTest passed (skipped tests are not failures); 2 when no
# usable container tool exists; otherwise CTest's or the build's failure status.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
image=terreate-non-nix:trixie
xvfb=1
build_type=Debug
ccache_dir=
tmpfs_size=
log=

while [ "$#" -gt 0 ]; do
  case "$1" in
    --no-xvfb) xvfb=0 ;;
    --build-type) shift; build_type=${1:?--build-type needs a value} ;;
    --ccache-dir) shift; ccache_dir=${1:?--ccache-dir needs a directory} ;;
    --tmpfs-size) shift; tmpfs_size=${1:?--tmpfs-size needs a size such as 12g} ;;
    --log) shift; log=${1:?--log needs a file} ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "verify-non-nix: unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

engine=
for candidate in docker podman; do
  if command -v "$candidate" >/dev/null 2>&1; then
    if "$candidate" info >/dev/null 2>&1; then
      engine=$candidate
      break
    fi
    echo "verify-non-nix: '$candidate' is installed but its daemon is not usable" \
      "(DOCKER_HOST=${DOCKER_HOST:-unset}); trying the next tool" >&2
  fi
done
if [ -z "$engine" ]; then
  echo "verify-non-nix: no usable container tool (need a working docker or podman)." >&2
  echo "  Nothing was built or tested. Start a daemon, or set DOCKER_HOST, and run again." >&2
  exit 2
fi
echo "verify-non-nix: using $engine (DOCKER_HOST=${DOCKER_HOST:-unset})"

"$engine" build -t "$image" -f "$here/non-nix/Dockerfile" "$root/tools"

mount_args=
if [ -n "$tmpfs_size" ]; then
  # Keep the build tree and compiler temporaries in memory instead of the container
  # storage (a Debug build of every test takes several gigabytes).
  mount_args="--tmpfs /work/build:rw,exec,nosuid,uid=1000,gid=1000,size=$tmpfs_size --tmpfs /tmp:rw,exec,nosuid,size=2g"
fi
env_ccache=
if [ -n "$ccache_dir" ]; then
  mkdir -p "$ccache_dir"
  ccache_dir=$(cd "$ccache_dir" && pwd)
  # The container user is uid 1000; the directory must be writable by it.
  mount_args="$mount_args -v $ccache_dir:/ccache"
  env_ccache="-e CCACHE_DIR=/ccache -e CMAKE_CXX_COMPILER_LAUNCHER=ccache"
fi

run_container() {
  # shellcheck disable=SC2086
  tar -C "$root" \
      --exclude=./.git --exclude=./.build --exclude=./compile_commands.json \
      -cf - . |
    "$engine" run --rm -i $mount_args $env_ccache \
      -e TERREATE_BUILD_TYPE="$build_type" \
      -e TERREATE_VERIFY_XVFB="$xvfb" \
      "$image" \
      sh -c 'mkdir -p /work/src && tar -xf - -C /work/src && sh /work/src/tools/non-nix/build-and-test.sh'
}

status=0
if [ -n "$log" ]; then
  # The status of a pipeline is the last command's, so record the container's status separately.
  statusfile=$(mktemp)
  { rc=0; run_container || rc=$?; echo "$rc" >"$statusfile"; } 2>&1 | tee "$log"
  status=$(cat "$statusfile")
  rm -f "$statusfile"
else
  run_container || status=$?
fi
exit "$status"

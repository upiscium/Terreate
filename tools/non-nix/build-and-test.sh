#!/bin/sh
# Runs INSIDE the verification container (see tools/verify-non-nix.sh): configure,
# build and run CTest on the Terreate source tree in /work/src, then print a summary.
# POSIX sh. Nothing here knows about Nix.
set -eu

src=${TERREATE_SRC:-/work/src}
build=${TERREATE_BUILD:-/work/build}
build_type=${TERREATE_BUILD_TYPE:-Debug}
use_xvfb=${TERREATE_VERIFY_XVFB:-1}
jobs=${TERREATE_VERIFY_JOBS:-$(nproc)}

# The directories fetched by tools/fetch-deps.sh when the image was built.
cgltf_dir=$(find /opt/deps -maxdepth 1 -type d -name 'cgltf-*' | head -n 1)
vulkan_headers_root=$(find /opt/deps -maxdepth 1 -type d -name 'Vulkan-Headers-*' | head -n 1)
vulkan_headers_dir=$vulkan_headers_root/include

echo "== versions"
g++ --version | head -n 1
cmake --version | head -n 1
ninja --version | sed 's/^/ninja /'
echo "GLFW: $(dpkg-query -W -f='${Version}' libglfw3-dev)"
echo "libvulkan-dev (loader and distribution headers; the headers are too old for Terreate): $(dpkg-query -W -f='${Version}' libvulkan-dev)"
echo "Vulkan-Hpp headers used: $vulkan_headers_dir (VK_HEADER_VERSION $(sed -n 's/^#define VK_HEADER_VERSION \([0-9]*\)$/\1/p' "$vulkan_headers_dir/vulkan/vulkan_core.h"))"
echo "VulkanMemoryAllocator: $(dpkg-query -W -f='${Version}' libvulkan-memory-allocator-dev)"
echo "stb: $(dpkg-query -W -f='${Version}' libstb-dev)"
echo "mesa-vulkan-drivers (lavapipe): $(dpkg-query -W -f='${Version}' mesa-vulkan-drivers)"
echo "vulkan-validationlayers: $(dpkg-query -W -f='${Version}' vulkan-validationlayers)"
echo "cgltf: ${cgltf_dir:-NOT FOUND}"

# The package test builds an external consumer of the installed package. A consumer finds
# Vulkan through FindVulkan, which starts at $VULKAN_SDK/include, before the distribution's
# too old headers in /usr/include.
export VULKAN_SDK="$vulkan_headers_root"

echo "== configure"
cd "$src"
cmake -S "$src" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DTERREATE_BUILD_DOCS=OFF \
  -DTERREATE_VULKAN_HEADERS_INCLUDE_DIR="$vulkan_headers_dir" \
  -DTERREATE_CGLTF_INCLUDE_DIR="$cgltf_dir"

echo "== build"
cmake --build "$build" -j "$jobs"

# run_ctest <ctest arguments>: under Xvfb (a window system in memory) unless disabled.
run_ctest() {
  if [ "$use_xvfb" = 1 ]; then
    xvfb-run -a -s "-screen 0 1280x1024x24" ctest "$@"
  else
    env -u DISPLAY -u WAYLAND_DISPLAY ctest "$@"
  fi
}

echo "== test"
cd "$build"
if [ "$use_xvfb" = 1 ]; then
  echo "(display: Xvfb through xvfb-run)"
else
  echo "(display: none; window tests are expected to SKIP)"
fi
status=0
log=$build/ctest-output.log
run_ctest -j "$jobs" >"$log" 2>&1 || status=$?

echo "== summary"
passed=$(grep -c ' Passed ' "$log" || true)
skipped=$(grep -c '\*\*\*Skipped' "$log" || true)
failed=$(grep -cE '\*\*\*(Failed|Exception|Timeout|Not Run)' "$log" || true)
# BLOCKED: tests that report a missing prerequisite (they fail in CTest); count the tests.
blocked=$(grep -E '^[0-9]+: .*BLOCKED' "$log" | sed -E 's/^([0-9]+):.*/\1/' | sort -u | wc -l | tr -d ' ')
echo "ctest exit status: $status"
echo "passed: $passed  skipped: $skipped  failed: $failed  blocked (tests printing BLOCKED): $blocked"
if [ "$skipped" -gt 0 ]; then
  echo "skipped tests and the reason they print:"
  grep '\*\*\*Skipped' "$log" | sed -E 's/^ *[0-9]+\/[0-9]+ Test +#[0-9]+: ([^ ]+) .*/\1/' |
  while read -r name; do
    echo "  $name"
    run_ctest -R "^${name}\$" -V </dev/null 2>&1 | grep -E '^[0-9]+: .*SKIP' | sed -E 's/^[0-9]+: +/      /' | sort -u | head -n 5 || true
  done
fi
if [ "$failed" -gt 0 ]; then
  echo "failed tests:"
  grep -E '\*\*\*(Failed|Exception|Timeout|Not Run)' "$log" | sed -E 's/^ *[0-9]+\/[0-9]+ Test +#[0-9]+: /  /'
  echo "== output of the failed tests (rerun)"
  run_ctest --rerun-failed --output-on-failure 2>&1 | head -n 400 || true
fi
exit "$status"

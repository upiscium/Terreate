# Building without Nix

Terreate is a library: you can build and use it with any toolchain that provides its dependencies.
The pinned [Nix](https://nixos.org/) shell (`nix develop`) stays the **reference environment**: it is the
only place the full quality gate runs, and it is the source of every version this page calls "pinned". This
page lists what Terreate needs, how CMake finds each dependency, where to get it without Nix (Debian 13
package names, plus a helper script for the two things Debian does not provide), the exact commands, and the
result of a real run on Debian 13 inside a container with no Nix.

CMake never downloads anything and has no Nix-specific logic. Every dependency location reaches CMake through
an environment variable, a CMake cache variable, or `find_package` / `find_path`.

## What is promised outside Nix

| | Outside Nix |
| --- | --- |
| Configure, build, and CTest | **Promised**, on Debian 13 with the packages and pinned downloads below (this is what `tools/verify-non-nix.sh` runs). Other distributions need the same dependencies at the minimum versions below; they are not verified |
| Documentation build and the CTests `docs.doxygen` / `docs.sphinx` | Optional. Configure with `-DTERREATE_BUILD_DOCS=OFF` (these tests then do not exist) |
| Format (`clang-format`), lint (`clang-tidy`) | Optional; the configuration files are in the repository, nothing else is needed from the build |
| The gate, `just project::check` | **Nix only.** It enters the pinned shell itself and also runs format, lint, the documentation tests, and the whole CTest suite in one pinned environment |

A CTest run outside Nix is therefore a valid check of "does it build and do the tests pass here", and is not the
gate. Anything the gate would catch through a newer or older compiler, `clang-tidy` or the documentation tests is
not covered.

## Dependency inventory

"Found by" says how CMake locates it: an environment variable, a cache variable, or a search. Where two
spellings exist the cache variable wins, and a value that is set but wrong is an error, never a fall-through to
something else on the system.

| Dependency | Needed for | Minimum | Found by | Debian 13 (trixie) | Notes |
| --- | --- | --- | --- | --- | --- |
| GCC/g++ (C++23) | everything | GCC 14 verified; older not verified | `CMAKE_CXX_COMPILER` (the build refuses any other compiler id) | `g++` (14.2) via `build-essential` | The Nix shell has GCC 15.3. Some tests link with GNU ld `-Wl,--wrap`, so they need GNU ld |
| CMake | everything | 3.28 | n/a | `cmake` (3.31.6) | The Nix shell has 4.3.4 |
| Ninja | the preset and the verified commands | any recent | `-G Ninja` | `ninja-build` (1.12.1) | Other generators are not verified |
| GLFW | Platform, the windowed examples | 3.4 | `find_package(glfw3 3.4 CONFIG)`; `glfw3_DIR` or `CMAKE_PREFIX_PATH` for a private prefix | `libglfw3-dev` (3.4) | Pulls in the X11 and Wayland development packages itself. The Nix shell has 3.4 |
| Vulkan loader and C headers | Graphics | Vulkan 1.3 | `find_package(Vulkan)` (provides `Vulkan::Vulkan`) | `libvulkan-dev` (1.4.309) | Debian's `libvulkan-dev` also ships `vulkan.hpp`, but see the next row |
| Vulkan-Hpp (`vulkan/vulkan.hpp`, `vulkan/vulkan_raii.hpp`) | Graphics | **Vulkan-Headers 1.4.321** (needs `vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR`) | **Explicit only**: cache variable `TERREATE_VULKAN_HEADERS_INCLUDE_DIR`, else environment variable `VULKAN_HEADERS_INCLUDE`; never searched | **Too old**: `libvulkan-dev` is 1.4.309 | Use `tools/fetch-deps.sh vulkan-headers` (pinned to v1.4.350, the version of the Nix shell). The directory that contains `vulkan/` and `vk_video/`, for example `<dir>/include`. The loader may be older than the headers |
| VulkanMemoryAllocator | Graphics | 3.2.1 verified (the Nix shell has 3.3.0); an older minimum is not determined | `find_package(VulkanMemoryAllocator CONFIG)` (provides `GPUOpen::VulkanMemoryAllocator`) | `libvulkan-memory-allocator-dev` (3.2.1) | |
| stb (`stb_image.h`) | `AdapterAssetImageStb` only | the header at any 2.x revision that builds (Debian: 2024-11-09) | cache variable `TERREATE_STB_INCLUDE_DIR`, else environment variable `STB_INCLUDE`, else a search for `stb_image.h` (also below an `stb` directory) | `libstb-dev` (installs `/usr/include/stb/stb_image.h`, found by the search) | A private build input of that one library, never a package dependency. Without it the Adapter is off by default |
| cgltf (`cgltf.h`) | `AdapterAssetGltfCgltf` only | the pinned revision `85cd623` (flake.lock) | cache variable `TERREATE_CGLTF_INCLUDE_DIR`, else environment variable `CGLTF_INCLUDE`, else a search for `cgltf.h` (also below a `cgltf` directory) | **Not packaged** | Use `tools/fetch-deps.sh cgltf`. A private build input, like stb. Without it the Adapter is off by default |
| Vulkan ICD for the tests | GPU tests | Vulkan 1.3 device | the Vulkan loader | `mesa-vulkan-drivers` (Mesa lavapipe, a software device) | Any real driver works as well |
| Khronos validation layer | GPU tests | any recent | the Vulkan loader (`VK_LAYER_PATH` if you installed it privately) | `vulkan-validationlayers` | Some tests fail without it; see below |
| X server (optional) | window tests | any | `DISPLAY` | `xvfb`, `xauth` (run under `xvfb-run`) | Without a display the window tests report SKIP |
| `vulkaninfo` (optional) | diagnosis | any | `PATH` | `vulkan-tools` | |
| ccache (optional) | speed | any | `CMAKE_CXX_COMPILER_LAUNCHER=ccache` | `ccache` | |
| Doxygen, Sphinx with Furo, MyST-Parser and Breathe | documentation (`TERREATE_BUILD_DOCS=ON`) | Doxygen 1.9, Sphinx 8 | `find_program(doxygen)`, `find_program(sphinx-build)` | `doxygen`, `python3-sphinx`, `python3-myst-parser`, `python3-breathe`; Furo is **not packaged**, install it with `pip install furo` | Optional. Not exercised in the verification below |
| `just`, `clang-format`, `clang-tidy` | the gate and its parts | the Nix shell's versions decide | `PATH` | `just`, `clang-format`, `clang-tidy` | Optional; only the gate needs them together |

Not needed by the build: the shader tools of the Nix shell (Slang, shaderc, glslang, SPIR-V Tools), `glm` and
`spdlog` (the verified Debian build installs none of them).

An installed Terreate is checked by `cmake/TerreateConfig.cmake.in`: it runs `find_package(Vulkan)`,
`find_package(VulkanMemoryAllocator CONFIG)` and `find_package(glfw3 3.4 CONFIG)` for the components requested,
and compiles a small probe against the `Vulkan::Vulkan` include directories to check that they hold Vulkan-Hpp
complete and new enough for Terreate's public headers. It does not read `TERREATE_VULKAN_HEADERS_INCLUDE_DIR`
or `VULKAN_HEADERS_INCLUDE`: a consumer needs a Vulkan package that provides such headers (see the note about
`VULKAN_SDK` and `Vulkan_INCLUDE_DIR` below).

## Quick path on Debian 13

```sh
# Packages (root). Everything except cgltf and the Vulkan-Hpp headers comes from Debian.
apt-get install --no-install-recommends \
  build-essential cmake ninja-build pkg-config git ca-certificates curl \
  libglfw3-dev libvulkan-dev libvulkan-memory-allocator-dev libstb-dev \
  mesa-vulkan-drivers vulkan-validationlayers vulkan-tools xvfb xauth

# cgltf and Vulkan-Headers 1.4.350: explicit download, pinned commit, SHA-256 checked before use.
tools/fetch-deps.sh "$HOME/terreate-deps"
# It prints the exact lines to use, for example:
#   export CGLTF_INCLUDE="$HOME/terreate-deps/cgltf-85cd62382dfea638278962690cf515023f33ed00"
#   export VULKAN_HEADERS_INCLUDE="$HOME/terreate-deps/Vulkan-Headers-8864cdc896bbc2a9b6eb36b3218fc9ef57908d77/include"
#   (or the -D spellings below)
```

## Configure, build, test

The `default` preset sets GCC/g++, Ninja, Debug, the build directory `.build/default`, and
`TERREATE_BUILD_DOCS=ON`. It contains nothing Nix-specific, so outside Nix it works with documentation turned
off on the command line (a `-D` on the command line overrides the preset). The explicit form below is the one
that was verified:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DTERREATE_BUILD_DOCS=OFF \
  -DTERREATE_VULKAN_HEADERS_INCLUDE_DIR="$HOME/terreate-deps/Vulkan-Headers-8864cdc896bbc2a9b6eb36b3218fc9ef57908d77/include" \
  -DTERREATE_CGLTF_INCLUDE_DIR="$HOME/terreate-deps/cgltf-85cd62382dfea638278962690cf515023f33ed00"
cmake --build build -j "$(nproc)"
export VULKAN_SDK="$HOME/terreate-deps/Vulkan-Headers-8864cdc896bbc2a9b6eb36b3218fc9ef57908d77"   # for cmake.package_smoke, see the notes
ctest --test-dir build --output-on-failure -j "$(nproc)"      # with a display: xvfb-run -a ctest ...
```

The same with the preset: `cmake --preset default -DTERREATE_BUILD_DOCS=OFF -D...` then
`cmake --build --preset default` and `ctest --preset default`.

Notes:

- Graphics fails closed at configure time without an explicit Vulkan-Hpp directory (the cache variable above or
  `VULKAN_HEADERS_INCLUDE`), and again if the directory does not hold complete Vulkan-Hpp headers new enough for
  Terreate. A Core-only, Asset-only or Graphics-off configure needs neither.
- stb needs no variable when `libstb-dev` is installed. The cgltf and stb Adapters turn themselves off by default
  when their header is not found; asking for one explicitly without its header is a configure error.
- If a directory is also the compiler's own system include directory (`/usr/include` for Vulkan-Hpp on a
  distribution whose headers are new enough), pass it the same way: `-DTERREATE_VULKAN_HEADERS_INCLUDE_DIR=/usr/include`.
- Package and matrix tests (`cmake.package_smoke`, `cmake.component_matrix`) start their own configures; they
  receive the Vulkan-Hpp, stb and cgltf directories from the parent configure, so the cache variables are enough
  for those three.
- An **installed** Terreate, and the external consumer that `cmake.package_smoke` builds, find Vulkan through
  `find_package(Vulkan)`, not through the Terreate variable. If the Vulkan headers that `FindVulkan` sees first
  (usually `/usr/include`) are older than 1.4.321, as on Debian 13, point it at the newer ones: for the test run
  `export VULKAN_SDK=<fetch dir>/Vulkan-Headers-<rev>` (FindVulkan looks in `$VULKAN_SDK/include` before the
  system directories), and for your own consumer project `-DVulkan_INCLUDE_DIR=<dir>/include`. The verification
  script exports `VULKAN_SDK` for this reason; without it `cmake.package_smoke` fails on Debian 13.

## Tests without a GPU, a driver, or a display

Install Mesa's lavapipe (`mesa-vulkan-drivers`): it is a conformant Vulkan device that runs on the CPU, and it
is what the Nix shell uses too. Install the Khronos validation layer (`vulkan-validationlayers`): the tests that
count validation messages need it. `vulkaninfo --summary` shows the device the loader will use.

Each test reports one of these words; the full rules are in the [test matrix](graphics-test-matrix.md).

| Word | Meaning here | CTest shows it as |
| --- | --- | --- |
| PASS | the test ran and its checks held | Passed |
| SKIP | the environment lacks something optional, for example a display or a device capability (the test prints which) | Skipped, never counted as passed |
| BLOCKED | a prerequisite the test needs is missing, for example no usable device, or no validation layer where the test requires one | Failed |
| FAIL | a check did not hold | Failed |

Without a display the window tests (`platform.native_*`, `integration.native_platform_graphics`,
`integration.two_window_swapchain`, `example.triangle`) report SKIP. To run them, start a virtual X server:
`xvfb-run -a ctest ...`. The verification script does this by default (`--no-xvfb` turns it off).

## Helper scripts

| Script | What it does |
| --- | --- |
| `tools/fetch-deps.sh <dir> [cgltf] [vulkan-headers]` | POSIX sh. The only place that uses the network, and only when you run it. Downloads the pinned archives (cgltf at the flake.lock revision; Vulkan-Headers v1.4.350) from GitHub, checks their SHA-256 against the value in the script, and only then unpacks them below `<dir>`. A mismatch unpacks nothing and exits non-zero. It prints the `export` and `-D` lines to use. It writes nothing outside `<dir>` and runs again without downloading what is already there. To move a pin, change the revision and the SHA-256 in the script |
| `tools/verify-non-nix.sh [--no-xvfb] [--build-type T] [--ccache-dir D] [--tmpfs-size S] [--log F]` | Builds the Debian 13 image, copies the working tree into a container, configures, builds, runs CTest as a non-root user and prints a summary: passed, skipped, failed and BLOCKED counts, and each skipped test with the reason it prints. Uses `docker` (or `podman`) and honours an already set `DOCKER_HOST`; if no container tool is usable it says so and exits with status 2 without building. `--tmpfs-size 12g` keeps the build tree in memory on a host with little disk |
| `tools/non-nix/Dockerfile` | The image: Debian 13, the apt packages above, a non-root `builder` user, and `tools/fetch-deps.sh` for the rest. No Nix |
| `tools/non-nix/build-and-test.sh` | Runs inside the container: prints versions, configures, builds, runs CTest and prints the summary. It is what the verification section below records |

## Verified on

Debian 13 (trixie) container, no Nix, rootless Docker, build user uid 1000. Run with
`DOCKER_HOST=unix:///run/user/1000/docker.sock tools/verify-non-nix.sh --tmpfs-size 12g`.

Date: 2026-11-28. Source: `origin/main` at `8ca8a99` plus the change set that introduced this page (the
working tree of that change; the page itself was written after the run).

| Component | Version found |
| --- | --- |
| GCC/g++ | 14.2.0 (Debian 14.2.0-19) |
| CMake / Ninja | 3.31.6 / 1.12.1 |
| GLFW | 3.4 (`libglfw3-dev` 3.4-3) |
| Vulkan loader and distribution headers | `libvulkan-dev` 1.4.309.0-1 (headers too old for Terreate, not used for Vulkan-Hpp) |
| Vulkan-Hpp headers used | Vulkan-Headers v1.4.350, `VK_HEADER_VERSION 350`, from `tools/fetch-deps.sh` |
| VulkanMemoryAllocator | 3.2.1 (`libvulkan-memory-allocator-dev` 3.2.1+dfsg-1) |
| stb | `libstb-dev` 0.0~git20241109.5c20573+ds-1, found by the search |
| cgltf | revision `85cd623`, from `tools/fetch-deps.sh` |
| Vulkan device and layer | Mesa lavapipe 25.0.7 (LLVM 19.1.7, Vulkan 1.4.305), `vulkan-validationlayers` 1.4.309.0-1 |
| Display | Xvfb through `xvfb-run` (1280x1024x24) |

Configuration: Debug, `-DTERREATE_BUILD_DOCS=OFF`, `-DTERREATE_VULKAN_HEADERS_INCLUDE_DIR` and
`-DTERREATE_CGLTF_INCLUDE_DIR` as in the commands above, `VULKAN_SDK` exported for the package test. Result:
configure and build succeeded (203 build steps, no environment variable other than `VULKAN_SDK` for the
package test); CTest ran 95 tests: **93 passed, 2 skipped, 0 failed, 0 BLOCKED**. This includes the window
tests (`platform.native_*`, `integration.native_platform_graphics`, `integration.two_window_swapchain`,
`example.triangle`), which passed under Xvfb, and `cmake.component_matrix` and `cmake.package_smoke`. The two
skips, with the reason each test prints:

- `graphics.retirement.multiqueue`: `true-multi-queue=SKIP (no valid distinct native queues)`. Lavapipe has one queue.
- `graphics.swapchain_retirement`: `retirement=SKIP reason=VK_KHR_surface_maintenance1 unavailable`. Lavapipe 25.0 lacks it.

What this does not show: behaviour with a real GPU or compositor, other distributions, other compiler versions,
the documentation build, format and lint. Because the validation layer (1.4.309) is older than the headers
(1.4.350) it may not know the newest structures; the tests did not report any validation message.

Problems that this run found and that are fixed in the same change: Debian's Vulkan headers are too old
(hence the Vulkan-Headers download); the header probe broke for a Vulkan-Hpp directory that is a system include
directory; two CMake-driven tests needed the Nix environment variables; one test script mistook the source
path `/work/src` for a private `src` directory; and one test compared uninitialised padding bytes of a
readback buffer.

# Debian 13 (trixie) build environment for Terreate WITHOUT Nix.
#
# Built and used by tools/verify-non-nix.sh (build context: the tools/ directory).
# Everything comes from Debian packages except cgltf, which Debian does not
# package: tools/fetch-deps.sh downloads the pinned archive, checks its SHA-256 and
# unpacks it. There is no Nix in this image.
FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain: GCC/g++, CMake, Ninja, ccache (optional), pkg-config.
# Libraries: GLFW (+ its X11/Wayland development packages as dependencies),
# Vulkan headers incl. Vulkan-Hpp and the loader (libvulkan-dev), VulkanMemoryAllocator, stb.
# Vulkan runtime for tests without a GPU: Mesa lavapipe (mesa-vulkan-drivers) and the
# Khronos validation layer (vulkan-validationlayers); vulkan-tools gives vulkaninfo.
# Window tests: xvfb + xauth (an X server in memory) and its Vulkan presentation path.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config ccache git ca-certificates curl \
      libglfw3-dev libvulkan-dev libvulkan-memory-allocator-dev libstb-dev \
      mesa-vulkan-drivers vulkan-validationlayers vulkan-tools \
      xvfb xauth \
 && rm -rf /var/lib/apt/lists/*

# A non-root build user (uid 1000) owns the build.
RUN useradd --create-home --uid 1000 --shell /bin/sh builder \
 && mkdir -p /work /opt/deps \
 && chown builder:builder /work /opt/deps

COPY fetch-deps.sh /usr/local/bin/terreate-fetch-deps
USER builder
WORKDIR /work

# cgltf and Vulkan-Headers (Debian 13 ships 1.4.309, Terreate needs >= 1.4.321): pinned and
# hash-verified by the helper script (explicit network use, at image build time).
RUN terreate-fetch-deps /opt/deps

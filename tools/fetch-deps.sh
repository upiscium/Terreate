#!/bin/sh
# Fetch the build inputs that a distribution does not package (or packages too old),
# for a build outside Nix. Network access happens only when you run this script, only
# for the pinned archives below, and an archive is used only after its SHA-256 matches.
# CMake never downloads anything.
#
# Usage: tools/fetch-deps.sh <destination-directory> [cgltf] [vulkan-headers]
#
# With no dependency name, every dependency below is fetched.
#
#   cgltf           the single header behind AdapterAssetGltfCgltf (no Debian package).
#                   Pinned to the revision in flake.lock (github:jkuhlmann/cgltf).
#   vulkan-headers  Vulkan-Hpp (vulkan/vulkan.hpp, vulkan_raii.hpp) and the C headers.
#                   Terreate needs 1.4.321 or newer (vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR);
#                   Debian 13 ships 1.4.309. Pinned to v1.4.350, the version of the Nix shell.
#
# The script writes only below <destination-directory> and prints the lines to use
# afterwards. It needs curl or wget, tar, and sha256sum (or shasum).
set -eu

CGLTF_REV=85cd62382dfea638278962690cf515023f33ed00
CGLTF_URL="https://github.com/jkuhlmann/cgltf/archive/${CGLTF_REV}.tar.gz"
CGLTF_SHA256=793aba47e4bbe7a1a4c5822313e0b187c07cdb0a610d1ff65dcbd0619e90a65b

# Vulkan-Headers v1.4.350 is this commit.
VULKAN_HEADERS_REV=8864cdc896bbc2a9b6eb36b3218fc9ef57908d77
VULKAN_HEADERS_URL="https://github.com/KhronosGroup/Vulkan-Headers/archive/${VULKAN_HEADERS_REV}.tar.gz"
VULKAN_HEADERS_SHA256=35824117f649befc664a12622bc801eddbba036ac03d906db400ee1c1e7903e9

usage() {
  echo "usage: $0 <destination-directory> [cgltf] [vulkan-headers]" >&2
  echo "  downloads the pinned archives, verifies their SHA-256 and unpacks them below" >&2
  echo "  <destination-directory>; with no name, fetches all of them" >&2
}

die() {
  echo "fetch-deps: $*" >&2
  exit 1
}

if [ "$#" -lt 1 ]; then
  usage
  exit 2
fi
case "$1" in
  -h|--help) usage; exit 0 ;;
esac

dest=$1
shift
if [ "$#" -eq 0 ]; then
  set -- cgltf vulkan-headers
fi
for name in "$@"; do
  case "$name" in
    cgltf|vulkan-headers) ;;
    *) usage; die "unknown dependency: $name" ;;
  esac
done

mkdir -p "$dest"
dest=$(cd "$dest" && pwd)

if command -v sha256sum >/dev/null 2>&1; then
  sha256_of() { sha256sum "$1" | cut -d ' ' -f 1; }
elif command -v shasum >/dev/null 2>&1; then
  sha256_of() { shasum -a 256 "$1" | cut -d ' ' -f 1; }
else
  die "need sha256sum or shasum to verify the downloads"
fi

if command -v curl >/dev/null 2>&1; then
  download() { curl --fail --silent --show-error --location --output "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
  download() { wget --quiet --output-document="$2" "$1"; }
else
  die "need curl or wget"
fi

work=$(mktemp -d "$dest/.fetch.XXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# fetch_archive <label> <url> <sha256> <top-level directory in the archive> <marker file inside it>
# Leaves the verified tree at <destination-directory>/<top-level directory>.
fetch_archive() {
  label=$1 url=$2 want=$3 top=$4 marker=$5
  if [ -f "$dest/$top/$marker" ]; then
    echo "fetch-deps: $label already present at $dest/$top (not downloaded again)" >&2
    return 0
  fi
  echo "fetch-deps: downloading $url" >&2
  download "$url" "$work/$label.tar.gz" || die "download failed: $url"
  actual=$(sha256_of "$work/$label.tar.gz")
  if [ "$actual" != "$want" ]; then
    die "SHA-256 mismatch for $label: expected $want, got $actual; nothing was unpacked"
  fi
  mkdir "$work/$label.unpack"
  tar -xzf "$work/$label.tar.gz" -C "$work/$label.unpack"
  [ -f "$work/$label.unpack/$top/$marker" ] || die "the $label archive does not contain $marker"
  [ ! -e "$dest/$top" ] || die "$dest/$top exists but does not hold $marker; remove it and run again"
  mv "$work/$label.unpack/$top" "$dest/$top"
  echo "fetch-deps: $label verified and unpacked to $dest/$top" >&2
}

use_lines=
for name in "$@"; do
  case "$name" in
    cgltf)
      top=cgltf-${CGLTF_REV}
      fetch_archive cgltf "$CGLTF_URL" "$CGLTF_SHA256" "$top" cgltf.h
      use_lines="${use_lines}
# cgltf
export CGLTF_INCLUDE=\"$dest/$top\"
#   or: cmake ... -DTERREATE_CGLTF_INCLUDE_DIR=\"$dest/$top\"
"
      ;;
    vulkan-headers)
      top=Vulkan-Headers-${VULKAN_HEADERS_REV}
      fetch_archive vulkan-headers "$VULKAN_HEADERS_URL" "$VULKAN_HEADERS_SHA256" "$top" include/vulkan/vulkan_raii.hpp
      use_lines="${use_lines}
# Vulkan-Hpp / Vulkan headers
export VULKAN_HEADERS_INCLUDE=\"$dest/$top/include\"
#   or: cmake ... -DTERREATE_VULKAN_HEADERS_INCLUDE_DIR=\"$dest/$top/include\"
"
      ;;
  esac
done

printf '\n# Use these (CMake reads the environment variable or the cache variable; the cache variable wins):\n%s' "$use_lines"

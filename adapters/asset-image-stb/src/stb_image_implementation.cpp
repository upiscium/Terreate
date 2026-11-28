// The one translation unit that compiles the stb_image implementation. The header is
// third-party code outside the repository (it comes from the build environment) and is
// included as a system header, so project warnings and clang-tidy do not apply to it.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image_backend.hpp"

#include <cstdlib>
#include <cstring>

namespace terreate::adapter::asset_image_stb::backend {
namespace {

// Every block has a 16-byte prefix holding its size (16 keeps the payload aligned).
constexpr std::size_t prefix = 16;
thread_local std::uint64_t live_bytes = 0;
thread_local std::uint64_t budget = UINT64_MAX;

auto sizeOf(const void *block) noexcept -> std::size_t {
  std::size_t size = 0;
  std::memcpy(&size, static_cast<const unsigned char *>(block) - prefix, sizeof size);
  return size;
}

} // namespace

void setBudget(std::uint64_t bytes) noexcept { budget = bytes; }

auto allocate(std::size_t size) noexcept -> void * {
  if (size > SIZE_MAX - prefix)
    return nullptr;
  const std::uint64_t total = size + prefix;
  if (live_bytes > budget || total > budget - live_bytes)
    return nullptr;
  void *base = std::malloc(total);
  if (base == nullptr)
    return nullptr;
  std::memcpy(base, &size, sizeof size);
  live_bytes += total;
  return static_cast<unsigned char *>(base) + prefix;
}

auto reallocate(void *block, std::size_t size) noexcept -> void * {
  if (block == nullptr)
    return allocate(size);
  void *fresh = allocate(size);
  if (fresh == nullptr)
    return nullptr;
  std::memcpy(fresh, block, sizeOf(block) < size ? sizeOf(block) : size);
  release(block);
  return fresh;
}

void release(void *block) noexcept {
  if (block == nullptr)
    return;
  const std::uint64_t total = sizeOf(block) + prefix;
  live_bytes = live_bytes >= total ? live_bytes - total : 0;
  std::free(static_cast<unsigned char *>(block) - prefix);
}

} // namespace terreate::adapter::asset_image_stb::backend

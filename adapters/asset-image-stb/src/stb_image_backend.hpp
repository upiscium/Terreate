#ifndef TERREATE_ADAPTER_ASSET_IMAGE_STB_BACKEND_HPP
#define TERREATE_ADAPTER_ASSET_IMAGE_STB_BACKEND_HPP

#include <cstddef>
#include <cstdint>

// The single place that configures stb_image. Private to this Adapter: it is not
// installed and no public header includes it. Both translation units that see stb
// (the decoder and the implementation unit) include this header, so the
// declarations and the implementation always agree on the feature set.
//
// Enabled: PNG, JPEG, BMP, TGA and Radiance HDR (with the float loader).
// Disabled on purpose:
//   - STBI_NO_STDIO: stb never touches the file system; only the from-memory entry
//     points exist.
//   - GIF, PSD, PIC, PNM: formats outside the baseline set (GIF and PSD in particular
//     are large attack surfaces with animation/layer features that ImageData cannot
//     represent). Their signatures are recognized by the decoder and reported as
//     unsupported formats.
//   - STBI_NO_SIMD: the portable code path only (no intrinsics header, identical
//     results on every compiler and CPU).
// Allocation goes through a per-thread byte budget (see `backend::setBudget`), so a
// decompression bomb (a tiny IDAT that inflates to gigabytes) fails as an allocation
// failure instead of exhausting memory.
// Left at their defaults: STBI_MAX_DIMENSIONS (1 << 24; the Adapter's DecodeLimits are
// far stricter and checked first) and STBI_ASSERT.

namespace terreate::adapter::asset_image_stb::backend {

/// Sets the number of live bytes the backend may hold on this thread (default: unlimited).
void setBudget(std::uint64_t bytes) noexcept;
/// Allocates `size` bytes within the budget; nullptr when it is exceeded or malloc fails.
[[nodiscard]] auto allocate(std::size_t size) noexcept -> void *;
/// Resizes within the budget; nullptr (the old block stays valid) on failure.
[[nodiscard]] auto reallocate(void *block, std::size_t size) noexcept -> void *;
/// Frees a block of allocate/reallocate (nullptr is fine).
void release(void *block) noexcept;

} // namespace terreate::adapter::asset_image_stb::backend

#define STBI_NO_STDIO
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_SIMD
#define STBI_MALLOC(size) ::terreate::adapter::asset_image_stb::backend::allocate(size)
#define STBI_REALLOC_SIZED(block, old_size, size)                                                  \
  ::terreate::adapter::asset_image_stb::backend::reallocate(block, size)
#define STBI_FREE(block) ::terreate::adapter::asset_image_stb::backend::release(block)

#include <stb_image.h>

#endif // TERREATE_ADAPTER_ASSET_IMAGE_STB_BACKEND_HPP

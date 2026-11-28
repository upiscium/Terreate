// #277: the stb_image Adapter decodes in-memory bytes into an ImageData. This test links
// the Adapter (and so Asset) alone: no Vulkan, no Graphics, no Platform, no display, no
// file system and no OS-, compiler- or linker-specific mechanism. The fixtures are byte
// arrays in this file: tiny files made once with a throw-away generator script (the
// repository does not carry the generator and the test needs no tool at run time).
#include <terreate/adapter/asset_image_stb/decoder.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR) ||             \
    defined(STBI_INCLUDE_STB_IMAGE_H)
#error "The Adapter's public header must not pull in Vulkan, GLFW or stb_image"
#endif

namespace {
namespace asset = terreate::asset;
using namespace terreate::adapter::asset_image_stb;

const char *test_name = "adapter.asset_image_stb";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

static_assert(std::is_error_code_enum_v<ImageDecodeError>);
static_assert(std::is_error_code_enum_v<asset::DecodeError>);

// Fixtures: tiny files, one array each (PNG: rgba/rgb/gray/gray-alpha 8-bit, 16-bit rgb/gray/rgba,
// palette with tRNS, 2-bit palette, color key, color chunks; BMP; TGA; JPEG; Radiance HDR; and
// the invalid ones: truncated, header lying about dimensions, huge or zero dimensions, GIF,
// garbage).
inline constexpr std::uint8_t png_rgba8_2x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
    0x24, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x08, 0x1b, 0x18, 0xc0, 0x34, 0x10, 0x00, 0x00, 0x3f, 0xd7, 0x08, 0x79, 0x5f, 0xc7, 0x6d,
    0xb1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_rgb8_2x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xfd,
    0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8,
    0xcf, 0xc0, 0xc0, 0x00, 0xc2, 0x0c, 0xff, 0x81, 0x00, 0x00, 0x1f, 0xee, 0x05, 0xfb, 0xf1,
    0xab, 0xba, 0x77, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_rgba8_1x1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
    0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x63, 0xe0, 0x12, 0x91, 0xd3, 0x00, 0x00, 0x00, 0xcd, 0x00, 0x65, 0xb5, 0xc7,
    0x96, 0x52, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_gray8_2x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00, 0x57,
    0xdd, 0x52, 0xf8, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60,
    0x70, 0x60, 0x68, 0xf8, 0x0f, 0x00, 0x03, 0x05, 0x01, 0xc0, 0x53, 0x5b, 0x15, 0x9f, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_gray_alpha8_2x1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00,
    0x00, 0x5e, 0x2b, 0xb7, 0x01, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x63, 0x38, 0xf1, 0x3f, 0xc5, 0x08, 0x00, 0x07, 0x1c, 0x02, 0x5e, 0x3f, 0x81,
    0xd6, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_rgb16_2x1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x10, 0x02, 0x00, 0x00, 0x00, 0x2b, 0xd0, 0x34,
    0x9e, 0x00, 0x00, 0x00, 0x15, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x10, 0x32, 0x09, 0xab,
    0x98, 0xb5, 0xe7, 0xff, 0x7f, 0x06, 0x86, 0x06, 0x06, 0x00, 0x20, 0x9e, 0x04, 0xe9, 0xeb, 0x94,
    0xe9, 0xba, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_gray16_1x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x00, 0x00, 0x00, 0xec,
    0x7a, 0x35, 0xb8, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x10,
    0x32, 0x61, 0xf8, 0xff, 0x1f, 0x00, 0x04, 0x2d, 0x02, 0x45, 0xd4, 0xca, 0x07, 0xdb, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_rgba16_1x1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x06, 0x00, 0x00, 0x00, 0x4f,
    0x85, 0x18, 0xca, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x10,
    0x14, 0x54, 0x52, 0x32, 0x36, 0x6e, 0x60, 0x00, 0x00, 0x04, 0xe3, 0x01, 0x4d, 0x87, 0xad,
    0xfa, 0x5d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_palette_trns_2x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x03, 0x00, 0x00, 0x00, 0x45, 0x68, 0xfd,
    0x16, 0x00, 0x00, 0x00, 0x09, 0x50, 0x4c, 0x54, 0x45, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0xff, 0x2d, 0x4a, 0xcd, 0x8a, 0x00, 0x00, 0x00, 0x02, 0x74, 0x52, 0x4e, 0x53, 0xff, 0x80,
    0x08, 0x0f, 0xb3, 0x6a, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60,
    0x60, 0x64, 0x60, 0x62, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x04, 0xdb, 0xe0, 0x32, 0x8e, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_palette_2x1_nobits[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x00, 0x00, 0x00, 0x89,
    0x4c, 0x97, 0x19, 0x00, 0x00, 0x00, 0x06, 0x50, 0x4c, 0x54, 0x45, 0x0a, 0x14, 0x1e, 0x28,
    0x32, 0x3c, 0xd5, 0x1b, 0xb4, 0xe9, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x63, 0x10, 0x00, 0x00, 0x00, 0x12, 0x00, 0x11, 0x08, 0xde, 0xbd, 0xc3, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_gray_trns_2x1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x00,
    0x00, 0xd1, 0x49, 0x20, 0x56, 0x00, 0x00, 0x00, 0x02, 0x74, 0x52, 0x4e, 0x53, 0x00,
    0x07, 0xe8, 0xf7, 0x58, 0x9b, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x63, 0x60, 0xe7, 0x04, 0x00, 0x00, 0x1a, 0x00, 0x11, 0xf3, 0x69, 0x53, 0x75,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_color_chunks_1x1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
    0xde, 0x00, 0x00, 0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61,
    0x05, 0x00, 0x00, 0x00, 0x20, 0x63, 0x48, 0x52, 0x4d, 0x00, 0x00, 0x7a, 0x26, 0x00, 0x00, 0x80,
    0x84, 0x00, 0x00, 0xfa, 0x00, 0x00, 0x00, 0x80, 0xe8, 0x00, 0x00, 0x75, 0x30, 0x00, 0x00, 0xea,
    0x60, 0x00, 0x00, 0x3a, 0x98, 0x00, 0x00, 0x17, 0x70, 0x9c, 0xba, 0x51, 0x3c, 0x00, 0x00, 0x00,
    0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c, 0xe9, 0x00, 0x00, 0x00, 0x20, 0x69, 0x43,
    0x43, 0x50, 0x74, 0x65, 0x73, 0x74, 0x00, 0x00, 0x78, 0x9c, 0xcb, 0xcb, 0x2f, 0x51, 0x48, 0x54,
    0x28, 0x4a, 0x4d, 0xcc, 0x51, 0x28, 0x28, 0xca, 0x4f, 0xcb, 0xcc, 0x49, 0x05, 0x00, 0x3d, 0x77,
    0x06, 0xa8, 0x16, 0x27, 0xe4, 0xbf, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda,
    0x63, 0x60, 0x64, 0x62, 0x06, 0x00, 0x00, 0x0e, 0x00, 0x07, 0xe9, 0x92, 0x37, 0xd4, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_lying_header[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x08, 0x06, 0x00, 0x00, 0x00, 0x70,
    0xe2, 0x95, 0x54, 0x00, 0x00, 0x00, 0x0f, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60,
    0x84, 0x02, 0x06, 0x26, 0x28, 0x00, 0x00, 0x00, 0xc6, 0x00, 0x19, 0x98, 0xca, 0xb6, 0x4d,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_huge_dimensions[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x01, 0x86, 0xa0, 0x00, 0x01, 0x86, 0xa0, 0x08, 0x06, 0x00, 0x00, 0x00, 0xa8,
    0x52, 0x0b, 0xc8, 0x00, 0x00, 0x00, 0x0f, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60,
    0x84, 0x02, 0x06, 0x26, 0x28, 0x00, 0x00, 0x00, 0xc6, 0x00, 0x19, 0x98, 0xca, 0xb6, 0x4d,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_zero_width[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x76,
    0x43, 0xdd, 0x19, 0x00, 0x00, 0x00, 0x0f, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60,
    0x84, 0x02, 0x06, 0x26, 0x28, 0x00, 0x00, 0x00, 0xc6, 0x00, 0x19, 0x98, 0xca, 0xb6, 0x4d,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

inline constexpr std::uint8_t png_truncated[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
    0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8,
    0xcf, 0xc0, 0xf0, 0x1f, 0x08, 0x1b, 0x18, 0xc0, 0x34, 0x10, 0x00, 0x00,
};

inline constexpr std::uint8_t bmp_rgb_2x2[] = {
    0x42, 0x4d, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
    0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0b, 0x00, 0x00,
    0x13, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
    0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00,
};

inline constexpr std::uint8_t bmp_truncated[] = {
    0x42, 0x4d, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0b, 0x00, 0x00, 0x13, 0x0b, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff,
};

inline constexpr std::uint8_t tga_rgba_2x2[] = {
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x02, 0x00, 0x20, 0x08, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff,
    0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0x80,
};

inline constexpr std::uint8_t jpeg_gray_8x8[] = {
    0xff, 0xd8, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x08, 0x00, 0x08,
    0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00, 0x08, 0x01,
    0x01, 0x00, 0x00, 0x3f, 0x00, 0xd4, 0x7f, 0xff, 0xd9,
};

inline constexpr std::uint8_t jpeg_truncated[] = {
    0xff, 0xd8, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x08, 0x00, 0x08,
    0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda,
};

inline constexpr std::uint8_t jpeg_gray_exif_icc[] = {
    0xff, 0xd8, 0xff, 0xe1, 0x00, 0x0c, 0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xe2, 0x00, 0x12, 0x49, 0x43, 0x43, 0x5f, 0x50, 0x52, 0x4f, 0x46, 0x49, 0x4c, 0x45, 0x00,
    0x01, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x08,
    0x00, 0x08, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00,
    0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00, 0xd4, 0x7f, 0xff, 0xd9,
};

inline constexpr std::uint8_t hdr_2x1[] = {
    0x23, 0x3f, 0x52, 0x41, 0x44, 0x49, 0x41, 0x4e, 0x43, 0x45, 0x0a, 0x46, 0x4f, 0x52,
    0x4d, 0x41, 0x54, 0x3d, 0x33, 0x32, 0x2d, 0x62, 0x69, 0x74, 0x5f, 0x72, 0x6c, 0x65,
    0x5f, 0x72, 0x67, 0x62, 0x65, 0x0a, 0x0a, 0x2d, 0x59, 0x20, 0x31, 0x20, 0x2b, 0x58,
    0x20, 0x32, 0x0a, 0x80, 0x40, 0x20, 0x81, 0x00, 0x00, 0x00, 0x00,
};

inline constexpr std::uint8_t hdr_truncated[] = {
    0x23, 0x3f, 0x52, 0x41, 0x44, 0x49, 0x41, 0x4e, 0x43, 0x45, 0x0a, 0x46, 0x4f,
    0x52, 0x4d, 0x41, 0x54, 0x3d, 0x33, 0x32, 0x2d, 0x62, 0x69, 0x74, 0x5f, 0x72,
    0x6c, 0x65, 0x5f, 0x72, 0x67, 0x62, 0x65, 0x0a, 0x0a, 0x2d, 0x59, 0x20, 0x31,
    0x20, 0x2b, 0x58, 0x20, 0x32, 0x0a, 0x80, 0x40, 0x20, 0x81, 0x00,
};

inline constexpr std::uint8_t gif_header[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x3b,
};

inline constexpr std::uint8_t garbage[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

template <std::size_t N> auto view(const std::uint8_t (&bytes)[N]) -> asset::ByteView {
  return std::as_bytes(std::span<const std::uint8_t, N>{bytes});
}

template <std::size_t N>
auto decode(const std::uint8_t (&bytes)[N], const ImageDecodeOptions &options = {},
            const char *name = "fixture") -> terreate::Result<ImageDecodeResult> {
  return decodeImage(asset::DecodeInput{view(bytes), asset::makeSourceIdentity(name)}, options);
}

template <typename T, typename E> bool failsWith(const terreate::Result<T> &result, E error) {
  return !result && result.error().code() == make_error_code(error);
}

auto u8(const asset::ImageData &image, std::size_t index) -> unsigned {
  return std::to_integer<unsigned>(image.data[index]);
}

auto u16(const asset::ImageData &image, std::size_t index) -> unsigned {
  std::uint16_t value = 0;
  std::memcpy(&value, image.data.data() + index * 2, 2);
  return value;
}

auto f32(const asset::ImageData &image, std::size_t index) -> float {
  float value = 0;
  std::memcpy(&value, image.data.data() + index * 4, 4);
  return value;
}

template <typename Container>
bool bytesEqual(const asset::ImageData &image, const Container &expected) {
  if (image.data.size() != expected.size())
    return false;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (u8(image, i) != static_cast<unsigned>(expected[i]))
      return false;
  }
  return true;
}

bool hasDiagnostic(const ImageDecodeResult &result, std::string_view code,
                   asset::DecodeSeverity severity) {
  return std::ranges::any_of(result.diagnostics, [&](const asset::DecodeDiagnostic &d) {
    return d.code == code && d.severity == severity;
  });
}

bool hasDiagnostic(const ImageDecodeResult &result, std::string_view code) {
  return std::ranges::any_of(result.diagnostics,
                             [&](const asset::DecodeDiagnostic &d) { return d.code == code; });
}

auto customValue(const asset::ImageData &image, std::string_view key) -> std::string {
  for (const auto &entry : image.custom) {
    if (entry.key == key)
      return std::get<std::string>(entry.value);
  }
  return "<missing>";
}

bool hasUnsupported(const asset::ImageData &image, std::string_view kind, std::string_view name) {
  return std::ranges::any_of(image.unsupported, [&](const asset::UnsupportedFeature &feature) {
    return feature.kind == kind && feature.name == name;
  });
}

// Every success is a valid image: the Adapter validates it, and so does this test.
bool decodesValid(const terreate::Result<ImageDecodeResult> &result, const char *message) {
  if (!check(result.has_value(), message))
    return false;
  return check(asset::validate(result->image).has_value(), "the decoded image validates");
}

// ---------------------------------------------------------------------------------------

void pngEightBit() {
  const auto rgba = decode(png_rgba8_2x2);
  if (decodesValid(rgba, "rgba8 png decodes")) {
    const auto &image = rgba->image;
    check(image.format == asset::PixelFormat::rgba8, "rgba png is rgba8");
    check(image.extent == asset::ImageExtent{2, 2, 1}, "rgba png extent");
    check(image.interpretation == asset::ColorInterpretation::srgb, "8-bit defaults to srgb");
    check(image.alpha == asset::AlphaMode::straight, "alpha defaults to straight");
    check(image.array_layers == 1 && image.mip_levels == 1 && image.subresources.size() == 1,
          "one tightly packed level");
    check(bytesEqual(image, std::vector<int>{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255, 255,
                                             255, 255}),
          "rgba png pixels, row 0 is the top row");
    check(image.unsupported.empty(), "nothing unsupported in a plain png");
    check(rgba->native.container == ImageContainer::png && rgba->native.channels == 4 &&
              rgba->native.bits_per_channel == 8 && !rgba->native.floating_point,
          "native info of an rgba png");
    check(customValue(image, "stb.container") == "png" &&
              customValue(image, "source.channels") == "4" &&
              customValue(image, "source.bits_per_channel") == "8" &&
              customValue(image, "png.color_type") == "rgba" &&
              customValue(image, "png.bit_depth") == "8",
          "custom entries of an rgba png");
    check(hasDiagnostic(*rgba, "image.interpretation_defaulted", asset::DecodeSeverity::info),
          "the defaulted interpretation is noted");
    check(rgba->source == asset::makeSourceIdentity("fixture"), "the source identity is kept");
    check(std::ranges::all_of(
              rgba->diagnostics,
              [](const asset::DecodeDiagnostic &d) { return d.location.source == "fixture"; }),
          "diagnostics name the source");
  }

  const auto rgb = decode(png_rgb8_2x2);
  if (decodesValid(rgb, "rgb8 png decodes")) {
    check(rgb->image.format == asset::PixelFormat::rgb8, "rgb png is rgb8");
    check(rgb->image.alpha == asset::AlphaMode::none, "no alpha without an alpha channel");
    check(bytesEqual(rgb->image, std::vector<int>{255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}),
          "rgb png pixels");
    check(!hasDiagnostic(*rgb, "image.channels_widened"), "rgb8 is not widened");
  }

  const auto one = decode(png_rgba8_1x1);
  if (decodesValid(one, "1x1 png decodes"))
    check(bytesEqual(one->image, std::vector<int>{10, 20, 30, 40}), "1x1 pixels");

  const auto gray = decode(png_gray8_2x2);
  if (decodesValid(gray, "gray png decodes")) {
    check(gray->image.format == asset::PixelFormat::r8, "gray png is r8");
    check(bytesEqual(gray->image, std::vector<int>{0, 64, 128, 255}), "gray pixels");
    check(customValue(gray->image, "png.color_type") == "gray", "gray color type");
  }

  const auto gray_alpha = decode(png_gray_alpha8_2x1);
  if (decodesValid(gray_alpha, "gray-alpha png decodes")) {
    check(gray_alpha->image.format == asset::PixelFormat::rg8, "gray-alpha png is rg8");
    check(gray_alpha->image.alpha == asset::AlphaMode::none, "rg8 has no alpha mode");
    check(bytesEqual(gray_alpha->image, std::vector<int>{200, 255, 100, 50}), "gray-alpha pixels");
    check(hasDiagnostic(*gray_alpha, "image.gray_alpha_stored_as_rg", asset::DecodeSeverity::info),
          "gray-alpha as rg is noted");
  }

  const auto palette = decode(png_palette_trns_2x2);
  if (decodesValid(palette, "palette png with transparency decodes")) {
    check(palette->image.format == asset::PixelFormat::rgba8, "palette + tRNS is rgba8");
    check(palette->native.channels == 4, "palette + tRNS has four source channels");
    check(bytesEqual(palette->image, std::vector<int>{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255,
                                                      255, 255, 0, 0, 255}),
          "palette pixels");
    check(customValue(palette->image, "png.color_type") == "palette", "palette color type");
    check(hasDiagnostic(*palette, "image.png.transparency_as_alpha"), "tRNS is noted");
  }

  const auto low_depth = decode(png_palette_2x1_nobits);
  if (decodesValid(low_depth, "2-bit palette png decodes")) {
    check(low_depth->image.format == asset::PixelFormat::rgb8, "palette without tRNS is rgb8");
    check(bytesEqual(low_depth->image, std::vector<int>{10, 20, 30, 40, 50, 60}),
          "2-bit palette pixels");
    check(customValue(low_depth->image, "png.bit_depth") == "2", "file bit depth is kept");
    check(hasDiagnostic(*low_depth, "image.png.low_bit_depth_expanded"), "expansion is noted");
  }

  const auto key = decode(png_gray_trns_2x1);
  if (decodesValid(key, "gray png with a color key decodes")) {
    check(key->image.format == asset::PixelFormat::rg8, "gray + tRNS is rg8");
    check(bytesEqual(key->image, std::vector<int>{7, 0, 9, 255}), "the color key is transparent");
  }
}

void pngSixteenBit() {
  const auto rgb = decode(png_rgb16_2x1);
  if (decodesValid(rgb, "rgb16 png decodes")) {
    const auto &image = rgb->image;
    check(image.format == asset::PixelFormat::rgba16, "rgb16 is widened to rgba16");
    check(image.interpretation == asset::ColorInterpretation::linear, "16-bit defaults to linear");
    check(image.alpha == asset::AlphaMode::none, "the widened alpha is not meaningful");
    check(u16(image, 0) == 0x1234 && u16(image, 1) == 0x5678 && u16(image, 2) == 0x9abc &&
              u16(image, 3) == 0xffff,
          "rgb16 pixel 0 keeps all 16 bits");
    check(u16(image, 4) == 0xffff && u16(image, 5) == 0 && u16(image, 6) == 0x8000 &&
              u16(image, 7) == 0xffff,
          "rgb16 pixel 1");
    check(rgb->native.bits_per_channel == 16 && rgb->native.channels == 3, "native 16-bit info");
    check(hasDiagnostic(*rgb, "image.channels_widened", asset::DecodeSeverity::info),
          "the widening is noted");
  }

  const auto gray = decode(png_gray16_1x2);
  if (decodesValid(gray, "gray16 png decodes")) {
    check(gray->image.format == asset::PixelFormat::r16, "gray16 is r16");
    check(u16(gray->image, 0) == 0x1234 && u16(gray->image, 1) == 0xffff, "gray16 pixels");
  }

  const auto rgba = decode(png_rgba16_1x1);
  if (decodesValid(rgba, "rgba16 png decodes")) {
    check(rgba->image.format == asset::PixelFormat::rgba16, "rgba16 is rgba16");
    check(rgba->image.alpha == asset::AlphaMode::straight, "rgba16 alpha is straight");
    check(u16(rgba->image, 0) == 0x1111 && u16(rgba->image, 3) == 0x8000, "rgba16 pixel");
  }
}

void pngColorChunks() {
  const auto result = decode(png_color_chunks_1x1);
  if (!decodesValid(result, "png with color chunks decodes"))
    return;
  const auto &image = result->image;
  check(hasUnsupported(image, "color-profile", "ICC"), "the ICC profile is listed as unsupported");
  check(hasUnsupported(image, "color-profile", "gAMA"), "the gamma is listed as unsupported");
  check(hasUnsupported(image, "color-profile", "cHRM"), "the chromaticities are listed");
  check(image.unsupported.size() == 3, "exactly the three color chunks are unsupported");
  for (const char *code : {"image.png.icc_profile_not_applied", "image.png.gamma_not_applied",
                           "image.png.chromaticities_not_applied"}) {
    check(hasDiagnostic(*result, code, asset::DecodeSeverity::warning), code);
  }
  const auto warnings = std::ranges::count_if(result->diagnostics, [](const auto &d) {
    return d.severity == asset::DecodeSeverity::warning && d.location.byte_offset.has_value();
  });
  check(warnings == 3, "each unsupported item has a located warning");
  check(customValue(image, "png.gamma_x100000") == "45455", "the gamma value is kept");
  check(customValue(image, "png.srgb_chunk") == "present", "the sRGB chunk is kept");
  check(bytesEqual(image, std::vector<int>{1, 2, 3}), "pixels are returned as stored");
}

void otherFormats() {
  const auto bmp = decode(bmp_rgb_2x2);
  if (decodesValid(bmp, "bmp decodes")) {
    check(bmp->image.format == asset::PixelFormat::rgb8, "bmp is rgb8");
    check(bytesEqual(bmp->image, std::vector<int>{255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}),
          "bmp pixels: bottom-up storage becomes top-first rows");
    check(bmp->native.container == ImageContainer::bmp, "bmp container");
  }

  const auto tga = decode(tga_rgba_2x2);
  if (decodesValid(tga, "tga decodes")) {
    check(tga->image.format == asset::PixelFormat::rgba8, "tga is rgba8");
    check(bytesEqual(tga->image, std::vector<int>{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255,
                                                  255, 255, 255}),
          "tga pixels: bottom-left origin becomes top-first rows");
    check(tga->native.container == ImageContainer::tga, "tga container");
    check(customValue(tga->image, "stb.container") == "tga", "tga custom entry");
  }

  const auto jpeg = decode(jpeg_gray_8x8);
  if (decodesValid(jpeg, "jpeg decodes")) {
    check(jpeg->image.format == asset::PixelFormat::r8, "gray jpeg is r8");
    check(jpeg->image.extent == asset::ImageExtent{8, 8, 1}, "jpeg extent");
    bool uniform = jpeg->image.data.size() == 64;
    for (std::size_t i = 0; uniform && i < 64; ++i)
      uniform = u8(jpeg->image, i) >= 147 && u8(jpeg->image, i) <= 149;
    check(uniform, "the jpeg decodes to the encoded gray level");
    check(jpeg->native.container == ImageContainer::jpeg && jpeg->native.channels == 1,
          "jpeg native info");
    check(jpeg->image.unsupported.empty(), "a plain jpeg has nothing unsupported");
  }

  const auto markers = decode(jpeg_gray_exif_icc);
  if (decodesValid(markers, "jpeg with EXIF and ICC decodes")) {
    check(hasUnsupported(markers->image, "color-profile", "ICC") &&
              hasUnsupported(markers->image, "metadata", "EXIF"),
          "ICC and EXIF are listed as unsupported");
    check(hasDiagnostic(*markers, "image.jpeg.icc_profile_not_applied",
                        asset::DecodeSeverity::warning) &&
              hasDiagnostic(*markers, "image.jpeg.exif_orientation_not_applied",
                            asset::DecodeSeverity::warning),
          "ICC and EXIF warnings");
  }

  const auto hdr = decode(hdr_2x1);
  if (decodesValid(hdr, "hdr decodes")) {
    const auto &image = hdr->image;
    check(image.format == asset::PixelFormat::rgba32f, "hdr is widened to rgba32f");
    check(image.interpretation == asset::ColorInterpretation::linear, "float is linear");
    check(image.alpha == asset::AlphaMode::none, "hdr has no alpha");
    check(f32(image, 0) == 1.0F && f32(image, 1) == 0.5F && f32(image, 2) == 0.25F &&
              f32(image, 3) == 1.0F && f32(image, 4) == 0.0F && f32(image, 7) == 1.0F,
          "hdr pixels are the decoded floats");
    check(hdr->native.container == ImageContainer::hdr && hdr->native.floating_point &&
              hdr->native.bits_per_channel == 32,
          "hdr native info");
    check(hasDiagnostic(*hdr, "image.channels_widened"), "the widening is noted");
  }

  check(containerName(ImageContainer::png) == "png" &&
            containerName(ImageContainer::jpeg) == "jpeg" &&
            containerName(ImageContainer::bmp) == "bmp" &&
            containerName(ImageContainer::tga) == "tga" &&
            containerName(ImageContainer::hdr) == "hdr",
        "container names");
}

void channelsAndAlpha() {
  ImageDecodeOptions options;

  options.channels = ChannelRequest::one;
  auto result = decode(png_rgba8_2x2, options);
  if (decodesValid(result, "rgba forced to one channel")) {
    check(result->image.format == asset::PixelFormat::r8 && result->image.data.size() == 4,
          "forced r8");
    check(u8(result->image, 3) == 255, "white stays white in luminance");
    check(hasDiagnostic(*result, "image.channels_converted"), "the conversion is noted");
    check(result->native.channels == 4, "native info keeps the source channels");
  }

  options.channels = ChannelRequest::four;
  result = decode(png_rgb8_2x2, options);
  if (decodesValid(result, "rgb forced to four channels")) {
    check(result->image.format == asset::PixelFormat::rgba8, "forced rgba8");
    check(u8(result->image, 3) == 255 && u8(result->image, 7) == 255, "added alpha is opaque");
    check(result->image.alpha == asset::AlphaMode::none,
          "an alpha the source did not have is not meaningful");
  }

  result = decode(png_gray8_2x2, options);
  if (decodesValid(result, "gray forced to four channels")) {
    check(u8(result->image, 4) == 64 && u8(result->image, 5) == 64 && u8(result->image, 6) == 64 &&
              u8(result->image, 7) == 255,
          "gray is replicated");
  }

  options.channels = ChannelRequest::two;
  result = decode(png_gray8_2x2, options);
  if (decodesValid(result, "gray forced to two channels")) {
    check(result->image.format == asset::PixelFormat::rg8 &&
              bytesEqual(result->image, std::vector<int>{0, 255, 64, 255, 128, 255, 255, 255}),
          "gray plus opaque alpha");
  }

  options.channels = ChannelRequest::one;
  result = decode(png_rgb16_2x1, options);
  if (decodesValid(result, "16-bit forced to one channel"))
    check(result->image.format == asset::PixelFormat::r16, "forced r16");
  result = decode(hdr_2x1, options);
  if (decodesValid(result, "hdr forced to one channel")) {
    check(result->image.format == asset::PixelFormat::r32f, "forced r32f");
    check(f32(result->image, 0) > 0.0F && f32(result->image, 1) == 0.0F, "luminance of the floats");
  }

  options.channels = ChannelRequest::as_decoded;
  options.alpha = AlphaHandling::premultiply;
  result = decode(png_rgba8_2x2, options);
  if (decodesValid(result, "premultiplied decode")) {
    check(result->image.alpha == asset::AlphaMode::premultiplied, "premultiplied mode");
    // (0, 255, 0, 128): green becomes (255 * 128 + 127) / 255 = 128; alpha 0 clears the color.
    check(bytesEqual(result->image, std::vector<int>{255, 0, 0, 255, 0, 128, 0, 128, 0, 0, 0, 0,
                                                     255, 255, 255, 255}),
          "premultiplied pixels");
    check(hasDiagnostic(*result, "image.alpha_premultiplied"), "premultiply is noted");
  }
  result = decode(png_rgba16_1x1, options);
  if (decodesValid(result, "premultiplied 16-bit decode")) {
    const auto expected = [](unsigned c) { return (c * 0x8000U + 32767U) / 65535U; };
    check(u16(result->image, 0) == expected(0x1111) && u16(result->image, 2) == expected(0x3333) &&
              u16(result->image, 3) == 0x8000,
          "premultiplied 16-bit pixels");
  }
  result = decode(png_rgb8_2x2, options);
  if (decodesValid(result, "premultiply on an image without alpha")) {
    check(result->image.alpha == asset::AlphaMode::none, "no alpha, no premultiplied mode");
    check(
        bytesEqual(result->image, std::vector<int>{255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}),
        "pixels untouched");
  }

  options.alpha = AlphaHandling::ignore;
  result = decode(png_rgba8_2x2, options);
  if (decodesValid(result, "alpha ignored")) {
    check(result->image.alpha == asset::AlphaMode::none &&
              result->image.format == asset::PixelFormat::rgba8,
          "alpha channel kept, mode none");
    check(u8(result->image, 7) == 128, "pixels untouched");
    check(hasDiagnostic(*result, "image.alpha_ignored"), "ignore is noted");
  }
}

void interpretationAndFlip() {
  ImageDecodeOptions options;
  options.interpretation = asset::ColorInterpretation::linear;
  auto result = decode(png_rgba8_2x2, options);
  if (decodesValid(result, "explicit linear")) {
    check(result->image.interpretation == asset::ColorInterpretation::linear, "linear override");
    check(!hasDiagnostic(*result, "image.interpretation_defaulted"),
          "no default note when explicit");
  }

  options.interpretation = asset::ColorInterpretation::normal_map;
  result = decode(png_rgb8_2x2, options);
  if (decodesValid(result, "normal map"))
    check(result->image.interpretation == asset::ColorInterpretation::normal_map, "normal_map");

  options.interpretation = asset::ColorInterpretation::srgb;
  check(failsWith(decode(png_rgb16_2x1, options), ImageDecodeError::invalid_option),
        "srgb is refused for 16-bit");
  check(failsWith(decode(hdr_2x1, options), ImageDecodeError::invalid_option),
        "srgb is refused for floats");
  options.interpretation = asset::ColorInterpretation::normal_map;
  check(failsWith(decode(hdr_2x1, options), ImageDecodeError::invalid_option),
        "normal_map is refused for floats");
  options.channels = ChannelRequest::one;
  check(failsWith(decode(png_rgb8_2x2, options), ImageDecodeError::invalid_option),
        "normal_map is refused for one component");
  const auto refused = decode(png_rgb8_2x2, options);
  check(!refused && !refused.error().detail().empty(), "the refusal explains itself");

  options = {};
  options.flip_vertically = true;
  result = decode(png_rgba8_2x2, options);
  if (decodesValid(result, "flipped decode")) {
    check(bytesEqual(result->image, std::vector<int>{0, 0, 255, 0, 255, 255, 255, 255, 255, 0, 0,
                                                     255, 0, 255, 0, 128}),
          "rows are swapped");
    check(hasDiagnostic(*result, "image.flipped"), "the flip is noted");
  }
  options.flip_vertically = false;
  result = decode(png_rgba8_2x2, options);
  check(result && u8(result->image, 0) == 255 && u8(result->image, 2) == 0,
        "no flip by default: row 0 is the top row");
}

void invalidInputs() {
  check(failsWith(decodeImage({}), ImageDecodeError::malformed_image), "empty input");

  check(failsWith(decode(png_truncated), ImageDecodeError::malformed_image), "truncated png");
  check(failsWith(decode(bmp_truncated), ImageDecodeError::malformed_image), "truncated bmp");
  check(failsWith(decode(jpeg_truncated), ImageDecodeError::malformed_image), "truncated jpeg");
  check(failsWith(decode(hdr_truncated), ImageDecodeError::malformed_image), "truncated hdr");

  check(failsWith(decode(garbage), ImageDecodeError::unsupported_format), "unrecognized bytes");
  const auto gif = decode(gif_header);
  check(failsWith(gif, ImageDecodeError::unsupported_format) &&
            gif.error().detail().find("gif") != std::string::npos,
        "a gif is a named unsupported format");

  check(failsWith(decode(png_zero_width), asset::DecodeError::dimension_exceeded), "zero width");
  check(failsWith(decode(png_lying_header), ImageDecodeError::malformed_image),
        "a header that claims more pixels than the data holds");
  check(failsWith(decode(png_huge_dimensions), asset::DecodeError::dimension_exceeded),
        "huge dimensions are refused before any allocation");

  ImageDecodeOptions raised;
  raised.limits.max_dimension = 200'000;
  check(failsWith(decode(png_huge_dimensions, raised), asset::DecodeError::pixel_count_exceeded),
        "the pixel limit still holds when the dimension limit is raised");

  // A hostile header must be refused by the limits alone, even with the biggest values.
  ImageDecodeOptions tight;
  tight.limits.max_dimension = 1;
  check(failsWith(decode(png_rgba8_2x2, tight), asset::DecodeError::dimension_exceeded),
        "dimension limit");
  tight = {};
  tight.limits.max_pixels = 3;
  check(failsWith(decode(png_rgba8_2x2, tight), asset::DecodeError::pixel_count_exceeded),
        "pixel limit");
  tight.limits.max_pixels = 4;
  check(decode(png_rgba8_2x2, tight).has_value(), "pixel limit is inclusive");
  tight = {};
  tight.limits.max_input_bytes = sizeof(png_rgba8_2x2) - 1;
  check(failsWith(decode(png_rgba8_2x2, tight), asset::DecodeError::input_too_large),
        "input size limit");
  tight.limits.max_input_bytes = sizeof(png_rgba8_2x2);
  check(decode(png_rgba8_2x2, tight).has_value(), "input size limit is inclusive");
  tight = {};
  tight.limits.max_input_bytes = 0;
  check(failsWith(decode(png_rgba8_1x1, tight), asset::DecodeError::input_too_large),
        "a zero input limit allows nothing");
}

// Truncating a file anywhere, or damaging any byte of it, ends in a named error or in a
// valid image: never a crash, never a limit overrun (the limits are tight, so a damaged
// header cannot ask for much memory).
template <std::size_t N> void hostile(const std::uint8_t (&bytes)[N], const char *name) {
  ImageDecodeOptions options;
  options.limits.max_dimension = 64;
  options.limits.max_pixels = 4096;
  std::vector<std::uint8_t> copy(bytes, bytes + N);
  const auto run = [&](const std::vector<std::uint8_t> &input) {
    const auto result = decodeImage(
        asset::DecodeInput{std::as_bytes(std::span<const std::uint8_t>{input}), {}}, options);
    if (result) {
      if (!check(asset::validate(result->image).has_value(), name))
        return;
      check(result->image.extent.width <= 64 && result->image.extent.height <= 64 &&
                result->image.data.size() <= std::size_t{4096} * 16,
            "a damaged file decodes within the limits");
    } else {
      check(result.error().code().category() == image_decode_error_category() ||
                result.error().code().category() == asset::decode_error_category(),
            "a damaged file fails with a named error");
    }
  };
  for (std::size_t size = 0; size <= N; ++size)
    run(std::vector<std::uint8_t>(copy.begin(), copy.begin() + static_cast<std::ptrdiff_t>(size)));
  for (std::size_t i = 0; i < N; ++i) {
    for (const std::uint8_t mask : {0xFFU, 0x01U, 0x80U}) {
      auto damaged = copy;
      damaged[i] = static_cast<std::uint8_t>(damaged[i] ^ mask);
      run(damaged);
    }
  }
}

void hostileInputs() {
  hostile(png_rgba8_2x2, "png_rgba8_2x2");
  hostile(png_rgb16_2x1, "png_rgb16_2x1");
  hostile(png_palette_trns_2x2, "png_palette_trns_2x2");
  hostile(png_color_chunks_1x1, "png_color_chunks_1x1");
  hostile(bmp_rgb_2x2, "bmp_rgb_2x2");
  hostile(tga_rgba_2x2, "tga_rgba_2x2");
  hostile(jpeg_gray_exif_icc, "jpeg_gray_exif_icc");
  hostile(hdr_2x1, "hdr_2x1");
}

// A PNG whose header says 1x1 gray but whose IDAT is a deflate stream (fixed Huffman, one
// literal and then `matches` copies of 258 bytes at distance 1) that inflates to megabytes. The
// backend's allocation budget makes it fail by name instead of growing without bound.
auto deflateBombPng(std::size_t matches) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bits_out;
  std::uint32_t buffer = 0;
  unsigned count = 0;
  const auto put = [&](std::uint32_t value, unsigned bits, bool most_significant_first) {
    for (unsigned i = 0; i < bits; ++i) {
      const std::uint32_t bit =
          most_significant_first ? (value >> (bits - 1 - i)) & 1U : (value >> i) & 1U;
      buffer |= bit << count;
      if (++count == 8) {
        bits_out.push_back(static_cast<std::uint8_t>(buffer));
        buffer = 0;
        count = 0;
      }
    }
  };
  put(1, 1, false);   // final block
  put(1, 2, false);   // fixed Huffman
  put(0x30, 8, true); // literal 0 (the PNG filter byte)
  for (std::size_t i = 0; i < matches; ++i) {
    put(0xC5, 8, true); // length symbol 285 = 258
    put(0, 5, true);    // distance 1
  }
  put(0, 7, true); // end of block
  if (count != 0)
    bits_out.push_back(static_cast<std::uint8_t>(buffer));
  std::vector<std::uint8_t> idat{0x78, 0x01};
  idat.insert(idat.end(), bits_out.begin(), bits_out.end());
  idat.insert(idat.end(), {0, 0, 0, 0}); // the checksum is not verified by the backend
  std::vector<std::uint8_t> png(png_gray8_2x2, png_gray8_2x2 + 33); // signature + IHDR
  const std::uint8_t header[] = {0, 0, 0, 1, 0, 0, 0, 1}; // width 1, height 1 (CRC not checked)
  std::copy(header, header + 8, png.begin() + 16);
  const auto length = static_cast<std::uint32_t>(idat.size());
  png.insert(png.end(),
             {static_cast<std::uint8_t>(length >> 24), static_cast<std::uint8_t>(length >> 16),
              static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length), 'I', 'D',
              'A', 'T'});
  png.insert(png.end(), idat.begin(), idat.end());
  png.insert(png.end(), {0, 0, 0, 0, 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0});
  return png;
}

void bombsAndUnsupported() {
  const auto bomb = deflateBombPng(20'000); // about 5 MB inflated
  const auto bombed = decodeImage(asset::DecodeInput{std::as_bytes(std::span{bomb}), {}}, {});
  check(failsWith(bombed, ImageDecodeError::out_of_memory), "a decompression bomb fails by name");
  const auto modest = deflateBombPng(1);
  const auto fine = decodeImage(asset::DecodeInput{std::as_bytes(std::span{modest}), {}}, {});
  check(fine.has_value() || fine.error().code().category() == image_decode_error_category(),
        "a small oversized stream never crashes");

  // Unsupported containers are named, not guessed.
  const std::uint8_t webp[] = {'R', 'I', 'F', 'F', 0,   0,   0,   0,
                               'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
  const std::uint8_t tiff[] = {'I', 'I', '*', 0, 8, 0, 0, 0};
  const std::uint8_t dds[] = {'D', 'D', 'S', ' ', 124, 0, 0, 0};
  const std::uint8_t pnm[] = {'P', '6', '\n', '1', ' ', '1', '\n', '2', '5', '5', '\n', 1, 2, 3};
  const std::uint8_t psd[] = {'8', 'B', 'P', 'S', 0, 1, 0, 0, 0, 0, 0, 0};
  const auto named = [](const terreate::Result<ImageDecodeResult> &result, std::string_view what) {
    return failsWith(result, ImageDecodeError::unsupported_format) &&
           result.error().detail().find(what) != std::string::npos;
  };
  check(named(decode(webp), "webp"), "webp is named");
  check(named(decode(tiff), "tiff"), "tiff is named");
  check(named(decode(dds), "dds"), "dds is named");
  check(named(decode(pnm), "pnm"), "pnm is named");
  check(named(decode(psd), "psd"), "psd is named");

  // A supported container that uses something the backend cannot decode.
  std::vector<std::uint8_t> rle(bmp_rgb_2x2, bmp_rgb_2x2 + sizeof(bmp_rgb_2x2));
  rle[30] = 1; // compression: RLE8
  check(failsWith(decodeImage(asset::DecodeInput{std::as_bytes(std::span{rle}), {}}, {}),
                  ImageDecodeError::unsupported_feature),
        "an RLE bmp is an unsupported feature");
  std::vector<std::uint8_t> critical(png_rgb8_2x2, png_rgb8_2x2 + 33);
  critical.insert(critical.end(), {0, 0, 0, 0, 'A', 'B', 'C', 'D', 0, 0, 0, 0});
  critical.insert(critical.end(), png_rgb8_2x2 + 33, png_rgb8_2x2 + sizeof(png_rgb8_2x2));
  check(failsWith(decodeImage(asset::DecodeInput{std::as_bytes(std::span{critical}), {}}, {}),
                  ImageDecodeError::unsupported_feature),
        "an unknown critical png chunk is an unsupported feature");

  // Forced channel counts on 16-bit and float samples.
  ImageDecodeOptions options;
  options.channels = ChannelRequest::four;
  auto result = decode(png_gray16_1x2, options);
  if (decodesValid(result, "gray16 forced to four"))
    check(result->image.format == asset::PixelFormat::rgba16 && u16(result->image, 0) == 0x1234 &&
              u16(result->image, 3) == 0xffff,
          "rgba16 from gray16");
  options.channels = ChannelRequest::two;
  result = decode(png_rgb16_2x1, options);
  if (decodesValid(result, "rgb16 forced to two"))
    check(result->image.format == asset::PixelFormat::rg16 &&
              result->image.alpha == asset::AlphaMode::none,
          "rg16");
  result = decode(hdr_2x1, options);
  if (decodesValid(result, "hdr forced to two"))
    check(result->image.format == asset::PixelFormat::rg32f, "rg32f");
  options.channels = ChannelRequest::four;
  result = decode(hdr_2x1, options);
  if (decodesValid(result, "hdr forced to four"))
    check(result->image.format == asset::PixelFormat::rgba32f, "rgba32f");
  options = {};
  options.alpha = AlphaHandling::premultiply;
  options.channels = ChannelRequest::two;
  result = decode(png_rgba8_2x2, options);
  if (decodesValid(result, "premultiply with two channels"))
    check(hasDiagnostic(*result, "image.alpha_not_applicable"),
          "an alpha request that cannot apply is noted");

  // A header that claims 2^40 pixels is refused by name, whatever the limits.
  ImageDecodeOptions huge;
  huge.limits.max_dimension = UINT32_MAX;
  huge.limits.max_pixels = UINT64_MAX;
  check(failsWith(decode(png_huge_dimensions, huge), asset::DecodeError::dimension_exceeded) ||
            !decode(png_huge_dimensions, huge).has_value(),
        "an enormous claimed size never decodes");
}

void determinismAndComposition() {
  const auto first = decode(png_color_chunks_1x1);
  const auto second = decode(png_color_chunks_1x1);
  check(first && second && first->image.data == second->image.data &&
            first->image.custom == second->image.custom &&
            first->image.unsupported == second->image.unsupported &&
            first->diagnostics.size() == second->diagnostics.size() &&
            first->image.format == second->image.format,
        "decoding twice gives the same result");
  for (std::size_t i = 0; first && second && i < first->diagnostics.size(); ++i) {
    check(first->diagnostics[i].code == second->diagnostics[i].code &&
              first->diagnostics[i].message == second->diagnostics[i].message &&
              first->diagnostics[i].location.byte_offset ==
                  second->diagnostics[i].location.byte_offset,
          "diagnostics are deterministic");
  }

  // The result owns its pixels: it outlives the input bytes.
  std::vector<std::uint8_t> copy(png_rgba8_2x2, png_rgba8_2x2 + sizeof(png_rgba8_2x2));
  auto owned =
      decodeImage(asset::DecodeInput{std::as_bytes(std::span<const std::uint8_t>{copy}), {}}, {});
  std::ranges::fill(copy, std::uint8_t{0});
  check(owned && u8(owned->image, 0) == 255, "the result does not borrow the input");

  // The signature of a decode callable an application can pass to a glTF adapter (#278):
  // the function is wrapped without any change, and a path-based convenience is the
  // application's own reading layer in front of it.
  const std::function<terreate::Result<asset::ImageData>(const asset::DecodeInput &)> callable =
      [](const asset::DecodeInput &input) -> terreate::Result<asset::ImageData> {
    auto decoded = decodeImage(input);
    if (!decoded)
      return std::unexpected(decoded.error());
    return std::move(decoded->image);
  };
  const std::map<std::string, asset::ByteView> files{{"textures/a.png", view(png_rgb8_2x2)}};
  const auto readFile = [&](const std::string &path) -> terreate::Result<asset::DecodeInput> {
    const auto found = files.find(path);
    if (found == files.end())
      return std::unexpected(
          terreate::Error{std::make_error_code(std::errc::no_such_file_or_directory), "readFile"});
    return asset::DecodeInput{found->second, asset::makeSourceIdentity(path)};
  };
  const auto input = readFile("textures/a.png");
  const auto composed = input ? callable(*input) : std::unexpected(input.error());
  check(composed && composed->format == asset::PixelFormat::rgb8, "path -> bytes -> ImageData");
  const auto via_path = input ? decodeImage(*input) : std::unexpected(input.error());
  check(via_path && via_path->source.base == "textures/",
        "the identity comes from the reading layer");
  check(!readFile("missing.png"), "the reading layer owns its own failure");
}

void errorCategory() {
  check(std::string_view{image_decode_error_category().name()} ==
            "terreate.adapter.asset_image_stb",
        "category name");
  for (const auto error : {ImageDecodeError::malformed_image, ImageDecodeError::unsupported_format,
                           ImageDecodeError::unsupported_feature, ImageDecodeError::invalid_option,
                           ImageDecodeError::out_of_memory}) {
    check(!make_error_code(error).message().empty() &&
              make_error_code(error).category() == image_decode_error_category(),
          "named code with a message");
  }
  const std::error_code converted = ImageDecodeError::invalid_option;
  check(converted == make_error_code(ImageDecodeError::invalid_option), "implicit conversion");
  check(make_error_code(ImageDecodeError::malformed_image) !=
            make_error_code(asset::DecodeError::malformed_input),
        "the Adapter's codes are not the shared ones");
}

} // namespace

int main() noexcept {
  pngEightBit();
  pngSixteenBit();
  pngColorChunks();
  otherFormats();
  channelsAndAlpha();
  interpretationAndFlip();
  invalidInputs();
  hostileInputs();
  bombsAndUnsupported();
  determinismAndComposition();
  errorCategory();
  if (failures != 0) {
    std::fprintf(stderr, "%s: assertion=FAIL: %d check(s) failed\n", test_name, failures);
    return EXIT_FAILURE;
  }
  std::printf("%s: image-stb=PASS\n", test_name);
  return EXIT_SUCCESS;
}

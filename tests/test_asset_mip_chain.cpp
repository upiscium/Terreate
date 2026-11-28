// #467: CPU mip chain generation of Asset. This test links Terreate::Asset alone; it needs no
// Vulkan, no Graphics, no Platform, no display and no OS- or linker-specific mechanism. Every
// expected value below was computed by hand from the filter documented in mip_chain.hpp.
#include <terreate/asset/mip_chain.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace terreate::asset;

const char *test_name = "asset.mip_chain";
int failures = 0;

bool check(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message.c_str());
  }
  return condition;
}

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
  return !result && result.error().code() == make_error_code(error);
}

#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR)
#error "Asset public headers must not pull in Vulkan or GLFW"
#endif
static_assert(std::is_error_code_enum_v<MipChainError>);

// A valid, tightly packed single-level image whose pixels are `values`.
template <typename T>
ImageData make(PixelFormat format, ColorInterpretation interpretation, AlphaMode alpha,
               ImageExtent extent, const std::vector<T> &values, std::uint32_t layers = 1) {
  ImageData image;
  image.format = format;
  image.interpretation = interpretation;
  image.alpha = alpha;
  image.extent = extent;
  image.array_layers = layers;
  auto layout = terreate::unwrap(packedLayout(format, extent, 1, layers));
  image.data.resize(layout.byte_size);
  if (values.size() * sizeof(T) != layout.byte_size)
    std::fprintf(stderr, "%s: bad fixture: %zu bytes for %zu\n", test_name,
                 values.size() * sizeof(T), layout.byte_size);
  std::memcpy(image.data.data(), values.data(),
              std::min(image.data.size(), values.size() * sizeof(T)));
  image.subresources = std::move(layout.subresources);
  return image;
}

using Bytes = std::vector<std::uint8_t>;

ImageData linear8(PixelFormat format, ImageExtent extent, const Bytes &values,
                  AlphaMode alpha = AlphaMode::none, std::uint32_t layers = 1) {
  return make(format, ColorInterpretation::linear, alpha, extent, values, layers);
}

// Generates and validates the result; a failure is reported once and gives an empty image.
ImageData generate(const ImageData &image, const MipChainOptions &options = {},
                   const char *what = "") {
  auto result = generateMipChain(image, options);
  if (!check(result.has_value(), std::string("generate: ") + what))
    return {};
  check(validate(*result).has_value(), std::string("the result validates: ") + what);
  return std::move(*result);
}

template <typename T>
std::vector<T> level(const ImageData &image, std::uint32_t mip, std::uint32_t layer = 0) {
  auto bytes = image.bytes(mip, layer);
  std::vector<T> out;
  if (!bytes)
    return out;
  out.resize(bytes->size() / sizeof(T));
  std::memcpy(out.data(), bytes->data(), out.size() * sizeof(T));
  return out;
}

bool sameValues(const Bytes &a, std::initializer_list<int> b) {
  if (a.size() != b.size())
    return false;
  std::size_t i = 0;
  for (int value : b)
    if (a[i++] != value)
      return false;
  return true;
}

void rgba8Linear() {
  const auto image = linear8(PixelFormat::rgba8, {2, 2, 1},
                             {0, 0, 0, 0, 255, 255, 255, 255, 0, 100, 200, 50, 10, 20, 30, 40});
  const auto chain = generate(image, {}, "rgba8 2x2");
  check(chain.mip_levels == 2 && chain.extent == ImageExtent{2, 2, 1}, "2x2 has two levels");
  check(level<std::uint8_t>(chain, 0) == level<std::uint8_t>(image, 0), "level 0 is the input");
  // r (0+255+0+10)/4 = 66.25, g (0+255+100+20)/4 = 93.75, b (0+255+200+30)/4 = 121.25,
  // a (0+255+50+40)/4 = 86.25
  check(sameValues(level<std::uint8_t>(chain, 1), {66, 94, 121, 86}), "rgba8 2x2 -> 1x1");
  check(chain.format == image.format && chain.interpretation == image.interpretation &&
            chain.alpha == image.alpha,
        "metadata is carried over");
}

void fourByFour() {
  const auto image = linear8(PixelFormat::r8, {4, 4, 1},
                             {0, 4, 8, 12, 2, 6, 10, 14, 100, 100, 200, 200, 100, 101, 200, 201});
  const auto chain = generate(image, {}, "4x4");
  check(chain.mip_levels == 3, "4x4 has three levels");
  // 2x2: (0+4+2+6)/4 = 3, (8+12+10+14)/4 = 11, (100+100+100+101)/4 = 100.25,
  // (200+200+200+201)/4 = 200.25. 1x1: (3+11+100+200)/4 = 78.5, ties up -> 79.
  check(sameValues(level<std::uint8_t>(chain, 1), {3, 11, 100, 200}), "4x4 -> 2x2");
  check(sameValues(level<std::uint8_t>(chain, 2), {79}), "2x2 -> 1x1 (tie rounds up)");
}

void oddExtents() {
  // 3x3 -> 1x1: every weight 1, mean of 1..9 is 5.
  auto chain =
      generate(linear8(PixelFormat::r8, {3, 3, 1}, {1, 2, 3, 4, 5, 6, 7, 8, 9}), {}, "3x3");
  check(chain.mip_levels == 2 && sameValues(level<std::uint8_t>(chain, 1), {5}), "3x3 -> 1x1");

  // 5x3: x weights (1, 1, 0.5)/2.5 and (0.5, 1, 1)/2.5, y weights 1, 1, 1. Columns 10..50 plus
  // the row offsets 0, 3, 6: dest0 = (10+20+15)/2.5 + 3 = 21, dest1 = (15+40+50)/2.5 + 3 = 45.
  const Bytes five{10, 20, 30, 40, 50, 13, 23, 33, 43, 53, 16, 26, 36, 46, 56};
  chain = generate(linear8(PixelFormat::r8, {5, 3, 1}, five), {}, "5x3");
  check(chain.mip_levels == 3 && chain.mipExtent(1) == ImageExtent{2, 1, 1} &&
            chain.mipExtent(2) == ImageExtent{1, 1, 1},
        "5x3 level extents");
  check(sameValues(level<std::uint8_t>(chain, 1), {21, 45}), "5x3 -> 2x1");
  check(sameValues(level<std::uint8_t>(chain, 2), {33}), "2x1 -> 1x1");

  // 1x4: only the height halves.
  chain = generate(linear8(PixelFormat::r8, {1, 4, 1}, {10, 20, 30, 50}), {}, "1x4");
  check(chain.mip_levels == 3 && sameValues(level<std::uint8_t>(chain, 1), {15, 40}) &&
            sameValues(level<std::uint8_t>(chain, 2), {28}),
        "1x4 -> 1x2 -> 1x1 (27.5 rounds up)");

  // 1x1: nothing to generate.
  chain = generate(linear8(PixelFormat::r8, {1, 1, 1}, {7}), {}, "1x1");
  check(chain.mip_levels == 1 && sameValues(level<std::uint8_t>(chain, 0), {7}),
        "1x1 is one level");

  // 7 -> 3: dest0 covers [0, 7/3): weights 3, 3, 1 (of 7); dest1 [7/3, 14/3): 2, 3, 2; dest2: 1,
  // 3, 3.
  chain = generate(linear8(PixelFormat::r8, {7, 1, 1}, {7, 14, 21, 28, 35, 42, 49}), {}, "7x1");
  check(chain.mipExtent(1) == ImageExtent{3, 1, 1} &&
            sameValues(level<std::uint8_t>(chain, 1), {12, 28, 44}),
        "7x1 -> 3x1");
}

void srgb() {
  const auto image = make(PixelFormat::rgba8, ColorInterpretation::srgb, AlphaMode::none,
                          ImageExtent{2, 1, 1}, Bytes{0, 0, 0, 100, 255, 255, 255, 200});
  const auto chain = generate(image, {}, "srgb 2x1");
  // Linear mean of 0 and 1 is 0.5; encoded 1.055 * 0.5^(1/2.4) - 0.055 = 0.73536 = 187.5 -> 188.
  // The alpha component is linear: (100 + 200) / 2.
  check(sameValues(level<std::uint8_t>(chain, 1), {188, 188, 188, 150}),
        "sRGB is averaged linearly");
  const auto flat = generate(
      linear8(PixelFormat::rgba8, {2, 1, 1}, {0, 0, 0, 100, 255, 255, 255, 200}), {}, "linear 2x1");
  check(sameValues(level<std::uint8_t>(flat, 1), {128, 128, 128, 150}),
        "the same bytes as linear data average to 128");
  // Equal neighbours give back the same value for every code.
  for (int v = 0; v < 256; ++v) {
    const auto same = generate(
        make(PixelFormat::r8, ColorInterpretation::srgb, AlphaMode::none, ImageExtent{2, 1, 1},
             Bytes{static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v)}),
        {}, "srgb identity");
    if (!check(sameValues(level<std::uint8_t>(same, 1), {v}),
               "sRGB identity of " + std::to_string(v)))
      break;
  }
  // rg8 srgb: both components are colors.
  const auto rg = generate(make(PixelFormat::rg8, ColorInterpretation::srgb, AlphaMode::none,
                                ImageExtent{2, 1, 1}, Bytes{0, 255, 255, 0}),
                           {}, "rg8 srgb");
  check(sameValues(level<std::uint8_t>(rg, 1), {188, 188}), "rg8 sRGB");
  // rgb8 srgb.
  const auto rgb = generate(make(PixelFormat::rgb8, ColorInterpretation::srgb, AlphaMode::none,
                                 ImageExtent{2, 1, 1}, Bytes{0, 0, 0, 255, 255, 255}),
                            {}, "rgb8 srgb");
  check(sameValues(level<std::uint8_t>(rgb, 1), {188, 188, 188}), "rgb8 sRGB");
}

void alphaModes() {
  const Bytes weighted{200, 10, 20, 255, 0, 100, 40, 0};
  auto chain = generate(linear8(PixelFormat::rgba8, {2, 1, 1}, weighted, AlphaMode::straight), {},
                        "straight, transparent neighbour");
  // The transparent texel has weight 0: the color is the opaque texel's; alpha is the plain mean.
  check(sameValues(level<std::uint8_t>(chain, 1), {200, 10, 20, 128}),
        "straight alpha ignores a transparent texel");
  const Bytes two{100, 0, 0, 50, 200, 0, 0, 150};
  chain =
      generate(linear8(PixelFormat::rgba8, {2, 1, 1}, two, AlphaMode::straight), {}, "straight");
  // r = (50 * 100 + 150 * 200) / 200 = 175, a = 100.
  check(sameValues(level<std::uint8_t>(chain, 1), {175, 0, 0, 100}),
        "straight alpha weights colors");
  chain = generate(linear8(PixelFormat::rgba8, {2, 1, 1}, two, AlphaMode::premultiplied), {},
                   "premultiplied");
  check(sameValues(level<std::uint8_t>(chain, 1), {150, 0, 0, 100}),
        "premultiplied is a plain mean");
  chain = generate(linear8(PixelFormat::rgba8, {2, 1, 1}, two, AlphaMode::none), {}, "no alpha");
  check(sameValues(level<std::uint8_t>(chain, 1), {150, 0, 0, 100}), "none is a plain mean");
  chain = generate(
      linear8(PixelFormat::rgba8, {2, 1, 1}, {10, 20, 30, 0, 20, 40, 60, 0}, AlphaMode::straight),
      {}, "all transparent");
  check(sameValues(level<std::uint8_t>(chain, 1), {15, 30, 45, 0}),
        "zero alpha weight gives the plain mean");
  // sRGB and straight alpha: weights apply to the linear colors; with full and zero alpha the
  // color of the opaque texel survives exactly.
  chain = generate(make(PixelFormat::rgba8, ColorInterpretation::srgb, AlphaMode::straight,
                        ImageExtent{2, 1, 1}, Bytes{200, 100, 50, 255, 0, 0, 0, 0}),
                   {}, "srgb straight");
  check(sameValues(level<std::uint8_t>(chain, 1), {200, 100, 50, 128}), "srgb straight alpha");
}

void normalMaps() {
  const Bytes normals{255, 128, 128, 255, 128, 255, 128, 255};
  const auto image = make(PixelFormat::rgba8, ColorInterpretation::normal_map, AlphaMode::none,
                          ImageExtent{2, 1, 1}, normals);
  // The mean is (191.5, 191.5, 128); as a vector (64, 64, 0.5) (half range 127.5) of length 90.511,
  // scaled by 127.5 / 90.511 = 1.40870 to (90.157, 90.157, 0.704), encoded 217.66, 217.66, 128.2.
  auto chain = generate(image, {}, "normal map");
  check(sameValues(level<std::uint8_t>(chain, 1), {218, 218, 128, 255}), "normal map renormalized");
  chain = generate(image, {.renormalize_normals = false}, "normal map plain");
  check(sameValues(level<std::uint8_t>(chain, 1), {192, 192, 128, 255}), "normal map plain mean");
  // A two-component normal has an implied z: renormalizing is not applied.
  chain = generate(make(PixelFormat::rg8, ColorInterpretation::normal_map, AlphaMode::none,
                        ImageExtent{2, 1, 1}, Bytes{255, 128, 128, 255}),
                   {}, "rg8 normal");
  check(sameValues(level<std::uint8_t>(chain, 1), {192, 192}), "rg8 normal is a plain mean");
  // Opposite normals average to the zero vector, which is left unchanged.
  chain = generate(make(PixelFormat::rgba8, ColorInterpretation::normal_map, AlphaMode::none,
                        ImageExtent{2, 1, 1}, Bytes{255, 255, 255, 255, 0, 0, 0, 255}),
                   {}, "zero normal");
  check(sameValues(level<std::uint8_t>(chain, 1), {128, 128, 128, 255}), "a zero vector stays");
  // The option does nothing for linear data.
  chain = generate(linear8(PixelFormat::rgba8, {2, 1, 1}, normals), {}, "linear option");
  check(sameValues(level<std::uint8_t>(chain, 1), {192, 192, 128, 255}),
        "renormalization is for normal maps only");
}

void sixteenBitAndFloat() {
  auto chain = generate(make(PixelFormat::r16, ColorInterpretation::linear, AlphaMode::none,
                             ImageExtent{2, 1, 1}, std::vector<std::uint16_t>{0, 65535}),
                        {}, "r16");
  check(level<std::uint16_t>(chain, 1) == std::vector<std::uint16_t>{32768},
        "r16 mean 32767.5 rounds up");
  chain = generate(make(PixelFormat::rg16, ColorInterpretation::linear, AlphaMode::none,
                        ImageExtent{2, 1, 1}, std::vector<std::uint16_t>{0, 10, 4, 21}),
                   {}, "rg16");
  check(level<std::uint16_t>(chain, 1) == std::vector<std::uint16_t>{2, 16}, "rg16 means");
  chain = generate(make(PixelFormat::rgba16, ColorInterpretation::linear, AlphaMode::straight,
                        ImageExtent{2, 1, 1},
                        std::vector<std::uint16_t>{1000, 0, 0, 100, 3000, 0, 0, 300}),
                   {}, "rgba16 straight");
  // r = (100 * 1000 + 300 * 3000) / 400 = 2500, a = 200.
  check(level<std::uint16_t>(chain, 1) == std::vector<std::uint16_t>{2500, 0, 0, 200},
        "rgba16 straight alpha");

  chain = generate(make(PixelFormat::r32f, ColorInterpretation::linear, AlphaMode::none,
                        ImageExtent{2, 2, 1}, std::vector<float>{1.0F, 2.0F, 3.0F, 4.5F}),
                   {}, "r32f");
  check(level<float>(chain, 1) == std::vector<float>{2.625F}, "r32f mean");
  chain = generate(make(PixelFormat::rg32f, ColorInterpretation::linear, AlphaMode::none,
                        ImageExtent{2, 1, 1}, std::vector<float>{0.0F, -1.0F, 1.0F, 1.0F}),
                   {}, "rg32f");
  check(level<float>(chain, 1) == std::vector<float>{0.5F, 0.0F}, "rg32f mean");
  chain = generate(make(PixelFormat::rgba32f, ColorInterpretation::linear, AlphaMode::straight,
                        ImageExtent{2, 1, 1},
                        std::vector<float>{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.25F}),
                   {}, "rgba32f straight");
  const auto f = level<float>(chain, 1);
  check(f.size() == 4 && std::fabs(f[0] - 0.8F) < 1e-6F && std::fabs(f[1] - 0.2F) < 1e-6F &&
            f[2] == 0.0F && f[3] == 0.625F,
        "rgba32f straight alpha (weights 1 and 0.25)");
}

void layersAndPitch() {
  const auto layered =
      linear8(PixelFormat::r8, {2, 2, 1}, {0, 0, 0, 4, 100, 100, 100, 104}, AlphaMode::none, 2);
  const auto chain = generate(layered, {}, "layers");
  check(chain.array_layers == 2 && chain.mip_levels == 2, "layers are kept");
  check(sameValues(level<std::uint8_t>(chain, 1, 0), {1}) &&
            sameValues(level<std::uint8_t>(chain, 1, 1), {101}),
        "each layer is filtered on its own");
  check(chain.subresources.size() == 4 && chain.subresources[1].mip == 1 &&
            chain.subresources[2].layer == 1,
        "the table is layer-major");

  // A padded input: 3x2 r8 with a row pitch of 5 (the padding bytes are 99 and unused).
  ImageData padded;
  padded.format = PixelFormat::r8;
  padded.interpretation = ColorInterpretation::linear;
  padded.extent = {3, 2, 1};
  for (int b : {1, 2, 3, 99, 99, 4, 5, 6})
    padded.data.push_back(static_cast<std::byte>(b));
  padded.subresources = {{0, 0, 0, 5, 10}};
  const auto copy = padded;
  const auto tight = generate(padded, {}, "padded");
  check(copy.data == padded.data && copy.subresources == padded.subresources,
        "the input is not changed");
  check(sameValues(level<std::uint8_t>(tight, 0), {1, 2, 3, 4, 5, 6}) &&
            tight.subresources[0].row_pitch == 3,
        "a padded input is repacked tightly");
  // 3x2 -> 1x1: x weights 1, 1, 1; y weights 1, 1: mean 21 / 6 = 3.5 -> 4.
  check(sameValues(level<std::uint8_t>(tight, 1), {4}), "padded 3x2 -> 1x1");
}

void threeD() {
  // 2x2x2 -> 1x1x1: the mean of 0..7 is 3.5, rounded up to 4 (a 2x2x2 box).
  auto chain = generate(linear8(PixelFormat::r8, {2, 2, 2}, {0, 1, 2, 3, 4, 5, 6, 7}), {}, "2x2x2");
  check(chain.mip_levels == 2 && chain.mipExtent(1) == ImageExtent{1, 1, 1} &&
            sameValues(level<std::uint8_t>(chain, 1), {4}),
        "2x2x2 -> 1x1x1");
  // 1x1x3 -> 1x1x1: weights 1, 1, 1.
  chain = generate(linear8(PixelFormat::r8, {1, 1, 3}, {3, 6, 9}), {}, "1x1x3");
  check(chain.mip_levels == 2 && sameValues(level<std::uint8_t>(chain, 1), {6}), "1x1x3 -> 1x1x1");
  // 4x2x2: only the first axis pairs differ; depth and height are boxed in pairs.
  Bytes v(16);
  for (std::size_t i = 0; i < v.size(); ++i)
    v[i] = static_cast<std::uint8_t>(i * 4);
  chain = generate(linear8(PixelFormat::r8, {4, 2, 2}, v), {}, "4x2x2");
  // dest (0): texels 0,1 of rows 0,1 of slices 0,1: x=0,1: (0+4+16+20+32+36+48+52)/8 = 26,
  // dest (1): x=2,3: (8+12+24+28+40+44+56+60)/8 = 34.
  check(chain.mipExtent(1) == ImageExtent{2, 1, 1} &&
            sameValues(level<std::uint8_t>(chain, 1), {26, 34}),
        "4x2x2 -> 2x1x1");
}

void options() {
  Bytes v(64);
  for (std::size_t i = 0; i < v.size(); ++i)
    v[i] = static_cast<std::uint8_t>(i);
  const auto image = linear8(PixelFormat::r8, {8, 8, 1}, v);
  check(generate(image, {}, "default").mip_levels == 4,
        "the default is the full chain (8x8: 4 levels)");
  const auto two = generate(image, {.levels = 2}, "two levels");
  check(two.mip_levels == 2 && two.data.size() == 64 + 16, "an option for fewer levels");
  const auto full = generate(image, {}, "full");
  check(level<std::uint8_t>(two, 1) == level<std::uint8_t>(full, 1),
        "fewer levels equal the head of the chain");
  const auto one = generate(image, {.levels = 1}, "one level");
  check(one.mip_levels == 1 && one.data == image.data, "one level is the repacked base");
  check(generate(image, {.levels = 4}, "four").mip_levels == 4, "levels equal to the full chain");
  check(hasError(generateMipChain(image, {.levels = 5}), MipChainError::invalid_level_count),
        "more levels than the full chain");
}

void errors() {
  const auto base = linear8(PixelFormat::r8, {4, 4, 1}, Bytes(16, 1));
  const auto chain = generate(base, {}, "base");
  const auto again = generateMipChain(chain);
  check(hasError(again, MipChainError::already_has_mips),
        "an image with mips is refused, not overwritten");
  check(again.error().detail().find("3 mip levels") != std::string::npos,
        "the detail names the level count");
  auto truncated = base;
  truncated.data.pop_back();
  check(hasError(generateMipChain(truncated), ImageDataError::data_out_of_bounds),
        "an invalid image gives its ImageDataError");
  ImageData empty;
  check(hasError(generateMipChain(empty), ImageDataError::zero_extent), "a zero extent");
  auto bad_alpha = base;
  bad_alpha.alpha = AlphaMode::straight;
  check(hasError(generateMipChain(bad_alpha), ImageDataError::alpha_mode_not_allowed),
        "alpha on r8");
  check(std::string(mip_chain_error_category().name()) == "terreate.asset.mip_chain" &&
            !make_error_code(MipChainError::already_has_mips).message().empty() &&
            !make_error_code(MipChainError::invalid_level_count).message().empty(),
        "the category and its messages");
  // A size that cannot be addressed is reported before any allocation.
  ImageData huge;
  huge.format = PixelFormat::rgba32f;
  huge.interpretation = ColorInterpretation::linear;
  huge.extent = {0x7FFFFFFFU, 0x7FFFFFFFU, 0x7FFFFFFFU};
  check(!generateMipChain(huge).has_value(),
        "an image beyond size_t or without data fails cleanly");
}

void metadataAndDeterminism() {
  auto image = linear8(PixelFormat::rgba8, {5, 3, 1},
                       Bytes{9, 8, 7, 6, 5, 4, 3, 2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 1, 2, 3,
                             4, 5, 6, 7, 8, 9, 8, 7, 6, 5, 4, 3, 2, 1, 9, 8, 7, 6, 5, 4,
                             3, 2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  image.alpha = AlphaMode::straight;
  image.custom.push_back({"key", std::string("value")});
  image.custom.push_back({"bytes", std::vector<std::byte>{std::byte{1}, std::byte{2}}});
  image.unsupported.push_back({"color-profile", "ICC", "dropped"});
  const auto first = generate(image, {}, "first");
  const auto second = generate(image, {}, "second");
  check(first.data == second.data && first.subresources == second.subresources, "deterministic");
  check(first.custom == image.custom && first.unsupported == image.unsupported &&
            first.alpha == image.alpha,
        "custom, unsupported and alpha are carried over");
  check(first.data.size() == std::size_t{72}, "the result is exactly the packed chain");
}

void uniformImages() {
  // A uniform image stays uniform in every level, for every format, odd extents included.
  struct Case {
    PixelFormat format;
    ColorInterpretation interpretation;
    AlphaMode alpha;
  };
  const Case cases[] = {
      {PixelFormat::r8, ColorInterpretation::srgb, AlphaMode::none},
      {PixelFormat::rg8, ColorInterpretation::normal_map, AlphaMode::none},
      {PixelFormat::rgb8, ColorInterpretation::linear, AlphaMode::none},
      {PixelFormat::rgba8, ColorInterpretation::srgb, AlphaMode::straight},
      {PixelFormat::rgba8, ColorInterpretation::normal_map, AlphaMode::none},
      {PixelFormat::r16, ColorInterpretation::linear, AlphaMode::none},
      {PixelFormat::rg16, ColorInterpretation::normal_map, AlphaMode::none},
      {PixelFormat::rgba16, ColorInterpretation::linear, AlphaMode::premultiplied},
      {PixelFormat::r32f, ColorInterpretation::linear, AlphaMode::none},
      {PixelFormat::rg32f, ColorInterpretation::linear, AlphaMode::none},
      {PixelFormat::rgba32f, ColorInterpretation::linear, AlphaMode::straight},
  };
  for (const auto &c : cases) {
    ImageData image;
    image.format = c.format;
    image.interpretation = c.interpretation;
    image.alpha = c.alpha;
    image.extent = {7, 5, 3};
    auto layout = terreate::unwrap(packedLayout(c.format, image.extent, 1, 1));
    // One pixel pattern repeated; for the normal maps a unit-length normal ((0, 0, 1)-like).
    const std::size_t texel = byteSize(c.format);
    image.data.resize(layout.byte_size);
    for (std::size_t p = 0; p < layout.byte_size; p += texel)
      for (std::size_t b = 0; b < texel; ++b)
        image.data[p + b] = static_cast<std::byte>(b % 2 == 0 ? 0x40 + b : 0x7F);
    if (c.format == PixelFormat::r32f || c.format == PixelFormat::rg32f ||
        c.format == PixelFormat::rgba32f) {
      for (std::size_t p = 0; p < layout.byte_size; p += 4) {
        const float value = 0.25F + static_cast<float>(p % texel) / 32.0F;
        std::memcpy(image.data.data() + p, &value, 4);
      }
    }
    image.subresources = std::move(layout.subresources);
    const std::string name = "uniform format " + std::to_string(static_cast<int>(c.format));
    const auto chain = generate(image, {.renormalize_normals = false}, name.c_str());
    if (chain.mip_levels == 0)
      continue;
    check(chain.mip_levels == 3, name + ": 7x5x3 has three levels");
    const std::size_t first_texel = 0;
    bool uniform = true;
    for (std::uint32_t mip = 1; mip < chain.mip_levels; ++mip) {
      const auto bytes = chain.bytes(mip, 0);
      if (!bytes) {
        uniform = false;
        break;
      }
      for (std::size_t p = 0; p + texel <= bytes->size(); p += texel)
        for (std::size_t b = 0; b < texel; ++b)
          if ((*bytes)[p + b] != image.data[first_texel + b])
            uniform = false;
    }
    check(uniform, name + ": a uniform image stays uniform");
  }
}

} // namespace

int main() {
  rgba8Linear();
  fourByFour();
  oddExtents();
  srgb();
  alphaModes();
  normalMaps();
  sixteenBitAndFloat();
  layersAndPitch();
  threeD();
  options();
  errors();
  metadataAndDeterminism();
  uniformImages();
  if (failures != 0) {
    std::fprintf(stderr, "%s: assertion=FAIL: %d check(s) failed\n", test_name, failures);
    return EXIT_FAILURE;
  }
  std::printf("%s: mip-chain=PASS\n", test_name);
  return EXIT_SUCCESS;
}

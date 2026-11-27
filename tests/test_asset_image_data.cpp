// #276 part 1: the CPU-only ImageData contract. This test links Terreate::Asset
// alone; it needs no Vulkan, no Graphics, no Platform, no display and no OS- or
// linker-specific mechanism.
#include <terreate/asset/image_data.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace terreate::asset;

const char *test_name = "asset.image_data";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

template <typename T> bool hasError(const terreate::Result<T> &result, ImageDataError error) {
  return !result && result.error().code() == make_error_code(error);
}

#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR)
#error "Asset public headers must not pull in Vulkan or GLFW"
#endif
static_assert(std::is_copy_constructible_v<ImageData> &&
              std::is_nothrow_move_constructible_v<ImageData>);
static_assert(std::is_error_code_enum_v<ImageDataError>);

// A valid, tightly packed image whose bytes are the byte index modulo 251.
ImageData makeImage(PixelFormat format, ImageExtent extent, std::uint32_t mips = 1,
                    std::uint32_t layers = 1) {
  ImageData image;
  image.format = format;
  image.interpretation = ColorInterpretation::linear;
  image.extent = extent;
  image.mip_levels = mips;
  image.array_layers = layers;
  auto layout = terreate::unwrap(packedLayout(format, extent, mips, layers));
  image.data.resize(layout.byte_size);
  for (std::size_t i = 0; i < image.data.size(); ++i)
    image.data[i] = static_cast<std::byte>(i % 251);
  image.subresources = std::move(layout.subresources);
  return image;
}

ImageData rgba() { return makeImage(PixelFormat::rgba8, {4, 2, 1}, 3, 2); }

void formats() {
  check(componentCount(PixelFormat::r8) == 1 && componentCount(PixelFormat::rg16) == 2 &&
            componentCount(PixelFormat::rgb8) == 3 && componentCount(PixelFormat::rgba32f) == 4,
        "component counts");
  check(byteSize(PixelFormat::r8) == 1 && byteSize(PixelFormat::rg8) == 2 &&
            byteSize(PixelFormat::rgb8) == 3 && byteSize(PixelFormat::rgba8) == 4 &&
            byteSize(PixelFormat::r16) == 2 && byteSize(PixelFormat::rg16) == 4 &&
            byteSize(PixelFormat::rgba16) == 8 && byteSize(PixelFormat::r32f) == 4 &&
            byteSize(PixelFormat::rg32f) == 8 && byteSize(PixelFormat::rgba32f) == 16,
        "pixel sizes");
  check(isFloat(PixelFormat::rg32f) && !isFloat(PixelFormat::rgba16), "isFloat");
  check(hasAlpha(PixelFormat::rgba16) && !hasAlpha(PixelFormat::rgb8), "hasAlpha");
}

void mipChain() {
  check(mipExtent({13, 7, 1}, 0) == ImageExtent{13, 7, 1}, "level 0 is the base");
  check(mipExtent({13, 7, 1}, 1) == ImageExtent{6, 3, 1}, "halving rounds down");
  check(mipExtent({13, 7, 1}, 2) == ImageExtent{3, 1, 1}, "a dimension stops at 1");
  check(mipExtent({13, 7, 1}, 3) == ImageExtent{1, 1, 1}, "the chain ends at 1x1x1");
  check(mipExtent({8, 8, 8}, 40) == ImageExtent{1, 1, 1}, "a huge level index is safe");
  check(maxMipLevels({1, 1, 1}) == 1 && maxMipLevels({13, 7, 1}) == 4 &&
            maxMipLevels({256, 1, 1}) == 9 && maxMipLevels({4, 2, 16}) == 5 &&
            maxMipLevels({std::numeric_limits<std::uint32_t>::max(), 1, 1}) == 32,
        "maxMipLevels");
  check(maxMipLevels({0, 4, 1}) == 0, "an empty extent has no chain");
}

void packed() {
  // 2D, no mips.
  auto one = terreate::unwrap(packedLayout(PixelFormat::rgb8, {5, 3, 1}, 1, 1));
  check(one.subresources.size() == 1 && one.byte_size == 45 &&
            one.subresources[0] == ImageSubresource{0, 0, 0, 15, 45},
        "2D odd size, one level");

  // Mip chain with odd sizes: 5x3, 2x1, 1x1 at 4 bytes.
  auto chain = terreate::unwrap(packedLayout(PixelFormat::rgba8, {5, 3, 1}, 3, 1));
  check(chain.subresources.size() == 3 && chain.byte_size == 60 + 8 + 4, "odd mip chain size");
  check(chain.subresources[1] == ImageSubresource{1, 0, 60, 8, 8} &&
            chain.subresources[2] == ImageSubresource{2, 0, 68, 4, 4},
        "odd mip chain offsets and pitches");

  // Layers are the outer loop; offsets are gap-free.
  auto layered = terreate::unwrap(packedLayout(PixelFormat::r16, {4, 4, 1}, 3, 3));
  check(layered.subresources.size() == 9, "mips x layers entries");
  std::size_t expected = 0;
  bool inOrder = true;
  for (std::size_t i = 0; i < layered.subresources.size(); ++i) {
    const auto &sub = layered.subresources[i];
    inOrder = inOrder && sub.layer == i / 3 && sub.mip == i % 3 && sub.offset == expected;
    const ImageExtent level = mipExtent({4, 4, 1}, sub.mip);
    inOrder = inOrder && sub.row_pitch == std::size_t{level.width} * 2 &&
              sub.slice_pitch == sub.row_pitch * std::size_t{level.height};
    expected += sub.slice_pitch * std::size_t{level.depth};
  }
  check(inOrder && layered.byte_size == expected && expected == 126U /* 3 layers x (32 + 8 + 2) */,
        "layer-major, contiguous, tight");

  // 3D: the depth halves too, and slices follow each other.
  auto volume = terreate::unwrap(packedLayout(PixelFormat::rg8, {4, 2, 6}, 3, 1));
  check(volume.subresources[0] == ImageSubresource{0, 0, 0, 8, 16} &&
            volume.subresources[1] == ImageSubresource{1, 0, 96, 4, 4} &&
            volume.subresources[2] == ImageSubresource{2, 0, 96 + 4 * 3, 2, 2} &&
            volume.byte_size == 96 + 12 + 2,
        "3D subresources shrink in depth");
  ImageData image = makeImage(PixelFormat::rg8, {4, 2, 6}, 3, 1);
  check(validate(image).has_value(), "a packed 3D image is valid");
  const auto slab = image.bytes(0, 0);
  check(slab && slab->size() == 96, "a 3D subresource spans all its slices");

  // Float formats and a 1D image.
  auto line = terreate::unwrap(packedLayout(PixelFormat::rgba32f, {9, 1, 1}, 4, 1));
  check(line.byte_size == std::size_t{9 + 4 + 2 + 1} * 16, "1D float chain");

  // The helper rejects what validate would reject.
  check(hasError(packedLayout(PixelFormat::r8, {0, 1, 1}, 1, 1), ImageDataError::zero_extent),
        "packedLayout zero extent");
  check(
      hasError(packedLayout(PixelFormat::r8, {2, 2, 1}, 1, 0), ImageDataError::invalid_layer_count),
      "packedLayout zero layers");
  check(hasError(packedLayout(PixelFormat::r8, {2, 2, 1}, 3, 1), ImageDataError::invalid_mip_count),
        "packedLayout too many mips");
  check(hasError(packedLayout(PixelFormat::r8, {2, 2, 1}, 0, 1), ImageDataError::invalid_mip_count),
        "packedLayout zero mips");
  check(
      hasError(packedLayout(PixelFormat::r8, {2, 2, 2}, 1, 2), ImageDataError::invalid_layer_count),
      "packedLayout 3D array");
  constexpr std::uint32_t big = std::numeric_limits<std::uint32_t>::max();
  check(hasError(packedLayout(PixelFormat::rgba32f, {big, big, 1}, 1, 1),
                 ImageDataError::size_overflow),
        "packedLayout overflow");
  check(hasError(packedLayout(PixelFormat::rgba32f, {big, 1, 1}, 1, big),
                 ImageDataError::size_overflow),
        "packedLayout overflow through the layer count");
}

void validImages() {
  check(validate(rgba()).has_value(), "the rgba image is valid");
  for (PixelFormat format :
       {PixelFormat::r8, PixelFormat::rg8, PixelFormat::rgb8, PixelFormat::rgba8, PixelFormat::r16,
        PixelFormat::rg16, PixelFormat::rgba16, PixelFormat::r32f, PixelFormat::rg32f,
        PixelFormat::rgba32f})
    check(validate(makeImage(format, {3, 3, 1}, 2, 2)).has_value(), "every format is valid");

  ImageData color = makeImage(PixelFormat::rgba8, {2, 2, 1});
  color.interpretation = ColorInterpretation::srgb;
  color.alpha = AlphaMode::premultiplied;
  check(validate(color).has_value(), "srgb premultiplied rgba8");
  color.interpretation = ColorInterpretation::normal_map;
  color.alpha = AlphaMode::straight;
  check(validate(color).has_value(), "normal map rgba8, straight alpha");
  ImageData normals = makeImage(PixelFormat::rg16, {2, 2, 1});
  normals.interpretation = ColorInterpretation::normal_map;
  check(validate(normals).has_value(), "normal map rg16");

  // A partial mip chain and a 1x1 image are valid.
  check(validate(makeImage(PixelFormat::r8, {16, 16, 1}, 2)).has_value(), "partial chain");
  check(validate(makeImage(PixelFormat::r8, {1, 1, 1})).has_value(), "1x1");
}

void everyError() {
  ImageData image = rgba();
  image.extent.height = 0;
  check(hasError(validate(image), ImageDataError::zero_extent), "zero_extent height");
  image = rgba();
  image.extent.depth = 0;
  check(hasError(validate(image), ImageDataError::zero_extent), "zero_extent depth");

  image = rgba();
  image.array_layers = 0;
  check(hasError(validate(image), ImageDataError::invalid_layer_count), "zero layers");
  image = makeImage(PixelFormat::r8, {2, 2, 2});
  image.array_layers = 2;
  check(hasError(validate(image), ImageDataError::invalid_layer_count), "3D with layers");

  image = rgba();
  image.mip_levels = 0;
  check(hasError(validate(image), ImageDataError::invalid_mip_count), "zero mips");
  image = rgba();
  image.mip_levels = 4;
  check(hasError(validate(image), ImageDataError::invalid_mip_count), "too many mips for 4x2");

  image = makeImage(PixelFormat::rgba32f, {2, 2, 1});
  image.interpretation = ColorInterpretation::srgb;
  check(hasError(validate(image), ImageDataError::interpretation_not_allowed), "float srgb");
  image.interpretation = ColorInterpretation::normal_map;
  check(hasError(validate(image), ImageDataError::interpretation_not_allowed), "float normal map");
  image = makeImage(PixelFormat::rgba16, {2, 2, 1});
  image.interpretation = ColorInterpretation::srgb;
  check(hasError(validate(image), ImageDataError::interpretation_not_allowed), "16-bit srgb");
  image = makeImage(PixelFormat::r8, {2, 2, 1});
  image.interpretation = ColorInterpretation::normal_map;
  check(hasError(validate(image), ImageDataError::interpretation_not_allowed),
        "one-component normal map");

  image = makeImage(PixelFormat::rgb8, {2, 2, 1});
  image.alpha = AlphaMode::straight;
  check(hasError(validate(image), ImageDataError::alpha_mode_not_allowed), "alpha on rgb8");
  image.alpha = AlphaMode::premultiplied;
  check(hasError(validate(image), ImageDataError::alpha_mode_not_allowed), "premultiplied rgb8");

  image = rgba();
  image.subresources[1].mip = 3;
  check(hasError(validate(image), ImageDataError::invalid_subresource), "mip out of range");
  image = rgba();
  image.subresources[1].layer = 2;
  check(hasError(validate(image), ImageDataError::invalid_subresource), "layer out of range");

  image = rgba();
  image.subresources[1] = image.subresources[0];
  check(hasError(validate(image), ImageDataError::duplicate_subresource), "duplicate");

  image = rgba();
  image.subresources.pop_back();
  check(hasError(validate(image), ImageDataError::missing_subresource), "missing last");
  image = rgba();
  image.subresources.erase(image.subresources.begin());
  check(hasError(validate(image), ImageDataError::missing_subresource), "missing first");
  image.subresources.clear();
  check(hasError(validate(image), ImageDataError::missing_subresource), "empty table");

  image = rgba();
  image.subresources[0].row_pitch -= 1;
  check(hasError(validate(image), ImageDataError::pitch_too_small), "row pitch below tight");
  image = makeImage(PixelFormat::rg8, {4, 2, 3});
  image.subresources[0].slice_pitch -= 1;
  check(hasError(validate(image), ImageDataError::pitch_too_small), "slice pitch below row*height");

  image = rgba();
  image.data.pop_back();
  check(hasError(validate(image), ImageDataError::data_out_of_bounds), "data one byte short");
  image = rgba();
  image.subresources[0].offset = image.data.size();
  check(hasError(validate(image), ImageDataError::data_out_of_bounds), "offset at the end");
  image = rgba();
  image.subresources[0].offset = std::numeric_limits<std::size_t>::max() - 3;
  check(hasError(validate(image), ImageDataError::size_overflow), "offset + size wraps");
  image = rgba();
  image.subresources[0].row_pitch = std::numeric_limits<std::size_t>::max() / 2 + 1;
  check(hasError(validate(image), ImageDataError::size_overflow), "pitch * rows wraps");
  image = makeImage(PixelFormat::rg8, {4, 2, 3});
  image.subresources[0].row_pitch = 8;
  image.subresources[0].slice_pitch = std::numeric_limits<std::size_t>::max() / 2 + 1;
  check(hasError(validate(image), ImageDataError::size_overflow), "slice pitch * depth wraps");

  constexpr std::uint32_t big = std::numeric_limits<std::uint32_t>::max();
  image = ImageData{};
  image.extent = {big, big, 1};
  image.format = PixelFormat::rgba32f;
  image.interpretation = ColorInterpretation::linear;
  check(hasError(validate(image), ImageDataError::size_overflow),
        "an extent whose tight size wraps is rejected before the table is read");

  image = rgba();
  image.custom.push_back({"", std::string("x")});
  check(hasError(validate(image), ImageDataError::invalid_custom_entry), "empty custom key");
  image = rgba();
  image.custom.push_back({"a", std::string("x")});
  image.custom.push_back({"a", std::vector<std::byte>{}});
  check(hasError(validate(image), ImageDataError::invalid_custom_entry), "repeated custom key");

  image = rgba();
  image.unsupported.push_back({"", "x", "y"});
  check(hasError(validate(image), ImageDataError::invalid_unsupported_entry), "empty kind");

  check(std::string(image_data_error_category().name()) == "terreate.asset.image_data",
        "category name");
  check(!image_data_error_category().message(static_cast<int>(ImageDataError::zero_extent)).empty(),
        "messages exist");
}

void paddedPitches() {
  // 3 pixels of rgba8 per row (12 bytes) padded to 16, 2 rows, then mip 1 (1x1) after 8 spare
  // bytes.
  ImageData image;
  image.format = PixelFormat::rgba8;
  image.interpretation = ColorInterpretation::linear;
  image.extent = {3, 2, 1};
  image.mip_levels = 2;
  image.data.resize(40 + 4);
  for (std::size_t i = 0; i < image.data.size(); ++i)
    image.data[i] = static_cast<std::byte>(i);
  image.subresources = {{0, 0, 0, 16, 32}, {1, 0, 40, 4, 4}};
  check(validate(image).has_value(), "padded row pitch is valid");
  const auto base = image.bytes(0, 0);
  check(base && base->size() == 16 + 12 && (*base)[16] == std::byte{16},
        "bytes span ends at the last row's pixels, not its padding");
  const auto small = image.bytes(1, 0);
  check(small && small->size() == 4 && (*small)[0] == std::byte{40}, "second subresource");

  // A padded slice pitch on a 3D image.
  ImageData volume;
  volume.format = PixelFormat::r8;
  volume.interpretation = ColorInterpretation::linear;
  volume.extent = {2, 2, 2};
  volume.data.resize(12 + 4 + 2);
  volume.subresources = {{0, 0, 0, 4, 12}};
  check(validate(volume).has_value(), "padded slice pitch is valid");
  const auto slices = volume.bytes(0, 0);
  check(slices && slices->size() == 12 + 4 + 2, "3D padded span size");
  volume.data.pop_back();
  check(hasError(validate(volume), ImageDataError::data_out_of_bounds),
        "padding does not hide a short buffer");

  // Subresources may be stored in any order, and with a non-zero start offset.
  ImageData shuffled = makeImage(PixelFormat::r8, {4, 4, 1}, 3, 2);
  std::swap(shuffled.subresources[0], shuffled.subresources[5]);
  check(validate(shuffled).has_value(), "table order is free");
  check(shuffled.find(2, 1) != nullptr && shuffled.find(0, 0) != nullptr, "find after shuffling");
}

void accessors() {
  ImageData image = rgba();
  check(image.find(0, 0) != nullptr && image.find(2, 1) != nullptr, "find");
  check(image.find(3, 0) == nullptr && image.find(0, 2) == nullptr, "find misses");
  check(image.mipExtent(1) == ImageExtent{2, 1, 1}, "member mipExtent");

  const auto bytes = image.bytes(1, 1);
  check(bytes && bytes->size() == 8, "mip 1 of 4x2 rgba8 is 2x1");
  // Layer 1 follows layer 0 (32 + 8 + 4 = 44 bytes), so mip 1 starts at 44 + 32.
  check(bytes && (*bytes)[0] == static_cast<std::byte>((44 + 32) % 251), "bytes point into data");
  check(bytes && bytes->data() == image.data.data() + 76, "the span is a view, not a copy");

  auto mutableBytes = image.bytes(0, 0);
  check(mutableBytes.has_value(), "mutable bytes");
  if (mutableBytes) {
    (*mutableBytes)[0] = std::byte{200};
    check(image.data[0] == std::byte{200}, "the mutable span writes through");
  }

  check(hasError(image.bytes(3, 0), ImageDataError::missing_subresource),
        "bytes of a missing entry");
  ImageData broken = rgba();
  broken.subresources[0].row_pitch = 1;
  check(hasError(broken.bytes(0, 0), ImageDataError::pitch_too_small), "bytes of a bad pitch");
  broken = rgba();
  broken.data.resize(10);
  check(hasError(broken.bytes(0, 0), ImageDataError::data_out_of_bounds), "bytes past the data");
  check(hasError(std::as_const(broken).bytes(0, 0), ImageDataError::data_out_of_bounds),
        "const bytes past the data");
}

void metadata() {
  ImageData image = rgba();
  image.custom.push_back({"exif", std::string("orientation=1")});
  image.custom.push_back({"blob", std::vector<std::byte>{std::byte{1}, std::byte{2}}});
  image.unsupported.push_back({"color-profile", "ICC", "embedded profile ignored"});
  image.unsupported.push_back({"animation", "", ""});
  check(validate(image).has_value(), "custom and unsupported entries are valid");

  const ImageData copy = image;
  check(copy.custom == image.custom && copy.unsupported == image.unsupported, "round trip by copy");
  check(std::get<std::string>(copy.custom[0].value) == "orientation=1" &&
            std::get<std::vector<std::byte>>(copy.custom[1].value).size() == 2 &&
            copy.unsupported[0].kind == "color-profile" && copy.unsupported[0].name == "ICC" &&
            copy.unsupported[0].detail == "embedded profile ignored",
        "entries keep their content");
  check(!(copy.custom[0] == copy.custom[1]), "entries compare by content");
}

void copyIndependence() {
  const ImageData original = rgba();
  ImageData copy = original;
  copy.data[0] = std::byte{255};
  copy.subresources.pop_back();
  copy.custom.push_back({"k", std::string("v")});
  copy.extent.width = 99;
  check(original.data[0] == std::byte{0} && original.subresources.size() == 6 &&
            original.custom.empty() && original.extent.width == 4,
        "a copy owns its bytes, table, custom list and extent");
  check(validate(original).has_value(), "the original stays valid after editing a copy");

  ImageData moved = rgba();
  const ImageData taken = std::move(moved);
  check(validate(taken).has_value(), "a moved-to image is valid");
}

} // namespace

int main() noexcept {
  formats();
  mipChain();
  packed();
  validImages();
  everyError();
  paddedPitches();
  accessors();
  metadata();
  copyIndependence();
  if (failures != 0) {
    std::fprintf(stderr, "%s: assertion=FAIL: %d check(s) failed\n", test_name, failures);
    return EXIT_FAILURE;
  }
  std::printf("%s: image-data=PASS\n", test_name);
  return EXIT_SUCCESS;
}

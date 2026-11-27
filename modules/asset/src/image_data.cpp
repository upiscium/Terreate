#include <terreate/asset/image_data.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace terreate::asset {
namespace {

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

class ImageDataErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.asset.image_data"; }
  std::string message(int value) const override {
    switch (static_cast<ImageDataError>(value)) {
    case ImageDataError::zero_extent:
      return "a dimension of the extent is 0";
    case ImageDataError::invalid_layer_count:
      return "the array layer count is not valid";
    case ImageDataError::invalid_mip_count:
      return "the mip level count is not valid for the extent";
    case ImageDataError::interpretation_not_allowed:
      return "the color interpretation is not allowed for the pixel format";
    case ImageDataError::alpha_mode_not_allowed:
      return "the alpha mode needs a format with an alpha component";
    case ImageDataError::invalid_subresource:
      return "a subresource names a mip level or layer outside the image";
    case ImageDataError::duplicate_subresource:
      return "two subresources have the same mip level and layer";
    case ImageDataError::missing_subresource:
      return "a mip level and layer has no subresource";
    case ImageDataError::pitch_too_small:
      return "a pitch is smaller than the tight size";
    case ImageDataError::data_out_of_bounds:
      return "a subresource reaches past the end of the data";
    case ImageDataError::size_overflow:
      return "a size computation does not fit std::size_t";
    case ImageDataError::invalid_custom_entry:
      return "a custom entry has an empty or repeated key";
    case ImageDataError::invalid_unsupported_entry:
      return "an unsupported-feature entry has an empty kind";
    }
    return "unknown ImageData error";
  }
};

// Error construction allocates; on exhaustion a code-only Error is returned,
// which does not allocate.
Error makeError(ImageDataError error, const char *operation, const std::string &detail) {
  try {
    return Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return Error{make_error_code(error)};
  }
}

bool mul(std::size_t a, std::size_t b, std::size_t &out) noexcept {
  if (a != 0 && b > kSizeMax / a)
    return false;
  out = a * b;
  return true;
}

bool add(std::size_t a, std::size_t b, std::size_t &out) noexcept {
  if (b > kSizeMax - a)
    return false;
  out = a + b;
  return true;
}

std::string describe(std::uint32_t mip, std::uint32_t layer) {
  return "mip " + std::to_string(mip) + " layer " + std::to_string(layer);
}

bool interpretationAllowed(PixelFormat format, ColorInterpretation interpretation) noexcept {
  switch (interpretation) {
  case ColorInterpretation::linear:
    return true;
  case ColorInterpretation::srgb:
    return componentSize(format) == 1;
  case ColorInterpretation::normal_map:
    return !isFloat(format) && componentCount(format) >= 2;
  }
  return false;
}

// The tight size of one mip level (all slices), or nullopt on overflow.
std::optional<std::size_t> tightMipSize(PixelFormat format, const ImageExtent &extent) noexcept {
  std::size_t size = 0;
  if (!mul(extent.width, byteSize(format), size) || !mul(size, extent.height, size) ||
      !mul(size, extent.depth, size))
    return std::nullopt;
  return size;
}

// The tight size of the whole image, or nullopt on overflow.
std::optional<std::size_t> tightTotal(PixelFormat format, const ImageExtent &extent,
                                      std::uint32_t mips, std::uint32_t layers) noexcept {
  std::size_t perLayer = 0;
  for (std::uint32_t mip = 0; mip < mips; ++mip) {
    const auto size = tightMipSize(format, mipExtent(extent, mip));
    if (!size || !add(perLayer, *size, perLayer))
      return std::nullopt;
  }
  std::size_t total = 0;
  if (!mul(perLayer, layers, total))
    return std::nullopt;
  return total;
}

// Counts that validate() and packedLayout() both require, in a fixed order.
terreate::Result<void> checkShape(const ImageExtent &extent, std::uint32_t mips,
                                  std::uint32_t layers, const char *op) {
  if (extent.width == 0 || extent.height == 0 || extent.depth == 0)
    return std::unexpected(makeError(ImageDataError::zero_extent, op,
                                     std::to_string(extent.width) + "x" +
                                         std::to_string(extent.height) + "x" +
                                         std::to_string(extent.depth)));
  if (layers == 0)
    return std::unexpected(makeError(ImageDataError::invalid_layer_count, op, "0 array layers"));
  if (extent.depth > 1 && layers != 1)
    return std::unexpected(makeError(ImageDataError::invalid_layer_count, op,
                                     "a 3D image has " + std::to_string(layers) + " array layers"));
  if (mips == 0 || mips > maxMipLevels(extent))
    return std::unexpected(makeError(ImageDataError::invalid_mip_count, op,
                                     std::to_string(mips) + " mip levels, at most " +
                                         std::to_string(maxMipLevels(extent))));
  return {};
}

// Checks one table entry against the image and returns the number of bytes it
// uses (first pixel to end of last row).
terreate::Result<std::size_t> checkSubresource(const ImageData &image, const ImageSubresource &sub,
                                               const char *op) {
  const std::string where = describe(sub.mip, sub.layer);
  const ImageExtent mip = image.mipExtent(sub.mip);
  std::size_t tightRow = 0;
  std::size_t tightSlice = 0;
  if (!mul(mip.width, byteSize(image.format), tightRow) || !mul(tightRow, mip.height, tightSlice))
    return std::unexpected(makeError(ImageDataError::size_overflow, op, where));
  if (sub.row_pitch < tightRow)
    return std::unexpected(makeError(ImageDataError::pitch_too_small, op,
                                     where + ": row pitch " + std::to_string(sub.row_pitch) +
                                         ", tight row " + std::to_string(tightRow)));
  std::size_t sliceNeed = 0;
  if (!mul(sub.row_pitch, mip.height, sliceNeed))
    return std::unexpected(makeError(ImageDataError::size_overflow, op, where));
  if (mip.depth > 1 && sub.slice_pitch < sliceNeed)
    return std::unexpected(makeError(ImageDataError::pitch_too_small, op,
                                     where + ": slice pitch " + std::to_string(sub.slice_pitch) +
                                         ", needs " + std::to_string(sliceNeed)));
  std::size_t used = 0;
  std::size_t part = 0;
  if (!mul(mip.depth - 1, sub.slice_pitch, part) || !add(used, part, used) ||
      !mul(mip.height - 1, sub.row_pitch, part) || !add(used, part, used) ||
      !add(used, tightRow, used))
    return std::unexpected(makeError(ImageDataError::size_overflow, op, where));
  std::size_t end = 0;
  if (!add(sub.offset, used, end))
    return std::unexpected(makeError(ImageDataError::size_overflow, op, where));
  if (end > image.data.size())
    return std::unexpected(makeError(ImageDataError::data_out_of_bounds, op,
                                     where + ": ends at " + std::to_string(end) + ", data has " +
                                         std::to_string(image.data.size()) + " bytes"));
  return used;
}

} // namespace

auto image_data_error_category() noexcept -> const std::error_category & {
  static const ImageDataErrorCategory category;
  return category;
}

auto make_error_code(ImageDataError error) noexcept -> std::error_code {
  return {static_cast<int>(error), image_data_error_category()};
}

auto mipExtent(const ImageExtent &base, std::uint32_t mip) noexcept -> ImageExtent {
  const auto level = [mip](std::uint32_t size) noexcept -> std::uint32_t {
    return mip >= 32 ? 1U : std::max<std::uint32_t>(size >> mip, 1U);
  };
  return {level(base.width), level(base.height), level(base.depth)};
}

auto maxMipLevels(const ImageExtent &base) noexcept -> std::uint32_t {
  if (base.width == 0 || base.height == 0 || base.depth == 0)
    return 0;
  return static_cast<std::uint32_t>(
      std::bit_width(std::max({base.width, base.height, base.depth})));
}

auto ImageData::find(std::uint32_t mip, std::uint32_t layer) const noexcept
    -> const ImageSubresource * {
  for (const auto &sub : subresources)
    if (sub.mip == mip && sub.layer == layer)
      return &sub;
  return nullptr;
}

auto ImageData::bytes(std::uint32_t mip, std::uint32_t layer) const
    -> terreate::Result<std::span<const std::byte>> {
  constexpr const char *op = "ImageData::bytes";
  const ImageSubresource *sub = find(mip, layer);
  if (sub == nullptr)
    return std::unexpected(
        makeError(ImageDataError::missing_subresource, op, describe(mip, layer)));
  auto used = checkSubresource(*this, *sub, op);
  if (!used)
    return std::unexpected(std::move(used.error()));
  return std::span<const std::byte>(data.data() + sub->offset, *used);
}

auto ImageData::bytes(std::uint32_t mip, std::uint32_t layer)
    -> terreate::Result<std::span<std::byte>> {
  auto view = std::as_const(*this).bytes(mip, layer);
  if (!view)
    return std::unexpected(std::move(view.error()));
  return std::span<std::byte>(const_cast<std::byte *>(view->data()), view->size());
}

auto packedLayout(PixelFormat format, const ImageExtent &extent, std::uint32_t mip_levels,
                  std::uint32_t array_layers) -> terreate::Result<PackedLayout> {
  constexpr const char *op = "asset::packedLayout";
  if (auto shape = checkShape(extent, mip_levels, array_layers, op); !shape)
    return std::unexpected(std::move(shape.error()));
  const auto total = tightTotal(format, extent, mip_levels, array_layers);
  if (!total)
    return std::unexpected(makeError(ImageDataError::size_overflow, op, "total size"));
  PackedLayout layout;
  layout.byte_size = *total;
  layout.subresources.reserve(static_cast<std::size_t>(mip_levels) * array_layers);
  std::size_t offset = 0;
  for (std::uint32_t layer = 0; layer < array_layers; ++layer) {
    for (std::uint32_t mip = 0; mip < mip_levels; ++mip) {
      const ImageExtent level = mipExtent(extent, mip);
      const std::size_t row = static_cast<std::size_t>(level.width) * byteSize(format);
      const std::size_t slice = row * level.height;
      layout.subresources.push_back({mip, layer, offset, row, slice});
      offset += slice * level.depth;
    }
  }
  return layout;
}

auto validate(const ImageData &image) -> terreate::Result<void> {
  constexpr const char *op = "asset::validate";
  if (auto shape = checkShape(image.extent, image.mip_levels, image.array_layers, op); !shape)
    return shape;
  if (!interpretationAllowed(image.format, image.interpretation))
    return std::unexpected(makeError(ImageDataError::interpretation_not_allowed, op,
                                     "format with " + std::to_string(componentCount(image.format)) +
                                         " component(s) of " +
                                         std::to_string(componentSize(image.format)) + " byte(s)"));
  if (image.alpha != AlphaMode::none && !hasAlpha(image.format))
    return std::unexpected(
        makeError(ImageDataError::alpha_mode_not_allowed, op, "format has no alpha component"));
  if (!tightTotal(image.format, image.extent, image.mip_levels, image.array_layers))
    return std::unexpected(
        makeError(ImageDataError::size_overflow, op, "the tight size of the image"));

  for (std::size_t i = 0; i < image.custom.size(); ++i) {
    if (image.custom[i].key.empty())
      return std::unexpected(makeError(ImageDataError::invalid_custom_entry, op,
                                       "entry " + std::to_string(i) + " has an empty key"));
    for (std::size_t j = 0; j < i; ++j)
      if (image.custom[j].key == image.custom[i].key)
        return std::unexpected(makeError(ImageDataError::invalid_custom_entry, op,
                                         "key '" + image.custom[i].key + "' is repeated"));
  }
  for (std::size_t i = 0; i < image.unsupported.size(); ++i)
    if (image.unsupported[i].kind.empty())
      return std::unexpected(makeError(ImageDataError::invalid_unsupported_entry, op,
                                       "entry " + std::to_string(i) + " has an empty kind"));

  for (const auto &sub : image.subresources) {
    if (sub.mip >= image.mip_levels || sub.layer >= image.array_layers)
      return std::unexpected(
          makeError(ImageDataError::invalid_subresource, op, describe(sub.mip, sub.layer)));
    if (auto used = checkSubresource(image, sub, op); !used)
      return std::unexpected(std::move(used.error()));
  }

  // Sorted (layer, mip) keys find duplicates and the first gap without a
  // table of mips * layers entries.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> keys;
  keys.reserve(image.subresources.size());
  for (const auto &sub : image.subresources)
    keys.emplace_back(sub.layer, sub.mip);
  std::sort(keys.begin(), keys.end());
  for (std::size_t i = 1; i < keys.size(); ++i)
    if (keys[i] == keys[i - 1])
      return std::unexpected(makeError(ImageDataError::duplicate_subresource, op,
                                       describe(keys[i].second, keys[i].first)));
  std::size_t next = 0;
  for (std::uint32_t layer = 0; layer < image.array_layers; ++layer)
    for (std::uint32_t mip = 0; mip < image.mip_levels; ++mip) {
      if (next < keys.size() && keys[next] == std::pair{layer, mip})
        ++next;
      else
        return std::unexpected(
            makeError(ImageDataError::missing_subresource, op, describe(mip, layer)));
    }
  return {};
}

} // namespace terreate::asset
// NOLINTEND(bugprone-easily-swappable-parameters)

#ifndef TERREATE_ASSET_IMAGE_DATA_HPP
#define TERREATE_ASSET_IMAGE_DATA_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <terreate/core/result.hpp>

namespace terreate::asset {

/// The storage format of one pixel of an ImageData. Components are stored in
/// the order r, g, b, a, in host byte order, tightly packed per pixel. The
/// integer formats are unsigned normalized (0..max maps to 0..1); the `f`
/// formats are IEEE 754 binary32 floats. Compressed formats are added as
/// enumerators later. No value is a Vulkan type; converting is the job of the
/// Asset-Graphics Integration.
enum class PixelFormat : std::uint8_t {
  /// One 8-bit unsigned normalized component.
  r8,
  /// Two 8-bit unsigned normalized components.
  rg8,
  /// Three 8-bit unsigned normalized components.
  rgb8,
  /// Four 8-bit unsigned normalized components.
  rgba8,
  /// One 16-bit unsigned normalized component.
  r16,
  /// Two 16-bit unsigned normalized components.
  rg16,
  /// Four 16-bit unsigned normalized components.
  rgba16,
  /// One 32-bit float component.
  r32f,
  /// Two 32-bit float components.
  rg32f,
  /// Four 32-bit float components.
  rgba32f,
};

/// How the stored values are to be interpreted. This is a separate field from
/// PixelFormat so that the same bytes can be a color or a data texture.
enum class ColorInterpretation : std::uint8_t {
  /// A color texture: color components are sRGB-encoded, alpha (if any) is
  /// linear. Allowed for the 8-bit formats only.
  srgb,
  /// A data texture or linear color: values are used as stored.
  linear,
  /// A tangent-space normal map: the first components hold a normal remapped
  /// from -1..1 to 0..1, stored linearly. Allowed for integer formats with at
  /// least two components.
  normal_map,
};

/// How alpha relates to the color components.
enum class AlphaMode : std::uint8_t {
  /// The image has no meaningful alpha. Allowed for every format (an alpha
  /// channel of an rgba format is then ignored).
  none,
  /// Color components are independent of alpha. Requires an rgba format.
  straight,
  /// Color components are already multiplied by alpha. Requires an rgba
  /// format.
  premultiplied,
};

/// The size of an image in pixels. A 1D image has height 1 and depth 1, a 2D
/// image depth 1.
struct ImageExtent {
  /// Width in pixels (at least 1 in a valid image).
  std::uint32_t width = 0;
  /// Height in pixels (at least 1 in a valid image).
  std::uint32_t height = 0;
  /// Depth in pixels; 1 for a 1D or 2D image (at least 1 in a valid image).
  std::uint32_t depth = 1;

  /// Extents are equal when all three dimensions are equal.
  [[nodiscard]] friend bool operator==(const ImageExtent &, const ImageExtent &) = default;
};

/// The number of components of a format (1 to 4).
[[nodiscard]] constexpr auto componentCount(PixelFormat format) noexcept -> std::size_t {
  switch (format) {
  case PixelFormat::r8:
  case PixelFormat::r16:
  case PixelFormat::r32f:
    return 1;
  case PixelFormat::rg8:
  case PixelFormat::rg16:
  case PixelFormat::rg32f:
    return 2;
  case PixelFormat::rgb8:
    return 3;
  case PixelFormat::rgba8:
  case PixelFormat::rgba16:
  case PixelFormat::rgba32f:
    return 4;
  }
  return 0;
}

/// The size in bytes of one component of a format.
[[nodiscard]] constexpr auto componentSize(PixelFormat format) noexcept -> std::size_t {
  switch (format) {
  case PixelFormat::r8:
  case PixelFormat::rg8:
  case PixelFormat::rgb8:
  case PixelFormat::rgba8:
    return 1;
  case PixelFormat::r16:
  case PixelFormat::rg16:
  case PixelFormat::rgba16:
    return 2;
  case PixelFormat::r32f:
  case PixelFormat::rg32f:
  case PixelFormat::rgba32f:
    return 4;
  }
  return 0;
}

/// The size in bytes of one pixel of a format.
[[nodiscard]] constexpr auto byteSize(PixelFormat format) noexcept -> std::size_t {
  return componentCount(format) * componentSize(format);
}

/// Whether the format stores floats (r32f, rg32f, rgba32f).
[[nodiscard]] constexpr auto isFloat(PixelFormat format) noexcept -> bool {
  return componentSize(format) == 4;
}

/// Whether the format has an alpha component (rgba8, rgba16, rgba32f).
[[nodiscard]] constexpr auto hasAlpha(PixelFormat format) noexcept -> bool {
  return componentCount(format) == 4;
}

/// The extent of mip level `mip`: each dimension is halved per level (rounding
/// down) and never below 1. Level 0 is `base` itself.
[[nodiscard]] auto mipExtent(const ImageExtent &base, std::uint32_t mip) noexcept -> ImageExtent;

/// The length of the full mip chain of `base`: the number of levels until every
/// dimension is 1 (`floor(log2(max dimension)) + 1`), or 0 when any dimension
/// of `base` is 0.
[[nodiscard]] auto maxMipLevels(const ImageExtent &base) noexcept -> std::uint32_t;

/// Where one (mip, layer) of an image lives in ImageData::data.
///
/// Rows run top-first: row 0 is the top row of the image, matching the
/// top-left texture-coordinate origin of MeshData. Within a subresource, the
/// slices of a 3D image follow each other `slice_pitch` bytes apart and the
/// rows of a slice `row_pitch` bytes apart.
struct ImageSubresource {
  /// The mip level (0 is the base level).
  std::uint32_t mip = 0;
  /// The array layer.
  std::uint32_t layer = 0;
  /// Byte offset of the first pixel inside ImageData::data.
  std::size_t offset = 0;
  /// Byte distance between consecutive rows; at least width * pixel size of
  /// this mip level, and may be larger (padding).
  std::size_t row_pitch = 0;
  /// Byte distance between consecutive slices of a 3D image; at least
  /// `row_pitch * height` of this mip level. Only checked when the image has a
  /// depth above 1.
  std::size_t slice_pitch = 0;

  /// Subresources are equal when all members are equal.
  [[nodiscard]] friend bool operator==(const ImageSubresource &,
                                       const ImageSubresource &) = default;
};

/// One format-specific item a decoder keeps instead of dropping it: a key with
/// either a string or a byte value.
struct CustomEntry {
  /// The key; not empty and unique within a list in a valid image.
  std::string key;
  /// The value: a string or raw bytes.
  std::variant<std::string, std::vector<std::byte>> value;

  /// Entries are equal when key and value are equal.
  [[nodiscard]] friend bool operator==(const CustomEntry &, const CustomEntry &) = default;
};

/// Something in the source that the decoder could not represent. A decoder
/// lists it here instead of dropping it silently; nothing inspects it beyond
/// validate() requiring a non-empty `kind`.
struct UnsupportedFeature {
  /// The category of the feature (for example "color-profile", "compression");
  /// not empty in a valid image.
  std::string kind;
  /// The specific name inside the category (for example "ICC").
  std::string name;
  /// Free-form detail for a human.
  std::string detail;

  /// Features are equal when all members are equal.
  [[nodiscard]] friend bool operator==(const UnsupportedFeature &,
                                       const UnsupportedFeature &) = default;
};

/// The bytes of a tightly packed image and the table that describes them.
struct PackedLayout {
  /// One entry per (mip, layer), layer-major: layer 0 with mips 0..n-1, then
  /// layer 1, and so on. Offsets are increasing and gap-free; row_pitch is the
  /// tight row size and slice_pitch is `row_pitch * height`.
  std::vector<ImageSubresource> subresources;
  /// The total size in bytes of the data the table describes.
  std::size_t byte_size = 0;
};

/// A reusable, backend-independent, CPU-side image: one decoded texture, which
/// may have mip levels, array layers and depth.
///
/// ImageData is an owning, copyable value. It holds no GPU handle. Members are
/// plain data, so a decoder can assemble it freely; nothing is guaranteed
/// until validate() succeeds, and nothing is repaired. Pixels are stored in
/// host byte order. Rows run top-first, matching the top-left origin of
/// texture coordinates. A cube map is not a separate kind: it is a 2D image
/// with a multiple of 6 layers, and the consumer decides (a flag may be added
/// later).
struct ImageData {
  /// The pixel format.
  PixelFormat format = PixelFormat::rgba8;
  /// How the values are interpreted.
  ColorInterpretation interpretation = ColorInterpretation::srgb;
  /// How alpha relates to color.
  AlphaMode alpha = AlphaMode::none;
  /// The size of mip level 0.
  ImageExtent extent;
  /// The number of array layers (at least 1; 1 for a 3D image).
  std::uint32_t array_layers = 1;
  /// The number of mip levels present (at least 1, at most maxMipLevels).
  std::uint32_t mip_levels = 1;
  /// All pixel bytes of all subresources.
  std::vector<std::byte> data;
  /// Where each (mip, layer) lives in `data`.
  std::vector<ImageSubresource> subresources;
  /// Format-specific information the decoder chose to keep.
  std::vector<CustomEntry> custom;
  /// Source features the decoder could not represent.
  std::vector<UnsupportedFeature> unsupported;

  /// The first table entry for (`mip`, `layer`), or nullptr.
  [[nodiscard]] auto find(std::uint32_t mip, std::uint32_t layer) const noexcept
      -> const ImageSubresource *;

  /// The extent of mip level `mip` of this image.
  [[nodiscard]] auto mipExtent(std::uint32_t mip) const noexcept -> ImageExtent {
    return terreate::asset::mipExtent(extent, mip);
  }

  /// The bytes of one subresource, from its first pixel to the end of its last
  /// row (trailing pitch padding is not included): `(depth - 1) * slice_pitch +
  /// (height - 1) * row_pitch + width * pixel size` bytes of that mip level.
  ///
  /// Fails with ImageDataError::missing_subresource when the table has no such
  /// entry, and with pitch_too_small, data_out_of_bounds or size_overflow when
  /// the entry does not fit the image or `data`. The span is valid until the
  /// image is changed or destroyed.
  [[nodiscard]] auto bytes(std::uint32_t mip, std::uint32_t layer) const
      -> terreate::Result<std::span<const std::byte>>;
  /// Mutable overload of bytes().
  [[nodiscard]] auto bytes(std::uint32_t mip, std::uint32_t layer)
      -> terreate::Result<std::span<std::byte>>;
};

/// The failures of ImageData validation and layout building. Values start at 1;
/// new values are appended.
enum class ImageDataError : std::uint8_t {
  /// A dimension of the extent is 0.
  zero_extent = 1,
  /// `array_layers` is 0, or a 3D image (depth above 1) has more than 1 layer.
  invalid_layer_count,
  /// `mip_levels` is 0 or exceeds the full mip chain of the extent.
  invalid_mip_count,
  /// The format does not allow the color interpretation (float formats are
  /// linear only; srgb needs an 8-bit format; normal_map needs an integer
  /// format with at least two components).
  interpretation_not_allowed,
  /// Alpha mode straight or premultiplied on a format without alpha.
  alpha_mode_not_allowed,
  /// A table entry names a mip or layer outside the image.
  invalid_subresource,
  /// Two table entries name the same (mip, layer).
  duplicate_subresource,
  /// A (mip, layer) of the image has no table entry.
  missing_subresource,
  /// A row or slice pitch is smaller than the tight size of its mip level.
  pitch_too_small,
  /// A subresource reaches past the end of `data`.
  data_out_of_bounds,
  /// A size computation does not fit std::size_t.
  size_overflow,
  /// A custom entry has an empty or repeated key.
  invalid_custom_entry,
  /// An unsupported-feature entry has an empty kind.
  invalid_unsupported_entry,
};

/// The category of ImageDataError codes (name "terreate.asset.image_data").
[[nodiscard]] auto image_data_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of an ImageDataError.
[[nodiscard]] auto make_error_code(ImageDataError error) noexcept -> std::error_code;

/// Builds the tightly packed table for an image of `format`, `extent`,
/// `mip_levels` and `array_layers`: every row exactly as long as its pixels,
/// every slice and subresource directly after the previous one.
///
/// Fails with zero_extent, invalid_layer_count or invalid_mip_count for
/// parameters validate() would reject, and with size_overflow when the total
/// size does not fit std::size_t (checked without wrapping, before any
/// allocation). Fill `ImageData::data` with `byte_size` bytes and assign
/// `subresources` to use the result. Allocation failure throws std::bad_alloc.
[[nodiscard]] auto packedLayout(PixelFormat format, const ImageExtent &extent,
                                std::uint32_t mip_levels, std::uint32_t array_layers)
    -> terreate::Result<PackedLayout>;

/// Checks the invariants of an image and reports the first violation, with the
/// offending field or subresource in the Error's detail. Nothing is repaired.
///
/// The invariants: non-zero extent, layer and mip counts in range, the
/// interpretation and alpha mode allowed for the format, the tight size of the
/// whole image fits std::size_t, unique and non-empty custom keys, non-empty
/// unsupported kinds, every table entry inside the image and `data` with
/// pitches at least the tight sizes, no (mip, layer) twice, and every (mip,
/// layer) present. Overlap between subresources is not checked.
[[nodiscard]] auto validate(const ImageData &image) -> terreate::Result<void>;

} // namespace terreate::asset

namespace std {
/// Lets ImageDataError convert to std::error_code.
template <> struct is_error_code_enum<terreate::asset::ImageDataError> : true_type {};
} // namespace std

#endif // TERREATE_ASSET_IMAGE_DATA_HPP

#ifndef TERREATE_ADAPTER_ASSET_IMAGE_STB_DECODER_HPP
#define TERREATE_ADAPTER_ASSET_IMAGE_STB_DECODER_HPP

#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include <terreate/asset/decode.hpp>
#include <terreate/asset/image_data.hpp>
#include <terreate/core/result.hpp>

/// The stb_image Adapter (#277): the first practical decoder from a byte source to a
/// CPU-side `asset::ImageData`. It implements the decode contracts of Asset
/// (`asset::DecodeInput`, `asset::DecodeLimits`, `asset::DecodeDiagnostic`) with one
/// backend, stb_image, which is a private implementation detail: no public header
/// includes it and a consumer needs no stb.
///
/// The Adapter reads only the bytes it is given. It never opens a file or a socket,
/// creates no GPU resource, owns no thread and keeps no global state, so it is
/// testable without Graphics and a path-based convenience is a thin layer the
/// application composes from its own file reading (see the architecture page).
///
/// Formats: PNG, JPEG, BMP, TGA and Radiance HDR. Everything else (GIF, PSD, PIC,
/// PNM, WebP, TIFF, ...) is reported as `ImageDecodeError::unsupported_format`.
namespace terreate::adapter::asset_image_stb {

/// The container formats this Adapter decodes.
enum class ImageContainer : std::uint8_t {
  /// PNG (8-bit and 16-bit, palette, gray, gray+alpha, RGB, RGBA, tRNS).
  png,
  /// JPEG (baseline and progressive; not arithmetic coded; 8-bit samples).
  jpeg,
  /// BMP (uncompressed and bitfield; not RLE).
  bmp,
  /// TGA (identified by stb_image's header heuristic, because TGA has no magic
  /// number: any input that has no other known signature and that the heuristic
  /// accepts).
  tga,
  /// Radiance HDR (`.hdr`), decoded to 32-bit floats.
  hdr,
};

/// The lower-case name of a container: `"png"`, `"jpeg"`, `"bmp"`, `"tga"` or `"hdr"`.
[[nodiscard]] auto containerName(ImageContainer container) noexcept -> std::string_view;

/// How many channels the decoded image has. Anything other than `as_decoded` makes
/// the backend convert (for example RGB to gray, or an opaque alpha added). `ImageData`
/// has no three-component 16-bit or float format, so `as_decoded` widens such an image
/// to four components with an opaque alpha and says so in a diagnostic.
enum class ChannelRequest : std::uint8_t {
  /// Keep the channels of the source: 1 (`r`), 2 (`rg`: gray and alpha, see
  /// `ImageDecodeResult`), 3 (`rgb8` only) or 4 (`rgba`).
  as_decoded = 0,
  /// One component (`r8`, `r16`, `r32f`): RGB is converted to luminance.
  one = 1,
  /// Two components (`rg8`, `rg16`, `rg32f`): gray (or luminance) and alpha.
  two = 2,
  /// Four components (`rgba8`, `rgba16`, `rgba32f`); a missing alpha is opaque.
  four = 4,
};

/// What the alpha of a four-component image means in the result. Applies only to an
/// image whose source has alpha; an image without alpha is always `AlphaMode::none`.
enum class AlphaHandling : std::uint8_t {
  /// Keep the color as stored and mark the image `AlphaMode::straight` (the default:
  /// every supported format stores straight alpha).
  straight,
  /// Multiply the color components by alpha (rounded to nearest for integer formats)
  /// and mark the image `AlphaMode::premultiplied`.
  premultiply,
  /// Keep the pixels and mark the image `AlphaMode::none`: the alpha channel is
  /// present but not meaningful.
  ignore,
};

/// The options of one decode. Plain data with documented defaults.
struct ImageDecodeOptions {
  /// The resource limits. The input size, the dimensions and the pixel count are
  /// checked before any pixel buffer is allocated. Default: `asset::defaultDecodeLimits()`.
  asset::DecodeLimits limits{};
  /// The color interpretation of the result. When empty (the default) the Adapter
  /// chooses `srgb` for an 8-bit image and `linear` for a 16-bit or float image
  /// (`ImageData` allows `srgb` for 8-bit formats only) and notes the choice in a
  /// diagnostic. An explicit value is checked against the resulting format and is
  /// refused with `ImageDecodeError::invalid_option` when `ImageData` does not allow it
  /// (`normal_map` needs an integer format with at least two components).
  std::optional<asset::ColorInterpretation> interpretation{};
  /// The alpha meaning; default `straight`.
  AlphaHandling alpha = AlphaHandling::straight;
  /// The channel count; default `as_decoded`.
  ChannelRequest channels = ChannelRequest::as_decoded;
  /// Flip the rows. Off by default: row 0 of the result is the top row of the image,
  /// as in `ImageData` and the top-left texture origin of `MeshData`; every supported
  /// format is decoded top-down already. The flip is done by this Adapter on the result,
  /// not through stb_image's global flip switch.
  bool flip_vertically = false;
};

/// What the backend found in the file, kept as plain data (the escape hatch). The same
/// information is in `ImageData::custom` (keys `stb.container`, `source.channels`,
/// `source.bits_per_channel` and, for PNG, `png.color_type` and `png.bit_depth`).
struct ImageSourceInfo {
  /// The detected container.
  ImageContainer container = ImageContainer::png;
  /// The channels of the decoded source: PNG palette images count as 3 (4 with a
  /// transparency table), a PNG `tRNS` color key adds an alpha channel.
  std::uint32_t channels = 0;
  /// The bits per channel of the decoded samples: 8, 16 or 32 (floating point).
  std::uint32_t bits_per_channel = 0;
  /// Whether the samples are floating point (Radiance HDR).
  bool floating_point = false;
};

/// The result of a successful decode: a valid image and everything the Adapter noted.
struct ImageDecodeResult {
  /// The decoded image. `asset::validate` succeeded on it. A gray image with alpha
  /// is stored as `rg` (gray in the first component, alpha in the second) with
  /// `AlphaMode::none`, because `ImageData` has no gray-alpha format; its diagnostic
  /// `image.gray_alpha_stored_as_rg` says so. Features of the source that could not be
  /// represented are listed in `ImageData::unsupported`.
  asset::ImageData image;
  /// Notes about the decode, in the order they were made: every decision the Adapter
  /// took (interpretation default, channel conversion, alpha handling, a flip) and
  /// every source feature that was not applied (ICC profile, gamma, chromaticities,
  /// EXIF orientation). Each item in `ImageData::unsupported` has a warning here.
  std::vector<asset::DecodeDiagnostic> diagnostics;
  /// The identity of the decoded bytes (copied from the input): provenance.
  asset::SourceIdentity source;
  /// What the backend found in the file.
  ImageSourceInfo native;
};

/// The failures specific to this Adapter. Limit failures are not here: they are the
/// shared `asset::DecodeError` values (`input_too_large`, `dimension_exceeded`,
/// `pixel_count_exceeded`), returned unchanged. Values start at 1; new values are
/// appended. The `Error::detail` of a failure holds the backend's short reason.
enum class ImageDecodeError : std::uint8_t {
  /// The input is empty, truncated or corrupt for its format (including a header that
  /// does not match the data and a zero dimension).
  malformed_image = 1,
  /// The input is not in a format this Adapter decodes (a known but disabled format
  /// such as GIF, or an unrecognized one). The detail names the format when known.
  unsupported_format,
  /// The input is in a supported format but uses something the backend cannot decode
  /// (for example arithmetic-coded JPEG or RLE BMP).
  unsupported_feature,
  /// An option does not fit the result (`ImageDecodeOptions::interpretation` is not
  /// allowed for the resulting pixel format).
  invalid_option,
  /// An allocation failed.
  out_of_memory,
};

/// The category of ImageDecodeError codes (name "terreate.adapter.asset_image_stb").
[[nodiscard]] auto image_decode_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of an ImageDecodeError.
[[nodiscard]] auto make_error_code(ImageDecodeError error) noexcept -> std::error_code;

/// Decodes the bytes of one image into an `asset::ImageData`.
///
/// The order is fixed and every step that can reject does so before the next
/// allocates: the input size against `options.limits` (`asset::checkInputSize`; also
/// at most 2^31 - 1 bytes, the backend's length type), the container signature, the
/// header (dimensions and component count, without decoding pixels), the dimensions
/// against `options.limits` (`asset::checkImageSize`), the options against the
/// resulting format, and only then the pixel decode. A truncated, corrupt or hostile
/// input fails with a named error; it never reads outside `input.bytes` and the backend's
/// allocations are capped per call (4 MiB plus 48 bytes per pixel of the checked size), so a
/// decompression bomb fails as `out_of_memory`. A legal image needs a small multiple of
/// `max_pixels` times the pixel size. Short BMP, TGA and flat HDR files are detected from
/// the header; a short RLE TGA, run-length HDR or JPEG cut inside a scan may decode partly
/// zero-filled (the backend's behavior).
///
/// Pixel formats: 8-bit sources give `r8`, `rg8`, `rgb8` or `rgba8`; 16-bit PNG gives
/// `r16`, `rg16` or `rgba16`; Radiance HDR gives `r32f`, `rg32f` or `rgba32f`. The
/// result is tightly packed, one mip, one layer, in host byte order.
///
/// Fails with `asset::DecodeError` (limits) or `ImageDecodeError`. The function is
/// thread-safe: it uses no global state, and `input.bytes` must stay alive and
/// unchanged during the call (the result owns its pixels and does not borrow them).
[[nodiscard]] auto decodeImage(const asset::DecodeInput &input,
                               const ImageDecodeOptions &options = {})
    -> terreate::Result<ImageDecodeResult>;

} // namespace terreate::adapter::asset_image_stb

namespace std {
/// Lets ImageDecodeError convert to std::error_code.
template <>
struct is_error_code_enum<terreate::adapter::asset_image_stb::ImageDecodeError> : true_type {};
} // namespace std

#endif // TERREATE_ADAPTER_ASSET_IMAGE_STB_DECODER_HPP

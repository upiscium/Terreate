#include <terreate/adapter/asset_image_stb/decoder.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "stb_image_backend.hpp"

namespace terreate::adapter::asset_image_stb {
namespace {

namespace asset = terreate::asset;

// ---------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------

class ImageDecodeErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.adapter.asset_image_stb"; }
  std::string message(int value) const override {
    switch (static_cast<ImageDecodeError>(value)) {
    case ImageDecodeError::malformed_image:
      return "the image input is empty, truncated or corrupt";
    case ImageDecodeError::unsupported_format:
      return "the image format is not decoded by this adapter";
    case ImageDecodeError::unsupported_feature:
      return "the image uses a feature the backend cannot decode";
    case ImageDecodeError::invalid_option:
      return "an option is not allowed for the resulting pixel format";
    case ImageDecodeError::out_of_memory:
      return "an allocation failed while decoding the image";
    }
    return "unknown image decode error";
  }
};

// Error construction allocates; on exhaustion a code-only Error is returned, which does
// not allocate.
template <typename Code>
auto makeError(Code code, const char *operation, const std::string &detail) -> terreate::Error {
  try {
    return terreate::Error{make_error_code(code), operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{make_error_code(code)};
  }
}

// ---------------------------------------------------------------------------------------
// Byte helpers (all reads are bounds-checked: the input may be hostile)
// ---------------------------------------------------------------------------------------

using Bytes = asset::ByteView;

auto byteAt(Bytes bytes, std::size_t offset) -> unsigned {
  return offset < bytes.size() ? static_cast<unsigned>(bytes[offset]) : 0x100U;
}

auto hasAt(Bytes bytes, std::size_t offset, std::string_view text) -> bool {
  if (offset > bytes.size() || bytes.size() - offset < text.size())
    return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (static_cast<unsigned char>(bytes[offset + i]) != static_cast<unsigned char>(text[i]))
      return false;
  }
  return true;
}

auto readBe32(Bytes bytes, std::size_t offset) -> std::uint32_t {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i)
    value = (value << 8) | (byteAt(bytes, offset + i) & 0xFFU);
  return value;
}

auto readBe16(Bytes bytes, std::size_t offset) -> std::uint32_t {
  return ((byteAt(bytes, offset) & 0xFFU) << 8) | (byteAt(bytes, offset + 1) & 0xFFU);
}

// ---------------------------------------------------------------------------------------
// Container detection
// ---------------------------------------------------------------------------------------

constexpr std::string_view png_signature{"\x89PNG\r\n\x1a\n", 8};

// The name of a recognizable format this adapter does not decode, or empty.
auto unsupportedFormatName(Bytes bytes) -> std::string_view {
  if (hasAt(bytes, 0, "GIF8"))
    return "gif";
  if (hasAt(bytes, 0, "8BPS"))
    return "psd";
  if (hasAt(bytes, 0, "RIFF") && hasAt(bytes, 8, "WEBP"))
    return "webp";
  if (hasAt(bytes, 0, std::string_view{"II*\0", 4}) ||
      hasAt(bytes, 0, std::string_view{"MM\0*", 4}))
    return "tiff";
  if (hasAt(bytes, 0, "DDS "))
    return "dds";
  if (hasAt(bytes, 4, "ftyp"))
    return "heif-or-avif";
  if (byteAt(bytes, 0) == 0xABU && hasAt(bytes, 1, "KTX"))
    return "ktx";
  if (byteAt(bytes, 0) == 0x53U && byteAt(bytes, 1) == 0x80U && byteAt(bytes, 2) == 0xF6U &&
      byteAt(bytes, 3) == 0x34U)
    return "pic";
  if (byteAt(bytes, 0) == 'P') {
    const unsigned kind = byteAt(bytes, 1);
    const unsigned after = byteAt(bytes, 2);
    const bool pnm_kind = (kind >= '1' && kind <= '6') || kind == 'F' || kind == 'f';
    if (pnm_kind && (after == ' ' || after == '\t' || after == '\n' || after == '\r'))
      return "pnm";
  }
  return {};
}

auto detectContainer(Bytes bytes) -> ImageContainer {
  if (hasAt(bytes, 0, png_signature))
    return ImageContainer::png;
  if (byteAt(bytes, 0) == 0xFFU && byteAt(bytes, 1) == 0xD8U && byteAt(bytes, 2) == 0xFFU)
    return ImageContainer::jpeg;
  if (hasAt(bytes, 0, "BM"))
    return ImageContainer::bmp;
  if (hasAt(bytes, 0, "#?RADIANCE\n") || hasAt(bytes, 0, "#?RGBE\n"))
    return ImageContainer::hdr;
  return ImageContainer::tga;
}

// ---------------------------------------------------------------------------------------
// Metadata scans (best effort and bounded: they only add notes, the decode does not
// depend on them except for the channel count of a PNG)
// ---------------------------------------------------------------------------------------

struct PngMetadata {
  bool valid = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  unsigned color_type = 0;
  unsigned bit_depth = 0;
  bool has_transparency = false;
  std::optional<std::uint64_t> icc_offset;
  std::optional<std::uint64_t> gamma_offset;
  std::uint32_t gamma = 0;
  std::optional<std::uint64_t> chromaticity_offset;
  std::optional<std::uint64_t> srgb_offset;
  bool unknown_critical = false;
};

// Walks the chunks up to the first IDAT (the chunks that matter must precede it). Every
// length is checked against the remaining bytes, and every step advances by at least 12
// bytes, so the walk is bounded by the input size.
auto scanPng(Bytes bytes) -> PngMetadata {
  PngMetadata meta;
  std::size_t position = png_signature.size();
  bool first = true;
  while (bytes.size() >= 12 && position <= bytes.size() - 12) {
    const std::uint32_t length = readBe32(bytes, position);
    if (length > bytes.size() - position - 12)
      break;
    const std::size_t data = position + 8;
    if (first) {
      if (!hasAt(bytes, position + 4, "IHDR") || length != 13)
        return meta;
      meta.width = readBe32(bytes, data);
      meta.height = readBe32(bytes, data + 4);
      meta.bit_depth = byteAt(bytes, data + 8);
      meta.color_type = byteAt(bytes, data + 9);
      meta.valid = true;
      first = false;
    } else if (hasAt(bytes, position + 4, "IDAT") || hasAt(bytes, position + 4, "IEND")) {
      break;
    } else if (hasAt(bytes, position + 4, "tRNS")) {
      meta.has_transparency = true;
    } else if (hasAt(bytes, position + 4, "iCCP")) {
      meta.icc_offset = position;
    } else if (hasAt(bytes, position + 4, "gAMA") && length == 4) {
      meta.gamma_offset = position;
      meta.gamma = readBe32(bytes, data);
    } else if (hasAt(bytes, position + 4, "cHRM")) {
      meta.chromaticity_offset = position;
    } else if (hasAt(bytes, position + 4, "sRGB")) {
      meta.srgb_offset = position;
    } else if (byteAt(bytes, position + 4) >= 'A' && byteAt(bytes, position + 4) <= 'Z' &&
               !hasAt(bytes, position + 4, "PLTE")) {
      // A critical chunk (upper-case first letter) the decoder does not know.
      meta.unknown_critical = true;
    }
    position += 12 + static_cast<std::size_t>(length);
  }
  return meta;
}

// The channels a PNG decodes to: gray 1, RGB 3, palette 3, gray-alpha 2, RGBA 4, plus one
// alpha channel for a tRNS color key or a palette transparency table (not for a type
// that already has alpha).
auto pngChannels(const PngMetadata &meta) -> std::uint32_t {
  std::uint32_t channels = 0;
  switch (meta.color_type) {
  case 0:
    channels = 1;
    break;
  case 2:
  case 3:
    channels = 3;
    break;
  case 4:
    channels = 2;
    break;
  case 6:
    channels = 4;
    break;
  default:
    return 0;
  }
  if (meta.has_transparency &&
      (meta.color_type == 0 || meta.color_type == 2 || meta.color_type == 3))
    ++channels;
  return channels;
}

auto pngColorTypeName(unsigned color_type) -> std::string_view {
  switch (color_type) {
  case 0:
    return "gray";
  case 2:
    return "rgb";
  case 3:
    return "palette";
  case 4:
    return "gray_alpha";
  case 6:
    return "rgba";
  default:
    return "unknown";
  }
}

struct JpegMetadata {
  bool has_dimensions = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::optional<std::uint64_t> icc_offset;
  std::optional<std::uint64_t> exif_offset;
};

// Walks the marker segments up to the start of the scan. Each step advances by at least
// two bytes, so the walk is bounded by the input size.
auto scanJpeg(Bytes bytes) -> JpegMetadata {
  JpegMetadata meta;
  std::size_t position = 2;
  while (position + 4 <= bytes.size()) {
    if (byteAt(bytes, position) != 0xFFU) {
      break;
    }
    const unsigned marker = byteAt(bytes, position + 1);
    if (marker == 0xFFU) { // fill byte
      ++position;
      continue;
    }
    if (marker == 0x01U || (marker >= 0xD0U && marker <= 0xD8U)) { // no length
      position += 2;
      continue;
    }
    if (marker == 0xD9U || marker == 0xDAU)
      break;
    const std::size_t length = readBe16(bytes, position + 2);
    if (length < 2)
      break;
    const std::size_t payload = position + 4;
    // Start of frame (not DHT 0xC4, JPG 0xC8 or DAC 0xCC): precision, height, width.
    if (marker >= 0xC0U && marker <= 0xCFU && marker != 0xC4U && marker != 0xC8U &&
        marker != 0xCCU && !meta.has_dimensions && length >= 7) {
      meta.height = readBe16(bytes, payload + 1);
      meta.width = readBe16(bytes, payload + 3);
      meta.has_dimensions = true;
    }
    if (marker == 0xE2U && hasAt(bytes, payload, std::string_view{"ICC_PROFILE\0", 12}))
      meta.icc_offset = position;
    if (marker == 0xE1U && hasAt(bytes, payload, std::string_view{"Exif\0\0", 6}))
      meta.exif_offset = position;
    position += 2 + length;
  }
  return meta;
}

// What the header says before the backend looks at it: the dimensions it claims (so a hostile
// size is refused by the limits, not by a backend failure that cannot tell why) and, for the
// formats whose backend reader does not notice a short file (it reads zeros past the end),
// the number of bytes the file must have.
struct Preflight {
  bool has_dimensions = false;
  std::uint64_t width = 0;
  std::uint64_t height = 0;
  std::uint64_t required_size = 0;
};

template <unsigned Count> auto readLe(Bytes bytes, std::size_t offset) -> std::uint64_t {
  std::uint64_t value = 0;
  for (unsigned i = Count; i > 0; --i)
    value = (value << 8) | (byteAt(bytes, offset + i - 1) & 0xFFU);
  return value;
}

// Radiance: the header lines end with an empty line, then `-Y <height> +X <width>`.
auto preflightHdr(Bytes bytes) -> Preflight {
  Preflight out;
  const std::size_t scan_end = std::min<std::size_t>(bytes.size(), 4096);
  std::size_t line = 0;
  while (line < scan_end) {
    std::size_t end = line;
    while (end < scan_end && byteAt(bytes, end) != '\n')
      ++end;
    if (end >= scan_end)
      return out;
    if (end == line) { // the empty line: the resolution line follows
      std::size_t cursor = end + 1;
      if (!hasAt(bytes, cursor, "-Y "))
        return out;
      cursor += 3;
      std::uint64_t height = 0;
      std::size_t digits = 0;
      while (byteAt(bytes, cursor) >= '0' && byteAt(bytes, cursor) <= '9' && digits < 10) {
        height = height * 10 + (byteAt(bytes, cursor) - '0');
        ++cursor;
        ++digits;
      }
      if (digits == 0 || !hasAt(bytes, cursor, " +X "))
        return out;
      cursor += 4;
      std::uint64_t width = 0;
      digits = 0;
      while (byteAt(bytes, cursor) >= '0' && byteAt(bytes, cursor) <= '9' && digits < 10) {
        width = width * 10 + (byteAt(bytes, cursor) - '0');
        ++cursor;
        ++digits;
      }
      if (digits == 0 || byteAt(bytes, cursor) != '\n')
        return out;
      ++cursor;
      out.has_dimensions = true;
      out.width = width;
      out.height = height;
      // Scanlines of 8..32767 pixels are run-length coded and the backend fails on a short
      // one; the others are flat, 4 bytes per pixel.
      // (Sizes beyond 2^20 are left to the limits, which refuse them by default.)
      if ((width < 8 || width >= 32768) && width != 0 && width <= (1ULL << 20) &&
          height <= (1ULL << 20))
        out.required_size = cursor + width * height * 4;
      return out;
    }
    line = end + 1;
  }
  return out;
}

auto preflight(Bytes bytes, ImageContainer container, const PngMetadata &png,
               const JpegMetadata &jpeg) -> Preflight {
  Preflight out;
  switch (container) {
  case ImageContainer::png:
    if (png.valid) {
      out.has_dimensions = true;
      out.width = png.width;
      out.height = png.height;
    }
    break;
  case ImageContainer::jpeg:
    if (jpeg.has_dimensions) {
      out.has_dimensions = true;
      out.width = jpeg.width;
      out.height = jpeg.height;
    }
    break;
  case ImageContainer::bmp: {
    const std::uint64_t header_size = readLe<4>(bytes, 14);
    if (header_size >= 40 && bytes.size() >= 54) {
      const auto width = static_cast<std::int32_t>(readLe<4>(bytes, 18));
      const auto raw_height = static_cast<std::int32_t>(readLe<4>(bytes, 22));
      const std::uint64_t height = raw_height < 0 ? -static_cast<std::int64_t>(raw_height)
                                                  : static_cast<std::uint64_t>(raw_height);
      const std::uint64_t bits = readLe<2>(bytes, 28);
      const std::uint64_t compression = readLe<4>(bytes, 30);
      if (width > 0 && height > 0) {
        out.has_dimensions = true;
        out.width = static_cast<std::uint64_t>(width);
        out.height = height;
        if ((compression == 0 || compression == 3) && bits > 0 && bits <= 32 &&
            out.width <= (1ULL << 20) && height <= (1ULL << 20)) {
          // The last row may omit its padding.
          const std::uint64_t stride = ((out.width * bits + 31) / 32) * 4;
          out.required_size =
              readLe<4>(bytes, 10) + stride * (height - 1) + (out.width * bits + 7) / 8;
        }
      }
    }
    break;
  }
  case ImageContainer::tga: {
    if (bytes.size() >= 18) {
      out.has_dimensions = true;
      out.width = readLe<2>(bytes, 12);
      out.height = readLe<2>(bytes, 14);
      const std::uint64_t image_type = byteAt(bytes, 2);
      if (image_type == 1 || image_type == 2 || image_type == 3) {
        const std::uint64_t palette_bytes =
            byteAt(bytes, 1) == 1 ? readLe<2>(bytes, 5) * ((byteAt(bytes, 7) + 7ULL) / 8) : 0;
        out.required_size = 18 + byteAt(bytes, 0) + palette_bytes +
                            out.width * out.height * ((byteAt(bytes, 16) + 7ULL) / 8);
      }
    }
    break;
  }
  case ImageContainer::hdr:
    out = preflightHdr(bytes);
    break;
  }
  return out;
}

auto checkClaimedSize(const Preflight &claim, const asset::DecodeLimits &limits)
    -> terreate::Result<void> {
  constexpr std::uint64_t cap = UINT32_MAX;
  if (claim.width > cap || claim.height > cap)
    return std::unexpected(makeError(asset::DecodeError::dimension_exceeded, "decodeImage",
                                     "the header claims " + std::to_string(claim.width) + "x" +
                                         std::to_string(claim.height)));
  return asset::checkImageSize(static_cast<std::uint32_t>(std::min(claim.width, cap)),
                               static_cast<std::uint32_t>(std::min(claim.height, cap)), 1, limits);
}

// ---------------------------------------------------------------------------------------
// Failure classification of the backend
// ---------------------------------------------------------------------------------------

auto backendFailure(const char *operation, ImageContainer container, bool header_stage)
    -> terreate::Error {
  const char *raw = stbi_failure_reason();
  const std::string reason = raw != nullptr ? raw : "unknown";
  const std::string_view view{reason};
  if (view == "too large")
    return makeError(asset::DecodeError::dimension_exceeded, operation,
                     "the backend refused the size: " + reason);
  if (view == "outofmem")
    return makeError(ImageDecodeError::out_of_memory, operation, reason);
  if (view == "unknown image type") {
    // The backend's header probe reports this for every refusal. A recognized signature
    // that the backend does not accept is corrupt; only a TGA candidate (no signature) is
    // "not an image of a known format".
    if (container == ImageContainer::tga && header_stage)
      return makeError(ImageDecodeError::unsupported_format, operation,
                       "unrecognized image format");
    return makeError(ImageDecodeError::malformed_image, operation,
                     header_stage ? "the header is not a valid " +
                                        std::string{containerName(container)} + " header"
                                  : reason);
  }
  constexpr std::string_view unsupported[] = {"only 8-bit",
                                              "no header height",
                                              "1/2/4/8/16-bit only",
                                              "unknown BMP",
                                              "BMP RLE",
                                              "BMP JPEG/PNG",
                                              "unsupported",
                                              "unsupported format",
                                              "unsupported data layout",
                                              "bad bits_per_channel"};
  const bool listed =
      std::find(std::begin(unsupported), std::end(unsupported), view) != std::end(unsupported);
  if (listed || view.find("PNG chunk not known") != std::string_view::npos)
    return makeError(ImageDecodeError::unsupported_feature, operation, reason);
  return makeError(ImageDecodeError::malformed_image, operation, reason);
}

// ---------------------------------------------------------------------------------------
// Pixel helpers
// ---------------------------------------------------------------------------------------

enum class Depth : std::uint8_t { eight, sixteen, floating };

auto formatFor(Depth depth, std::uint32_t channels) -> asset::PixelFormat {
  using asset::PixelFormat;
  switch (depth) {
  case Depth::eight:
    return channels == 1   ? PixelFormat::r8
           : channels == 2 ? PixelFormat::rg8
           : channels == 3 ? PixelFormat::rgb8
                           : PixelFormat::rgba8;
  case Depth::sixteen:
    return channels == 1   ? PixelFormat::r16
           : channels == 2 ? PixelFormat::rg16
                           : PixelFormat::rgba16;
  case Depth::floating:
    return channels == 1   ? PixelFormat::r32f
           : channels == 2 ? PixelFormat::rg32f
                           : PixelFormat::rgba32f;
  }
  return PixelFormat::rgba8;
}

auto interpretationName(asset::ColorInterpretation interpretation) -> std::string_view {
  switch (interpretation) {
  case asset::ColorInterpretation::srgb:
    return "srgb";
  case asset::ColorInterpretation::linear:
    return "linear";
  case asset::ColorInterpretation::normal_map:
    return "normal_map";
  }
  return "unknown";
}

// ImageData's rules (image_data.hpp): float formats are linear only, srgb needs an 8-bit
// format, normal_map needs an integer format with at least two components.
auto interpretationAllowed(asset::PixelFormat format, asset::ColorInterpretation interpretation)
    -> bool {
  switch (interpretation) {
  case asset::ColorInterpretation::srgb:
    return asset::componentSize(format) == 1;
  case asset::ColorInterpretation::linear:
    return true;
  case asset::ColorInterpretation::normal_map:
    return !asset::isFloat(format) && asset::componentCount(format) >= 2;
  }
  return false;
}

struct StbDeleter {
  void operator()(void *pixels) const noexcept { stbi_image_free(pixels); }
};
using StbPixels = std::unique_ptr<void, StbDeleter>;

template <typename Sample, typename Op>
void forEachPixelRgba(void *pixels, std::size_t pixel_count, Op op) {
  auto *samples = static_cast<Sample *>(pixels);
  for (std::size_t i = 0; i < pixel_count; ++i)
    op(samples + i * 4);
}

void premultiply(void *pixels, std::size_t pixel_count, Depth depth) {
  switch (depth) {
  case Depth::eight:
    forEachPixelRgba<std::uint8_t>(pixels, pixel_count, [](std::uint8_t *p) {
      for (int c = 0; c < 3; ++c)
        p[c] = static_cast<std::uint8_t>((static_cast<unsigned>(p[c]) * p[3] + 127U) / 255U);
    });
    break;
  case Depth::sixteen:
    forEachPixelRgba<std::uint16_t>(pixels, pixel_count, [](std::uint16_t *p) {
      for (int c = 0; c < 3; ++c)
        p[c] =
            static_cast<std::uint16_t>((static_cast<std::uint64_t>(p[c]) * p[3] + 32767U) / 65535U);
    });
    break;
  case Depth::floating:
    forEachPixelRgba<float>(pixels, pixel_count, [](float *p) {
      for (int c = 0; c < 3; ++c)
        p[c] *= p[3];
    });
    break;
  }
}

// ---------------------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------------------

struct Notes {
  const std::string &source;
  std::vector<asset::DecodeDiagnostic> &diagnostics;

  void add(asset::DecodeSeverity severity, std::string code, std::string message,
           std::optional<std::uint64_t> offset = {}) const {
    asset::DecodeDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.code = std::move(code);
    diagnostic.message = std::move(message);
    diagnostic.location.source = source;
    diagnostic.location.byte_offset = offset;
    diagnostics.push_back(std::move(diagnostic));
  }
};

void addCustom(asset::ImageData &image, std::string key, std::string value) {
  image.custom.push_back(asset::CustomEntry{std::move(key), std::move(value)});
}

} // namespace

// ---------------------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------------------

auto containerName(ImageContainer container) noexcept -> std::string_view {
  switch (container) {
  case ImageContainer::png:
    return "png";
  case ImageContainer::jpeg:
    return "jpeg";
  case ImageContainer::bmp:
    return "bmp";
  case ImageContainer::tga:
    return "tga";
  case ImageContainer::hdr:
    return "hdr";
  }
  return "unknown";
}

auto image_decode_error_category() noexcept -> const std::error_category & {
  static const ImageDecodeErrorCategory category;
  return category;
}

auto make_error_code(ImageDecodeError error) noexcept -> std::error_code {
  return {static_cast<int>(error), image_decode_error_category()};
}

namespace {

auto decodeImageImpl(const asset::DecodeInput &input, const ImageDecodeOptions &options)
    -> terreate::Result<ImageDecodeResult> {
  const Bytes bytes = input.bytes;
  constexpr const char *operation = "decodeImage";

  // 1. The encoded size, before the bytes are touched.
  if (auto size = asset::checkInputSize(bytes, options.limits); !size)
    return std::unexpected(std::move(size.error()));
  if (bytes.empty())
    return std::unexpected(makeError(ImageDecodeError::malformed_image, operation, "empty input"));
  if (bytes.size() > static_cast<std::size_t>(INT_MAX))
    return std::unexpected(makeError(asset::DecodeError::input_too_large, operation,
                                     "the backend takes at most 2^31 - 1 bytes"));

  // 2. The signature.
  if (const auto name = unsupportedFormatName(bytes); !name.empty())
    return std::unexpected(
        makeError(ImageDecodeError::unsupported_format, operation,
                  "recognized as " + std::string{name} + ", which this adapter does not decode"));
  const ImageContainer container = detectContainer(bytes);
  const auto *const data = reinterpret_cast<const stbi_uc *>(bytes.data());
  const int length = static_cast<int>(bytes.size());

  // 3. What the header claims, then the backend's own header probe (dimensions and
  //    components, no pixels). The claim is checked against the limits first, so a hostile
  //    size is refused by name; a TGA candidate (no signature) must first be accepted by the
  //    backend, or its "header" is just arbitrary bytes.
  PngMetadata png;
  JpegMetadata jpeg;
  if (container == ImageContainer::png)
    png = scanPng(bytes);
  else if (container == ImageContainer::jpeg)
    jpeg = scanJpeg(bytes);
  const Preflight claim = preflight(bytes, container, png, jpeg);
  const auto checkClaim = [&]() -> terreate::Result<void> {
    if (claim.has_dimensions) {
      if (auto fits = checkClaimedSize(claim, options.limits); !fits)
        return fits;
    }
    if (claim.required_size > bytes.size())
      return std::unexpected(makeError(ImageDecodeError::malformed_image, operation,
                                       "truncated: the header needs " +
                                           std::to_string(claim.required_size) + " bytes, got " +
                                           std::to_string(bytes.size())));
    return {};
  };
  if (container != ImageContainer::tga) {
    if (auto claimed = checkClaim(); !claimed)
      return std::unexpected(std::move(claimed.error()));
  }
  // The backend's allocations are capped per call: a small budget for the header probe, then
  // a multiple of the largest decoded image the limits allow for this header (see
  // stb_image_backend.hpp: a decompression bomb ends as an allocation failure).
  struct BudgetReset {
    ~BudgetReset() { backend::setBudget(UINT64_MAX); }
  } budget_reset;
  backend::setBudget(4ULL << 20);
  int width = 0;
  int height = 0;
  int header_components = 0;
  if (stbi_info_from_memory(data, length, &width, &height, &header_components) == 0) {
    // The backend's header probe gives one reason for every refusal; the two refusals that are
    // "valid but not decodable" are told apart from the header itself.
    if (png.unknown_critical)
      return std::unexpected(makeError(ImageDecodeError::unsupported_feature, operation,
                                       "the PNG has a critical chunk this decoder does not know"));
    if (container == ImageContainer::bmp && readLe<4>(bytes, 14) >= 40) {
      const std::uint64_t compression = readLe<4>(bytes, 30);
      if (compression == 1 || compression == 2 || compression >= 4)
        return std::unexpected(
            makeError(ImageDecodeError::unsupported_feature, operation,
                      "BMP compression " + std::to_string(compression) + " is not supported"));
    }
    return std::unexpected(backendFailure(operation, container, true));
  }
  if (container == ImageContainer::tga) {
    if (auto claimed = checkClaim(); !claimed)
      return std::unexpected(std::move(claimed.error()));
  }
  if (width <= 0 || height <= 0)
    return std::unexpected(
        makeError(ImageDecodeError::malformed_image, operation, "non-positive dimensions"));

  // 4. The limits on what the backend reports, before any pixel buffer exists.
  if (auto fits = asset::checkImageSize(static_cast<std::uint32_t>(width),
                                        static_cast<std::uint32_t>(height), 1, options.limits);
      !fits)
    return std::unexpected(std::move(fits.error()));

  // 5. What the decode will produce.
  std::uint32_t source_channels = static_cast<std::uint32_t>(std::clamp(header_components, 1, 4));
  if (container == ImageContainer::png && png.valid && pngChannels(png) != 0)
    source_channels = pngChannels(png);
  const bool floating = container == ImageContainer::hdr;
  const bool sixteen =
      container == ImageContainer::png && stbi_is_16_bit_from_memory(data, length) != 0;
  const Depth depth = floating ? Depth::floating : (sixteen ? Depth::sixteen : Depth::eight);
  const bool source_alpha = source_channels == 2 || source_channels == 4;

  std::uint32_t channels = static_cast<std::uint32_t>(options.channels);
  const bool forced = options.channels != ChannelRequest::as_decoded;
  bool widened = false;
  if (!forced) {
    channels = source_channels;
    if (channels == 3 && depth != Depth::eight) {
      channels = 4;
      widened = true;
    }
  }
  const asset::PixelFormat format = formatFor(depth, channels);

  const bool defaulted_interpretation = !options.interpretation.has_value();
  const asset::ColorInterpretation interpretation =
      options.interpretation.value_or(depth == Depth::eight ? asset::ColorInterpretation::srgb
                                                            : asset::ColorInterpretation::linear);
  if (!interpretationAllowed(format, interpretation))
    return std::unexpected(
        makeError(ImageDecodeError::invalid_option, operation,
                  "color interpretation " + std::string{interpretationName(interpretation)} +
                      " is not allowed for " + std::to_string(channels) + " channel(s) of " +
                      (depth == Depth::eight     ? "8-bit"
                       : depth == Depth::sixteen ? "16-bit"
                                                 : "float") +
                      " samples"));

  const asset::ImageExtent extent{static_cast<std::uint32_t>(width),
                                  static_cast<std::uint32_t>(height), 1};
  auto layout = asset::packedLayout(format, extent, 1, 1);
  if (!layout)
    return std::unexpected(std::move(layout.error()));

  // 6. The pixel decode.
  int decoded_width = 0;
  int decoded_height = 0;
  int decoded_components = 0;
  const int wanted = static_cast<int>(channels);
  StbPixels pixels;
  backend::setBudget((4ULL << 20) + 3ULL * 16ULL * static_cast<std::uint64_t>(width) *
                                        static_cast<std::uint64_t>(height));
  switch (depth) {
  case Depth::eight:
    pixels.reset(stbi_load_from_memory(data, length, &decoded_width, &decoded_height,
                                       &decoded_components, wanted));
    break;
  case Depth::sixteen:
    pixels.reset(stbi_load_16_from_memory(data, length, &decoded_width, &decoded_height,
                                          &decoded_components, wanted));
    break;
  case Depth::floating:
    pixels.reset(stbi_loadf_from_memory(data, length, &decoded_width, &decoded_height,
                                        &decoded_components, wanted));
    break;
  }
  if (!pixels)
    return std::unexpected(backendFailure(operation, container, false));
  if (decoded_width != width || decoded_height != height)
    return std::unexpected(makeError(ImageDecodeError::malformed_image, operation,
                                     "the pixel data does not match the header"));

  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  const bool has_alpha_channel = channels == 4 && source_alpha;
  asset::AlphaMode alpha_mode = asset::AlphaMode::none;
  if (has_alpha_channel) {
    switch (options.alpha) {
    case AlphaHandling::straight:
      alpha_mode = asset::AlphaMode::straight;
      break;
    case AlphaHandling::premultiply:
      premultiply(pixels.get(), pixel_count, depth);
      alpha_mode = asset::AlphaMode::premultiplied;
      break;
    case AlphaHandling::ignore:
      alpha_mode = asset::AlphaMode::none;
      break;
    }
  }

  // 7. The result.
  ImageDecodeResult result;
  result.source = input.source;
  result.native.container = container;
  result.native.channels = source_channels;
  result.native.floating_point = floating;
  result.native.bits_per_channel = floating ? 32U : (sixteen ? 16U : 8U);

  asset::ImageData &image = result.image;
  image.format = format;
  image.interpretation = interpretation;
  image.alpha = alpha_mode;
  image.extent = extent;
  image.array_layers = 1;
  image.mip_levels = 1;
  image.subresources = layout->subresources;
  image.data.resize(layout->byte_size);

  const std::size_t row_bytes = layout->subresources.front().row_pitch;
  const auto *source_rows = static_cast<const std::byte *>(pixels.get());
  if (options.flip_vertically) {
    for (std::size_t row = 0; row < static_cast<std::size_t>(height); ++row)
      std::memcpy(image.data.data() + row * row_bytes,
                  source_rows + (static_cast<std::size_t>(height) - 1 - row) * row_bytes,
                  row_bytes);
  } else {
    std::memcpy(image.data.data(), source_rows, layout->byte_size);
  }
  pixels.reset();

  const Notes notes{input.source.name, result.diagnostics};
  using asset::DecodeSeverity;

  addCustom(image, "stb.container", std::string{containerName(container)});
  addCustom(image, "source.channels", std::to_string(source_channels));
  addCustom(image, "source.bits_per_channel", std::to_string(result.native.bits_per_channel));

  if (container == ImageContainer::png && png.valid) {
    addCustom(image, "png.color_type", std::string{pngColorTypeName(png.color_type)});
    addCustom(image, "png.bit_depth", std::to_string(png.bit_depth));
    if (png.has_transparency)
      notes.add(DecodeSeverity::info, "image.png.transparency_as_alpha",
                "the PNG transparency (tRNS) became an alpha channel of the decoded image");
    if (png.bit_depth < 8)
      notes.add(DecodeSeverity::info, "image.png.low_bit_depth_expanded",
                std::to_string(png.bit_depth) + "-bit samples were expanded to 8 bits");
    if (png.srgb_offset)
      addCustom(image, "png.srgb_chunk", "present");
    if (png.icc_offset) {
      image.unsupported.push_back({"color-profile", "ICC", "iCCP chunk present; not applied"});
      notes.add(DecodeSeverity::warning, "image.png.icc_profile_not_applied",
                "the PNG has an embedded ICC profile (iCCP); the pixels are returned as stored",
                png.icc_offset);
    }
    if (png.gamma_offset) {
      image.unsupported.push_back(
          {"color-profile", "gAMA",
           "gamma " + std::to_string(png.gamma) + " (x100000) present; not applied"});
      addCustom(image, "png.gamma_x100000", std::to_string(png.gamma));
      notes.add(DecodeSeverity::warning, "image.png.gamma_not_applied",
                "the PNG has a gamma chunk (gAMA); the pixels are returned as stored",
                png.gamma_offset);
    }
    if (png.chromaticity_offset) {
      image.unsupported.push_back({"color-profile", "cHRM", "chromaticities present; not applied"});
      notes.add(DecodeSeverity::warning, "image.png.chromaticities_not_applied",
                "the PNG has chromaticities (cHRM); the pixels are returned as stored",
                png.chromaticity_offset);
    }
  }
  if (container == ImageContainer::jpeg) {
    if (jpeg.icc_offset) {
      image.unsupported.push_back(
          {"color-profile", "ICC", "APP2 ICC_PROFILE present; not applied"});
      notes.add(DecodeSeverity::warning, "image.jpeg.icc_profile_not_applied",
                "the JPEG has an embedded ICC profile; the pixels are returned as stored",
                jpeg.icc_offset);
    }
    if (jpeg.exif_offset) {
      image.unsupported.push_back(
          {"metadata", "EXIF", "EXIF block present; the orientation is not applied"});
      notes.add(DecodeSeverity::warning, "image.jpeg.exif_orientation_not_applied",
                "the JPEG has an EXIF block; an EXIF orientation is neither read nor applied",
                jpeg.exif_offset);
    }
  }

  if (defaulted_interpretation)
    notes.add(DecodeSeverity::info, "image.interpretation_defaulted",
              "no color interpretation was requested; " +
                  std::string{interpretationName(interpretation)} + " was chosen for " +
                  (depth == Depth::eight     ? "8-bit"
                   : depth == Depth::sixteen ? "16-bit"
                                             : "float") +
                  " samples");
  if (forced && channels != source_channels)
    notes.add(DecodeSeverity::info, "image.channels_converted",
              "the source has " + std::to_string(source_channels) + " channel(s); " +
                  std::to_string(channels) + " were requested and converted by the backend");
  if (widened)
    notes.add(DecodeSeverity::info, "image.channels_widened",
              "ImageData has no 3-component " +
                  std::string{depth == Depth::sixteen ? "16-bit" : "float"} +
                  " format; the image was widened to 4 components with an opaque alpha");
  if (channels == 2)
    notes.add(DecodeSeverity::info, "image.gray_alpha_stored_as_rg",
              "the first component is gray and the second is alpha; ImageData has no gray-alpha "
              "format, so the image is rg with alpha mode none");
  if (has_alpha_channel && options.alpha == AlphaHandling::premultiply)
    notes.add(DecodeSeverity::info, "image.alpha_premultiplied",
              "the color components were multiplied by alpha");
  if (has_alpha_channel && options.alpha == AlphaHandling::ignore)
    notes.add(DecodeSeverity::info, "image.alpha_ignored",
              "the alpha channel is kept but marked as not meaningful");
  if (!has_alpha_channel && options.alpha != AlphaHandling::straight)
    notes.add(DecodeSeverity::info, "image.alpha_not_applicable",
              "an alpha handling was requested but the result has no alpha channel with a meaning");
  if (options.flip_vertically)
    notes.add(DecodeSeverity::info, "image.flipped", "the rows were flipped vertically on request");

  if (auto valid = asset::validate(image); !valid)
    return std::unexpected(std::move(valid.error()));
  return result;
}

} // namespace

auto decodeImage(const asset::DecodeInput &input, const ImageDecodeOptions &options)
    -> terreate::Result<ImageDecodeResult> {
  try {
    return decodeImageImpl(input, options);
  } catch (const std::bad_alloc &) {
    // A code-only Error does not allocate.
    return std::unexpected(terreate::Error{make_error_code(ImageDecodeError::out_of_memory)});
  }
}

} // namespace terreate::adapter::asset_image_stb

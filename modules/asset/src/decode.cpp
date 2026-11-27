#include <terreate/asset/decode.hpp>

#include <new>
#include <string>
#include <utility>

namespace terreate::asset {
namespace {

class DecodeErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.asset.decode"; }
  std::string message(int value) const override {
    switch (static_cast<DecodeError>(value)) {
    case DecodeError::input_too_large:
      return "the encoded input is larger than the input limit";
    case DecodeError::dimension_exceeded:
      return "an image dimension is 0 or above the dimension limit";
    case DecodeError::pixel_count_exceeded:
      return "the image pixel count is above the pixel limit";
    case DecodeError::count_exceeded:
      return "a count is above its limit";
    case DecodeError::hierarchy_too_deep:
      return "the node hierarchy is deeper than the hierarchy limit";
    case DecodeError::malformed_input:
      return "the input is not valid for the format";
    case DecodeError::unsupported_input:
      return "the input uses something the decoder cannot represent";
    }
    return "unknown decode error";
  }
};

// Error construction allocates; on exhaustion a code-only Error is returned,
// which does not allocate.
Error makeError(DecodeError error, const char *operation, const std::string &detail) {
  try {
    return Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return Error{make_error_code(error)};
  }
}

} // namespace

auto makeSourceIdentity(std::string name) -> SourceIdentity {
  SourceIdentity identity;
  const auto slash = name.rfind('/');
  if (slash != std::string::npos)
    identity.base = name.substr(0, slash + 1);
  identity.name = std::move(name);
  return identity;
}

auto defaultDecodeLimits() noexcept -> DecodeLimits { return DecodeLimits{}; }

auto decode_error_category() noexcept -> const std::error_category & {
  static const DecodeErrorCategory category;
  return category;
}

auto make_error_code(DecodeError error) noexcept -> std::error_code {
  return {static_cast<int>(error), decode_error_category()};
}

auto checkInputSize(ByteView bytes, const DecodeLimits &limits) -> terreate::Result<void> {
  if (bytes.size() > limits.max_input_bytes)
    return std::unexpected(makeError(DecodeError::input_too_large, "checkInputSize",
                                     std::to_string(bytes.size()) + " bytes, at most " +
                                         std::to_string(limits.max_input_bytes)));
  return {};
}

auto checkImageSize(std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                    const DecodeLimits &limits) -> terreate::Result<void> {
  for (const std::uint32_t dimension : {width, height, depth}) {
    if (dimension == 0 || dimension > limits.max_dimension)
      return std::unexpected(makeError(DecodeError::dimension_exceeded, "checkImageSize",
                                       std::to_string(width) + "x" + std::to_string(height) + "x" +
                                           std::to_string(depth) + ", dimensions in 1.." +
                                           std::to_string(limits.max_dimension)));
  }
  // Each factor is below 2^32, so width * height fits in 64 bits; the depth
  // multiplication is checked by division.
  const std::uint64_t plane = static_cast<std::uint64_t>(width) * height;
  if (plane > limits.max_pixels || depth > limits.max_pixels / plane)
    return std::unexpected(makeError(DecodeError::pixel_count_exceeded, "checkImageSize",
                                     std::to_string(width) + "x" + std::to_string(height) + "x" +
                                         std::to_string(depth) + " pixels, at most " +
                                         std::to_string(limits.max_pixels)));
  return {};
}

} // namespace terreate::asset

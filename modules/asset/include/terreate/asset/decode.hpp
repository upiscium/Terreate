#ifndef TERREATE_ASSET_DECODE_HPP
#define TERREATE_ASSET_DECODE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <system_error>

#include <terreate/core/result.hpp>

namespace terreate::asset {

/// The byte-source convention of every decoder: a decoder receives its input as
/// a borrowed, read-only span of bytes. It never opens a file, a socket or an
/// archive itself. The caller owns the bytes and keeps them alive for the
/// duration of the decode call; a decoder that needs the bytes afterwards copies
/// them. Bytes that come from an external reference reach the decoder through the
/// resolver (see resource_resolver.hpp) and then follow the same convention.
using ByteView = std::span<const std::byte>;

/// Where a piece of data came from, for provenance and for resolving relative
/// references. It is metadata only: it grants no access to anything.
struct SourceIdentity {
  /// A human-readable name or URI of the source, exactly as the application
  /// calls it (for example `models/ship.gltf` or `https://host/ship.gltf`).
  /// Never interpreted as an access path by Asset.
  std::string name{};
  /// The directory against which relative references of this source resolve: a
  /// root-less, `/`-separated normalized path such as `models/` or
  /// `models/ship`. Empty means the root of the application's resolver (so a
  /// relative reference `a.bin` asks for `a.bin`). It is never a scheme, a drive
  /// or an absolute path: an application whose resolver is bound to a URL or a
  /// directory keeps that root inside the resolver.
  std::string base{};

  /// Identities are equal when name and base are equal.
  [[nodiscard]] friend bool operator==(const SourceIdentity &, const SourceIdentity &) = default;
};

/// Builds the identity of a source called `name`: `base` is `name` up to and
/// including its last `/` (empty when `name` has no `/`). An application with a
/// different layout sets `base` itself.
[[nodiscard]] auto makeSourceIdentity(std::string name) -> SourceIdentity;

/// The input of a decode call: the bytes and the identity they came from.
struct DecodeInput {
  /// The borrowed bytes to decode.
  ByteView bytes{};
  /// The identity of the bytes (provenance and base for relative references).
  SourceIdentity source{};
};

/// The resource limits of one decode. Every decoder and the ResolveSession
/// enforce them and fail with a named error when one is exceeded; nothing is
/// truncated. The defaults (see defaultDecodeLimits) are conservative for
/// untrusted input: raising a limit is the application's explicit act, done by
/// assigning the field. A limit of 0 allows none of the thing; there is no
/// "unlimited" value other than the maximum of the type.
struct DecodeLimits {
  /// The largest encoded input (one image file, one glTF/GLB): 256 MiB.
  std::uint64_t max_input_bytes = 256ULL * 1024 * 1024;
  /// The largest pixel count (width * height * depth) of one decoded image:
  /// 67108864 (8192 x 8192).
  std::uint64_t max_pixels = 67'108'864ULL;
  /// The largest width, height or depth of one image: 16384.
  std::uint32_t max_dimension = 16'384;
  /// The largest number of nodes of one model: 65536.
  std::uint32_t max_nodes = 65'536;
  /// The largest number of meshes of one model: 16384.
  std::uint32_t max_meshes = 16'384;
  /// The largest number of primitives of one model: 65536.
  std::uint32_t max_primitives = 65'536;
  /// The deepest node hierarchy (a root is depth 1): 256.
  std::uint32_t max_hierarchy_depth = 256;
  /// The largest number of external resource requests of one decode (failed
  /// requests count): 256.
  std::uint32_t max_external_resources = 256;
  /// The largest size of one resolved resource: 256 MiB.
  std::uint64_t max_resource_bytes = 256ULL * 1024 * 1024;
  /// The largest total size of all resolved resources of one decode: 1 GiB.
  std::uint64_t max_total_resource_bytes = 1024ULL * 1024 * 1024;
  /// The largest nesting of resolves (a reference found inside a resolved
  /// resource is one level deeper); a request at depth `d` needs `d` below this:
  /// 4.
  std::uint32_t max_resolve_depth = 4;
};

/// The conservative default limits (a value-initialized DecodeLimits).
[[nodiscard]] auto defaultDecodeLimits() noexcept -> DecodeLimits;

/// The failures decoders report for the shared checks. Values start at 1; new
/// values are appended.
enum class DecodeError : std::uint8_t {
  /// The encoded input is larger than `DecodeLimits::max_input_bytes`.
  input_too_large = 1,
  /// An image dimension is 0 or above `DecodeLimits::max_dimension`.
  dimension_exceeded,
  /// An image pixel count is above `DecodeLimits::max_pixels`.
  pixel_count_exceeded,
  /// A count (nodes, meshes, primitives) is above its limit.
  count_exceeded,
  /// The node hierarchy is deeper than `DecodeLimits::max_hierarchy_depth`.
  hierarchy_too_deep,
  /// The input is not valid for the format.
  malformed_input,
  /// The input is valid but uses something the decoder cannot represent and
  /// cannot report as `unsupported`.
  unsupported_input,
};

/// The category of DecodeError codes (name "terreate.asset.decode").
[[nodiscard]] auto decode_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of a DecodeError.
[[nodiscard]] auto make_error_code(DecodeError error) noexcept -> std::error_code;

/// Checks the size of an encoded input against `limits.max_input_bytes`.
/// Fails with DecodeError::input_too_large. Call before touching the bytes.
[[nodiscard]] auto checkInputSize(ByteView bytes, const DecodeLimits &limits)
    -> terreate::Result<void>;

/// Checks an image size (before allocating or decoding it): every dimension
/// must be in `1..max_dimension` (DecodeError::dimension_exceeded) and
/// `width * height * depth` must not exceed `max_pixels` (pixel_count_exceeded;
/// computed without overflow).
[[nodiscard]] auto checkImageSize(std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                  const DecodeLimits &limits) -> terreate::Result<void>;

/// The severity of a DecodeDiagnostic.
enum class DecodeSeverity : std::uint8_t {
  /// Information; the decode is complete.
  info,
  /// Something was not represented or was repaired by a documented rule; the
  /// data was reported in `unsupported`, not dropped silently.
  warning,
  /// The decode failed or a part of it failed.
  error,
};

/// Where in a source a diagnostic applies.
struct DecodeLocation {
  /// The name of the SourceIdentity the location is in (empty when unknown).
  std::string source{};
  /// A format-specific path inside the source, for example `/nodes/3` or
  /// `buffers[0].uri` (empty when unknown).
  std::string path{};
  /// A byte offset into the source, when known.
  std::optional<std::uint64_t> byte_offset{};
};

/// A decoder's note about the decode. Owning, copyable.
struct DecodeDiagnostic {
  /// How serious the diagnostic is.
  DecodeSeverity severity = DecodeSeverity::info;
  /// A stable, machine-readable code chosen by the decoder (for example
  /// `gltf.unsupported.animation`).
  std::string code{};
  /// A human-readable message.
  std::string message{};
  /// Where the diagnostic applies.
  DecodeLocation location{};
};

} // namespace terreate::asset

namespace std {
/// Lets DecodeError convert to std::error_code.
template <> struct is_error_code_enum<terreate::asset::DecodeError> : true_type {};
} // namespace std

#endif // TERREATE_ASSET_DECODE_HPP

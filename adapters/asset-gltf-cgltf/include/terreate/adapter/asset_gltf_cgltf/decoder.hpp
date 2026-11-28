#ifndef TERREATE_ADAPTER_ASSET_GLTF_CGLTF_DECODER_HPP
#define TERREATE_ADAPTER_ASSET_GLTF_CGLTF_DECODER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include <terreate/asset/decode.hpp>
#include <terreate/asset/image_data.hpp>
#include <terreate/asset/model_data.hpp>
#include <terreate/asset/resource_resolver.hpp>
#include <terreate/core/result.hpp>

/// The cgltf Adapter (#278): decodes glTF 2.0 (`.gltf` JSON and `.glb` binary) from a byte source
/// into a CPU-side `asset::ModelData`. It implements the decode contracts of Asset
/// (`asset::DecodeInput`, `asset::DecodeLimits`, `asset::ResolveSession`,
/// `asset::DecodeDiagnostic`, `asset::DecodeProvenance`) with one backend, cgltf, which is a
/// private implementation detail: no public header includes it and a consumer needs no cgltf.
/// The Adapter depends on Asset (and Core) only; it does not depend on the stb_image Adapter.
///
/// The Adapter reads only the bytes it is given. External buffers and images are requested
/// through the application's `asset::ResourceResolverView` (never through a file or a socket of
/// the Adapter), glTF images are decoded to `asset::ImageData` by an application-provided
/// callable (`ImageDecoderView`; the stb_image Adapter's `decodeImage` can be wrapped as that
/// callable), no GPU resource is created, no thread is started and no global state is kept.
///
/// Supported: scenes, nodes (TRS or matrix) and the hierarchy, meshes and primitives
/// (POSITION, NORMAL, TANGENT, TEXCOORD_n, COLOR_n, indices, topologies point, line, triangle and
/// triangle strip), materials (core metallic-roughness, alpha mode and cutoff, double sided,
/// normal, occlusion and emissive textures with texcoord sets), textures, samplers and images.
/// Everything else the file contains (animations, skins, morph targets, cameras, lights,
/// extensions, extras, other attributes and topologies) is listed in
/// `asset::ModelData::unsupported` and in the diagnostics, never dropped silently, and a file
/// that *requires* an extension is refused.
namespace terreate::adapter::asset_gltf_cgltf {

/// The container the input was in.
enum class GltfContainer : std::uint8_t {
  /// A glTF JSON document (`.gltf`).
  gltf,
  /// A GLB binary container (`.glb`): a JSON chunk and an optional BIN chunk.
  glb,
};

/// The lower-case name of a container: `"gltf"` or `"glb"`.
[[nodiscard]] auto containerName(GltfContainer container) noexcept -> std::string_view;

/// What the decoder asks of the application's image decoder for one glTF image. The request is
/// valid for the duration of the call only; the callable copies what it needs afterwards.
struct ImageDecodeRequest {
  /// The encoded image bytes (borrowed for the call) and their identity: the identity the
  /// resolver returned for an external image, or `<source name>#images[i]` (with the source's
  /// base) for an image embedded as a data URI or stored in a GLB buffer view.
  asset::DecodeInput input{};
  /// The media type the glTF declares (`image/png`, `image/jpeg`), possibly empty. Untrusted.
  std::string mime_type{};
  /// The color interpretation the model needs: `srgb` for an image used as a base color or
  /// emissive texture, `normal_map` for a normal texture, `linear` otherwise (see the mapping
  /// rules). The decoder checks nothing about it; the callable should honor it, because the
  /// result is used as it is.
  asset::ColorInterpretation interpretation = asset::ColorInterpretation::srgb;
  /// The decode limits in force (the same as `GltfDecodeOptions::limits`); the callable should
  /// pass them to its decoder. The Adapter checks the extent of the result against them again.
  asset::DecodeLimits limits{};
};

/// A small, non-owning, synchronous view of an application-provided image decoder.
///
/// The bound target (an object with `operator()` or any callable object) is borrowed: it must
/// outlive this view and every call. Binding allocates nothing and takes an object lvalue (not
/// a temporary, not a const object), like asset::ResourceResolverView. The target is called as
/// `Result<ImageData>(const ImageDecodeRequest &)`; its errors are returned by the glTF decoder
/// unchanged, and an exception is reported as GltfDecodeError::image_decoder_failed.
class ImageDecoderView {
private:
  using Callback = terreate::Result<asset::ImageData> (*)(const ImageDecodeRequest &, void *);

  constexpr ImageDecoderView(Callback callback, void *state) noexcept
      : callback_(callback), state_(state) {}

  template <typename Target>
  static auto invoke(const ImageDecodeRequest &request, void *state)
      -> terreate::Result<asset::ImageData> {
    return std::invoke(*static_cast<Target *>(state), request);
  }

  Callback callback_ = nullptr;
  void *state_ = nullptr;

public:
  /// An unbound view: a glTF with an image to decode fails with
  /// GltfDecodeError::missing_image_decoder.
  constexpr ImageDecoderView() noexcept = default;

  /// Binds a borrowed callable object without allocating or taking ownership.
  template <typename Target>
    requires std::is_object_v<Target> && std::is_same_v<Target, std::remove_cv_t<Target>> &&
             std::is_invocable_r_v<terreate::Result<asset::ImageData>, Target &,
                                   const ImageDecodeRequest &>
  [[nodiscard]] static constexpr ImageDecoderView bind(Target &target) noexcept {
    return ImageDecoderView{&invoke<Target>, static_cast<void *>(std::addressof(target))};
  }

  /// Whether a decoder is bound.
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return callback_ != nullptr; }

  /// Calls the bound decoder. Must only be called when bound (check `operator bool`); the
  /// result is returned as the callable produced it.
  [[nodiscard]] auto decode(const ImageDecodeRequest &request) const
      -> terreate::Result<asset::ImageData> {
    return callback_(request, state_);
  }
};

/// The options of one decode. Plain data with documented defaults.
struct GltfDecodeOptions {
  /// The resource limits: input size, node, mesh and primitive counts, hierarchy depth, the
  /// external resource count and sizes (enforced by the `asset::ResolveSession`), and the size of
  /// each decoded image (through the image decoder's result). Default:
  /// `asset::defaultDecodeLimits()`.
  asset::DecodeLimits limits{};
  /// How references (URIs) found in the file are validated before the resolver sees them.
  /// Default: only relative paths pass. A URI scheme (`https`) is allowed only by adding it
  /// here, and the resolver is then responsible for it. A `data:` URI never reaches the
  /// resolver (see `embedded_data_uris`).
  asset::ReferenceOptions references{};
  /// The most bytes of decoded geometry (vertex streams and indices of all primitives) and of
  /// decoded images that one decode may produce: 1 GiB. Checked before each allocation (an
  /// accessor that has no buffer data, or a sparse accessor, can describe far more elements than
  /// the file holds). Exceeding it fails with GltfDecodeError::decoded_size_exceeded.
  std::uint64_t max_decoded_bytes = 1024ULL * 1024 * 1024;
  /// Whether `data:` URIs of buffers and images are decoded by the Adapter (`true`, the
  /// default). Only `data:[<media type>];base64,<payload>` is understood. The decoded size is
  /// checked against `limits.max_resource_bytes` and the total against
  /// `limits.max_total_resource_bytes` (together with resolved resources) before the payload is
  /// decoded. When `false`, a data URI is an ordinary reference and is rejected by the
  /// reference check unless `references.allowed_schemes` lists `data`, in which case the
  /// resolver is asked for it.
  bool embedded_data_uris = true;
};

/// What the decoder found in the file's header, kept as plain data (the escape hatch). The same
/// information is in `ModelData::custom` (keys `gltf.container`, `gltf.version`,
/// `gltf.min_version`, `gltf.generator`, `gltf.copyright`; empty values are left out).
struct GltfSourceInfo {
  /// The container the input was in.
  GltfContainer container = GltfContainer::gltf;
  /// `asset.version` (always `2.x`).
  std::string version{};
  /// `asset.minVersion`, when present.
  std::string min_version{};
  /// `asset.generator`, when present.
  std::string generator{};
  /// `asset.copyright`, when present.
  std::string copyright{};
  /// The names listed in `extensionsUsed`, in file order. All of them are reported as
  /// unsupported (the Adapter implements none).
  std::vector<std::string> extensions_used{};
};

/// The result of a successful decode: a valid model and everything the Adapter noted.
struct GltfDecodeResult {
  /// The decoded model. `asset::validate` succeeded on it. Features of the file that could not
  /// be represented are listed in `ModelData::unsupported`.
  asset::ModelData model{};
  /// Notes about the decode, in the order they were made: every decision the Adapter took
  /// (conversion of normalized data, image interpretation, widened indices, data URIs) as
  /// `info`, and every feature that was not applied as `warning` (each unsupported feature has
  /// both an entry in `ModelData::unsupported` and a warning here). At most 64 items of each
  /// code are listed individually; one further item with the same code reports how many were
  /// left out, so the totals are exact and the memory is bounded (the same holds for the
  /// entries of `ModelData::unsupported`, per kind, with the name `(further)`).
  std::vector<asset::DecodeDiagnostic> diagnostics{};
  /// The identity of the decoded bytes and every external resource request, in order, as
  /// recorded by the `asset::ResolveSession`. A failed request ends the decode, so a result lists
  /// successful requests only. Data URIs are not requests and are noted in `diagnostics`.
  asset::DecodeProvenance provenance{};
  /// What the decoder found in the header of the file.
  GltfSourceInfo native{};
};

/// The failures specific to this Adapter. Limit failures are not here: they are the shared
/// `asset::DecodeError` values (`input_too_large`, `count_exceeded`, `hierarchy_too_deep`,
/// `dimension_exceeded`, `pixel_count_exceeded`), the resolver's failures are
/// `asset::ResolveError` values, an invalid image from the image decoder is an
/// `asset::ImageDataError`, and a model that does not validate is reported with the
/// `asset::ModelDataError` or `asset::MeshDataError` of `asset::validate`; all of them are
/// returned unchanged. Values start at 1; new values are appended. The `Error::detail` of a
/// failure names the offending object (for example `accessors[3]`) and the reason.
enum class GltfDecodeError : std::uint8_t {
  /// The input is empty, truncated, not valid JSON or GLB, nests JSON deeper than 128 levels, or
  /// is not valid glTF (a bad index, a node with both a matrix and TRS values, a mesh without
  /// primitives, a malformed data URI reference, ...).
  malformed_gltf = 1,
  /// `asset.version` is missing or is not 2.x, or `asset.minVersion` is above 2.0, or the GLB
  /// container version is not 2.
  unsupported_version,
  /// The file lists an extension in `extensionsRequired`; the Adapter implements none, so it
  /// refuses instead of decoding something other than the author meant. The detail names it.
  unsupported_required_extension,
  /// An external buffer or image is referenced and no resolver is bound. The detail names the
  /// reference.
  missing_resolver,
  /// An image has to be decoded and no image decoder is bound.
  missing_image_decoder,
  /// The image decoder threw an exception (its own errors are returned unchanged).
  image_decoder_failed,
  /// A buffer has no data (no URI and no GLB chunk), has less data than it declares, or a data
  /// URI is not valid base64.
  invalid_buffer,
  /// An accessor, buffer view or sparse accessor is not valid or does not fit its buffer, an
  /// attribute has a type, component type or count the semantic does not allow, or an index is
  /// outside the vertex range.
  invalid_accessor,
  /// An image has neither a URI nor a buffer view, or a texture has no image source, or an
  /// enumerator (sampler filter or wrap) is not a glTF value.
  invalid_resource,
  /// The decoded geometry and images are larger than `GltfDecodeOptions::max_decoded_bytes`.
  decoded_size_exceeded,
  /// An allocation failed, or the parser's memory budget (16 MiB plus 64 bytes per input byte)
  /// was exceeded.
  out_of_memory,
};

/// The category of GltfDecodeError codes (name "terreate.adapter.asset_gltf_cgltf").
[[nodiscard]] auto gltf_decode_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of a GltfDecodeError.
[[nodiscard]] auto make_error_code(GltfDecodeError error) noexcept -> std::error_code;

/// Decodes the bytes of one glTF or GLB file into an `asset::ModelData`.
///
/// `input.source` is the identity of the bytes: its `base` is the directory against which the
/// relative URIs of external buffers and images resolve (see `asset::makeSourceIdentity`).
/// `resolver` may be unbound when the file has no external resource; `image_decoder` may be
/// unbound when the file has no image. Both are borrowed for the call.
///
/// The order is fixed and every step that can reject does so before the next allocates: the
/// input size (`asset::checkInputSize`), the container (a GLB header and its chunks are checked
/// against the input length, and the JSON nesting against 128 levels, before the parser sees
/// the text), the parse (its allocations are capped per call), the version, the required
/// extensions, the node, mesh and primitive counts and the hierarchy (depth, cycles) against
/// the limits, and then, in this order, samplers, materials, meshes, nodes and scenes (a buffer
/// is loaded the first time a primitive needs it: GLB chunk, `data:` URI or the resolver),
/// textures and images (decoded through `image_decoder`). Buffers and images that nothing
/// supported refers to (an animation's, a skin's) are not loaded. A truncated, corrupt or
/// hostile input fails with a named error; it never reads outside `input.bytes` or a buffer
/// and never allocates more than the limits and `max_decoded_bytes` allow.
///
/// Nothing is repaired and nothing is guessed: missing normals or tangents are not generated,
/// units and handedness are not converted. See the architecture page for the mapping rules.
///
/// Fails with `GltfDecodeError`, `asset::DecodeError`, `asset::ResolveError`,
/// `asset::ImageDataError`, `asset::MeshDataError` or `asset::ModelDataError`. The function is
/// thread-safe: it uses no global state; `input.bytes` must stay alive and unchanged during the
/// call (the result owns everything and does not borrow it).
[[nodiscard]] auto
decodeGltf(const asset::DecodeInput &input, asset::ResourceResolverView resolver = {},
           ImageDecoderView image_decoder = {}, const GltfDecodeOptions &options = {})
    -> terreate::Result<GltfDecodeResult>;

} // namespace terreate::adapter::asset_gltf_cgltf

namespace std {
/// Lets GltfDecodeError convert to std::error_code.
template <>
struct is_error_code_enum<terreate::adapter::asset_gltf_cgltf::GltfDecodeError> : true_type {};
} // namespace std

#endif // TERREATE_ADAPTER_ASSET_GLTF_CGLTF_DECODER_HPP

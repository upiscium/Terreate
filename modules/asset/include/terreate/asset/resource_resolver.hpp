#ifndef TERREATE_ASSET_RESOURCE_RESOLVER_HPP
#define TERREATE_ASSET_RESOURCE_RESOLVER_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <terreate/asset/decode.hpp>
#include <terreate/core/result.hpp>

namespace terreate::asset {

/// What a reference names. A hint for the resolver and for provenance.
enum class ResourceKind : std::uint8_t {
  /// Binary geometry or animation data (a glTF buffer).
  buffer,
  /// An encoded image (a glTF image).
  image,
  /// Anything else.
  other,
};

/// Why a reference was rejected. Values start at 1; new values are appended.
enum class ReferenceError : std::uint8_t {
  /// The reference is empty, or normalizes to nothing.
  empty = 1,
  /// The reference is longer than `ReferenceOptions::max_length`.
  too_long,
  /// The reference contains a NUL byte (raw or as `%00`).
  contains_nul,
  /// The reference contains another control character (below 0x20 or 0x7F),
  /// raw or percent-encoded.
  control_character,
  /// The reference starts with `/` (an absolute path or a `//host` form).
  absolute_path,
  /// The reference contains `\`, raw or as `%5C`.
  backslash,
  /// The reference starts with a drive letter (`C:`).
  drive_letter,
  /// The reference has a URI scheme that `ReferenceOptions::allowed_schemes`
  /// does not list.
  scheme_not_allowed,
  /// A `..` segment would leave the root (the base directory plus the
  /// reference), or a URI reference contains a `..` segment at all.
  escapes_root,
  /// A `%` is not followed by two hexadecimal digits.
  invalid_percent_encoding,
  /// A percent-encoded `/` (`%2F`), which would hide a path separator.
  encoded_separator,
  /// A reserved character in a path: an unencoded `?` or `#`, or a `:` (raw
  /// or as `%3A`) anywhere except after a scheme.
  reserved_character,
  /// The reference names a directory (it ends with `/`, `.` or `..`).
  names_directory,
  /// The base directory of a SourceIdentity is not a valid base (it starts with
  /// `/`, has a `..` that escapes, or contains a control character, `\` or `:`).
  invalid_base,
};

/// The category of ReferenceError codes (name "terreate.asset.reference").
[[nodiscard]] auto reference_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of a ReferenceError.
[[nodiscard]] auto make_error_code(ReferenceError error) noexcept -> std::error_code;

/// Options of normalizeReference.
struct ReferenceOptions {
  /// The URI schemes (lowercase, without the colon) a reference may use. Empty
  /// (the default) allows none: only relative paths pass. Adding `https` or
  /// `data` is the application's explicit act.
  std::vector<std::string> allowed_schemes{};
  /// The longest reference accepted, in bytes: 2048.
  std::size_t max_length = 2048;
};

/// Validates and normalizes a reference string found in an untrusted asset.
///
/// This is a purely lexical check that touches no file system and no network.
///
/// Relative references (no scheme): the reference is split at `/`, each segment
/// is percent-decoded exactly once, then empty and `.` segments are dropped and
/// `..` pops the previous segment. The starting point is `base_directory` (a
/// root-less normalized path, `/`-separated, possibly empty), so
/// `../textures/a.png` with base `models/` gives `textures/a.png`. A `..` that
/// would go above the root is rejected (ReferenceError::escapes_root), as are:
/// empty references, NUL and other control characters (raw or decoded), a
/// leading `/`, `\` (raw or decoded), a drive letter, `?`, `#` and `:`
/// in a path, a decoded `/`, a malformed percent escape, and a reference that
/// ends in a directory. Non-ASCII bytes are not validated or altered. Because
/// decoding happens once, per segment and before the `.`/`..` handling,
/// `%2e%2e` is a `..` and `%252e` is the literal name `%2e`.
///
/// URI references (`scheme:`): the scheme must be in
/// `options.allowed_schemes` (compared case-insensitively) and is returned in
/// lowercase; the remainder is kept as written, not decoded and not normalized,
/// but must contain no control character, no `\` and no `..` segment (a segment
/// whose percent-decoded value is `..`). The application's resolver is then
/// responsible for the URI. A one-letter scheme is a drive letter and always
/// rejected.
///
/// The result is the normalized reference: for relative references a
/// root-less `/`-separated path without `.`/`..`, never empty, never starting
/// with `/`. It is what a ResolveSession passes to the resolver as
/// `ResourceRequest::normalized`. Fails with a ReferenceError.
///
/// A URI reference is not percent-decoded for the result: a resolver that decodes
/// it again sees `%252e` as `%2e`, and one that decodes twice would see `..`; a
/// URI resolver must decode once. A `%2F` in a URI remainder is also rejected, and
/// the length limit applies to URIs too (raise `max_length` to allow a long `data:`
/// URI, or let the decoder read data URIs itself).
///
/// This does not make a file system lookup safe by itself: symbolic links,
/// case-insensitive or Unicode-normalizing file systems and device names of an
/// operating system are the resolver's concern.
[[nodiscard]] auto normalizeReference(std::string_view reference,
                                      std::string_view base_directory = {},
                                      const ReferenceOptions &options = {})
    -> terreate::Result<std::string>;

/// A request to resolve one reference of an asset into bytes.
struct ResourceRequest {
  /// The reference exactly as written in the asset (not decoded, not normalized).
  std::string reference{};
  /// The normalized reference. Filled by ResolveSession before the resolver
  /// runs; for relative references it is the full root-less path (base
  /// directory applied). A resolver should use this, not `reference`.
  std::string normalized{};
  /// The identity of the source that contains the reference (provides the base
  /// directory and provenance).
  SourceIdentity base{};
  /// What the reference names.
  ResourceKind kind = ResourceKind::other;
  /// The size the asset declares for the resource (a glTF `byteLength`), if any.
  /// Untrusted: used only to refuse early, never to size an allocation.
  std::optional<std::uint64_t> declared_size{};
  /// The most bytes the resolver may return: the smaller of the per-resource limit
  /// and the remaining total, filled by ResolveSession before the resolver runs
  /// (the maximum of the type when the request is built by hand). A resolver that
  /// reads a file or a network stream should stop and fail with
  /// ResolveError::limit_exceeded rather than read more; ResolveSession checks the
  /// returned size again but only after the resolver allocated it.
  std::uint64_t max_bytes = 0xFFFFFFFFFFFFFFFFULL;
  /// The media type the asset declares (for example `image/png`), if any.
  /// Untrusted hint.
  std::string media_type{};
};

/// The bytes of a resolved resource and the identity they were resolved to.
/// Owns its bytes (directly or shared); copying shares the storage.
class ResolvedResource {
public:
  /// An empty resource with an empty identity.
  ResolvedResource() = default;
  /// A resource that owns `bytes`.
  ResolvedResource(SourceIdentity identity, std::vector<std::byte> bytes);
  /// A resource that shares `bytes` (for example one stored in a
  /// MemoryResolver). A null pointer is an empty resource.
  ResolvedResource(SourceIdentity identity, std::shared_ptr<const std::vector<std::byte>> bytes);

  /// What the request was resolved to (for example the file path or URL that was
  /// read); `base` is the base for references found inside this resource.
  [[nodiscard]] auto identity() const noexcept -> const SourceIdentity & { return identity_; }
  /// The bytes, valid as long as this object or a copy exists.
  [[nodiscard]] auto bytes() const noexcept -> ByteView;
  /// The number of bytes.
  [[nodiscard]] auto size() const noexcept -> std::size_t { return bytes().size(); }
  /// The shared storage (may be null for an empty resource).
  [[nodiscard]] auto storage() const noexcept
      -> const std::shared_ptr<const std::vector<std::byte>> & {
    return data_;
  }

private:
  SourceIdentity identity_{};
  std::shared_ptr<const std::vector<std::byte>> data_{};
};

/// The failures of resolving. Values start at 1; new values are appended.
enum class ResolveError : std::uint8_t {
  /// A decoder needs an external resource and no resolver is bound.
  no_resolver = 1,
  /// The reference was rejected by normalizeReference (the detail names the
  /// ReferenceError).
  reference_rejected,
  /// The resolver has no resource for the request.
  not_found,
  /// A limit was exceeded: the request count, a resource size (declared or
  /// actual) or the total size.
  limit_exceeded,
  /// The resolver failed for a reason that is not one of these codes. The
  /// resolver's own error is wrapped: its category and message are in the
  /// detail.
  resolver_failed,
  /// The request is nested deeper than `DecodeLimits::max_resolve_depth`.
  depth_exceeded,
};

/// The category of ResolveError codes (name "terreate.asset.resolve").
[[nodiscard]] auto resolve_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of a ResolveError.
[[nodiscard]] auto make_error_code(ResolveError error) noexcept -> std::error_code;

/// A small, non-owning, synchronous view of an application-provided resolver.
///
/// The bound target (an object with `operator()` or any callable object) is
/// borrowed: it must outlive this view and every call. Binding allocates nothing
/// and takes an object lvalue (not a temporary, not a const object), like
/// DiagnosticSinkView. The target is called as
/// `Result<ResolvedResource>(const ResourceRequest &)` and may throw; a
/// ResolveSession turns an exception into ResolveError::resolver_failed.
///
/// Synchronous only: the call returns the bytes or an error. An application
/// that gets bytes asynchronously (network, streaming archive) resolves ahead of
/// time and gives the decoder a MemoryResolver holding the finished bytes. A
/// resolver may be called again for nested requests; it must be reentrant only
/// if the application's decoder nests resolves (a ResolveSession never calls it
/// concurrently).
class ResourceResolverView {
private:
  using Callback = terreate::Result<ResolvedResource> (*)(const ResourceRequest &, void *);

  constexpr ResourceResolverView(Callback callback, void *state) noexcept
      : callback_(callback), state_(state) {}

  template <typename Target>
  static auto invoke(const ResourceRequest &request, void *state)
      -> terreate::Result<ResolvedResource> {
    return std::invoke(*static_cast<Target *>(state), request);
  }

  Callback callback_ = nullptr;
  void *state_ = nullptr;

public:
  /// An unbound view: resolving through it fails with ResolveError::no_resolver.
  constexpr ResourceResolverView() noexcept = default;

  /// Binds a borrowed callable object without allocating or taking ownership.
  template <typename Target>
    requires std::is_object_v<Target> && std::is_same_v<Target, std::remove_cv_t<Target>> &&
             std::is_invocable_r_v<terreate::Result<ResolvedResource>, Target &,
                                   const ResourceRequest &>
  [[nodiscard]] static constexpr ResourceResolverView bind(Target &target) noexcept {
    return ResourceResolverView{&invoke<Target>, static_cast<void *>(std::addressof(target))};
  }

  /// Whether a resolver is bound.
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return callback_ != nullptr; }

  /// Calls the bound resolver (ResolveError::no_resolver when unbound). The
  /// result is returned as the resolver produced it; see ResolveSession for the
  /// limit checks and error wrapping.
  [[nodiscard]] auto resolve(const ResourceRequest &request) const
      -> terreate::Result<ResolvedResource>;
};

/// A reference resolver over bytes the application already holds: a map from a
/// normalized reference to bytes. It touches no file system or network, so it is
/// the resolver for tests, for resources loaded ahead of time (including the
/// result of an asynchronous fetch) and for archive-like use (the application
/// adds the entries of an archive it has read). Bind it with
/// ResourceResolverView::bind.
class MemoryResolver {
public:
  /// A resolver whose keys are normalized with `options`.
  explicit MemoryResolver(ReferenceOptions options = {}) : options_(std::move(options)) {}

  /// Adds (or replaces) the resource for `reference`, which is normalized with
  /// the options (no base directory) and used as the key. Fails with
  /// ResolveError::reference_rejected when it is not a valid reference.
  [[nodiscard]] auto add(std::string_view reference, std::vector<std::byte> bytes)
      -> terreate::Result<void>;
  /// Like add(), sharing `bytes` (no copy).
  [[nodiscard]] auto add(std::string_view reference,
                         std::shared_ptr<const std::vector<std::byte>> bytes)
      -> terreate::Result<void>;

  /// Whether a resource is stored under the normalized key.
  [[nodiscard]] auto contains(std::string_view normalized) const -> bool;
  /// The number of stored resources.
  [[nodiscard]] auto size() const noexcept -> std::size_t { return entries_.size(); }

  /// Resolves a request: looks up `request.normalized` (when empty, the
  /// reference is normalized against `request.base.base` first). Fails with
  /// ResolveError::not_found or reference_rejected. The identity of the result
  /// has `name` and `base` derived from the key (makeSourceIdentity).
  [[nodiscard]] auto operator()(const ResourceRequest &request) const
      -> terreate::Result<ResolvedResource>;

private:
  ReferenceOptions options_;
  std::map<std::string, std::shared_ptr<const std::vector<std::byte>>, std::less<>> entries_;
};

/// One request made through a ResolveSession and what became of it.
struct ResolveRecord {
  /// The request as the resolver saw it (with `normalized` filled when the
  /// reference was valid).
  ResourceRequest request{};
  /// The nesting depth of the request.
  std::uint32_t depth = 0;
  /// What the request resolved to; empty when the request failed.
  std::optional<SourceIdentity> resolved{};
  /// The size of the resolved bytes; 0 when the request failed.
  std::uint64_t size = 0;
  /// The outcome: a default (empty) code on success, else the failure (a
  /// ResolveError code, or whatever code the resolver itself reported).
  std::error_code outcome{};
};

/// What a decode used: the source and every external resource request, in
/// order. Filled by ResolveSession; observable by the application afterwards.
struct DecodeProvenance {
  /// The identity of the primary input.
  SourceIdentity source{};
  /// Every request made, successful or not.
  std::vector<ResolveRecord> resources{};
};

/// The bounded resolve context of one decode. A decoder resolves every external
/// reference through it; it enforces the DecodeLimits centrally and records
/// provenance, so no adapter re-implements those checks.
///
/// Each resolve() call, in this order: fails with limit_exceeded when
/// `max_external_resources` requests were already made, then with depth_exceeded
/// when `depth` is not below `max_resolve_depth` (every request counts,
/// failed ones too; the session does not de-duplicate, so a decoder that
/// refers to a resource several times resolves it once and keeps the result);
/// fails with no_resolver when no resolver is bound; normalizes the reference
/// against `request.base.base` (reference_rejected; the detail names the
/// ReferenceError); refuses with limit_exceeded a `declared_size` above the
/// per-resource limit or the remaining total, before the resolver runs; calls
/// the resolver; then checks the actual size against the per-resource limit and
/// the remaining total (overflow-safe). Every call leaves a ResolveRecord.
///
/// Errors of the resolver: a failure whose code is already a ResolveError code
/// is returned as it is; any other failure (or an exception) is wrapped as
/// ResolveError::resolver_failed with the original category, value and message
/// in the detail. The resolver is never called concurrently by the session; the
/// session itself is not thread-safe.
class ResolveSession {
public:
  /// A session for the decode of `source`, with a borrowed resolver (may be
  /// unbound), the limits and the reference options (copied).
  ResolveSession(ResourceResolverView resolver, const DecodeLimits &limits, SourceIdentity source,
                 ReferenceOptions options = {});

  /// A request for `reference` found in the primary source (its `base` is the
  /// session's source identity). For a reference inside a resolved resource set
  /// `base` to that resource's identity() and pass depth + 1.
  [[nodiscard]] auto makeRequest(std::string reference, ResourceKind kind) const -> ResourceRequest;

  /// Resolves `request` at nesting `depth` (0 for a reference of the primary
  /// source). See the class description for the checks and errors.
  [[nodiscard]] auto resolve(ResourceRequest request, std::uint32_t depth = 0)
      -> terreate::Result<ResolvedResource>;

  /// The provenance recorded so far.
  [[nodiscard]] auto provenance() const noexcept -> const DecodeProvenance & { return provenance_; }
  /// The number of requests made (including failed ones).
  [[nodiscard]] auto requestCount() const noexcept -> std::uint32_t { return requests_; }
  /// The total size of the successfully resolved resources.
  [[nodiscard]] auto resolvedBytes() const noexcept -> std::uint64_t { return total_bytes_; }

private:
  ResourceResolverView resolver_;
  DecodeLimits limits_;
  ReferenceOptions options_;
  DecodeProvenance provenance_{};
  std::uint32_t requests_ = 0;
  std::uint64_t total_bytes_ = 0;
};

} // namespace terreate::asset

namespace std {
/// Lets ReferenceError convert to std::error_code.
template <> struct is_error_code_enum<terreate::asset::ReferenceError> : true_type {};
/// Lets ResolveError convert to std::error_code.
template <> struct is_error_code_enum<terreate::asset::ResolveError> : true_type {};
} // namespace std

#endif // TERREATE_ASSET_RESOURCE_RESOLVER_HPP

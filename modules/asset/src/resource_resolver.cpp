#include <terreate/asset/resource_resolver.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace terreate::asset {
namespace {

class ReferenceErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.asset.reference"; }
  std::string message(int value) const override {
    switch (static_cast<ReferenceError>(value)) {
    case ReferenceError::empty:
      return "the reference is empty";
    case ReferenceError::too_long:
      return "the reference is longer than the length limit";
    case ReferenceError::contains_nul:
      return "the reference contains a NUL byte";
    case ReferenceError::control_character:
      return "the reference contains a control character";
    case ReferenceError::absolute_path:
      return "the reference is an absolute path";
    case ReferenceError::backslash:
      return "the reference contains a backslash";
    case ReferenceError::drive_letter:
      return "the reference starts with a drive letter";
    case ReferenceError::scheme_not_allowed:
      return "the reference has a URI scheme that is not allowed";
    case ReferenceError::escapes_root:
      return "the reference leaves the root with '..'";
    case ReferenceError::invalid_percent_encoding:
      return "the reference has a malformed percent escape";
    case ReferenceError::encoded_separator:
      return "the reference has a percent-encoded path separator";
    case ReferenceError::reserved_character:
      return "the reference has a reserved character in its path";
    case ReferenceError::names_directory:
      return "the reference names a directory";
    case ReferenceError::invalid_base:
      return "the base directory is not a normalized root-less path";
    }
    return "unknown reference error";
  }
};

class ResolveErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.asset.resolve"; }
  std::string message(int value) const override {
    switch (static_cast<ResolveError>(value)) {
    case ResolveError::no_resolver:
      return "an external resource is needed and no resolver is bound";
    case ResolveError::reference_rejected:
      return "the reference was rejected";
    case ResolveError::not_found:
      return "the resolver has no resource for the request";
    case ResolveError::limit_exceeded:
      return "a resolve limit was exceeded";
    case ResolveError::resolver_failed:
      return "the resolver failed";
    case ResolveError::depth_exceeded:
      return "the request is nested deeper than the resolve depth limit";
    }
    return "unknown resolve error";
  }
};

// Error construction allocates; on exhaustion a code-only Error is returned,
// which does not allocate.
template <typename Code>
Error makeError(Code error, const char *operation, const std::string &detail) {
  try {
    return Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return Error{make_error_code(error)};
  }
}

bool isAlpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

int hexValue(char c) noexcept {
  if (isDigit(c))
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

char lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// A raw control character (NUL, below 0x20, or DEL) as a ReferenceError.
std::optional<ReferenceError> controlError(unsigned char c) noexcept {
  if (c == 0)
    return ReferenceError::contains_nul;
  if (c < 0x20 || c == 0x7F)
    return ReferenceError::control_character;
  return std::nullopt;
}

// Percent-decodes one segment once. A decoded '/' and '\\' are rejected so that
// an escape cannot hide a separator; so are decoded control characters.
// `colon_reserved` rejects ':' (raw or decoded) in path segments.
terreate::Result<std::string> decodeSegment(std::string_view segment, bool colon_reserved) {
  std::string out;
  out.reserve(segment.size());
  for (std::size_t i = 0; i < segment.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(segment[i]);
    if (c == '%') {
      const int hi = i + 1 < segment.size() ? hexValue(segment[i + 1]) : -1;
      const int lo = i + 2 < segment.size() ? hexValue(segment[i + 2]) : -1;
      if (hi < 0 || lo < 0)
        return std::unexpected(makeError(ReferenceError::invalid_percent_encoding,
                                         "normalizeReference", std::string(segment)));
      c = static_cast<unsigned char>(hi * 16 + lo);
      i += 2;
      if (c == '/')
        return std::unexpected(makeError(ReferenceError::encoded_separator, "normalizeReference",
                                         std::string(segment)));
      if (c == '\\')
        return std::unexpected(
            makeError(ReferenceError::backslash, "normalizeReference", std::string(segment)));
    } else if (c == '\\') {
      return std::unexpected(
          makeError(ReferenceError::backslash, "normalizeReference", std::string(segment)));
    }
    if (const auto control = controlError(c))
      return std::unexpected(makeError(*control, "normalizeReference", std::string(segment)));
    if (colon_reserved && c == ':')
      return std::unexpected(makeError(ReferenceError::reserved_character, "normalizeReference",
                                       "':' in " + std::string(segment)));
    out.push_back(static_cast<char>(c));
  }
  return out;
}

// Splits on '/', keeping empty segments.
std::vector<std::string_view> split(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (;;) {
    const auto slash = text.find('/', start);
    if (slash == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, slash - start));
    start = slash + 1;
  }
}

// The segments of a base directory: not decoded, no control characters, '\\' or
// ':', and '..' may not leave the root.
terreate::Result<std::vector<std::string>> baseSegments(std::string_view base) {
  std::vector<std::string> stack;
  if (!base.empty() && base.front() == '/')
    return std::unexpected(
        makeError(ReferenceError::invalid_base, "normalizeReference", std::string(base)));
  for (const auto part : split(base)) {
    if (part.empty() || part == ".")
      continue;
    if (part == "..") {
      if (stack.empty())
        return std::unexpected(
            makeError(ReferenceError::invalid_base, "normalizeReference", std::string(base)));
      stack.pop_back();
      continue;
    }
    for (const char c : part) {
      if (controlError(static_cast<unsigned char>(c)) || c == '\\' || c == ':')
        return std::unexpected(
            makeError(ReferenceError::invalid_base, "normalizeReference", std::string(base)));
    }
    stack.emplace_back(part);
  }
  return stack;
}

} // namespace

auto reference_error_category() noexcept -> const std::error_category & {
  static const ReferenceErrorCategory category;
  return category;
}

auto make_error_code(ReferenceError error) noexcept -> std::error_code {
  return {static_cast<int>(error), reference_error_category()};
}

auto resolve_error_category() noexcept -> const std::error_category & {
  static const ResolveErrorCategory category;
  return category;
}

auto make_error_code(ResolveError error) noexcept -> std::error_code {
  return {static_cast<int>(error), resolve_error_category()};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto normalizeReference(std::string_view reference, std::string_view base_directory,
                        const ReferenceOptions &options) -> terreate::Result<std::string> {
  constexpr const char *op = "normalizeReference";
  if (reference.empty())
    return std::unexpected(makeError(ReferenceError::empty, op, "empty reference"));
  if (reference.size() > options.max_length)
    return std::unexpected(makeError(ReferenceError::too_long, op,
                                     std::to_string(reference.size()) + " bytes, at most " +
                                         std::to_string(options.max_length)));
  for (const char c : reference) {
    if (const auto control = controlError(static_cast<unsigned char>(c)))
      return std::unexpected(makeError(*control, op, "in the raw reference"));
    if (c == '\\')
      return std::unexpected(makeError(ReferenceError::backslash, op, std::string(reference)));
  }
  if (reference.front() == '/')
    return std::unexpected(makeError(ReferenceError::absolute_path, op, std::string(reference)));

  // A colon before any '/', '?' or '#' ends a scheme (or a drive letter).
  const auto colon = reference.find(':');
  const auto delimiter = reference.find_first_of("/?#");
  if (colon != std::string_view::npos &&
      (delimiter == std::string_view::npos || colon < delimiter)) {
    const auto scheme = reference.substr(0, colon);
    if (scheme.size() == 1 && isAlpha(scheme.front()))
      return std::unexpected(makeError(ReferenceError::drive_letter, op, std::string(reference)));
    const bool grammar =
        !scheme.empty() && isAlpha(scheme.front()) && std::ranges::all_of(scheme, [](char c) {
          return isAlpha(c) || isDigit(c) || c == '+' || c == '-' || c == '.';
        });
    if (!grammar)
      return std::unexpected(
          makeError(ReferenceError::reserved_character, op, "':' in " + std::string(reference)));
    std::string lowered;
    for (const char c : scheme)
      lowered.push_back(lower(c));
    if (std::ranges::find(options.allowed_schemes, lowered) == options.allowed_schemes.end())
      return std::unexpected(makeError(ReferenceError::scheme_not_allowed, op, lowered));
    // The remainder is the resolver's; it must still not contain '..'.
    for (const auto part : split(reference.substr(colon + 1))) {
      auto decoded = decodeSegment(part, false);
      if (!decoded)
        return std::unexpected(decoded.error());
      if (*decoded == "..")
        return std::unexpected(makeError(ReferenceError::escapes_root, op, std::string(reference)));
    }
    return lowered + std::string(reference.substr(colon));
  }

  auto stack = baseSegments(base_directory);
  if (!stack)
    return std::unexpected(stack.error());
  const auto parts = split(reference);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto part = parts[i];
    if (part.find_first_of("?#") != std::string_view::npos)
      return std::unexpected(makeError(ReferenceError::reserved_character, op,
                                       "'?' or '#' in " + std::string(reference)));
    auto decoded = decodeSegment(part, true);
    if (!decoded)
      return std::unexpected(decoded.error());
    const bool last = i + 1 == parts.size();
    if (decoded->empty() || *decoded == "." || *decoded == "..") {
      if (last)
        return std::unexpected(
            makeError(ReferenceError::names_directory, op, std::string(reference)));
      if (*decoded == "..") {
        if (stack->empty())
          return std::unexpected(
              makeError(ReferenceError::escapes_root, op, std::string(reference)));
        stack->pop_back();
      }
      continue;
    }
    stack->push_back(std::move(*decoded));
  }
  if (stack->empty())
    return std::unexpected(makeError(ReferenceError::empty, op, std::string(reference)));
  std::string result;
  for (const auto &segment : *stack) {
    if (!result.empty())
      result.push_back('/');
    result += segment;
  }
  return result;
}

ResolvedResource::ResolvedResource(SourceIdentity identity, std::vector<std::byte> bytes)
    : identity_(std::move(identity)),
      data_(std::make_shared<const std::vector<std::byte>>(std::move(bytes))) {}

ResolvedResource::ResolvedResource(SourceIdentity identity,
                                   std::shared_ptr<const std::vector<std::byte>> bytes)
    : identity_(std::move(identity)), data_(std::move(bytes)) {}

auto ResolvedResource::bytes() const noexcept -> ByteView {
  return data_ ? ByteView{*data_} : ByteView{};
}

auto ResourceResolverView::resolve(const ResourceRequest &request) const
    -> terreate::Result<ResolvedResource> {
  if (callback_ == nullptr)
    return std::unexpected(
        makeError(ResolveError::no_resolver, "ResourceResolverView::resolve", request.reference));
  return callback_(request, state_);
}

auto MemoryResolver::add(std::string_view reference, std::vector<std::byte> bytes)
    -> terreate::Result<void> {
  return add(reference, std::make_shared<const std::vector<std::byte>>(std::move(bytes)));
}

auto MemoryResolver::add(std::string_view reference,
                         std::shared_ptr<const std::vector<std::byte>> bytes)
    -> terreate::Result<void> {
  auto key = normalizeReference(reference, {}, options_);
  if (!key)
    return std::unexpected(makeError(ResolveError::reference_rejected, "MemoryResolver::add",
                                     std::string(reference) + ": " + key.error().code().message()));
  if (!bytes)
    bytes = std::make_shared<const std::vector<std::byte>>();
  entries_.insert_or_assign(std::move(*key), std::move(bytes));
  return {};
}

auto MemoryResolver::contains(std::string_view normalized) const -> bool {
  return entries_.find(normalized) != entries_.end();
}

auto MemoryResolver::operator()(const ResourceRequest &request) const
    -> terreate::Result<ResolvedResource> {
  std::string key = request.normalized;
  if (key.empty()) {
    auto normalized = normalizeReference(request.reference, request.base.base, options_);
    if (!normalized)
      return std::unexpected(
          makeError(ResolveError::reference_rejected, "MemoryResolver::resolve",
                    request.reference + ": " + normalized.error().code().message()));
    key = std::move(*normalized);
  }
  const auto found = entries_.find(key);
  if (found == entries_.end())
    return std::unexpected(makeError(ResolveError::not_found, "MemoryResolver::resolve", key));
  return ResolvedResource{makeSourceIdentity(key), found->second};
}

ResolveSession::ResolveSession(ResourceResolverView resolver, const DecodeLimits &limits,
                               SourceIdentity source, ReferenceOptions options)
    : resolver_(resolver), limits_(limits), options_(std::move(options)) {
  provenance_.source = std::move(source);
}

auto ResolveSession::makeRequest(std::string reference, ResourceKind kind) const
    -> ResourceRequest {
  ResourceRequest request;
  request.reference = std::move(reference);
  request.base = provenance_.source;
  request.kind = kind;
  return request;
}

auto ResolveSession::resolve(ResourceRequest request, std::uint32_t depth)
    -> terreate::Result<ResolvedResource> {
  constexpr const char *op = "ResolveSession::resolve";
  const bool over_count = requests_ >= limits_.max_external_resources;
  try {
    provenance_.resources.push_back({request, depth, std::nullopt, 0, {}});
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error{make_error_code(ResolveError::resolver_failed)});
  }
  const std::size_t index = provenance_.resources.size() - 1;
  if (requests_ < std::numeric_limits<std::uint32_t>::max())
    ++requests_;
  // The record is edited in place; the vector is only appended to by this
  // function, so the index stays valid.
  const auto fail = [&](Error error) -> terreate::Result<ResolvedResource> {
    provenance_.resources[index].outcome = error.code();
    return std::unexpected(std::move(error));
  };
  // The reference is untrusted: only a short, printable prefix goes into details.
  std::string what = request.reference.substr(0, 96);
  for (char &c : what) {
    if (controlError(static_cast<unsigned char>(c)))
      c = '?';
  }

  if (over_count)
    return fail(makeError(ResolveError::limit_exceeded, op,
                          "more than " + std::to_string(limits_.max_external_resources) +
                              " external resources: " + what));
  if (depth >= limits_.max_resolve_depth)
    return fail(makeError(ResolveError::depth_exceeded, op,
                          "depth " + std::to_string(depth) + ", at most " +
                              std::to_string(limits_.max_resolve_depth) + ": " + what));
  if (!resolver_)
    return fail(makeError(ResolveError::no_resolver, op, what));

  auto normalized = normalizeReference(request.reference, request.base.base, options_);
  if (!normalized)
    return fail(makeError(ResolveError::reference_rejected, op,
                          what + ": " + normalized.error().code().message()));
  request.normalized = std::move(*normalized);
  provenance_.resources[index].request = request;

  const std::uint64_t remaining =
      limits_.max_total_resource_bytes - std::min(total_bytes_, limits_.max_total_resource_bytes);
  request.max_bytes = std::min(limits_.max_resource_bytes, remaining);
  if (request.declared_size &&
      (*request.declared_size > limits_.max_resource_bytes || *request.declared_size > remaining))
    return fail(makeError(ResolveError::limit_exceeded, op,
                          "declared size " + std::to_string(*request.declared_size) +
                              " exceeds the resource or total limit: " + request.normalized));

  terreate::Result<ResolvedResource> result =
      std::unexpected(Error{make_error_code(ResolveError::resolver_failed)});
  try {
    result = resolver_.resolve(request);
  } catch (const std::exception &exception) {
    return fail(makeError(ResolveError::resolver_failed, op,
                          request.normalized + ": exception: " + exception.what()));
  } catch (...) {
    return fail(
        makeError(ResolveError::resolver_failed, op, request.normalized + ": unknown exception"));
  }
  if (!result) {
    const auto &code = result.error().code();
    if (code.category() == resolve_error_category())
      return fail(result.error());
    return fail(makeError(ResolveError::resolver_failed, op,
                          request.normalized + ": " + std::string(code.category().name()) + " " +
                              std::to_string(code.value()) + ": " + code.message()));
  }

  const std::uint64_t size = result->size();
  if (size > limits_.max_resource_bytes || size > remaining)
    return fail(makeError(ResolveError::limit_exceeded, op,
                          "resource of " + std::to_string(size) +
                              " bytes exceeds the resource or total limit: " + request.normalized));
  total_bytes_ += size;
  auto &record = provenance_.resources[index];
  record.resolved = result->identity();
  record.size = size;
  return result;
}

} // namespace terreate::asset

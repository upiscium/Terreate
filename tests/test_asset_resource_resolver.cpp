// #280: the CPU-only decode contracts (byte source, source identity, limits,
// reference validation, resolver, bounded ResolveSession, provenance). This test
// links Terreate::Asset alone; it needs no Vulkan, no Graphics, no Platform, no
// display, no file system and no OS- or linker-specific mechanism.
#include <terreate/asset/decode.hpp>
#include <terreate/asset/resource_resolver.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace terreate::asset;

const char *test_name = "asset.resource_resolver";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR)
#error "Asset public headers must not pull in Vulkan or GLFW"
#endif
static_assert(std::is_error_code_enum_v<ResolveError> &&
              std::is_error_code_enum_v<ReferenceError> && std::is_error_code_enum_v<DecodeError>);
static_assert(std::is_nothrow_move_constructible_v<ResolvedResource>);

template <typename T, typename E> bool failsWith(const terreate::Result<T> &result, E error) {
  return !result && result.error().code() == make_error_code(error);
}

std::vector<std::byte> bytesOf(std::size_t count, unsigned value = 7) {
  return std::vector<std::byte>(count, static_cast<std::byte>(value));
}

void defaults() {
  const DecodeLimits limits = defaultDecodeLimits();
  check(limits.max_input_bytes == 256ULL * 1024 * 1024, "default input bytes");
  check(limits.max_pixels == 67'108'864ULL, "default pixels");
  check(limits.max_dimension == 16'384, "default dimension");
  check(limits.max_nodes == 65'536, "default nodes");
  check(limits.max_meshes == 16'384, "default meshes");
  check(limits.max_primitives == 65'536, "default primitives");
  check(limits.max_hierarchy_depth == 256, "default hierarchy depth");
  check(limits.max_external_resources == 256, "default external resources");
  check(limits.max_resource_bytes == 256ULL * 1024 * 1024, "default resource bytes");
  check(limits.max_total_resource_bytes == 1024ULL * 1024 * 1024, "default total bytes");
  check(limits.max_resolve_depth == 4, "default resolve depth");
  check(ReferenceOptions{}.allowed_schemes.empty() && ReferenceOptions{}.max_length == 2048,
        "default reference options allow no scheme");
}

void identityAndChecks() {
  check(makeSourceIdentity("models/ship.gltf").base == "models/", "base is the directory");
  check(makeSourceIdentity("ship.gltf").base.empty(), "no directory, empty base");
  check(makeSourceIdentity("a/b/c.bin").name == "a/b/c.bin", "name is kept");

  DecodeLimits limits;
  limits.max_input_bytes = 4;
  const auto five = bytesOf(5);
  check(checkInputSize(ByteView{five}.first(4), limits).has_value(), "input at the limit");
  check(failsWith(checkInputSize(five, limits), DecodeError::input_too_large), "input over limit");

  limits = {};
  limits.max_dimension = 100;
  limits.max_pixels = 5000;
  check(checkImageSize(100, 50, 1, limits).has_value(), "image at both limits");
  check(failsWith(checkImageSize(0, 5, 1, limits), DecodeError::dimension_exceeded), "zero width");
  check(failsWith(checkImageSize(5, 101, 1, limits), DecodeError::dimension_exceeded),
        "dimension over limit");
  check(failsWith(checkImageSize(100, 51, 1, limits), DecodeError::pixel_count_exceeded),
        "pixels over limit");
  check(failsWith(checkImageSize(100, 50, 2, limits), DecodeError::pixel_count_exceeded),
        "depth counts as pixels");
  limits = {};
  limits.max_dimension = 0xFFFFFFFFU;
  limits.max_pixels = 0xFFFFFFFFFFFFFFFFULL;
  limits.max_pixels = 1ULL << 40;
  check(failsWith(checkImageSize(0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, limits),
                  DecodeError::pixel_count_exceeded),
        "overflowing product is refused, not wrapped");
}

void rejected(std::string_view reference, ReferenceError error, const char *label,
              std::string_view base = {}, const ReferenceOptions &options = {}) {
  const auto result = normalizeReference(reference, base, options);
  if (!check(!result, label))
    return;
  if (!check(result.error().code() == make_error_code(error), label))
    std::fprintf(stderr, "  got: %s\n", result.error().code().message().c_str());
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void accepted(std::string_view reference, std::string_view expected, const char *label,
              std::string_view base = {}, const ReferenceOptions &options = {}) {
  const auto result = normalizeReference(reference, base, options);
  if (!check(result.has_value(), label))
    return;
  if (!check(*result == expected, label))
    std::fprintf(stderr, "  got: %s\n", result->c_str());
}

void references() {
  using R = ReferenceError;
  rejected("", R::empty, "empty");
  rejected(std::string(2049, 'a'), R::too_long, "too long");
  rejected(std::string_view("a\0b", 3), R::contains_nul, "raw NUL");
  rejected("a%00b", R::contains_nul, "encoded NUL");
  rejected("a\nb", R::control_character, "raw newline");
  rejected("a%0Ab", R::control_character, "encoded newline");
  rejected("a\x7F", R::control_character, "DEL");
  rejected("/etc/passwd", R::absolute_path, "absolute");
  rejected("//host/share/x", R::absolute_path, "UNC-like");
  rejected("a\\b", R::backslash, "backslash");
  rejected("..\\x", R::backslash, "backslash traversal");
  rejected("a%5Cb", R::backslash, "encoded backslash");
  rejected("a%5cb", R::backslash, "encoded backslash lowercase");
  rejected("C:/x", R::drive_letter, "drive letter");
  rejected("c:x", R::drive_letter, "drive letter without slash");
  rejected("C:\\x", R::backslash, "drive letter with backslash");
  rejected("../x", R::escapes_root, "leading ..");
  rejected("a/../../x", R::escapes_root, "escaping ..");
  rejected("%2e%2e/x", R::escapes_root, "encoded ..");
  rejected("%2E%2e/x", R::escapes_root, "encoded mixed-case ..");
  rejected("../../x", R::escapes_root, "escaping the base", "models/");
  rejected("a/%2e%2e/%2e%2e/x", R::escapes_root, "encoded .. escaping");
  rejected("a%2Fb", R::encoded_separator, "encoded slash");
  rejected("a%2fb", R::encoded_separator, "encoded slash lowercase");
  rejected("a%2", R::invalid_percent_encoding, "truncated escape");
  rejected("a%", R::invalid_percent_encoding, "lone percent");
  rejected("a%zz", R::invalid_percent_encoding, "non-hex escape");
  rejected("a?x=1", R::reserved_character, "query");
  rejected("a#frag", R::reserved_character, "fragment");
  rejected("a/b:c", R::reserved_character, "colon in path");
  rejected("a%3Ab", R::reserved_character, "encoded colon");
  rejected("a b:c", R::reserved_character, "colon in first segment, no scheme");
  rejected(":x", R::reserved_character, "leading colon");
  rejected("a/", R::names_directory, "trailing slash");
  rejected(".", R::names_directory, "dot");
  rejected("a/..", R::names_directory, "ends in ..");
  rejected("a/.", R::names_directory, "ends in .");
  rejected("file:///etc/passwd", R::scheme_not_allowed, "file scheme");
  rejected("https://host/x.bin", R::scheme_not_allowed, "https scheme by default");
  rejected("data:application/octet-stream;base64,AAAA", R::scheme_not_allowed,
           "data scheme by default");
  rejected("a", R::invalid_base, "base that escapes", "..");
  rejected("a", R::invalid_base, "base with colon", "C:/x");
  rejected("a", R::invalid_base, "base with backslash", "a\\b");
  rejected("a", R::invalid_base, "absolute base", "/etc/");

  ReferenceOptions https;
  https.allowed_schemes = {"https"};
  rejected("file:///x", R::scheme_not_allowed, "other scheme with https allowed", {}, https);
  rejected("https://host/a/../b", R::escapes_root, "URI with .. segment", {}, https);
  rejected("https://host/a/%2e%2e/b", R::escapes_root, "URI with encoded ..", {}, https);
  rejected("https://host/a\\b", R::backslash, "URI with backslash", {}, https);
  rejected("https://host/a%00", R::contains_nul, "URI with encoded NUL", {}, https);
  rejected("C:/x", R::drive_letter, "drive letter even when schemes allowed", {}, https);
  accepted("HTTPS://host/a.bin?x=1#y", "https://host/a.bin?x=1#y",
           "URI scheme lowercased, rest kept", {}, https);
}

void normalization() {
  accepted("a.bin", "a.bin", "plain");
  accepted("./a.bin", "a.bin", "dot segment");
  accepted("a//b.bin", "a/b.bin", "empty segment collapsed");
  accepted("a/./b/../c.bin", "a/c.bin", "inner ..");
  accepted("a/b/../../c.bin", "c.bin", "..s back to the root");
  accepted("my%20tex.png", "my tex.png", "percent-decoded once");
  accepted("%252e%252e/x", "%2e%2e/x", "no double decoding");
  accepted("%2e/a", "a", "encoded dot segment");
  accepted("caf\xC3\xA9.png", "caf\xC3\xA9.png", "non-ASCII passes");
  accepted("textures/a.png", "models/textures/a.png", "base applied", "models/");
  accepted("../textures/a.png", "textures/a.png", "base popped", "models/");
  accepted("a.bin", "a/b/a.bin", "nested base", "a/./b//");
  accepted("a.bin", "a.bin", "empty base", "");
  ReferenceOptions limited;
  limited.max_length = 5;
  accepted("a.bin", "a.bin", "exactly at max length", {}, limited);
  rejected("ab.bin", ReferenceError::too_long, "over a custom max length", {}, limited);
}

void maxBytes() {
  DecodeLimits limit;
  limit.max_resource_bytes = 10;
  limit.max_total_resource_bytes = 14;
  std::uint64_t seen = 0;
  auto spy = [&seen](const ResourceRequest &request) -> terreate::Result<ResolvedResource> {
    seen = request.max_bytes;
    return ResolvedResource{SourceIdentity{}, bytesOf(8)};
  };
  ResolveSession session{ResourceResolverView::bind(spy), limit, makeSourceIdentity("a")};
  check(session.resolve(session.makeRequest("a", ResourceKind::buffer)).has_value() && seen == 10,
        "max_bytes is the per-resource limit");
  check(failsWith(session.resolve(session.makeRequest("b", ResourceKind::buffer)),
                  ResolveError::limit_exceeded) &&
            seen == 6,
        "max_bytes is the remaining total");
  check(ResourceRequest{}.max_bytes == 0xFFFFFFFFFFFFFFFFULL, "a hand-built request is unbounded");
}

void longReference() {
  auto never = [](const ResourceRequest &) -> terreate::Result<ResolvedResource> {
    return ResolvedResource{};
  };
  ResolveSession session{ResourceResolverView::bind(never), {}, makeSourceIdentity("a")};
  const auto result =
      session.resolve(session.makeRequest("\n" + std::string(5000, 'a'), ResourceKind::buffer));
  check(failsWith(result, ResolveError::reference_rejected) &&
            result.error().detail().size() < 300 &&
            result.error().detail().find('\n') == std::string::npos,
        "details do not echo a long or control-character reference");
}

void identity() {
  const auto a = ResolvedResource{makeSourceIdentity("x/y.bin"), bytesOf(3)};
  const auto b = a; // NOLINT(performance-unnecessary-copy-initialization)
  check(a.size() == 3 && b.bytes().data() == a.bytes().data(), "a copy shares the storage");
  check(a.identity().name == "x/y.bin" && a.identity().base == "x/", "identity kept");
  check(ResolvedResource{}.bytes().empty() && ResolvedResource{}.size() == 0, "default is empty");
  const ResolvedResource null{SourceIdentity{}, std::shared_ptr<const std::vector<std::byte>>{}};
  check(null.size() == 0, "null storage is empty");
}

void memoryResolver() {
  MemoryResolver memory;
  check(memory.add("models/a.bin", bytesOf(4)).has_value(), "add");
  check(memory.add("./models//b.bin", bytesOf(2)).has_value(), "add normalizes the key");
  check(memory.contains("models/b.bin") && memory.size() == 2, "normalized key stored");
  check(failsWith(memory.add("../x", bytesOf(1)), ResolveError::reference_rejected),
        "add rejects a bad key");
  check(failsWith(memory.add("", bytesOf(1)), ResolveError::reference_rejected),
        "add rejects empty");
  check(memory.add("models/a.bin", bytesOf(9)).has_value() && memory.size() == 2, "add replaces");

  ResourceRequest request;
  request.reference = "a.bin";
  request.base = makeSourceIdentity("models/ship.gltf");
  auto found = memory(request);
  check(found && found->size() == 9 && found->identity().name == "models/a.bin" &&
            found->identity().base == "models/",
        "found through base and reference");
  request.reference = "missing.bin";
  check(failsWith(memory(request), ResolveError::not_found), "not found");
  request.reference = "../../x";
  check(failsWith(memory(request), ResolveError::reference_rejected), "bad reference");
  request.reference = "ignored";
  request.normalized = "models/b.bin";
  check(memory(request).has_value(), "the normalized field wins");

  auto shared = std::make_shared<const std::vector<std::byte>>(bytesOf(6));
  check(memory.add("s.bin", shared).has_value(), "add shared");
  request.normalized = "s.bin";
  check(memory(request)->storage() == shared, "shared bytes are not copied");
}

struct Counter {
  int calls = 0;
  std::size_t size = 3;
  std::error_code fail{};
  bool throws = false;
  terreate::Result<ResolvedResource> operator()(const ResourceRequest &request) {
    ++calls;
    if (throws)
      throw std::runtime_error("boom");
    if (fail)
      return std::unexpected(terreate::Error{fail});
    return ResolvedResource{SourceIdentity{"resolved:" + request.normalized, "res/"},
                            bytesOf(size)};
  }
};

void sessionBasics() {
  MemoryResolver memory;
  check(memory.add("models/a.bin", bytesOf(4)).has_value(), "add a");
  check(memory.add("models/t.png", bytesOf(6)).has_value(), "add t");
  const auto source = makeSourceIdentity("models/ship.gltf");
  ResolveSession session{ResourceResolverView::bind(memory), defaultDecodeLimits(), source};
  auto request = session.makeRequest("a.bin", ResourceKind::buffer);
  check(request.base == source && request.kind == ResourceKind::buffer &&
            request.reference == "a.bin",
        "makeRequest");
  request.declared_size = 4;
  request.media_type = "application/octet-stream";
  const auto a = session.resolve(request);
  check(a && a->size() == 4, "resolve a");
  const auto t = session.resolve(session.makeRequest("t%2Epng", ResourceKind::image));
  check(t && t->size() == 6, "resolve t through percent-decoding");
  const auto miss = session.resolve(session.makeRequest("nope.bin", ResourceKind::buffer));
  check(failsWith(miss, ResolveError::not_found), "not_found passes through");

  const auto &p = session.provenance();
  check(p.source == source, "provenance source");
  if (check(p.resources.size() == 3, "every request recorded")) {
    check(p.resources[0].request.reference == "a.bin" &&
              p.resources[0].request.normalized == "models/a.bin" &&
              p.resources[0].request.kind == ResourceKind::buffer &&
              p.resources[0].request.declared_size == 4U &&
              p.resources[0].request.media_type == "application/octet-stream" &&
              p.resources[0].resolved == makeSourceIdentity("models/a.bin") &&
              p.resources[0].size == 4 && !p.resources[0].outcome && p.resources[0].depth == 0,
          "record of a success has request, identity and size");
    check(p.resources[1].request.reference == "t%2Epng" &&
              p.resources[1].request.normalized == "models/t.png" && p.resources[1].size == 6,
          "the reference is recorded as written");
    check(!p.resources[2].resolved && p.resources[2].size == 0 &&
              p.resources[2].outcome == make_error_code(ResolveError::not_found),
          "record of a failure has the outcome");
  }
  check(session.requestCount() == 3 && session.resolvedBytes() == 10, "counters");
}

void sessionErrors() {
  const auto source = makeSourceIdentity("a.gltf");
  {
    ResolveSession session{ResourceResolverView{}, defaultDecodeLimits(), source};
    const auto result = session.resolve(session.makeRequest("a.bin", ResourceKind::buffer));
    check(failsWith(result, ResolveError::no_resolver), "no resolver");
    check(session.provenance().resources.size() == 1 &&
              session.provenance().resources[0].outcome ==
                  make_error_code(ResolveError::no_resolver),
          "no resolver is recorded");
    check(failsWith(ResourceResolverView{}.resolve(ResourceRequest{}), ResolveError::no_resolver),
          "an unbound view fails with no_resolver");
    check(!ResourceResolverView{}, "an unbound view is false");
  }
  Counter counter;
  {
    ResolveSession session{ResourceResolverView::bind(counter), defaultDecodeLimits(), source};
    const auto result = session.resolve(session.makeRequest("../x.bin", ResourceKind::buffer));
    check(failsWith(result, ResolveError::reference_rejected), "rejected reference");
    check(counter.calls == 0, "the resolver is not called for a rejected reference");
    check(result.error().detail().find("..") != std::string::npos, "detail names the reference");
    check(session.provenance().resources[0].request.reference == "../x.bin" &&
              session.provenance().resources[0].request.normalized.empty(),
          "a rejected reference is recorded as written");
  }
  {
    // Failing resolver: a foreign error is wrapped, a ResolveError passes as it is.
    counter.fail = std::make_error_code(std::errc::permission_denied);
    ResolveSession session{ResourceResolverView::bind(counter), defaultDecodeLimits(), source};
    const auto wrapped = session.resolve(session.makeRequest("a.bin", ResourceKind::buffer));
    check(failsWith(wrapped, ResolveError::resolver_failed), "foreign error is wrapped");
    check(wrapped.error().detail().find("generic") != std::string::npos &&
              wrapped.error().detail().find("a.bin") != std::string::npos,
          "wrapped detail names the original category and the resource");
    counter.fail = make_error_code(ResolveError::not_found);
    check(failsWith(session.resolve(session.makeRequest("a.bin", ResourceKind::buffer)),
                    ResolveError::not_found),
          "a ResolveError code is passed through");
    counter.fail = {};
    counter.throws = true;
    check(failsWith(session.resolve(session.makeRequest("a.bin", ResourceKind::buffer)),
                    ResolveError::resolver_failed),
          "an exception becomes resolver_failed");
    counter.throws = false;
  }
  counter = {};
}

void limits() {
  const auto source = makeSourceIdentity("a.gltf");
  Counter counter;
  const auto view = ResourceResolverView::bind(counter);
  {
    DecodeLimits limit;
    limit.max_external_resources = 2;
    ResolveSession session{view, limit, source};
    check(session.resolve(session.makeRequest("a", ResourceKind::buffer)).has_value() &&
              session.resolve(session.makeRequest("b", ResourceKind::buffer)).has_value(),
          "two requests are within the count limit");
    const int calls = counter.calls;
    check(failsWith(session.resolve(session.makeRequest("c", ResourceKind::buffer)),
                    ResolveError::limit_exceeded),
          "the third request exceeds the count");
    check(counter.calls == calls, "the resolver is not called over the count");
    check(session.provenance().resources.size() == 3, "the refused request is recorded");
  }
  {
    DecodeLimits limit;
    limit.max_external_resources = 1;
    ResolveSession session{ResourceResolverView{}, limit, source};
    check(failsWith(session.resolve(session.makeRequest("a", ResourceKind::buffer)),
                    ResolveError::no_resolver) &&
              failsWith(session.resolve(session.makeRequest("a", ResourceKind::buffer)),
                        ResolveError::limit_exceeded),
          "failed requests count toward the limit");
  }
  {
    DecodeLimits limit;
    limit.max_resolve_depth = 2;
    ResolveSession session{view, limit, source};
    check(session.resolve(session.makeRequest("a", ResourceKind::buffer), 1).has_value(),
          "depth below the limit");
    check(failsWith(session.resolve(session.makeRequest("a", ResourceKind::buffer), 2),
                    ResolveError::depth_exceeded),
          "depth at the limit is refused");
    check(session.provenance().resources[1].depth == 2, "depth recorded");
    limit.max_resolve_depth = 0;
    ResolveSession none{view, limit, source};
    check(failsWith(none.resolve(none.makeRequest("a", ResourceKind::buffer)),
                    ResolveError::depth_exceeded),
          "depth limit 0 allows nothing");
  }
  {
    DecodeLimits limit;
    limit.max_resource_bytes = 10;
    ResolveSession session{view, limit, source};
    counter.size = 10;
    check(session.resolve(session.makeRequest("a", ResourceKind::buffer)).has_value(),
          "resource at the per-resource limit");
    counter.size = 11;
    check(failsWith(session.resolve(session.makeRequest("b", ResourceKind::buffer)),
                    ResolveError::limit_exceeded),
          "actual size over the per-resource limit");
    check(session.resolvedBytes() == 10 && !session.provenance().resources[1].resolved,
          "the refused resource is not counted or recorded as resolved");
    counter.size = 3;
    auto request = session.makeRequest("c", ResourceKind::buffer);
    request.declared_size = 11;
    const int calls = counter.calls;
    check(failsWith(session.resolve(request), ResolveError::limit_exceeded),
          "declared size over the limit");
    check(counter.calls == calls, "a declared size over the limit refuses before the resolver");
    request.declared_size = 0xFFFFFFFFFFFFFFFFULL;
    check(failsWith(session.resolve(request), ResolveError::limit_exceeded),
          "a huge declared size is refused");
  }
  {
    DecodeLimits limit;
    limit.max_total_resource_bytes = 10;
    ResolveSession session{view, limit, source};
    counter.size = 6;
    check(session.resolve(session.makeRequest("a", ResourceKind::buffer)).has_value(),
          "first resource within the total");
    check(failsWith(session.resolve(session.makeRequest("b", ResourceKind::buffer)),
                    ResolveError::limit_exceeded),
          "second resource over the total");
    auto request = session.makeRequest("c", ResourceKind::buffer);
    request.declared_size = 5;
    check(failsWith(session.resolve(request), ResolveError::limit_exceeded),
          "declared size over the remaining total");
    counter.size = 4;
    check(session.resolve(session.makeRequest("d", ResourceKind::buffer)).has_value() &&
              session.resolvedBytes() == 10,
          "the total can be reached exactly");
  }
  {
    // A scheme is rejected through the session unless the options allow it.
    ResolveSession session{view, {}, source};
    check(failsWith(session.resolve(session.makeRequest("https://h/a", ResourceKind::buffer)),
                    ResolveError::reference_rejected),
          "a scheme is rejected by default");
    ReferenceOptions https;
    https.allowed_schemes = {"https"};
    ResolveSession allowing{view, {}, source, https};
    check(allowing.resolve(allowing.makeRequest("https://h/a", ResourceKind::buffer)).has_value(),
          "an allowed scheme passes through to the resolver");
  }
}

// A resolver that is a plain callable object and one bound to an application
// object with state, resolving a nested reference.
void customResolvers() {
  const auto source = makeSourceIdentity("m/scene.gltf");
  int seen = 0;
  auto lambda = [&seen](const ResourceRequest &request) -> terreate::Result<ResolvedResource> {
    ++seen;
    return ResolvedResource{SourceIdentity{"lambda:" + request.normalized, "lambda/"}, bytesOf(1)};
  };
  ResolveSession session{ResourceResolverView::bind(lambda), {}, source};
  const auto result = session.resolve(session.makeRequest("tex.png", ResourceKind::image));
  check(result && seen == 1 && result->identity().name == "lambda:m/tex.png",
        "a lambda resolver receives the normalized reference");

  // A nested request resolves against the identity of the resolved resource.
  const auto nested = session.makeRequest("inner.bin", ResourceKind::buffer);
  ResourceRequest deeper = nested;
  deeper.base = result->identity();
  const auto inner = session.resolve(deeper, 1);
  check(inner && inner->identity().name == "lambda:lambda/inner.bin",
        "a nested request uses the base of the resolved identity");

  struct Archive {
    std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
    terreate::Result<ResolvedResource> operator()(const ResourceRequest &request) const {
      for (const auto &[name, data] : entries) {
        if (name == request.normalized)
          return ResolvedResource{makeSourceIdentity("archive:" + name), data};
      }
      return std::unexpected(terreate::Error{make_error_code(ResolveError::not_found)});
    }
  };
  Archive archive;
  archive.entries.emplace_back("m/a.bin", bytesOf(5));
  ResolveSession fromArchive{ResourceResolverView::bind(archive), {}, source};
  const auto a = fromArchive.resolve(fromArchive.makeRequest("a.bin", ResourceKind::buffer));
  check(a && a->size() == 5 && a->identity().name == "archive:m/a.bin", "an object-bound resolver");
  check(failsWith(fromArchive.resolve(fromArchive.makeRequest("b.bin", ResourceKind::buffer)),
                  ResolveError::not_found),
        "the object-bound resolver reports not_found");
  check(!!ResourceResolverView::bind(archive), "a bound view is true");
}

void diagnostics() {
  DecodeDiagnostic diagnostic;
  diagnostic.severity = DecodeSeverity::warning;
  diagnostic.code = "gltf.unsupported.animation";
  diagnostic.message = "animations are not represented";
  diagnostic.location = {"a.gltf", "/animations/0", 12};
  const DecodeDiagnostic copy = diagnostic;
  check(copy.severity == DecodeSeverity::warning && copy.location.path == "/animations/0" &&
            copy.location.byte_offset == 12U && copy.location.source == "a.gltf",
        "a diagnostic is a copyable value");
  check(!DecodeDiagnostic{}.location.byte_offset, "location offset is optional");
  check(std::string(decode_error_category().name()) == "terreate.asset.decode" &&
            std::string(resolve_error_category().name()) == "terreate.asset.resolve" &&
            std::string(reference_error_category().name()) == "terreate.asset.reference",
        "category names");
  check(!make_error_code(ResolveError::depth_exceeded).message().empty() &&
            !make_error_code(ReferenceError::invalid_base).message().empty() &&
            !make_error_code(DecodeError::unsupported_input).message().empty(),
        "messages");
}

} // namespace

int main() noexcept {
  defaults();
  identityAndChecks();
  references();
  normalization();
  identity();
  memoryResolver();
  sessionBasics();
  sessionErrors();
  limits();
  customResolvers();
  maxBytes();
  longReference();
  diagnostics();
  if (failures != 0) {
    std::fprintf(stderr, "%s: assertion=FAIL: %d check(s) failed\n", test_name, failures);
    return EXIT_FAILURE;
  }
  std::printf("%s: resource-resolver=PASS\n", test_name);
  return EXIT_SUCCESS;
}

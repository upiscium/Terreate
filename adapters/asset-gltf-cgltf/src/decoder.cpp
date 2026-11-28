#include <terreate/adapter/asset_gltf_cgltf/decoder.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "cgltf_backend.hpp"

namespace terreate::adapter::asset_gltf_cgltf {
namespace {

namespace asset = terreate::asset;

// ---------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------

// The deepest JSON nesting the parser is shown. cgltf skips unknown values recursively, so a
// hostile document nested a million levels deep would overflow the stack; legal glTF nests far
// less than this (extras are the only free-form part).
constexpr std::size_t max_json_depth = 128;
// The parser's memory budget: a base plus a multiple of the JSON text size. A legal file needs a
// few times its JSON size; the multiple leaves room for compact files and still stops a short
// text that describes a huge number of objects.
constexpr std::uint64_t parse_base_bytes = 16ULL * 1024 * 1024;
constexpr std::uint64_t parse_bytes_per_input_byte = 64;
// At most this many diagnostics (per code) and unsupported entries (per kind) are listed
// individually; the rest is counted in one summary item.
constexpr std::size_t report_cap = 64;
// At most this many distinct extension names are collected from the objects of a file.
constexpr std::size_t extension_name_cap = 256;

constexpr std::uint32_t glb_magic = 0x46546C67U;      // "glTF"
constexpr std::uint32_t glb_json_chunk = 0x4E4F534AU; // "JSON"
constexpr std::uint32_t glb_bin_chunk = 0x004E4942U;  // "BIN\0"
constexpr std::size_t glb_header_size = 12;
constexpr std::size_t glb_chunk_header_size = 8;

// ---------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------

class GltfDecodeErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.adapter.asset_gltf_cgltf"; }
  std::string message(int value) const override {
    switch (static_cast<GltfDecodeError>(value)) {
    case GltfDecodeError::malformed_gltf:
      return "the glTF input is empty, truncated or not valid glTF";
    case GltfDecodeError::unsupported_version:
      return "the glTF version is not 2.x";
    case GltfDecodeError::unsupported_required_extension:
      return "the glTF requires an extension this adapter does not implement";
    case GltfDecodeError::missing_resolver:
      return "the glTF refers to an external resource and no resolver is bound";
    case GltfDecodeError::missing_image_decoder:
      return "the glTF has an image to decode and no image decoder is bound";
    case GltfDecodeError::image_decoder_failed:
      return "the image decoder threw an exception";
    case GltfDecodeError::invalid_buffer:
      return "a glTF buffer has no data, too little data or an invalid data URI";
    case GltfDecodeError::invalid_accessor:
      return "a glTF accessor, buffer view or index is not valid";
    case GltfDecodeError::invalid_resource:
      return "a glTF image, texture or sampler is not valid";
    case GltfDecodeError::decoded_size_exceeded:
      return "the decoded geometry and images exceed the decoded size limit";
    case GltfDecodeError::out_of_memory:
      return "an allocation failed or the parser's memory budget was exceeded";
    }
    return "unknown glTF decode error";
  }
};

constexpr const char *operation = "decodeGltf";

// Error construction allocates; on exhaustion a code-only Error is returned, which does not
// allocate.
template <typename Code> auto makeError(Code code, const std::string &detail) -> terreate::Error {
  try {
    return terreate::Error{make_error_code(code), operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{make_error_code(code)};
  }
}

template <typename Code>
auto fail(Code code, const std::string &detail) -> std::unexpected<terreate::Error> {
  return std::unexpected(makeError(code, detail));
}

// Whether a `minVersion` string is above 2.0, without locale-dependent parsing.
auto minVersionAbove20(std::string_view version) -> bool {
  std::size_t i = 0;
  std::uint64_t major = 0;
  while (i < version.size() && version[i] >= '0' && version[i] <= '9' && major < 1000)
    major = major * 10 + static_cast<std::uint64_t>(version[i++] - '0');
  if (major != 2)
    return major > 2;
  if (i < version.size() && version[i] == '.') {
    for (++i; i < version.size(); ++i) {
      if (version[i] != '0')
        return true;
    }
  }
  return false;
}

auto text(const char *value) -> std::string { return value == nullptr ? std::string{} : value; }

auto indexPath(std::string_view list, std::size_t index) -> std::string {
  return std::string{list} + "[" + std::to_string(index) + "]";
}

// ---------------------------------------------------------------------------------------
// Checked arithmetic (the sizes come from an untrusted file)
// ---------------------------------------------------------------------------------------

auto checkedMul(std::uint64_t a, std::uint64_t b) -> std::optional<std::uint64_t> {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
    return std::nullopt;
  return a * b;
}

auto checkedAdd(std::uint64_t a, std::uint64_t b) -> std::optional<std::uint64_t> {
  if (b > std::numeric_limits<std::uint64_t>::max() - a)
    return std::nullopt;
  return a + b;
}

// ---------------------------------------------------------------------------------------
// Byte helpers (all reads are bounds-checked: the input may be hostile)
// ---------------------------------------------------------------------------------------

using Bytes = asset::ByteView;

auto readLe32(Bytes bytes, std::size_t offset) -> std::uint32_t {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const std::size_t position = offset + i;
    const auto byte = position < bytes.size() ? static_cast<std::uint32_t>(bytes[position]) : 0U;
    value |= (byte & 0xFFU) << (8U * static_cast<std::uint32_t>(i));
  }
  return value;
}

template <typename Value> auto loadValue(const std::byte *source) -> Value {
  Value value{};
  std::memcpy(&value, source, sizeof value);
  return value;
}

// ---------------------------------------------------------------------------------------
// The container: GLB chunks and the JSON text
// ---------------------------------------------------------------------------------------

struct Container {
  GltfContainer kind = GltfContainer::gltf;
  // The bytes cgltf is shown: the whole input for glTF, the part the GLB header declares for GLB.
  Bytes whole{};
  Bytes json{};
  Bytes bin{};
  bool has_bin = false;
  std::size_t trailing = 0;
};

auto locateContainer(Bytes bytes) -> terreate::Result<Container> {
  Container container;
  container.whole = bytes;
  container.json = bytes;
  if (bytes.size() < 4 || readLe32(bytes, 0) != glb_magic) {
    if (bytes.size() < glb_header_size)
      return fail(GltfDecodeError::malformed_gltf, "the input is too short to be a glTF document");
    return container;
  }

  container.kind = GltfContainer::glb;
  if (bytes.size() < glb_header_size + glb_chunk_header_size)
    return fail(GltfDecodeError::malformed_gltf, "the GLB is shorter than its headers");
  const std::uint32_t version = readLe32(bytes, 4);
  if (version != 2)
    return fail(GltfDecodeError::unsupported_version,
                "the GLB container version is " + std::to_string(version));
  const std::uint64_t length = readLe32(bytes, 8);
  if (length > bytes.size())
    return fail(GltfDecodeError::malformed_gltf, "the GLB length field (" + std::to_string(length) +
                                                     ") is larger than the input (" +
                                                     std::to_string(bytes.size()) + ")");
  if (length < glb_header_size + glb_chunk_header_size)
    return fail(GltfDecodeError::malformed_gltf,
                "the GLB length field is shorter than its headers");
  container.trailing = bytes.size() - static_cast<std::size_t>(length);
  container.whole = bytes.first(static_cast<std::size_t>(length));
  const Bytes glb = container.whole;

  // The JSON chunk.
  const std::uint64_t json_length = readLe32(glb, glb_header_size);
  if (readLe32(glb, glb_header_size + 4) != glb_json_chunk)
    return fail(GltfDecodeError::malformed_gltf, "the first GLB chunk is not the JSON chunk");
  const std::size_t json_begin = glb_header_size + glb_chunk_header_size;
  if (json_length > glb.size() - json_begin)
    return fail(GltfDecodeError::malformed_gltf, "the GLB JSON chunk runs past the GLB length");
  container.json = glb.subspan(json_begin, static_cast<std::size_t>(json_length));

  // The optional BIN chunk; anything shorter than a chunk header after it is ignored (and
  // reported as trailing bytes).
  const std::size_t next = json_begin + static_cast<std::size_t>(json_length);
  if (glb.size() - next >= glb_chunk_header_size) {
    const std::uint64_t bin_length = readLe32(glb, next);
    if (readLe32(glb, next + 4) != glb_bin_chunk)
      return fail(
          GltfDecodeError::malformed_gltf,
          "the second GLB chunk is not the BIN chunk (other chunk types are not supported)");
    const std::size_t bin_begin = next + glb_chunk_header_size;
    if (bin_length > glb.size() - bin_begin)
      return fail(GltfDecodeError::malformed_gltf, "the GLB BIN chunk runs past the GLB length");
    container.bin = glb.subspan(bin_begin, static_cast<std::size_t>(bin_length));
    container.has_bin = true;
    container.trailing += glb.size() - bin_begin - static_cast<std::size_t>(bin_length);
  } else {
    container.trailing += glb.size() - next;
  }
  return container;
}

// The JSON text nests at most `max_json_depth` levels (brackets inside strings do not count).
auto jsonDepthAllowed(Bytes json) -> bool {
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const std::byte raw : json) {
    const auto c = static_cast<char>(raw);
    if (in_string) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        in_string = false;
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '[' || c == '{') {
      if (++depth > max_json_depth)
        return false;
    } else if ((c == ']' || c == '}') && depth > 0) {
      --depth;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------
// The parser's allocator: every cgltf allocation counts against a budget
// ---------------------------------------------------------------------------------------

// Every block has a 16-byte prefix holding its size (16 keeps the payload aligned).
constexpr std::size_t block_prefix = 16;

struct ParseBudget {
  std::uint64_t limit = 0;
  std::uint64_t live = 0;
};

auto budgetAllocate(void *user, cgltf_size size) -> void * {
  auto *budget = static_cast<ParseBudget *>(user);
  if (size > std::numeric_limits<std::size_t>::max() - block_prefix)
    return nullptr;
  const std::uint64_t total = static_cast<std::uint64_t>(size) + block_prefix;
  if (budget->live > budget->limit || total > budget->limit - budget->live)
    return nullptr;
  void *base = std::malloc(static_cast<std::size_t>(total));
  if (base == nullptr)
    return nullptr;
  std::memcpy(base, &size, sizeof size);
  budget->live += total;
  return static_cast<unsigned char *>(base) + block_prefix;
}

// The signature is the one cgltf_memory_options::free_func prescribes.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void budgetFree(void *user, void *block) {
  if (block == nullptr)
    return;
  auto *budget = static_cast<ParseBudget *>(user);
  unsigned char *base = static_cast<unsigned char *>(block) - block_prefix;
  cgltf_size size = 0;
  std::memcpy(&size, base, sizeof size);
  const std::uint64_t total = static_cast<std::uint64_t>(size) + block_prefix;
  budget->live = budget->live >= total ? budget->live - total : 0;
  std::free(base);
}

struct DataDeleter {
  void operator()(cgltf_data *data) const noexcept { cgltf_free(data); }
};
using ParsedGltf = std::unique_ptr<cgltf_data, DataDeleter>;

// ---------------------------------------------------------------------------------------
// Reporting: diagnostics and unsupported features, bounded and exact in their totals
// ---------------------------------------------------------------------------------------

struct Note {
  asset::DecodeSeverity severity = asset::DecodeSeverity::info;
  std::string code;
  std::string message;
  std::string path;
};

struct Finding {
  std::string kind;
  std::string name;
  std::string detail;
  std::string path;
};

class Reporter {
public:
  explicit Reporter(std::string source) : source_(std::move(source)) {}

  void note(Note item) {
    Counter &counter = diagnostic_counts_[item.code];
    counter.severity = item.severity;
    ++counter.total;
    if (counter.total > report_cap)
      return;
    asset::DecodeDiagnostic diagnostic;
    diagnostic.severity = item.severity;
    diagnostic.code = std::move(item.code);
    diagnostic.message = std::move(item.message);
    diagnostic.location.source = source_;
    diagnostic.location.path = std::move(item.path);
    diagnostics_.push_back(std::move(diagnostic));
  }

  void unsupported(Finding finding) {
    note(Note{asset::DecodeSeverity::warning, "gltf.unsupported." + finding.kind,
              finding.kind + " '" + finding.name + "' is not represented: " + finding.detail,
              finding.path});
    Counter &counter = unsupported_counts_[finding.kind];
    ++counter.total;
    if (counter.total > report_cap)
      return;
    unsupported_.push_back(asset::UnsupportedFeature{
        std::move(finding.kind), std::move(finding.name), std::move(finding.detail)});
  }

  // Appends the summary items and moves everything out.
  void finish(std::vector<asset::DecodeDiagnostic> &diagnostics,
              std::vector<asset::UnsupportedFeature> &unsupported) {
    for (const auto &[code, counter] : diagnostic_counts_) {
      if (counter.total <= report_cap)
        continue;
      asset::DecodeDiagnostic summary;
      summary.severity = counter.severity;
      summary.code = code;
      summary.message =
          std::to_string(counter.total - report_cap) + " further occurrences are not listed";
      summary.location.source = source_;
      diagnostics_.push_back(std::move(summary));
    }
    for (const auto &[kind, counter] : unsupported_counts_) {
      if (counter.total <= report_cap)
        continue;
      unsupported_.push_back(asset::UnsupportedFeature{
          kind, "(further)", std::to_string(counter.total - report_cap) + " further not listed"});
    }
    diagnostics = std::move(diagnostics_);
    unsupported = std::move(unsupported_);
  }

private:
  struct Counter {
    std::size_t total = 0;
    asset::DecodeSeverity severity = asset::DecodeSeverity::info;
  };

  std::string source_;
  std::vector<asset::DecodeDiagnostic> diagnostics_;
  std::vector<asset::UnsupportedFeature> unsupported_;
  std::map<std::string, Counter> diagnostic_counts_;
  std::map<std::string, Counter> unsupported_counts_;
};

// ---------------------------------------------------------------------------------------
// Base64 data URIs
// ---------------------------------------------------------------------------------------

auto lowerAscii(char c) -> char {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

auto startsWithNoCase(std::string_view value, std::string_view prefix) -> bool {
  if (value.size() < prefix.size())
    return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (lowerAscii(value[i]) != prefix[i])
      return false;
  }
  return true;
}

auto isDataUri(std::string_view uri) -> bool { return startsWithNoCase(uri, "data:"); }

auto base64Value(char c) -> int {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

struct DataUriPayload {
  std::string_view base64;
  std::size_t decoded_size = 0;
};

// Parses `data:[<media type>][;<parameter>]*;base64,<payload>` and computes the decoded size
// without decoding. Only padded, canonical-length base64 is accepted.
auto parseDataUri(std::string_view uri) -> std::optional<DataUriPayload> {
  const std::size_t comma = uri.find(',');
  if (comma == std::string_view::npos)
    return std::nullopt;
  const std::string_view header = uri.substr(5, comma - 5);
  constexpr std::string_view marker = ";base64";
  if (header.size() < marker.size() ||
      !startsWithNoCase(header.substr(header.size() - marker.size()), marker))
    return std::nullopt;
  const std::string_view payload = uri.substr(comma + 1);
  if (payload.size() % 4 != 0)
    return std::nullopt;
  std::size_t padding = 0;
  while (padding < 2 && padding < payload.size() && payload[payload.size() - 1 - padding] == '=')
    ++padding;
  for (std::size_t i = 0; i + padding < payload.size(); ++i) {
    if (base64Value(payload[i]) < 0)
      return std::nullopt;
  }
  return DataUriPayload{payload, payload.size() / 4 * 3 - padding};
}

auto decodeBase64(const DataUriPayload &payload) -> std::vector<std::byte> {
  std::vector<std::byte> bytes;
  bytes.reserve(payload.decoded_size);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : payload.base64) {
    if (c == '=')
      break;
    buffer = (buffer << 6U) | static_cast<std::uint32_t>(base64Value(c));
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<std::byte>((buffer >> static_cast<unsigned>(bits)) & 0xFFU));
    }
  }
  bytes.resize(payload.decoded_size);
  return bytes;
}

// ---------------------------------------------------------------------------------------
// Enumerator mapping
// ---------------------------------------------------------------------------------------

auto componentByteSize(cgltf_component_type type) -> std::size_t {
  switch (type) {
  case cgltf_component_type_r_8:
  case cgltf_component_type_r_8u:
    return 1;
  case cgltf_component_type_r_16:
  case cgltf_component_type_r_16u:
    return 2;
  case cgltf_component_type_r_32u:
  case cgltf_component_type_r_32f:
    return 4;
  default:
    return 0;
  }
}

auto componentsOf(cgltf_type type) -> std::size_t {
  switch (type) {
  case cgltf_type_scalar:
    return 1;
  case cgltf_type_vec2:
    return 2;
  case cgltf_type_vec3:
    return 3;
  case cgltf_type_vec4:
    return 4;
  default:
    return 0; // matrices are not valid for any attribute or index
  }
}

auto formatOf(std::size_t components) -> asset::AttributeFormat {
  switch (components) {
  case 2:
    return asset::AttributeFormat::float32x2;
  case 3:
    return asset::AttributeFormat::float32x3;
  case 4:
    return asset::AttributeFormat::float32x4;
  default:
    return asset::AttributeFormat::float32x1;
  }
}

// One component converted to float. `normalized` applies to the integer types.
auto readComponent(const std::byte *source, cgltf_component_type type, bool normalized) -> float {
  switch (type) {
  case cgltf_component_type_r_32f:
    return loadValue<float>(source);
  case cgltf_component_type_r_8: {
    const auto value = static_cast<float>(loadValue<std::int8_t>(source));
    return normalized ? std::max(value / 127.0F, -1.0F) : value;
  }
  case cgltf_component_type_r_8u: {
    const auto value = static_cast<float>(loadValue<std::uint8_t>(source));
    return normalized ? value / 255.0F : value;
  }
  case cgltf_component_type_r_16: {
    const auto value = static_cast<float>(loadValue<std::int16_t>(source));
    return normalized ? std::max(value / 32767.0F, -1.0F) : value;
  }
  case cgltf_component_type_r_16u: {
    const auto value = static_cast<float>(loadValue<std::uint16_t>(source));
    return normalized ? value / 65535.0F : value;
  }
  default:
    return 0.0F;
  }
}

auto readUnsigned(const std::byte *source, cgltf_component_type type) -> std::uint64_t {
  switch (type) {
  case cgltf_component_type_r_8u:
    return loadValue<std::uint8_t>(source);
  case cgltf_component_type_r_16u:
    return loadValue<std::uint16_t>(source);
  case cgltf_component_type_r_32u:
    return loadValue<std::uint32_t>(source);
  default:
    return 0;
  }
}

auto isUnsignedIntegerComponent(cgltf_component_type type) -> bool {
  return type == cgltf_component_type_r_8u || type == cgltf_component_type_r_16u ||
         type == cgltf_component_type_r_32u;
}

// cgltf stores the integer of the file in these enum members without checking it, so a value that
// is not an enumerator may be there; reading such an object as the enum is undefined, reading its
// bytes is not.
template <typename Enum> auto storedValue(const Enum &member) -> int {
  static_assert(sizeof(Enum) == sizeof(int));
  int value = 0;
  std::memcpy(&value, &member, sizeof value);
  return value;
}

auto samplerFilterOf(int value, bool minification)
    -> std::optional<std::pair<asset::SamplerFilter, asset::SamplerMipmapMode>> {
  using Filter = asset::SamplerFilter;
  using Mip = asset::SamplerMipmapMode;
  switch (value) {
  case cgltf_filter_type_nearest_mipmap_nearest:
  case cgltf_filter_type_linear_mipmap_nearest:
  case cgltf_filter_type_nearest_mipmap_linear:
  case cgltf_filter_type_linear_mipmap_linear:
    if (!minification)
      return std::nullopt; // a magnification filter is nearest or linear only
    break;
  default:
    break;
  }
  switch (value) {
  case cgltf_filter_type_undefined:
    // The glTF leaves an undefined filter to the implementation: the SamplerData defaults.
    return std::pair{Filter::linear, minification ? Mip::linear : Mip::none};
  case cgltf_filter_type_nearest:
    return std::pair{Filter::nearest, Mip::none};
  case cgltf_filter_type_linear:
    return std::pair{Filter::linear, Mip::none};
  case cgltf_filter_type_nearest_mipmap_nearest:
    return std::pair{Filter::nearest, Mip::nearest};
  case cgltf_filter_type_linear_mipmap_nearest:
    return std::pair{Filter::linear, Mip::nearest};
  case cgltf_filter_type_nearest_mipmap_linear:
    return std::pair{Filter::nearest, Mip::linear};
  case cgltf_filter_type_linear_mipmap_linear:
    return std::pair{Filter::linear, Mip::linear};
  default:
    return std::nullopt;
  }
}

auto samplerWrapOf(int value) -> std::optional<asset::SamplerWrap> {
  switch (value) {
  case cgltf_wrap_mode_repeat:
    return asset::SamplerWrap::repeat;
  case cgltf_wrap_mode_mirrored_repeat:
    return asset::SamplerWrap::mirrored_repeat;
  case cgltf_wrap_mode_clamp_to_edge:
    return asset::SamplerWrap::clamp_to_edge;
  default:
    return std::nullopt;
  }
}

auto topologyOf(cgltf_primitive_type type) -> std::optional<asset::PrimitiveTopology> {
  switch (type) {
  case cgltf_primitive_type_points:
    return asset::PrimitiveTopology::point_list;
  case cgltf_primitive_type_lines:
    return asset::PrimitiveTopology::line_list;
  case cgltf_primitive_type_triangles:
    return asset::PrimitiveTopology::triangle_list;
  case cgltf_primitive_type_triangle_strip:
    return asset::PrimitiveTopology::triangle_strip;
  default:
    return std::nullopt;
  }
}

auto modeName(cgltf_primitive_type type) -> std::string {
  switch (type) {
  case cgltf_primitive_type_line_loop:
    return "LINE_LOOP";
  case cgltf_primitive_type_line_strip:
    return "LINE_STRIP";
  case cgltf_primitive_type_triangle_fan:
    return "TRIANGLE_FAN";
  default:
    return "unknown mode";
  }
}

// How an image is used decides its color interpretation.
auto interpretationName(asset::ColorInterpretation value) -> std::string_view {
  switch (value) {
  case asset::ColorInterpretation::srgb:
    return "srgb";
  case asset::ColorInterpretation::linear:
    return "linear";
  case asset::ColorInterpretation::normal_map:
    return "normal_map";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------------------

// A loaded buffer: its bytes (the first `byteLength` of what the source gave) and what keeps them
// alive.
struct BufferSlot {
  Bytes bytes{};
  std::vector<std::byte> owned{};
  asset::ResolvedResource resolved{};
};

// Where the elements of an accessor are, after every bound has been checked.
struct Located {
  bool present = false; // false: no buffer view, the elements are zero
  Bytes data{};         // the buffer view bytes
  std::size_t offset = 0;
  std::size_t stride = 0;
};

struct SparseData {
  std::size_t count = 0;
  Bytes indices{};
  Bytes values{};
  std::size_t index_offset = 0;
  std::size_t value_offset = 0;
  cgltf_component_type index_type = cgltf_component_type_invalid;
};

// One kept attribute of a primitive, before it is read.
struct AttributePlan {
  asset::AttributeKey key;
  const cgltf_accessor *accessor = nullptr;
  std::size_t accessor_index = 0;
  std::string name;
};

// What the first use of an image says about its color interpretation.
struct ImageUse {
  std::optional<asset::ColorInterpretation> interpretation;
  bool conflict = false;
  std::string first_use;
};

class Decoder {
public:
  Decoder(const asset::DecodeInput &input, asset::ResourceResolverView resolver,
          ImageDecoderView image_decoder, const GltfDecodeOptions &options)
      : input_(input), resolver_(resolver), image_decoder_(image_decoder), options_(options),
        session_(resolver, options.limits, input.source, options.references),
        reporter_(input.source.name) {}

  auto run() -> terreate::Result<GltfDecodeResult>;

private:
  // Stages (decoder.cpp order).
  auto parse(const Container &container, ParseBudget &budget) -> terreate::Result<ParsedGltf>;
  auto checkHeader() -> terreate::Result<void>;
  auto checkCounts() -> terreate::Result<void>;
  void reportUnsupportedFeatures();
  void reportExtras();
  void reportExtensions();
  void collectExtensions(const cgltf_extension *extensions, std::size_t count,
                         const std::string &where);
  auto buildSamplers() -> terreate::Result<void>;
  auto buildMaterials() -> terreate::Result<void>;
  auto buildMeshes() -> terreate::Result<void>;
  auto buildNodes() -> terreate::Result<void>;
  auto buildTextures() -> terreate::Result<void>;
  auto buildImages() -> terreate::Result<void>;
  void buildSource();

  // Materials.
  auto textureRef(const cgltf_texture_view &view, asset::ColorInterpretation use,
                  const std::string &path) -> terreate::Result<std::optional<asset::TextureRef>>;

  // Meshes and accessors.
  auto buildPrimitive(std::size_t mesh_index, std::size_t primitive_index,
                      const cgltf_primitive &primitive)
      -> terreate::Result<std::optional<asset::ModelPrimitive>>;
  auto planAttributes(const cgltf_primitive &primitive, const std::string &path,
                      std::vector<AttributePlan> &plans) -> bool;
  auto readStream(const AttributePlan &plan, std::size_t vertex_count, const std::string &path)
      -> terreate::Result<asset::AttributeStream>;
  auto readIndices(const cgltf_accessor &accessor, std::size_t vertex_count,
                   const std::string &path) -> terreate::Result<asset::IndexBuffer>;
  auto locate(const cgltf_accessor &accessor, std::size_t element_size, const std::string &path)
      -> terreate::Result<Located>;
  auto locateSparse(const cgltf_accessor &accessor, std::size_t element_size,
                    const std::string &path) -> terreate::Result<std::optional<SparseData>>;
  auto viewBytes(const cgltf_buffer_view *view, const std::string &path) -> terreate::Result<Bytes>;
  auto bufferBytes(std::size_t index, const std::string &path) -> terreate::Result<Bytes>;
  auto charge(std::uint64_t bytes, const std::string &path) -> terreate::Result<void>;

  // Resources.
  auto loadExternal(const std::string &uri, asset::ResourceKind kind,
                    std::optional<std::uint64_t> declared_size, const std::string &path)
      -> terreate::Result<asset::ResolvedResource>;
  auto loadDataUri(const char *uri, const std::string &path)
      -> terreate::Result<std::vector<std::byte>>;
  auto decodeImage(std::size_t index, asset::ColorInterpretation interpretation)
      -> terreate::Result<asset::ImageData>;

  const asset::DecodeInput &input_;
  asset::ResourceResolverView resolver_;
  ImageDecoderView image_decoder_;
  const GltfDecodeOptions &options_;
  asset::ResolveSession session_;
  Reporter reporter_;

  const cgltf_data *gltf_ = nullptr;
  Container container_{};
  asset::ModelData model_{};
  GltfSourceInfo native_{};
  std::map<std::size_t, BufferSlot> buffers_;
  std::vector<std::optional<std::size_t>> model_mesh_of_;
  std::vector<ImageUse> image_use_;
  std::uint64_t decoded_bytes_ = 0;
  std::uint64_t embedded_bytes_ = 0;
  std::map<std::string, std::string> extension_names_;
  std::size_t extension_overflow_ = 0;
};

// ---------------------------------------------------------------------------------------
// Parse and header
// ---------------------------------------------------------------------------------------

auto Decoder::parse(const Container &container, ParseBudget &budget)
    -> terreate::Result<ParsedGltf> {
  cgltf_options options{};
  options.type = container.kind == GltfContainer::glb ? cgltf_file_type_glb : cgltf_file_type_gltf;
  options.memory.alloc_func = &budgetAllocate;
  options.memory.free_func = &budgetFree;
  options.memory.user_data = &budget;

  cgltf_data *raw = nullptr;
  const cgltf_result result =
      cgltf_parse(&options, container.whole.data(), container.whole.size(), &raw);
  ParsedGltf parsed{raw};
  switch (result) {
  case cgltf_result_success:
    break;
  case cgltf_result_out_of_memory:
    return fail(GltfDecodeError::out_of_memory,
                "the parser's memory budget or an allocation failed");
  case cgltf_result_legacy_gltf:
    return fail(GltfDecodeError::unsupported_version, "the file is a glTF 1.0 document");
  case cgltf_result_invalid_json:
    return fail(GltfDecodeError::malformed_gltf, "the JSON text is not valid");
  case cgltf_result_data_too_short:
    return fail(GltfDecodeError::malformed_gltf, "the input is truncated");
  case cgltf_result_unknown_format:
    return fail(GltfDecodeError::malformed_gltf, "the input is not a glTF 2.0 container");
  default:
    return fail(GltfDecodeError::malformed_gltf,
                "the JSON is not a valid glTF document (a bad index or a wrong value)");
  }
  if (!parsed)
    return fail(GltfDecodeError::malformed_gltf, "the parser returned no document");
  return parsed;
}

auto Decoder::checkHeader() -> terreate::Result<void> {
  const cgltf_asset &header = gltf_->asset;
  if (header.version == nullptr)
    return fail(GltfDecodeError::unsupported_version, "asset.version is missing");
  const std::string_view version{header.version};
  if (version.size() < 2 || version[0] != '2' || version[1] != '.')
    return fail(GltfDecodeError::unsupported_version,
                "asset.version is '" + std::string{version} + "', not 2.x");
  if (header.min_version != nullptr) {
    const std::string_view minimum{header.min_version};
    // Anything above 2.0 asks for features this adapter cannot know.
    if (minVersionAbove20(minimum))
      return fail(GltfDecodeError::unsupported_version,
                  "asset.minVersion is '" + std::string{minimum} + "', above 2.0");
  }

  native_.container = container_.kind;
  native_.version = std::string{version};
  native_.min_version = text(header.min_version);
  native_.generator = text(header.generator);
  native_.copyright = text(header.copyright);
  for (std::size_t i = 0; i < gltf_->extensions_used_count; ++i)
    native_.extensions_used.push_back(text(gltf_->extensions_used[i]));

  if (gltf_->extensions_required_count > 0) {
    std::string names;
    for (std::size_t i = 0; i < gltf_->extensions_required_count && i < 8; ++i) {
      if (!names.empty())
        names += ", ";
      names += text(gltf_->extensions_required[i]);
    }
    if (gltf_->extensions_required_count > 8)
      names += ", ...";
    return fail(GltfDecodeError::unsupported_required_extension,
                "extensionsRequired lists " + names + "; this adapter implements no extension");
  }
  return {};
}

auto Decoder::checkCounts() -> terreate::Result<void> {
  const asset::DecodeLimits &limits = options_.limits;
  if (gltf_->nodes_count > limits.max_nodes)
    return fail(asset::DecodeError::count_exceeded, std::to_string(gltf_->nodes_count) +
                                                        " nodes, the limit is " +
                                                        std::to_string(limits.max_nodes));
  if (gltf_->meshes_count > limits.max_meshes)
    return fail(asset::DecodeError::count_exceeded, std::to_string(gltf_->meshes_count) +
                                                        " meshes, the limit is " +
                                                        std::to_string(limits.max_meshes));
  std::uint64_t primitives = 0;
  for (std::size_t i = 0; i < gltf_->meshes_count; ++i) {
    const cgltf_mesh &mesh = gltf_->meshes[i];
    if (mesh.primitives_count == 0)
      return fail(GltfDecodeError::malformed_gltf, indexPath("meshes", i) + " has no primitives");
    primitives += mesh.primitives_count;
    if (primitives > limits.max_primitives)
      return fail(asset::DecodeError::count_exceeded,
                  "more than " + std::to_string(limits.max_primitives) + " primitives, the limit");
  }

  // The hierarchy: depth from the parentless nodes down, iteratively. A node that is never
  // reached is on a cycle (every node has at most one parent).
  const std::size_t count = gltf_->nodes_count;
  std::vector<std::uint32_t> depth(count, 0);
  std::vector<std::size_t> queue;
  queue.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (gltf_->nodes[i].parent == nullptr) {
      depth[i] = 1;
      queue.push_back(i);
    }
  }
  for (std::size_t head = 0; head < queue.size(); ++head) {
    const std::size_t current = queue[head];
    const cgltf_node &node = gltf_->nodes[current];
    for (std::size_t c = 0; c < node.children_count; ++c) {
      const auto child = static_cast<std::size_t>(node.children[c] - gltf_->nodes);
      depth[child] = depth[current] + 1;
      if (depth[child] > limits.max_hierarchy_depth)
        return fail(asset::DecodeError::hierarchy_too_deep,
                    "the node hierarchy is deeper than " +
                        std::to_string(limits.max_hierarchy_depth));
      queue.push_back(child);
    }
  }
  if (queue.size() != count)
    return fail(GltfDecodeError::malformed_gltf, "the node hierarchy contains a cycle");
  return {};
}

// ---------------------------------------------------------------------------------------
// Reporting what is not represented
// ---------------------------------------------------------------------------------------

void Decoder::collectExtensions(const cgltf_extension *extensions, std::size_t count,
                                const std::string &where) {
  for (std::size_t i = 0; i < count; ++i) {
    const std::string name = text(extensions[i].name);
    if (extension_names_.contains(name))
      continue;
    if (extension_names_.size() >= extension_name_cap) {
      ++extension_overflow_;
      continue;
    }
    extension_names_.emplace(name, where);
  }
}

template <typename Object>
void countExtras(const Object *objects, std::size_t count, std::string_view list,
                 Reporter &reporter) {
  std::size_t found = 0;
  std::size_t first = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (objects[i].extras.data != nullptr) {
      if (found == 0)
        first = i;
      ++found;
    }
  }
  if (found > 0)
    reporter.unsupported(Finding{"extras", std::string{list},
                                 std::to_string(found) + " object(s) carry extras; first is " +
                                     indexPath(list, first),
                                 indexPath(list, first)});
}

void Decoder::reportExtras() {
  const cgltf_data &d = *gltf_;
  if (d.extras.data != nullptr)
    reporter_.unsupported(Finding{"extras", "root", "the glTF root carries extras", "/"});
  if (d.asset.extras.data != nullptr)
    reporter_.unsupported(Finding{"extras", "asset", "asset carries extras", "/asset"});
  countExtras(d.accessors, d.accessors_count, "accessors", reporter_);
  countExtras(d.buffer_views, d.buffer_views_count, "bufferViews", reporter_);
  countExtras(d.buffers, d.buffers_count, "buffers", reporter_);
  countExtras(d.images, d.images_count, "images", reporter_);
  countExtras(d.textures, d.textures_count, "textures", reporter_);
  countExtras(d.samplers, d.samplers_count, "samplers", reporter_);
  countExtras(d.materials, d.materials_count, "materials", reporter_);
  countExtras(d.meshes, d.meshes_count, "meshes", reporter_);
  countExtras(d.skins, d.skins_count, "skins", reporter_);
  countExtras(d.cameras, d.cameras_count, "cameras", reporter_);
  countExtras(d.lights, d.lights_count, "extensions/KHR_lights_punctual/lights", reporter_);
  countExtras(d.nodes, d.nodes_count, "nodes", reporter_);
  countExtras(d.scenes, d.scenes_count, "scenes", reporter_);
  countExtras(d.animations, d.animations_count, "animations", reporter_);
  std::size_t primitive_extras = 0;
  for (std::size_t i = 0; i < d.meshes_count; ++i) {
    for (std::size_t j = 0; j < d.meshes[i].primitives_count; ++j) {
      if (d.meshes[i].primitives[j].extras.data != nullptr)
        ++primitive_extras;
    }
  }
  if (primitive_extras > 0)
    reporter_.unsupported(Finding{"extras", "primitives",
                                  std::to_string(primitive_extras) + " primitive(s) carry extras",
                                  "/meshes"});
}

void Decoder::reportExtensions() {
  const cgltf_data &d = *gltf_;
  collectExtensions(d.data_extensions, d.data_extensions_count, "root");
  collectExtensions(d.asset.extensions, d.asset.extensions_count, "asset");
  const auto scan = [this](const auto *objects, std::size_t count, std::string_view list) {
    for (std::size_t i = 0; i < count; ++i)
      collectExtensions(objects[i].extensions, objects[i].extensions_count, indexPath(list, i));
  };
  scan(d.accessors, d.accessors_count, "accessors");
  scan(d.buffer_views, d.buffer_views_count, "bufferViews");
  scan(d.buffers, d.buffers_count, "buffers");
  scan(d.images, d.images_count, "images");
  scan(d.textures, d.textures_count, "textures");
  scan(d.samplers, d.samplers_count, "samplers");
  scan(d.materials, d.materials_count, "materials");
  scan(d.meshes, d.meshes_count, "meshes");
  scan(d.skins, d.skins_count, "skins");
  scan(d.cameras, d.cameras_count, "cameras");
  scan(d.nodes, d.nodes_count, "nodes");
  scan(d.scenes, d.scenes_count, "scenes");
  scan(d.animations, d.animations_count, "animations");
  for (std::size_t i = 0; i < d.meshes_count; ++i) {
    for (std::size_t j = 0; j < d.meshes[i].primitives_count; ++j) {
      const cgltf_primitive &primitive = d.meshes[i].primitives[j];
      collectExtensions(primitive.extensions, primitive.extensions_count,
                        indexPath("meshes", i) + "/primitives" + "[" + std::to_string(j) + "]");
    }
  }

  // extensionsUsed names every extension the file uses; the adapter implements none of them.
  for (const std::string &name : native_.extensions_used)
    extension_names_.try_emplace(name, "extensionsUsed");
  for (const auto &[name, where] : extension_names_)
    reporter_.unsupported(
        Finding{"extension", name, "the extension is not implemented (" + where + ")", where});
  if (extension_overflow_ > 0)
    reporter_.unsupported(Finding{
        "extension", "(further)",
        std::to_string(extension_overflow_) + " further extension names were not collected", "/"});
}

void Decoder::reportUnsupportedFeatures() {
  const cgltf_data &d = *gltf_;
  for (std::size_t i = 0; i < d.animations_count; ++i) {
    const cgltf_animation &animation = d.animations[i];
    reporter_.unsupported(Finding{"animation", text(animation.name),
                                  indexPath("animations", i) + " with " +
                                      std::to_string(animation.channels_count) + " channel(s)",
                                  indexPath("animations", i)});
  }
  for (std::size_t i = 0; i < d.skins_count; ++i)
    reporter_.unsupported(Finding{"skin", text(d.skins[i].name),
                                  indexPath("skins", i) + " with " +
                                      std::to_string(d.skins[i].joints_count) + " joint(s)",
                                  indexPath("skins", i)});
  for (std::size_t i = 0; i < d.cameras_count; ++i)
    reporter_.unsupported(Finding{"camera", text(d.cameras[i].name),
                                  indexPath("cameras", i) + " is not kept",
                                  indexPath("cameras", i)});
  for (std::size_t i = 0; i < d.lights_count; ++i)
    reporter_.unsupported(Finding{"light", text(d.lights[i].name),
                                  "KHR_lights_punctual light " + std::to_string(i) + " is not kept",
                                  "/extensions/KHR_lights_punctual/lights/" + std::to_string(i)});
  for (std::size_t i = 0; i < d.nodes_count; ++i) {
    const cgltf_node &node = d.nodes[i];
    if (node.weights_count > 0)
      reporter_.unsupported(Finding{"morph-target", text(node.name),
                                    indexPath("nodes", i) + " sets morph target weights",
                                    indexPath("nodes", i)});
    if (node.has_mesh_gpu_instancing)
      reporter_.unsupported(
          Finding{"mesh-gpu-instancing", text(node.name),
                  indexPath("nodes", i) + " uses EXT_mesh_gpu_instancing; the mesh is placed once",
                  indexPath("nodes", i)});
  }
  for (std::size_t i = 0; i < d.meshes_count; ++i) {
    if (d.meshes[i].weights_count > 0)
      reporter_.unsupported(Finding{"morph-target", text(d.meshes[i].name),
                                    indexPath("meshes", i) + " sets default morph target weights",
                                    indexPath("meshes", i)});
  }
  if (d.variants_count > 0)
    reporter_.unsupported(Finding{"material-variants", "KHR_materials_variants",
                                  std::to_string(d.variants_count) + " variant(s) are not kept",
                                  "/extensions/KHR_materials_variants"});
  for (std::size_t i = 0; i < d.buffer_views_count; ++i) {
    if (d.buffer_views[i].has_meshopt_compression)
      reporter_.unsupported(
          Finding{"compression", "EXT_meshopt_compression",
                  indexPath("bufferViews", i) + " is meshopt-compressed; its fallback data is read",
                  indexPath("bufferViews", i)});
  }
  reportExtensions();
  reportExtras();
}

// ---------------------------------------------------------------------------------------
// Resources: external references and data URIs
// ---------------------------------------------------------------------------------------

auto Decoder::charge(std::uint64_t bytes, const std::string &path) -> terreate::Result<void> {
  const auto total = checkedAdd(decoded_bytes_, bytes);
  if (!total || *total > options_.max_decoded_bytes)
    return fail(GltfDecodeError::decoded_size_exceeded,
                path + ": the decoded size would exceed " +
                    std::to_string(options_.max_decoded_bytes) + " bytes");
  decoded_bytes_ = *total;
  return {};
}

auto Decoder::loadExternal(const std::string &uri, asset::ResourceKind kind,
                           std::optional<std::uint64_t> declared_size, const std::string &path)
    -> terreate::Result<asset::ResolvedResource> {
  if (!resolver_)
    return fail(GltfDecodeError::missing_resolver,
                path + " refers to '" + uri + "' and no resolver is bound");
  asset::ResourceRequest request = session_.makeRequest(uri, kind);
  request.declared_size = declared_size;
  auto resolved = session_.resolve(std::move(request), 0);
  if (!resolved)
    return std::unexpected(std::move(resolved.error()));
  // The session knows resolved bytes only; the embedded data URIs count toward the same total.
  if (embedded_bytes_ > 0 &&
      session_.resolvedBytes() >
          options_.limits.max_total_resource_bytes -
              std::min(embedded_bytes_, options_.limits.max_total_resource_bytes))
    return fail(asset::ResolveError::limit_exceeded,
                path + ": resolved and embedded resources exceed max_total_resource_bytes");
  return resolved;
}

auto Decoder::loadDataUri(const char *uri, const std::string &path)
    -> terreate::Result<std::vector<std::byte>> {
  const auto payload = parseDataUri(uri);
  if (!payload)
    return fail(GltfDecodeError::invalid_buffer,
                path + ": the data URI is not `data:...;base64,<payload>` with valid base64");
  const asset::DecodeLimits &limits = options_.limits;
  const std::uint64_t used = embedded_bytes_ + session_.resolvedBytes();
  if (payload->decoded_size > limits.max_resource_bytes || used > limits.max_total_resource_bytes ||
      payload->decoded_size > limits.max_total_resource_bytes - used)
    return fail(asset::ResolveError::limit_exceeded,
                path + ": the embedded data URI is larger than the resource limits allow");
  embedded_bytes_ += payload->decoded_size;
  reporter_.note(Note{
      asset::DecodeSeverity::info, "gltf.data_uri",
      "decoded an embedded data URI of " + std::to_string(payload->decoded_size) + " bytes", path});
  return decodeBase64(*payload);
}

auto Decoder::bufferBytes(std::size_t index, const std::string &path) -> terreate::Result<Bytes> {
  if (const auto found = buffers_.find(index); found != buffers_.end())
    return found->second.bytes;
  const cgltf_buffer &buffer = gltf_->buffers[index];
  const std::string where = indexPath("buffers", index);
  BufferSlot slot;
  Bytes bytes;
  if (buffer.uri == nullptr) {
    if (index != 0 || container_.kind != GltfContainer::glb || !container_.has_bin)
      return fail(GltfDecodeError::invalid_buffer,
                  where + " has no uri and is not the GLB BIN chunk (used by " + path + ")");
    bytes = container_.bin;
  } else if (options_.embedded_data_uris && isDataUri(buffer.uri)) {
    auto decoded = loadDataUri(buffer.uri, where + ".uri");
    if (!decoded)
      return std::unexpected(std::move(decoded.error()));
    slot.owned = std::move(*decoded);
    bytes = slot.owned;
  } else {
    auto resolved = loadExternal(buffer.uri, asset::ResourceKind::buffer, buffer.size, where);
    if (!resolved)
      return std::unexpected(std::move(resolved.error()));
    slot.resolved = std::move(*resolved);
    bytes = slot.resolved.bytes();
  }
  if (bytes.size() < buffer.size)
    return fail(GltfDecodeError::invalid_buffer,
                where + " declares " + std::to_string(buffer.size) + " bytes but has " +
                    std::to_string(bytes.size()));
  slot.bytes = bytes.first(buffer.size);
  // Moving a slot moves its vector and its shared storage without moving their bytes, so the
  // span stays valid.
  return buffers_.emplace(index, std::move(slot)).first->second.bytes;
}

auto Decoder::viewBytes(const cgltf_buffer_view *view, const std::string &path)
    -> terreate::Result<Bytes> {
  if (view == nullptr || view->buffer == nullptr)
    return fail(GltfDecodeError::invalid_accessor, path + ": the buffer view is missing");
  const auto buffer_index = static_cast<std::size_t>(view->buffer - gltf_->buffers);
  auto buffer = bufferBytes(buffer_index, path);
  if (!buffer)
    return std::unexpected(std::move(buffer.error()));
  const auto end = checkedAdd(view->offset, view->size);
  if (!end || *end > buffer->size())
    return fail(GltfDecodeError::invalid_accessor,
                path + ": the buffer view (offset " + std::to_string(view->offset) + ", size " +
                    std::to_string(view->size) + ") does not fit its buffer of " +
                    std::to_string(buffer->size()) + " bytes");
  return buffer->subspan(static_cast<std::size_t>(view->offset),
                         static_cast<std::size_t>(view->size));
}

// ---------------------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------------------

auto Decoder::locate(const cgltf_accessor &accessor, std::size_t element_size,
                     const std::string &path) -> terreate::Result<Located> {
  Located located;
  if (accessor.count == 0)
    return fail(GltfDecodeError::invalid_accessor, path + ": the accessor has no elements");
  if (accessor.buffer_view == nullptr)
    return located;
  auto view = viewBytes(accessor.buffer_view, path);
  if (!view)
    return std::unexpected(std::move(view.error()));
  const std::uint64_t byte_stride = accessor.buffer_view->stride;
  if (byte_stride != 0 && byte_stride < element_size)
    return fail(GltfDecodeError::invalid_accessor,
                path + ": the byteStride " + std::to_string(byte_stride) +
                    " is smaller than the element size " + std::to_string(element_size));
  const std::uint64_t stride = byte_stride != 0 ? byte_stride : element_size;
  // offset + stride * (count - 1) + element_size must fit the view.
  const auto span = checkedMul(stride, accessor.count - 1);
  const auto last = span ? checkedAdd(accessor.offset, *span) : std::nullopt;
  const auto end = last ? checkedAdd(*last, element_size) : std::nullopt;
  if (!end || *end > view->size())
    return fail(GltfDecodeError::invalid_accessor,
                path + ": " + std::to_string(accessor.count) + " element(s) at offset " +
                    std::to_string(accessor.offset) + " and stride " + std::to_string(stride) +
                    " do not fit the buffer view of " + std::to_string(view->size()) + " bytes");
  located.present = true;
  located.data = *view;
  located.offset = static_cast<std::size_t>(accessor.offset);
  located.stride = static_cast<std::size_t>(stride);
  return located;
}

auto Decoder::locateSparse(const cgltf_accessor &accessor, std::size_t element_size,
                           const std::string &path) -> terreate::Result<std::optional<SparseData>> {
  if (!accessor.is_sparse)
    return std::optional<SparseData>{};
  const cgltf_accessor_sparse &sparse = accessor.sparse;
  if (!isUnsignedIntegerComponent(sparse.indices_component_type))
    return fail(GltfDecodeError::invalid_accessor,
                path + ": the sparse indices are not unsigned integers");
  if (sparse.count > accessor.count)
    return fail(GltfDecodeError::invalid_accessor,
                path + ": the sparse count " + std::to_string(sparse.count) +
                    " is larger than the accessor count " + std::to_string(accessor.count));
  SparseData data;
  data.count = static_cast<std::size_t>(sparse.count);
  data.index_type = sparse.indices_component_type;
  data.index_offset = 0;

  auto indices = viewBytes(sparse.indices_buffer_view, path + ".sparse.indices");
  if (!indices)
    return std::unexpected(std::move(indices.error()));
  auto values = viewBytes(sparse.values_buffer_view, path + ".sparse.values");
  if (!values)
    return std::unexpected(std::move(values.error()));
  const std::uint64_t index_size = componentByteSize(sparse.indices_component_type);
  const auto index_span = checkedMul(index_size, sparse.count);
  const auto index_end =
      index_span ? checkedAdd(sparse.indices_byte_offset, *index_span) : std::nullopt;
  const auto value_span = checkedMul(element_size, sparse.count);
  const auto value_end =
      value_span ? checkedAdd(sparse.values_byte_offset, *value_span) : std::nullopt;
  if (!index_end || *index_end > indices->size() || !value_end || *value_end > values->size())
    return fail(GltfDecodeError::invalid_accessor,
                path + ": the sparse indices or values do not fit their buffer views");
  data.indices = *indices;
  data.values = *values;
  data.index_offset = static_cast<std::size_t>(sparse.indices_byte_offset);
  data.value_offset = static_cast<std::size_t>(sparse.values_byte_offset);
  return std::optional<SparseData>{data};
}

auto Decoder::readStream(const AttributePlan &plan, std::size_t vertex_count,
                         const std::string &path) -> terreate::Result<asset::AttributeStream> {
  const cgltf_accessor &accessor = *plan.accessor;
  const std::size_t components = componentsOf(accessor.type);
  const std::size_t component_size = componentByteSize(accessor.component_type);
  const bool is_float = accessor.component_type == cgltf_component_type_r_32f;
  const bool normalized_integer =
      accessor.normalized != 0 && component_size != 0 && component_size < 4 && !is_float;
  if (components == 0 || component_size == 0 || (!is_float && !normalized_integer))
    return fail(GltfDecodeError::invalid_accessor,
                path + ": " + plan.name +
                    " must be float or a normalized 8/16-bit integer (accessor " +
                    std::to_string(plan.accessor_index) + ")");
  if (accessor.count != vertex_count)
    return fail(GltfDecodeError::invalid_accessor,
                path + ": " + plan.name + " has " + std::to_string(accessor.count) +
                    " element(s), the position accessor has " + std::to_string(vertex_count));

  const std::size_t element_size = components * component_size;
  const std::string where = indexPath("accessors", plan.accessor_index);
  const std::uint64_t output_bytes = static_cast<std::uint64_t>(vertex_count) * components * 4U;
  if (auto charged = charge(output_bytes, where); !charged)
    return std::unexpected(std::move(charged.error()));

  auto located = locate(accessor, element_size, where);
  if (!located)
    return std::unexpected(std::move(located.error()));
  auto sparse = locateSparse(accessor, element_size, where);
  if (!sparse)
    return std::unexpected(std::move(sparse.error()));

  asset::AttributeStream stream;
  stream.key = plan.key;
  stream.format = formatOf(components);
  stream.count = vertex_count;
  stream.data.assign(static_cast<std::size_t>(output_bytes), std::byte{0});

  const auto convert = [&](const std::byte *source, std::size_t vertex) {
    for (std::size_t c = 0; c < components; ++c) {
      const float value =
          readComponent(source + c * component_size, accessor.component_type, normalized_integer);
      std::memcpy(stream.data.data() + (vertex * components + c) * sizeof(float), &value,
                  sizeof value);
    }
  };
  if (located->present) {
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex)
      convert(located->data.data() + located->offset + vertex * located->stride, vertex);
  }
  if (*sparse) {
    const SparseData &data = **sparse;
    const std::size_t index_size = componentByteSize(data.index_type);
    for (std::size_t k = 0; k < data.count; ++k) {
      const std::uint64_t target =
          readUnsigned(data.indices.data() + data.index_offset + k * index_size, data.index_type);
      if (target >= vertex_count)
        return fail(GltfDecodeError::invalid_accessor,
                    where + ": a sparse index (" + std::to_string(target) +
                        ") is not below the count " + std::to_string(vertex_count));
      convert(data.values.data() + data.value_offset + k * element_size,
              static_cast<std::size_t>(target));
    }
  }
  if (normalized_integer)
    reporter_.note(Note{asset::DecodeSeverity::info, "gltf.attribute.normalized_converted",
                        plan.name + " is a normalized integer accessor, converted to float32",
                        path});
  return stream;
}

auto Decoder::readIndices(const cgltf_accessor &accessor, std::size_t vertex_count,
                          const std::string &path) -> terreate::Result<asset::IndexBuffer> {
  const std::string where =
      indexPath("accessors", static_cast<std::size_t>(&accessor - gltf_->accessors));
  if (accessor.type != cgltf_type_scalar || !isUnsignedIntegerComponent(accessor.component_type))
    return fail(GltfDecodeError::invalid_accessor,
                path + ": the indices (" + where +
                    ") must be a scalar unsigned byte, short or int accessor");
  if (accessor.count == 0)
    return fail(GltfDecodeError::invalid_accessor, path + ": the index accessor is empty");
  const std::size_t component_size = componentByteSize(accessor.component_type);
  const bool wide = accessor.component_type == cgltf_component_type_r_32u;
  if (accessor.count > std::numeric_limits<std::size_t>::max() / 4)
    return fail(GltfDecodeError::decoded_size_exceeded, where + ": too many indices");
  const auto count = static_cast<std::size_t>(accessor.count);
  if (auto charged = charge(static_cast<std::uint64_t>(count) * (wide ? 4U : 6U), where); !charged)
    return std::unexpected(std::move(charged.error()));

  auto located = locate(accessor, component_size, where);
  if (!located)
    return std::unexpected(std::move(located.error()));
  auto sparse = locateSparse(accessor, component_size, where);
  if (!sparse)
    return std::unexpected(std::move(sparse.error()));

  std::vector<std::uint32_t> values(count, 0);
  if (located->present) {
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint64_t value = readUnsigned(
          located->data.data() + located->offset + i * located->stride, accessor.component_type);
      if (value > std::numeric_limits<std::uint32_t>::max())
        return fail(GltfDecodeError::invalid_accessor, where + ": an index does not fit 32 bits");
      values[i] = static_cast<std::uint32_t>(value);
    }
  }
  if (*sparse) {
    const SparseData &data = **sparse;
    const std::size_t index_size = componentByteSize(data.index_type);
    for (std::size_t k = 0; k < data.count; ++k) {
      const std::uint64_t target =
          readUnsigned(data.indices.data() + data.index_offset + k * index_size, data.index_type);
      if (target >= count)
        return fail(GltfDecodeError::invalid_accessor,
                    where + ": a sparse index is not below the count");
      values[static_cast<std::size_t>(target)] = static_cast<std::uint32_t>(readUnsigned(
          data.values.data() + data.value_offset + k * component_size, accessor.component_type));
    }
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (values[i] >= vertex_count)
      return fail(GltfDecodeError::invalid_accessor,
                  where + ": index " + std::to_string(i) + " is " + std::to_string(values[i]) +
                      ", not below the vertex count " + std::to_string(vertex_count));
  }
  if (wide)
    return asset::IndexBuffer{std::move(values)};
  if (accessor.component_type == cgltf_component_type_r_8u)
    reporter_.note(Note{asset::DecodeSeverity::info, "gltf.index.widened",
                        "unsigned byte indices were widened to 16 bits", path});
  std::vector<std::uint16_t> narrow(count);
  std::transform(values.begin(), values.end(), narrow.begin(),
                 [](std::uint32_t value) { return static_cast<std::uint16_t>(value); });
  return asset::IndexBuffer{std::move(narrow)};
}

// ---------------------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------------------

// Chooses the streams of a primitive; the others are reported. Returns false when there is no
// position stream.
auto Decoder::planAttributes(const cgltf_primitive &primitive, const std::string &path,
                             std::vector<AttributePlan> &plans) -> bool {
  bool has_position = false;
  for (std::size_t i = 0; i < primitive.attributes_count; ++i) {
    const cgltf_attribute &attribute = primitive.attributes[i];
    AttributePlan plan;
    plan.accessor = attribute.data;
    plan.accessor_index = static_cast<std::size_t>(attribute.data - gltf_->accessors);
    plan.name = text(attribute.name);
    const auto set = static_cast<std::uint32_t>(attribute.index < 0 ? 0 : attribute.index);
    bool kept = false;
    switch (attribute.type) {
    case cgltf_attribute_type_position:
      kept = set == 0;
      plan.key = {asset::AttributeSemantic::position, 0, {}};
      has_position = has_position || kept;
      break;
    case cgltf_attribute_type_normal:
      kept = set == 0;
      plan.key = {asset::AttributeSemantic::normal, 0, {}};
      break;
    case cgltf_attribute_type_tangent:
      kept = set == 0;
      plan.key = {asset::AttributeSemantic::tangent, 0, {}};
      break;
    case cgltf_attribute_type_texcoord:
      kept = true;
      plan.key = {asset::AttributeSemantic::texcoord, set, {}};
      break;
    case cgltf_attribute_type_color:
      kept = true;
      plan.key = {asset::AttributeSemantic::color, set, {}};
      break;
    default:
      break;
    }
    if (kept) {
      plans.push_back(std::move(plan));
    } else {
      // JOINTS_n and WEIGHTS_n belong to skinning, custom (`_NAME`) attributes have no
      // defined meaning: neither is kept (MeshData could hold them as `custom` float streams,
      // but their integer data would be converted without a defined rule).
      reporter_.unsupported(Finding{"attribute", plan.name,
                                    path + " attribute " + plan.name + " is not kept",
                                    path + "/attributes/" + plan.name});
    }
  }
  return has_position;
}

auto Decoder::buildPrimitive(std::size_t mesh_index, std::size_t primitive_index,
                             const cgltf_primitive &primitive)
    -> terreate::Result<std::optional<asset::ModelPrimitive>> {
  const std::string path =
      indexPath("meshes", mesh_index) + ".primitives[" + std::to_string(primitive_index) + "]";
  const std::string pointer =
      "/meshes/" + std::to_string(mesh_index) + "/primitives/" + std::to_string(primitive_index);

  const auto topology = topologyOf(primitive.type);
  if (!topology) {
    reporter_.unsupported(Finding{"primitive-mode", modeName(primitive.type),
                                  path + " uses " + modeName(primitive.type) +
                                      "; MeshData has no such topology, the primitive is skipped",
                                  pointer});
    return std::optional<asset::ModelPrimitive>{};
  }
  if (primitive.has_draco_mesh_compression) {
    reporter_.unsupported(Finding{"compression", "KHR_draco_mesh_compression",
                                  path + " is Draco-compressed, the primitive is skipped",
                                  pointer});
    return std::optional<asset::ModelPrimitive>{};
  }

  std::vector<AttributePlan> plans;
  if (!planAttributes(primitive, path, plans)) {
    reporter_.unsupported(Finding{"primitive", "no-position",
                                  path + " has no POSITION attribute, the primitive is skipped",
                                  pointer});
    return std::optional<asset::ModelPrimitive>{};
  }
  if (primitive.targets_count > 0)
    reporter_.unsupported(Finding{"morph-target", path,
                                  std::to_string(primitive.targets_count) +
                                      " morph target(s) are not kept; the base geometry is",
                                  pointer});

  // The position accessor defines the vertex count; every other stream must match it.
  const AttributePlan *position = nullptr;
  for (const AttributePlan &plan : plans) {
    if (plan.key.semantic == asset::AttributeSemantic::position)
      position = &plan;
  }
  if (position == nullptr || position->accessor->count == 0 ||
      position->accessor->count > std::numeric_limits<std::uint32_t>::max())
    return fail(GltfDecodeError::invalid_accessor,
                path + ": the position accessor is empty or has more than 2^32 - 1 elements");
  const auto vertex_count = static_cast<std::size_t>(position->accessor->count);

  asset::MeshData mesh;
  mesh.topology = *topology;
  for (const AttributePlan &plan : plans) {
    const auto semantic = plan.key.semantic;
    const std::size_t components = componentsOf(plan.accessor->type);
    const bool shape_ok =
        (semantic == asset::AttributeSemantic::position && components == 3) ||
        (semantic == asset::AttributeSemantic::normal && components == 3) ||
        (semantic == asset::AttributeSemantic::tangent && components == 4) ||
        (semantic == asset::AttributeSemantic::texcoord && components == 2) ||
        (semantic == asset::AttributeSemantic::color && (components == 3 || components == 4));
    if (!shape_ok)
      return fail(GltfDecodeError::invalid_accessor,
                  path + ": " + plan.name + " has a type its semantic does not allow");
    auto stream = readStream(plan, vertex_count, pointer);
    if (!stream)
      return std::unexpected(std::move(stream.error()));
    mesh.attributes.push_back(std::move(*stream));
  }
  if (primitive.indices != nullptr) {
    auto indices = readIndices(*primitive.indices, vertex_count, pointer);
    if (!indices)
      return std::unexpected(std::move(indices.error()));
    mesh.indices = std::move(*indices);
  }
  if (auto bounds = asset::computeBounds(mesh); bounds)
    mesh.bounds = *bounds;
  else
    reporter_.note(Note{asset::DecodeSeverity::warning, "gltf.bounds_not_computed",
                        path + ": the bounds were not computed because a position is not finite",
                        pointer});

  asset::ModelPrimitive out;
  out.mesh = model_.meshes.size();
  if (primitive.material != nullptr)
    out.material = static_cast<std::size_t>(primitive.material - gltf_->materials);
  model_.meshes.push_back(std::move(mesh));
  return std::optional<asset::ModelPrimitive>{out};
}

auto Decoder::buildMeshes() -> terreate::Result<void> {
  model_mesh_of_.assign(gltf_->meshes_count, std::nullopt);
  for (std::size_t i = 0; i < gltf_->meshes_count; ++i) {
    const cgltf_mesh &mesh = gltf_->meshes[i];
    asset::ModelMesh out;
    out.name = text(mesh.name);
    for (std::size_t j = 0; j < mesh.primitives_count; ++j) {
      auto primitive = buildPrimitive(i, j, mesh.primitives[j]);
      if (!primitive)
        return std::unexpected(std::move(primitive.error()));
      if (!*primitive)
        continue;
      out.primitives.push_back(model_.primitives.size());
      model_.primitives.push_back(**primitive);
    }
    if (out.primitives.empty()) {
      reporter_.unsupported(Finding{"mesh", out.name,
                                    indexPath("meshes", i) +
                                        " has no representable primitive; nodes that place it "
                                        "have no mesh",
                                    indexPath("meshes", i)});
      continue;
    }
    model_mesh_of_[i] = model_.model_meshes.size();
    model_.model_meshes.push_back(std::move(out));
  }
  return {};
}

// ---------------------------------------------------------------------------------------
// Samplers, materials, textures, images
// ---------------------------------------------------------------------------------------

auto Decoder::buildSamplers() -> terreate::Result<void> {
  model_.samplers.reserve(gltf_->samplers_count);
  for (std::size_t i = 0; i < gltf_->samplers_count; ++i) {
    const cgltf_sampler &sampler = gltf_->samplers[i];
    const auto min_filter = samplerFilterOf(storedValue(sampler.min_filter), true);
    const auto mag_filter = samplerFilterOf(storedValue(sampler.mag_filter), false);
    const auto wrap_u = samplerWrapOf(storedValue(sampler.wrap_s));
    const auto wrap_v = samplerWrapOf(storedValue(sampler.wrap_t));
    if (!min_filter || !mag_filter || !wrap_u || !wrap_v)
      return fail(GltfDecodeError::invalid_resource,
                  indexPath("samplers", i) +
                      " has a filter or wrap value that is not a glTF value");
    asset::SamplerData out;
    out.min_filter = min_filter->first;
    out.mipmap_mode = min_filter->second;
    out.mag_filter = mag_filter->first;
    out.wrap_u = *wrap_u;
    out.wrap_v = *wrap_v;
    model_.samplers.push_back(out);
  }
  return {};
}

auto Decoder::textureRef(const cgltf_texture_view &view, asset::ColorInterpretation use,
                         const std::string &path)
    -> terreate::Result<std::optional<asset::TextureRef>> {
  if (view.texture == nullptr)
    return std::optional<asset::TextureRef>{};
  if (view.texcoord < 0)
    return fail(GltfDecodeError::malformed_gltf, path + " has a negative texCoord");
  const auto texture = static_cast<std::size_t>(view.texture - gltf_->textures);
  if (view.has_transform)
    reporter_.unsupported(Finding{"texture-transform", path,
                                  path + " uses KHR_texture_transform; the transform is ignored",
                                  path});
  if (view.texture->image != nullptr) {
    const auto image = static_cast<std::size_t>(view.texture->image - gltf_->images);
    ImageUse &image_use = image_use_[image];
    if (!image_use.interpretation) {
      image_use.interpretation = use;
      image_use.first_use = path;
    } else if (*image_use.interpretation != use && !image_use.conflict) {
      image_use.conflict = true;
      reporter_.note(Note{asset::DecodeSeverity::warning, "gltf.image.interpretation_conflict",
                          indexPath("images", image) + " is used as " +
                              std::string{interpretationName(*image_use.interpretation)} + " by " +
                              image_use.first_use + " and as " +
                              std::string{interpretationName(use)} + " by " + path +
                              "; it is decoded once, as the first",
                          indexPath("images", image)});
    }
  }
  return std::optional<asset::TextureRef>{
      asset::TextureRef{texture, static_cast<std::uint32_t>(view.texcoord)}};
}

auto Decoder::buildMaterials() -> terreate::Result<void> {
  image_use_.assign(gltf_->images_count, ImageUse{});
  model_.materials.reserve(gltf_->materials_count);
  for (std::size_t i = 0; i < gltf_->materials_count; ++i) {
    const cgltf_material &material = gltf_->materials[i];
    const std::string path = indexPath("materials", i);
    asset::MaterialData out;
    out.name = text(material.name);
    const cgltf_pbr_metallic_roughness &pbr = material.pbr_metallic_roughness;
    std::copy(std::begin(pbr.base_color_factor), std::end(pbr.base_color_factor),
              out.base_color_factor.begin());
    out.metallic_factor = pbr.metallic_factor;
    out.roughness_factor = pbr.roughness_factor;
    std::copy(std::begin(material.emissive_factor), std::end(material.emissive_factor),
              out.emissive_factor.begin());
    // cgltf sets a texture view's scale to 1 only when the view is in the file.
    if (material.normal_texture.texture != nullptr)
      out.normal_scale = material.normal_texture.scale;
    if (material.occlusion_texture.texture != nullptr)
      out.occlusion_strength = material.occlusion_texture.scale;
    out.alpha_cutoff = material.alpha_cutoff;
    out.double_sided = material.double_sided != 0;
    switch (material.alpha_mode) {
    case cgltf_alpha_mode_opaque:
      out.alpha_mode = asset::MaterialAlphaMode::opaque;
      break;
    case cgltf_alpha_mode_mask:
      out.alpha_mode = asset::MaterialAlphaMode::mask;
      break;
    case cgltf_alpha_mode_blend:
      out.alpha_mode = asset::MaterialAlphaMode::blend;
      break;
    default:
      return fail(GltfDecodeError::malformed_gltf, path + " has an unknown alphaMode");
    }

    using Use = asset::ColorInterpretation;
    struct Slot {
      const cgltf_texture_view *view;
      Use use;
      std::optional<asset::TextureRef> *target;
      const char *name;
    };
    const std::array<Slot, 5> slots{{
        {&pbr.base_color_texture, Use::srgb, &out.base_color_texture, "baseColorTexture"},
        {&pbr.metallic_roughness_texture, Use::linear, &out.metallic_roughness_texture,
         "metallicRoughnessTexture"},
        {&material.normal_texture, Use::normal_map, &out.normal_texture, "normalTexture"},
        {&material.occlusion_texture, Use::linear, &out.occlusion_texture, "occlusionTexture"},
        {&material.emissive_texture, Use::srgb, &out.emissive_texture, "emissiveTexture"},
    }};
    for (const Slot &slot : slots) {
      auto reference = textureRef(*slot.view, slot.use, path + "." + slot.name);
      if (!reference)
        return std::unexpected(std::move(reference.error()));
      *slot.target = *reference;
    }

    struct Flag {
      cgltf_bool present;
      const char *name;
    };
    const std::array<Flag, 13> flags{{
        {material.has_pbr_specular_glossiness, "KHR_materials_pbrSpecularGlossiness"},
        {material.has_clearcoat, "KHR_materials_clearcoat"},
        {material.has_transmission, "KHR_materials_transmission"},
        {material.has_volume, "KHR_materials_volume"},
        {material.has_ior, "KHR_materials_ior"},
        {material.has_specular, "KHR_materials_specular"},
        {material.has_sheen, "KHR_materials_sheen"},
        {material.has_emissive_strength, "KHR_materials_emissive_strength"},
        {material.has_iridescence, "KHR_materials_iridescence"},
        {material.has_diffuse_transmission, "KHR_materials_diffuse_transmission"},
        {material.has_anisotropy, "KHR_materials_anisotropy"},
        {material.has_dispersion, "KHR_materials_dispersion"},
        {material.unlit, "KHR_materials_unlit"},
    }};
    for (const Flag &flag : flags) {
      if (flag.present != 0)
        reporter_.unsupported(Finding{"material-extension", flag.name,
                                      path + " uses " + flag.name + ", which is not applied",
                                      path});
    }
    model_.materials.push_back(std::move(out));
  }
  return {};
}

auto Decoder::buildTextures() -> terreate::Result<void> {
  model_.textures.reserve(gltf_->textures_count);
  for (std::size_t i = 0; i < gltf_->textures_count; ++i) {
    const cgltf_texture &texture = gltf_->textures[i];
    if (texture.image == nullptr)
      return fail(GltfDecodeError::invalid_resource,
                  indexPath("textures", i) +
                      " has no image source (an extension texture source is not supported)");
    asset::TextureData out;
    out.image = static_cast<std::size_t>(texture.image - gltf_->images);
    if (texture.sampler != nullptr)
      out.sampler = static_cast<std::size_t>(texture.sampler - gltf_->samplers);
    model_.textures.push_back(out);
  }
  return {};
}

auto Decoder::decodeImage(std::size_t index, asset::ColorInterpretation interpretation)
    -> terreate::Result<asset::ImageData> {
  const cgltf_image &image = gltf_->images[index];
  const std::string where = indexPath("images", index);
  if (!image_decoder_)
    return fail(GltfDecodeError::missing_image_decoder,
                where + " has to be decoded and no image decoder is bound");

  asset::ResolvedResource resolved;
  std::vector<std::byte> embedded;
  Bytes bytes;
  asset::SourceIdentity identity;
  identity.name = input_.source.name + "#" + where;
  identity.base = input_.source.base;
  if (image.uri != nullptr) {
    if (options_.embedded_data_uris && isDataUri(image.uri)) {
      auto decoded = loadDataUri(image.uri, where + ".uri");
      if (!decoded)
        return std::unexpected(std::move(decoded.error()));
      embedded = std::move(*decoded);
      bytes = embedded;
    } else {
      auto loaded = loadExternal(image.uri, asset::ResourceKind::image, std::nullopt, where);
      if (!loaded)
        return std::unexpected(std::move(loaded.error()));
      resolved = std::move(*loaded);
      bytes = resolved.bytes();
      identity = resolved.identity();
    }
  } else if (image.buffer_view != nullptr) {
    auto view = viewBytes(image.buffer_view, where);
    if (!view)
      return std::unexpected(std::move(view.error()));
    bytes = *view;
  } else {
    return fail(GltfDecodeError::invalid_resource, where + " has neither a uri nor a bufferView");
  }

  ImageDecodeRequest request;
  request.input = asset::DecodeInput{bytes, std::move(identity)};
  request.mime_type = text(image.mime_type);
  request.interpretation = interpretation;
  request.limits = options_.limits;
  terreate::Result<asset::ImageData> decoded =
      std::unexpected(terreate::Error{make_error_code(GltfDecodeError::image_decoder_failed)});
  try {
    decoded = image_decoder_.decode(request);
  } catch (const std::bad_alloc &) {
    throw;
  } catch (const std::exception &error) {
    return fail(GltfDecodeError::image_decoder_failed,
                where + ": the image decoder threw: " + error.what());
  } catch (...) {
    return fail(GltfDecodeError::image_decoder_failed,
                where + ": the image decoder threw a non-standard exception");
  }
  if (!decoded)
    return decoded;
  if (auto valid = asset::validate(*decoded); !valid)
    return std::unexpected(std::move(valid.error()));
  if (auto size = asset::checkImageSize(decoded->extent.width, decoded->extent.height,
                                        decoded->extent.depth, options_.limits);
      !size)
    return std::unexpected(std::move(size.error()));
  if (auto charged = charge(decoded->data.size(), where); !charged)
    return std::unexpected(std::move(charged.error()));
  if (decoded->interpretation != interpretation)
    reporter_.note(Note{asset::DecodeSeverity::warning, "gltf.image.interpretation_mismatch",
                        where + " was requested as " +
                            std::string{interpretationName(interpretation)} +
                            " and the image decoder returned " +
                            std::string{interpretationName(decoded->interpretation)} +
                            "; the result is kept as it is",
                        where});
  return decoded;
}

auto Decoder::buildImages() -> terreate::Result<void> {
  model_.images.reserve(gltf_->images_count);
  for (std::size_t i = 0; i < gltf_->images_count; ++i) {
    asset::ColorInterpretation interpretation = asset::ColorInterpretation::srgb;
    const auto &use = image_use_[i].interpretation;
    if (use.has_value()) {
      interpretation = use.value();
    } else {
      reporter_.note(Note{asset::DecodeSeverity::info, "gltf.image.interpretation_defaulted",
                          indexPath("images", i) +
                              " is not used by a supported material slot; it is decoded as srgb",
                          indexPath("images", i)});
    }
    auto image = decodeImage(i, interpretation);
    if (!image)
      return std::unexpected(std::move(image.error()));
    model_.images.push_back(std::move(*image));
  }
  return {};
}

// ---------------------------------------------------------------------------------------
// Nodes and scenes
// ---------------------------------------------------------------------------------------

auto Decoder::buildNodes() -> terreate::Result<void> {
  model_.nodes.reserve(gltf_->nodes_count);
  for (std::size_t i = 0; i < gltf_->nodes_count; ++i) {
    const cgltf_node &node = gltf_->nodes[i];
    asset::ModelNode out;
    out.name = text(node.name);
    for (std::size_t c = 0; c < node.children_count; ++c)
      out.children.push_back(static_cast<std::size_t>(node.children[c] - gltf_->nodes));
    const bool trs = node.has_translation != 0 || node.has_rotation != 0 || node.has_scale != 0;
    if (node.has_matrix != 0 && trs)
      return fail(GltfDecodeError::malformed_gltf,
                  indexPath("nodes", i) + " has both a matrix and translation/rotation/scale");
    if (node.has_matrix != 0) {
      asset::MatrixTransform matrix;
      std::copy(std::begin(node.matrix), std::end(node.matrix), matrix.matrix.begin());
      out.transform = matrix;
    } else {
      asset::TrsTransform transform;
      if (node.has_translation != 0)
        std::copy(std::begin(node.translation), std::end(node.translation),
                  transform.translation.begin());
      if (node.has_rotation != 0)
        std::copy(std::begin(node.rotation), std::end(node.rotation), transform.rotation.begin());
      if (node.has_scale != 0)
        std::copy(std::begin(node.scale), std::end(node.scale), transform.scale.begin());
      out.transform = transform;
    }
    if (node.mesh != nullptr) {
      const auto mesh = static_cast<std::size_t>(node.mesh - gltf_->meshes);
      out.mesh = model_mesh_of_[mesh];
    }
    model_.nodes.push_back(std::move(out));
  }
  for (std::size_t i = 0; i < gltf_->scenes_count; ++i) {
    const cgltf_scene &scene = gltf_->scenes[i];
    asset::ModelScene out;
    out.name = text(scene.name);
    for (std::size_t n = 0; n < scene.nodes_count; ++n)
      out.roots.push_back(static_cast<std::size_t>(scene.nodes[n] - gltf_->nodes));
    model_.scenes.push_back(std::move(out));
  }
  if (gltf_->scene != nullptr)
    model_.default_scene = static_cast<std::size_t>(gltf_->scene - gltf_->scenes);
  return {};
}

void Decoder::buildSource() {
  const auto add = [this](const char *key, const std::string &value) {
    if (!value.empty())
      model_.custom.push_back(asset::CustomEntry{key, value});
  };
  add("gltf.container", std::string{containerName(native_.container)});
  add("gltf.version", native_.version);
  add("gltf.min_version", native_.min_version);
  add("gltf.generator", native_.generator);
  add("gltf.copyright", native_.copyright);
}

// ---------------------------------------------------------------------------------------
// The whole decode
// ---------------------------------------------------------------------------------------

auto Decoder::run() -> terreate::Result<GltfDecodeResult> {
  const asset::DecodeLimits &limits = options_.limits;
  const Bytes bytes = input_.bytes;

  // 1. The encoded size, before the bytes are touched.
  if (auto size = asset::checkInputSize(bytes, limits); !size)
    return std::unexpected(std::move(size.error()));
  if (bytes.empty())
    return fail(GltfDecodeError::malformed_gltf, "empty input");

  // 2. The container and the JSON nesting, before the parser sees the text.
  auto located = locateContainer(bytes);
  if (!located)
    return std::unexpected(std::move(located.error()));
  container_ = *located;
  if (!jsonDepthAllowed(container_.json))
    return fail(GltfDecodeError::malformed_gltf,
                "the JSON nests deeper than " + std::to_string(max_json_depth) + " levels");
  if (container_.trailing > 0)
    reporter_.note(Note{
        asset::DecodeSeverity::info, "gltf.glb.trailing_bytes",
        std::to_string(container_.trailing) + " byte(s) after the last GLB chunk ignored", "/"});

  // 3. The parse, on a budget that scales with the JSON size.
  ParseBudget budget;
  const std::uint64_t scaled =
      checkedMul(container_.json.size(), parse_bytes_per_input_byte).value_or(0);
  budget.limit =
      checkedAdd(parse_base_bytes, scaled).value_or(std::numeric_limits<std::uint64_t>::max());
  auto parsed = parse(container_, budget);
  if (!parsed)
    return std::unexpected(std::move(parsed.error()));
  gltf_ = parsed->get();

  // 4. Header, extensions, counts and hierarchy.
  if (auto header = checkHeader(); !header)
    return std::unexpected(std::move(header.error()));
  if (auto counts = checkCounts(); !counts)
    return std::unexpected(std::move(counts.error()));
  reporter_.note(Note{asset::DecodeSeverity::info, "gltf.container",
                      "decoded a " + std::string{containerName(container_.kind)} + " (glTF " +
                          native_.version + ")",
                      "/"});
  reportUnsupportedFeatures();

  // 5. The model, in dependency order: what a later step names comes first.
  if (auto step = buildSamplers(); !step)
    return std::unexpected(std::move(step.error()));
  if (auto step = buildMaterials(); !step)
    return std::unexpected(std::move(step.error()));
  if (auto step = buildMeshes(); !step)
    return std::unexpected(std::move(step.error()));
  if (auto step = buildNodes(); !step)
    return std::unexpected(std::move(step.error()));
  if (auto step = buildTextures(); !step)
    return std::unexpected(std::move(step.error()));
  if (auto step = buildImages(); !step)
    return std::unexpected(std::move(step.error()));
  buildSource();

  GltfDecodeResult result;
  reporter_.finish(result.diagnostics, model_.unsupported);
  if (auto valid = asset::validate(model_); !valid)
    return std::unexpected(std::move(valid.error()));
  result.model = std::move(model_);
  result.provenance = session_.provenance();
  result.native = std::move(native_);
  return result;
}

} // namespace

// ---------------------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------------------

auto containerName(GltfContainer container) noexcept -> std::string_view {
  switch (container) {
  case GltfContainer::gltf:
    return "gltf";
  case GltfContainer::glb:
    return "glb";
  }
  return "unknown";
}

auto gltf_decode_error_category() noexcept -> const std::error_category & {
  static const GltfDecodeErrorCategory category;
  return category;
}

auto make_error_code(GltfDecodeError error) noexcept -> std::error_code {
  return {static_cast<int>(error), gltf_decode_error_category()};
}

auto decodeGltf(const asset::DecodeInput &input, asset::ResourceResolverView resolver,
                ImageDecoderView image_decoder, const GltfDecodeOptions &options)
    -> terreate::Result<GltfDecodeResult> {
  try {
    Decoder decoder{input, resolver, image_decoder, options};
    return decoder.run();
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(GltfDecodeError::out_of_memory)});
  }
}

} // namespace terreate::adapter::asset_gltf_cgltf

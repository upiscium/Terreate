// #278: the cgltf Adapter decodes glTF and GLB bytes into a ModelData. This test links the
// Adapter (and so Asset) alone: no Vulkan, no Graphics, no Platform, no display, no file system
// and no OS-, compiler- or linker-specific mechanism. The reference fixtures are byte arrays and
// strings in this file, made once with a throw-away generator script (the repository does not
// carry it and the test needs no tool at run time); the other documents are assembled below from
// literal JSON text and floats written out by the test itself, so the expected values are
// independent of the decoder.
#include <terreate/adapter/asset_gltf_cgltf/decoder.hpp>
#include <terreate/asset/mesh_data.hpp>
#include <terreate/asset/primitives.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR) ||             \
    defined(CGLTF_H_INCLUDED__)
#error "The Adapter's public header must not pull in Vulkan, GLFW or cgltf"
#endif

namespace {
namespace asset = terreate::asset;
using namespace terreate::adapter::asset_gltf_cgltf;

const char *test_name = "adapter.asset_gltf_cgltf";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

static_assert(std::is_error_code_enum_v<GltfDecodeError>);
static_assert(std::is_error_code_enum_v<asset::DecodeError>);

// Reference fixtures: a triangle as GLB and as glTF with a data URI, made by an independent
// script (struct and base64 of a scripting language).
inline constexpr std::uint8_t triangle_glb[] = {
    0x67, 0x6c, 0x54, 0x46, 0x02, 0x00, 0x00, 0x00, 0xbc, 0x01, 0x00, 0x00, 0x7c, 0x01, 0x00, 0x00,
    0x4a, 0x53, 0x4f, 0x4e, 0x7b, 0x22, 0x61, 0x73, 0x73, 0x65, 0x74, 0x22, 0x3a, 0x7b, 0x22, 0x76,
    0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x22, 0x3a, 0x22, 0x32, 0x2e, 0x30, 0x22, 0x2c, 0x22, 0x67,
    0x65, 0x6e, 0x65, 0x72, 0x61, 0x74, 0x6f, 0x72, 0x22, 0x3a, 0x22, 0x74, 0x65, 0x72, 0x72, 0x65,
    0x61, 0x74, 0x65, 0x2d, 0x66, 0x69, 0x78, 0x74, 0x75, 0x72, 0x65, 0x22, 0x7d, 0x2c, 0x22, 0x73,
    0x63, 0x65, 0x6e, 0x65, 0x22, 0x3a, 0x30, 0x2c, 0x22, 0x73, 0x63, 0x65, 0x6e, 0x65, 0x73, 0x22,
    0x3a, 0x5b, 0x7b, 0x22, 0x6e, 0x6f, 0x64, 0x65, 0x73, 0x22, 0x3a, 0x5b, 0x30, 0x5d, 0x7d, 0x5d,
    0x2c, 0x22, 0x6e, 0x6f, 0x64, 0x65, 0x73, 0x22, 0x3a, 0x5b, 0x7b, 0x22, 0x6d, 0x65, 0x73, 0x68,
    0x22, 0x3a, 0x30, 0x2c, 0x22, 0x6e, 0x61, 0x6d, 0x65, 0x22, 0x3a, 0x22, 0x74, 0x72, 0x69, 0x22,
    0x7d, 0x5d, 0x2c, 0x22, 0x6d, 0x65, 0x73, 0x68, 0x65, 0x73, 0x22, 0x3a, 0x5b, 0x7b, 0x22, 0x70,
    0x72, 0x69, 0x6d, 0x69, 0x74, 0x69, 0x76, 0x65, 0x73, 0x22, 0x3a, 0x5b, 0x7b, 0x22, 0x61, 0x74,
    0x74, 0x72, 0x69, 0x62, 0x75, 0x74, 0x65, 0x73, 0x22, 0x3a, 0x7b, 0x22, 0x50, 0x4f, 0x53, 0x49,
    0x54, 0x49, 0x4f, 0x4e, 0x22, 0x3a, 0x30, 0x7d, 0x7d, 0x5d, 0x7d, 0x5d, 0x2c, 0x22, 0x61, 0x63,
    0x63, 0x65, 0x73, 0x73, 0x6f, 0x72, 0x73, 0x22, 0x3a, 0x5b, 0x7b, 0x22, 0x62, 0x75, 0x66, 0x66,
    0x65, 0x72, 0x56, 0x69, 0x65, 0x77, 0x22, 0x3a, 0x30, 0x2c, 0x22, 0x63, 0x6f, 0x6d, 0x70, 0x6f,
    0x6e, 0x65, 0x6e, 0x74, 0x54, 0x79, 0x70, 0x65, 0x22, 0x3a, 0x35, 0x31, 0x32, 0x36, 0x2c, 0x22,
    0x63, 0x6f, 0x75, 0x6e, 0x74, 0x22, 0x3a, 0x33, 0x2c, 0x22, 0x74, 0x79, 0x70, 0x65, 0x22, 0x3a,
    0x22, 0x56, 0x45, 0x43, 0x33, 0x22, 0x2c, 0x22, 0x6d, 0x69, 0x6e, 0x22, 0x3a, 0x5b, 0x30, 0x2c,
    0x30, 0x2c, 0x30, 0x5d, 0x2c, 0x22, 0x6d, 0x61, 0x78, 0x22, 0x3a, 0x5b, 0x31, 0x2c, 0x31, 0x2c,
    0x30, 0x5d, 0x7d, 0x5d, 0x2c, 0x22, 0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x56, 0x69, 0x65, 0x77,
    0x73, 0x22, 0x3a, 0x5b, 0x7b, 0x22, 0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x22, 0x3a, 0x30, 0x2c,
    0x22, 0x62, 0x79, 0x74, 0x65, 0x4f, 0x66, 0x66, 0x73, 0x65, 0x74, 0x22, 0x3a, 0x30, 0x2c, 0x22,
    0x62, 0x79, 0x74, 0x65, 0x4c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x22, 0x3a, 0x33, 0x36, 0x7d, 0x5d,
    0x2c, 0x22, 0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x73, 0x22, 0x3a, 0x5b, 0x7b, 0x22, 0x62, 0x79,
    0x74, 0x65, 0x4c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x22, 0x3a, 0x33, 0x36, 0x7d, 0x5d, 0x7d, 0x20,
    0x24, 0x00, 0x00, 0x00, 0x42, 0x49, 0x4e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00,
};

inline constexpr std::string_view triangle_gltf =
    R"json({"asset":{"version":"2.0","generator":"terreate-fixture"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,"name":"tri"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],"buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]})json";

// ---------------------------------------------------------------------------------------
// Helpers: bytes, base64, GLB assembly, a small JSON document builder
// ---------------------------------------------------------------------------------------

using Bytes = std::vector<std::byte>;

Bytes toBytes(std::string_view text) {
  Bytes bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

template <std::size_t N> Bytes toBytes(const std::uint8_t (&array)[N]) {
  Bytes bytes(N);
  std::memcpy(bytes.data(), array, N);
  return bytes;
}

std::string base64(const Bytes &bytes) {
  static constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::size_t left = bytes.size() - i;
    std::uint32_t chunk = static_cast<std::uint32_t>(bytes[i]) << 16U;
    if (left > 1)
      chunk |= static_cast<std::uint32_t>(bytes[i + 1]) << 8U;
    if (left > 2)
      chunk |= static_cast<std::uint32_t>(bytes[i + 2]);
    out.push_back(alphabet[(chunk >> 18U) & 63U]);
    out.push_back(alphabet[(chunk >> 12U) & 63U]);
    out.push_back(left > 1 ? alphabet[(chunk >> 6U) & 63U] : '=');
    out.push_back(left > 2 ? alphabet[chunk & 63U] : '=');
  }
  return out;
}

std::string dataUri(const Bytes &bytes, const char *media = "application/octet-stream") {
  return std::string{"data:"} + media + ";base64," + base64(bytes);
}

void putLe32(Bytes &out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}

Bytes makeGlb(std::string_view json, const Bytes &bin, bool with_bin = true) {
  std::string padded{json};
  while (padded.size() % 4 != 0)
    padded.push_back(' ');
  Bytes padded_bin = bin;
  while (padded_bin.size() % 4 != 0)
    padded_bin.push_back(std::byte{0});
  const std::size_t total =
      12 + 8 + padded.size() + (with_bin ? 8 + padded_bin.size() : std::size_t{0});
  Bytes out;
  putLe32(out, 0x46546C67U);
  putLe32(out, 2);
  putLe32(out, static_cast<std::uint32_t>(total));
  putLe32(out, static_cast<std::uint32_t>(padded.size()));
  putLe32(out, 0x4E4F534AU);
  const Bytes json_bytes = toBytes(padded);
  out.insert(out.end(), json_bytes.begin(), json_bytes.end());
  if (with_bin) {
    putLe32(out, static_cast<std::uint32_t>(padded_bin.size()));
    putLe32(out, 0x004E4942U);
    out.insert(out.end(), padded_bin.begin(), padded_bin.end());
  }
  return out;
}

// Appends little-endian values to a binary buffer, returning the offset they start at.
struct BinBuilder {
  Bytes data;
  std::size_t align() {
    while (data.size() % 4 != 0)
      data.push_back(std::byte{0});
    return data.size();
  }
  std::size_t floats(std::span<const float> values) {
    const std::size_t at = align();
    const auto *raw = reinterpret_cast<const std::byte *>(values.data());
    data.insert(data.end(), raw, raw + values.size_bytes());
    return at;
  }
  std::size_t u8(std::span<const std::uint8_t> values) {
    const std::size_t at = align();
    for (const auto v : values)
      data.push_back(static_cast<std::byte>(v));
    return at;
  }
  std::size_t u16(std::span<const std::uint16_t> values) {
    const std::size_t at = align();
    for (const auto v : values) {
      data.push_back(static_cast<std::byte>(v & 0xFFU));
      data.push_back(static_cast<std::byte>((v >> 8U) & 0xFFU));
    }
    return at;
  }
  std::size_t u32(std::span<const std::uint32_t> values) {
    const std::size_t at = align();
    for (const auto v : values)
      putLe32(data, v);
    return at;
  }
};

// A glTF JSON document: top-level members by key, replaceable by the tests.
struct Doc {
  std::vector<std::pair<std::string, std::string>> parts;

  Doc &set(const std::string &key, std::string value) {
    for (auto &part : parts) {
      if (part.first == key) {
        part.second = std::move(value);
        return *this;
      }
    }
    parts.emplace_back(key, std::move(value));
    return *this;
  }
  Doc &drop(const std::string &key) {
    std::erase_if(parts, [&](const auto &part) { return part.first == key; });
    return *this;
  }
  [[nodiscard]] std::string str() const {
    std::string out = "{";
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i != 0)
        out += ',';
      out += "\"" + parts[i].first + "\":" + parts[i].second;
    }
    return out + "}";
  }
};

// The smallest valid document: one triangle (three VEC3 floats in a 36-byte buffer).
Doc triangleDoc(const std::string &buffer_uri) {
  Doc doc;
  doc.set("asset", R"({"version":"2.0"})");
  doc.set("scene", "0");
  doc.set("scenes", R"([{"nodes":[0]}])");
  doc.set("nodes", R"([{"mesh":0}])");
  doc.set("meshes", R"([{"primitives":[{"attributes":{"POSITION":0}}]}])");
  doc.set("accessors", R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}])");
  doc.set("bufferViews", R"([{"buffer":0,"byteLength":36}])");
  doc.set("buffers", buffer_uri.empty() ? std::string{R"([{"byteLength":36}])"}
                                        : R"([{"byteLength":36,"uri":")" + buffer_uri + R"("}])");
  return doc;
}

Bytes triangleBin() {
  BinBuilder bin;
  const std::array<float, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
  bin.floats(positions);
  return bin.data;
}

asset::DecodeInput inputOf(const Bytes &bytes, const char *name = "assets/model.gltf") {
  return asset::DecodeInput{bytes, asset::makeSourceIdentity(name)};
}

asset::DecodeInput inputOf(const std::string &text, Bytes &storage,
                           const char *name = "assets/model.gltf") {
  storage = toBytes(text);
  return inputOf(storage, name);
}

// An image decoder for the tests: it records what it was asked and returns a 2x2 rgba8 image
// with the requested interpretation.
struct SeenImage {
  std::string name;
  std::string base;
  std::string mime;
  std::size_t size = 0;
  asset::ColorInterpretation interpretation = asset::ColorInterpretation::srgb;
};

struct StubImages {
  std::vector<SeenImage> seen;
  std::function<terreate::Result<asset::ImageData>(const ImageDecodeRequest &)> override_;

  terreate::Result<asset::ImageData> operator()(const ImageDecodeRequest &request) {
    seen.push_back({request.input.source.name, request.input.source.base, request.mime_type,
                    request.input.bytes.size(), request.interpretation});
    if (override_)
      return override_(request);
    return makeImage(request.interpretation, 2, 2);
  }

  static asset::ImageData makeImage(asset::ColorInterpretation interpretation, std::uint32_t width,
                                    std::uint32_t height) {
    asset::ImageData image;
    image.format = asset::PixelFormat::rgba8;
    image.interpretation = interpretation;
    image.extent = {width, height, 1};
    const auto layout = asset::packedLayout(image.format, image.extent, 1, 1);
    image.subresources = layout->subresources;
    image.data.assign(layout->byte_size, std::byte{0x7F});
    return image;
  }
};

// The resolver of the tests: a MemoryResolver plus a count of calls.
struct Files {
  asset::MemoryResolver memory;
  Files() = default;
  void add(std::string_view name, Bytes bytes) {
    check(memory.add(name, std::move(bytes)).has_value(), "add file");
  }
};

using Result = terreate::Result<GltfDecodeResult>;

bool failedWith(const Result &result, std::error_code code, const char *message) {
  if (!check(!result.has_value(), message))
    return false;
  const bool same = result.error().code() == code;
  if (!same)
    std::fprintf(stderr, "%s: expected %s, got %s: %s (%s)\n", test_name, code.message().c_str(),
                 result.error().code().message().c_str(), result.error().detail().c_str(), message);
  return check(same, message);
}

bool hasUnsupported(const asset::ModelData &model, std::string_view kind) {
  return std::ranges::any_of(model.unsupported, [&](const asset::UnsupportedFeature &feature) {
    return feature.kind == kind;
  });
}

bool hasUnsupported(const asset::ModelData &model, std::string_view kind, std::string_view name) {
  return std::ranges::any_of(model.unsupported, [&](const asset::UnsupportedFeature &feature) {
    return feature.kind == kind && feature.name == name;
  });
}

bool hasDiagnostic(const GltfDecodeResult &result, std::string_view code,
                   std::optional<asset::DecodeSeverity> severity = std::nullopt) {
  return std::ranges::any_of(result.diagnostics, [&](const asset::DecodeDiagnostic &d) {
    return d.code == code && (!severity || d.severity == *severity);
  });
}

std::string customValue(const asset::ModelData &model, std::string_view key) {
  for (const auto &entry : model.custom) {
    if (entry.key == key && std::holds_alternative<std::string>(entry.value))
      return std::get<std::string>(entry.value);
  }
  return "<missing>";
}

std::vector<float> floatsOf(const asset::MeshData &mesh, const asset::AttributeKey &key) {
  const auto *stream = mesh.find(key);
  if (stream == nullptr)
    return {};
  const auto floats = stream->toFloats();
  return floats ? *floats : std::vector<float>{};
}

std::vector<float> positionsOf(const asset::MeshData &mesh) {
  return floatsOf(mesh, {asset::AttributeSemantic::position, 0, {}});
}

// Every success is a valid model: the Adapter validates it, and so does this test.
bool decodesValid(const Result &result, const char *message) {
  if (!check(result.has_value(), message)) {
    if (!result.has_value())
      std::fprintf(stderr, "  error: %s: %s\n", result.error().code().message().c_str(),
                   result.error().detail().c_str());
    return false;
  }
  return check(asset::validate(result->model).has_value(), "the decoded model validates");
}

Result decode(const Bytes &bytes, asset::ResourceResolverView resolver = {},
              ImageDecoderView images = {}, const GltfDecodeOptions &options = {},
              const char *name = "assets/model.gltf") {
  return decodeGltf(inputOf(bytes, name), resolver, images, options);
}

Result decodeText(const std::string &text, asset::ResourceResolverView resolver = {},
                  ImageDecoderView images = {}, const GltfDecodeOptions &options = {}) {
  const Bytes bytes = toBytes(text);
  return decode(bytes, resolver, images, options);
}

constexpr float epsilon = 1e-6F;

bool near(const std::vector<float> &actual, const std::vector<float> &expected) {
  if (actual.size() != expected.size())
    return false;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    if (std::fabs(actual[i] - expected[i]) > epsilon)
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------
// The box fixture: an indexed box with normals and UVs, one material that uses three textures,
// three images (external, data URI, external or GLB buffer views) and one sampler
// ---------------------------------------------------------------------------------------

const Bytes fake_png_a = toBytes("PNG-A-bytes");
const Bytes fake_png_normal = toBytes("PNG-normal-bytes");
const Bytes fake_png_orm = toBytes("PNG-ORM-bytes!");

struct Box {
  asset::MeshData mesh;
  Bytes bin;
  std::string json;
  Bytes glb;
};

Box makeBox(bool glb) {
  Box box;
  box.mesh = *asset::box({.width = 1.0F, .height = 2.0F, .depth = 3.0F});
  BinBuilder bin;
  const auto positions = floatsOf(box.mesh, {asset::AttributeSemantic::position, 0, {}});
  const auto normals = floatsOf(box.mesh, {asset::AttributeSemantic::normal, 0, {}});
  const auto uvs = floatsOf(box.mesh, {asset::AttributeSemantic::texcoord, 0, {}});
  const auto &indices = std::get<std::vector<std::uint16_t>>(box.mesh.indices);
  const std::size_t position_at = bin.floats(positions);
  const std::size_t normal_at = bin.floats(normals);
  const std::size_t uv_at = bin.floats(uvs);
  const std::size_t index_at = bin.u16(indices);
  const std::size_t index_end = bin.data.size();
  std::size_t image_a_at = 0;
  std::size_t image_orm_at = 0;
  if (glb) {
    image_a_at = bin.align();
    bin.data.insert(bin.data.end(), fake_png_a.begin(), fake_png_a.end());
    image_orm_at = bin.align();
    bin.data.insert(bin.data.end(), fake_png_orm.begin(), fake_png_orm.end());
  }
  const auto num = [](std::size_t value) { return std::to_string(value); };

  Doc doc;
  doc.set("asset", R"({"version":"2.0","generator":"test-generator","copyright":"(c) test"})");
  doc.set("scene", "0");
  doc.set("scenes", R"([{"name":"main","nodes":[0]}])");
  doc.set("nodes", R"([{"name":"box","mesh":0,"translation":[1,2,3]}])");
  doc.set("meshes", R"([{"name":"boxmesh","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,)"
                    R"("TEXCOORD_0":2},"indices":3,"material":0}]}])");
  doc.set("materials",
          R"([{"name":"mat","pbrMetallicRoughness":{"baseColorFactor":[0.5,0.25,0.125,1],)"
          R"("metallicFactor":0.25,"roughnessFactor":0.75,"baseColorTexture":{"index":0},)"
          R"("metallicRoughnessTexture":{"index":2}},"normalTexture":{"index":1,"scale":0.5},)"
          R"("occlusionTexture":{"index":2,"strength":0.5},"emissiveFactor":[0.1,0.2,0.3],)"
          R"("emissiveTexture":{"index":0,"texCoord":0},"alphaMode":"MASK","alphaCutoff":0.25,)"
          R"("doubleSided":true}])");
  doc.set("textures", R"([{"source":0,"sampler":0},{"source":1},{"source":2}])");
  doc.set("samplers", R"([{"magFilter":9728,"minFilter":9987,"wrapS":33071,"wrapT":33648}])");
  const std::string normal_uri = dataUri(fake_png_normal, "image/png");
  if (glb) {
    doc.set("images", R"([{"bufferView":4,"mimeType":"image/png"},{"uri":")" + normal_uri +
                          R"("},{"bufferView":5,"mimeType":"image/png"}])");
  } else {
    doc.set("images", R"([{"uri":"textures/albedo.png","mimeType":"image/png"},{"uri":")" +
                          normal_uri + R"("},{"uri":"textures/orm.png"}])");
  }
  doc.set("accessors",
          R"([{"bufferView":0,"componentType":5126,"count":24,"type":"VEC3","min":[-0.5,-1,-1.5],)"
          R"("max":[0.5,1,1.5]},{"bufferView":1,"componentType":5126,"count":24,"type":"VEC3"},)"
          R"({"bufferView":2,"componentType":5126,"count":24,"type":"VEC2"},)"
          R"({"bufferView":3,"componentType":5123,"count":36,"type":"SCALAR"}])");
  std::string views = R"([{"buffer":0,"byteOffset":)" + num(position_at) +
                      R"(,"byteLength":288},{"buffer":0,"byteOffset":)" + num(normal_at) +
                      R"(,"byteLength":288},{"buffer":0,"byteOffset":)" + num(uv_at) +
                      R"(,"byteLength":192},{"buffer":0,"byteOffset":)" + num(index_at) +
                      R"(,"byteLength":72})";
  if (glb)
    views += R"(,{"buffer":0,"byteOffset":)" + num(image_a_at) + R"(,"byteLength":)" +
             num(fake_png_a.size()) + R"(},{"buffer":0,"byteOffset":)" + num(image_orm_at) +
             R"(,"byteLength":)" + num(fake_png_orm.size()) + "}";
  doc.set("bufferViews", views + "]");
  box.bin = bin.data;
  if (glb) {
    doc.set("buffers", R"([{"byteLength":)" + num(bin.data.size()) + "}]");
  } else {
    doc.set("buffers", R"([{"byteLength":)" + num(bin.data.size()) + R"(,"uri":"box.bin"}])");
  }
  (void)index_end;
  box.json = doc.str();
  if (glb)
    box.glb = makeGlb(box.json, box.bin);
  return box;
}

void boxFiles(Files &files, const Box &box) {
  files.add("assets/box.bin", box.bin);
  files.add("assets/textures/albedo.png", fake_png_a);
  files.add("assets/textures/orm.png", fake_png_orm);
}

bool sameBytes(const asset::AttributeStream *a, const asset::AttributeStream *b) {
  return a != nullptr && b != nullptr && a->count == b->count && a->format == b->format &&
         a->data == b->data;
}

// ---------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------

void referenceTriangle() {
  const Bytes glb = toBytes(triangle_glb);
  const auto from_glb = decode(glb, {}, {}, {}, "tri.glb");
  if (decodesValid(from_glb, "the reference GLB decodes")) {
    const auto &model = from_glb->model;
    check(model.meshes.size() == 1 && model.primitives.size() == 1 &&
              model.model_meshes.size() == 1 && model.nodes.size() == 1 && model.scenes.size() == 1,
          "one mesh, primitive, model mesh, node and scene");
    check(near(positionsOf(model.meshes[0]), {0, 0, 0, 1, 0, 0, 0, 1, 0}), "triangle positions");
    check(model.meshes[0].topology == asset::PrimitiveTopology::triangle_list,
          "the default mode is triangles");
    check(model.meshes[0].indexType() == asset::IndexType::none, "no indices stay non-indexed");
    const auto &bounds = model.meshes[0].bounds;
    check(bounds.has_value() && bounds.value().min == std::array<float, 3>{0, 0, 0} &&
              bounds.value().max == std::array<float, 3>{1, 1, 0},
          "the bounds are computed from the positions");
    check(model.nodes[0].name == "tri" && model.nodes[0].mesh == 0, "the node places the mesh");
    check(model.scenes[0].roots == std::vector<std::size_t>{0} && model.default_scene == 0,
          "the scene and the default scene");
    check(std::holds_alternative<asset::TrsTransform>(model.nodes[0].transform) &&
              std::get<asset::TrsTransform>(model.nodes[0].transform) == asset::TrsTransform{},
          "a node without a transform is the identity TRS");
    check(model.primitives[0].material == std::nullopt, "no material");
    check(model.unsupported.empty(), "nothing unsupported in the triangle");
    check(from_glb->native.container == GltfContainer::glb && from_glb->native.version == "2.0" &&
              from_glb->native.generator == "terreate-fixture",
          "native info of the GLB");
    check(customValue(model, "gltf.container") == "glb" &&
              customValue(model, "gltf.version") == "2.0" &&
              customValue(model, "gltf.generator") == "terreate-fixture",
          "custom entries of the GLB");
    check(from_glb->provenance.source == asset::makeSourceIdentity("tri.glb") &&
              from_glb->provenance.resources.empty(),
          "provenance: the source, and no external resource for a GLB chunk");
    check(hasDiagnostic(*from_glb, "gltf.container", asset::DecodeSeverity::info),
          "the container is noted");
    check(std::ranges::all_of(from_glb->diagnostics,
                              [](const auto &d) { return d.location.source == "tri.glb"; }),
          "diagnostics name the source");
  }

  const auto from_gltf = decodeText(std::string{triangle_gltf});
  if (decodesValid(from_gltf, "the reference glTF with a data URI decodes")) {
    check(near(positionsOf(from_gltf->model.meshes[0]), {0, 0, 0, 1, 0, 0, 0, 1, 0}),
          "the data URI buffer gives the same positions");
    check(from_gltf->native.container == GltfContainer::gltf, "glTF container");
    check(hasDiagnostic(*from_gltf, "gltf.data_uri", asset::DecodeSeverity::info),
          "the data URI is noted");
    check(from_gltf->provenance.resources.empty(), "a data URI is not an external request");
    if (from_glb)
      check(positionsOf(from_glb->model.meshes[0]) == positionsOf(from_gltf->model.meshes[0]),
            "GLB and glTF decode the same geometry");
  }
}

void boxWithResolverAndImages() {
  const Box box = makeBox(false);
  Files files;
  boxFiles(files, box);
  auto resolver = asset::ResourceResolverView::bind(files.memory);
  StubImages images;
  auto decoder = ImageDecoderView::bind(images);
  const auto result = decode(toBytes(box.json), resolver, decoder, {}, "assets/box.gltf");
  if (!decodesValid(result, "the box with external buffer and images decodes"))
    return;
  const auto &model = result->model;
  check(model.meshes.size() == 1, "one mesh");
  const auto &mesh = model.meshes[0];
  check(sameBytes(mesh.find({asset::AttributeSemantic::position, 0, {}}),
                  box.mesh.find({asset::AttributeSemantic::position, 0, {}})),
        "positions equal the generated box");
  check(sameBytes(mesh.find({asset::AttributeSemantic::normal, 0, {}}),
                  box.mesh.find({asset::AttributeSemantic::normal, 0, {}})),
        "normals equal the generated box");
  check(sameBytes(mesh.find({asset::AttributeSemantic::texcoord, 0, {}}),
                  box.mesh.find({asset::AttributeSemantic::texcoord, 0, {}})),
        "uvs equal the generated box");
  check(mesh.indexType() == asset::IndexType::uint16 && mesh.indices == box.mesh.indices,
        "indices equal the generated box (uint16)");
  check(mesh.find({asset::AttributeSemantic::tangent, 0, {}}) == nullptr,
        "no tangents are invented");

  check(model.primitives.size() == 1 && model.primitives[0].material == 0 &&
            model.primitives[0].mesh == 0,
        "the primitive keeps its material");
  check(model.model_meshes.size() == 1 && model.model_meshes[0].name == "boxmesh" &&
            model.model_meshes[0].primitives == std::vector<std::size_t>{0},
        "the model mesh");
  const auto *translation = std::get_if<asset::TrsTransform>(&model.nodes[0].transform);
  check(translation != nullptr && translation->translation == std::array<float, 3>{1, 2, 3},
        "the node translation");

  check(model.materials.size() == 1, "one material");
  const auto &material = model.materials[0];
  check(material.name == "mat" &&
            material.base_color_factor == std::array<float, 4>{0.5F, 0.25F, 0.125F, 1.0F} &&
            material.metallic_factor == 0.25F && material.roughness_factor == 0.75F &&
            material.normal_scale == 0.5F && material.occlusion_strength == 0.5F &&
            material.emissive_factor == std::array<float, 3>{0.1F, 0.2F, 0.3F} &&
            material.alpha_mode == asset::MaterialAlphaMode::mask &&
            material.alpha_cutoff == 0.25F && material.double_sided,
        "material factors and flags");
  check(material.base_color_texture == asset::TextureRef{0, 0} &&
            material.metallic_roughness_texture == asset::TextureRef{2, 0} &&
            material.normal_texture == asset::TextureRef{1, 0} &&
            material.occlusion_texture == asset::TextureRef{2, 0} &&
            material.emissive_texture == asset::TextureRef{0, 0},
        "material texture references");

  check(model.textures.size() == 3 && model.textures[0].image == 0 &&
            model.textures[0].sampler == 0 && model.textures[1].image == 1 &&
            !model.textures[1].sampler && model.textures[2].image == 2,
        "textures map to images and samplers");
  check(model.samplers.size() == 1 &&
            model.samplers[0].mag_filter == asset::SamplerFilter::nearest &&
            model.samplers[0].min_filter == asset::SamplerFilter::linear &&
            model.samplers[0].mipmap_mode == asset::SamplerMipmapMode::linear &&
            model.samplers[0].wrap_u == asset::SamplerWrap::clamp_to_edge &&
            model.samplers[0].wrap_v == asset::SamplerWrap::mirrored_repeat,
        "sampler enumerators map");

  check(images.seen.size() == 3, "three images were decoded");
  if (images.seen.size() == 3) {
    check(images.seen[0].name == "assets/textures/albedo.png" &&
              images.seen[0].base == "assets/textures/" &&
              images.seen[0].size == fake_png_a.size() && images.seen[0].mime == "image/png" &&
              images.seen[0].interpretation == asset::ColorInterpretation::srgb,
          "image 0 (base color and emissive) is srgb, with the resolved identity");
    check(images.seen[1].name == "assets/box.gltf#images[1]" &&
              images.seen[1].size == fake_png_normal.size() &&
              images.seen[1].interpretation == asset::ColorInterpretation::normal_map,
          "image 1 (normal) is a normal map, from the data URI");
    check(images.seen[2].name == "assets/textures/orm.png" &&
              images.seen[2].interpretation == asset::ColorInterpretation::linear,
          "image 2 (metallic-roughness and occlusion) is linear");
  }
  check(model.images.size() == 3 &&
            model.images[0].interpretation == asset::ColorInterpretation::srgb &&
            model.images[1].interpretation == asset::ColorInterpretation::normal_map &&
            model.images[2].interpretation == asset::ColorInterpretation::linear,
        "the images carry the interpretation");

  const auto &resources = result->provenance.resources;
  check(resources.size() == 3 && resources[0].request.normalized == "assets/box.bin" &&
            resources[0].request.kind == asset::ResourceKind::buffer &&
            resources[0].request.declared_size == box.bin.size() &&
            resources[1].request.normalized == "assets/textures/albedo.png" &&
            resources[1].request.kind == asset::ResourceKind::image &&
            resources[2].request.normalized == "assets/textures/orm.png",
        "provenance lists the external requests in order, with kind and declared size");
  check(!resources.empty() && resources[0].size == box.bin.size() && !resources[0].outcome,
        "provenance records sizes and success");
  check(hasDiagnostic(*result, "gltf.data_uri") &&
            !hasDiagnostic(*result, "gltf.image.interpretation_conflict"),
        "the data URI is noted; no conflict");
  check(customValue(model, "gltf.generator") == "test-generator" &&
            customValue(model, "gltf.copyright") == "(c) test",
        "generator and copyright are kept");
}

void boxAsGlb() {
  const Box box = makeBox(true);
  StubImages images;
  auto decoder = ImageDecoderView::bind(images);
  const auto result = decode(box.glb, {}, decoder, {}, "assets/box.glb");
  if (!decodesValid(result, "the GLB box decodes without a resolver"))
    return;
  check(result->provenance.resources.empty(), "no external request");
  check(sameBytes(result->model.meshes[0].find({asset::AttributeSemantic::position, 0, {}}),
                  box.mesh.find({asset::AttributeSemantic::position, 0, {}})),
        "GLB positions equal the generated box");
  check(images.seen.size() == 3 && images.seen[0].name == "assets/box.glb#images[0]" &&
            images.seen[0].size == fake_png_a.size() && images.seen[2].size == fake_png_orm.size(),
        "buffer view images reach the decoder with their own bytes");
}

void instancingAndTransforms() {
  BinBuilder bin;
  const std::array<float, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
  const std::array<float, 9> other{0, 0, 1, 1, 0, 1, 0, 1, 1};
  bin.floats(positions);
  bin.floats(other);
  Doc doc = triangleDoc(dataUri(bin.data));
  doc.set("buffers", R"([{"byteLength":72,"uri":")" + dataUri(bin.data) + R"("}])");
  doc.set("bufferViews", R"([{"buffer":0,"byteLength":72}])");
  doc.set("accessors",
          R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
          R"({"bufferView":0,"byteOffset":36,"componentType":5126,"count":3,"type":"VEC3"}])");
  doc.set("meshes", R"([{"name":"two","primitives":[{"attributes":{"POSITION":0}},)"
                    R"({"attributes":{"POSITION":1},"mode":4}]}])");
  doc.set("nodes",
          R"([{"name":"a","mesh":0,"translation":[1,0,0],"rotation":[0,0,0.7071068,0.7071068],)"
          R"("scale":[2,2,2],"children":[2]},{"name":"b","mesh":0,"translation":[0,5,0]},)"
          R"({"name":"c","matrix":[1,0,0,0,0,1,0,0,0,0,1,0,7,8,9,1]},{"name":"d"}])");
  doc.set("scenes", R"([{"name":"s","nodes":[0,1]},{"name":"unused","nodes":[3]}])");
  doc.set("scene", "1");
  const auto result = decodeText(doc.str());
  if (!decodesValid(result, "a two-primitive mesh placed by two nodes decodes"))
    return;
  const auto &model = result->model;
  check(model.meshes.size() == 2 && model.primitives.size() == 2 &&
            model.model_meshes.size() == 1 && model.nodes.size() == 4,
        "two meshes, two primitives, one model mesh, four nodes (no node is invented)");
  check(model.model_meshes[0].primitives == std::vector<std::size_t>{0, 1}, "both primitives");
  check(model.nodes[0].mesh == 0 && model.nodes[1].mesh == 0 && !model.nodes[2].mesh &&
            !model.nodes[3].mesh,
        "two nodes share the mesh (instancing)");
  check(near(positionsOf(model.meshes[1]), {0, 0, 1, 1, 0, 1, 0, 1, 1}),
        "the second primitive's own positions");
  check(model.scenes.size() == 2 && model.scenes[0].roots == std::vector<std::size_t>{0, 1} &&
            model.default_scene == 1,
        "scenes and the default scene");
  check(model.nodes[0].children == std::vector<std::size_t>{2}, "the hierarchy");
  const auto *trs = std::get_if<asset::TrsTransform>(&model.nodes[0].transform);
  check(trs != nullptr && trs->translation == std::array<float, 3>{1, 0, 0} &&
            trs->scale == std::array<float, 3>{2, 2, 2} &&
            std::fabs(trs->rotation[2] - 0.7071068F) < 1e-6F &&
            std::fabs(trs->rotation[3] - 0.7071068F) < 1e-6F,
        "TRS node");
  const auto *matrix = std::get_if<asset::MatrixTransform>(&model.nodes[2].transform);
  check(matrix != nullptr && matrix->matrix[12] == 7.0F && matrix->matrix[13] == 8.0F &&
            matrix->matrix[14] == 9.0F && matrix->matrix[0] == 1.0F,
        "matrix node keeps the column-major elements");
  check(std::holds_alternative<asset::TrsTransform>(model.nodes[3].transform),
        "a node without transform is TRS identity");

  // A matrix together with TRS values is not valid glTF.
  Doc both = doc;
  both.set("nodes",
           R"([{"mesh":0,"translation":[1,0,0],"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}])");
  both.set("scenes", R"([{"nodes":[0]}])");
  both.set("scene", "0");
  failedWith(decodeText(both.str()), GltfDecodeError::malformed_gltf,
             "a node with both matrix and TRS is malformed");
}

void resolverFailures() {
  const Box box = makeBox(false);
  const Bytes json = toBytes(box.json);
  StubImages images;
  auto decoder = ImageDecoderView::bind(images);

  // No resolver: a named error, not a silent skip.
  failedWith(decode(json, {}, decoder, {}, "assets/box.gltf"), GltfDecodeError::missing_resolver,
             "an external buffer without a resolver");
  check(images.seen.empty(), "nothing was decoded after the failure");

  // A resolver without the file.
  Files empty;
  auto none = asset::ResourceResolverView::bind(empty.memory);
  failedWith(decode(json, none, decoder, {}, "assets/box.gltf"), asset::ResolveError::not_found,
             "a resolver without the buffer");

  // A resolver that fails with its own error: wrapped by the resolve session.
  auto failing = [](const asset::ResourceRequest &) -> terreate::Result<asset::ResolvedResource> {
    return std::unexpected(terreate::Error{std::make_error_code(std::errc::io_error), "read"});
  };
  failedWith(
      decode(json, asset::ResourceResolverView::bind(failing), decoder, {}, "assets/box.gltf"),
      asset::ResolveError::resolver_failed, "a failing resolver");
  auto throwing = [](const asset::ResourceRequest &) -> terreate::Result<asset::ResolvedResource> {
    throw std::runtime_error("boom");
  };
  failedWith(
      decode(json, asset::ResourceResolverView::bind(throwing), decoder, {}, "assets/box.gltf"),
      asset::ResolveError::resolver_failed, "a throwing resolver");

  // A resolver that returns fewer bytes than the buffer declares.
  Files shorter;
  shorter.add("assets/box.bin", Bytes(box.bin.begin(), box.bin.begin() + 10));
  failedWith(decode(json, asset::ResourceResolverView::bind(shorter.memory), decoder, {},
                    "assets/box.gltf"),
             GltfDecodeError::invalid_buffer, "a buffer shorter than its byteLength");

  // References are checked before the resolver sees them.
  for (const char *uri : {"../../escape.bin", "/abs/path.bin", "https://host/x.bin", "C:/x.bin",
                          "a\\\\b.bin", "a%2fb.bin"}) {
    Doc doc = triangleDoc(uri);
    int calls = 0;
    auto counting =
        [&calls](const asset::ResourceRequest &) -> terreate::Result<asset::ResolvedResource> {
      ++calls;
      return std::unexpected(terreate::Error{std::make_error_code(std::errc::io_error)});
    };
    const auto result = decodeText(doc.str(), asset::ResourceResolverView::bind(counting));
    failedWith(result, asset::ResolveError::reference_rejected,
               (std::string{"the reference "} + uri + " is rejected").c_str());
    check(calls == 0, "a rejected reference never reaches the resolver");
  }
  // A scheme is allowed only by the application.
  {
    Doc doc = triangleDoc("https://host/tri.bin");
    std::string seen;
    auto fetch =
        [&seen](
            const asset::ResourceRequest &request) -> terreate::Result<asset::ResolvedResource> {
      seen = request.normalized;
      return asset::ResolvedResource{asset::makeSourceIdentity("remote"), triangleBin()};
    };
    GltfDecodeOptions options;
    options.references.allowed_schemes = {"https"};
    const auto result =
        decodeText(doc.str(), asset::ResourceResolverView::bind(fetch), {}, options);
    check(decodesValid(result, "an allowed scheme reaches the resolver") &&
              seen == "https://host/tri.bin",
          "the resolver receives the normalized URI");
  }
  // The base of the source applies; the decoder never opens anything itself.
  {
    Files files;
    files.add("models/sub/tri.bin", triangleBin());
    const Doc doc = triangleDoc("sub/tri.bin");
    const Bytes bytes = toBytes(doc.str());
    const auto result =
        decode(bytes, asset::ResourceResolverView::bind(files.memory), {}, {}, "models/tri.gltf");
    check(decodesValid(result, "a relative reference resolves against the source base"),
          "relative reference");
  }
}

void embeddedUris() {
  const Bytes bin = triangleBin();
  // A malformed data URI.
  for (const std::string &uri : {std::string{"data:application/octet-stream;base64,@@@@"},
                                 std::string{"data:text/plain,hello"},
                                 std::string{"data:application/octet-stream;base64,AAA"},
                                 std::string{"data:application/octet-stream;base64"},
                                 std::string{"data:application/octet-stream;base64,AA=A"}}) {
    failedWith(decodeText(triangleDoc(uri).str()), GltfDecodeError::invalid_buffer,
               ("malformed data URI: " + uri).c_str());
  }
  // Embedded data URIs can be switched off; they are then ordinary (rejected) references.
  GltfDecodeOptions off;
  off.embedded_data_uris = false;
  Files bound;
  failedWith(decodeText(triangleDoc(dataUri(bin)).str(),
                        asset::ResourceResolverView::bind(bound.memory), {}, off),
             asset::ResolveError::reference_rejected, "data URIs off: the reference check refuses");
  failedWith(decodeText(triangleDoc(dataUri(bin)).str(), {}, {}, off),
             GltfDecodeError::missing_resolver,
             "data URIs off and no resolver: the reference is external");
  // An embedded buffer counts against the resource limits.
  GltfDecodeOptions small;
  small.limits.max_resource_bytes = 16;
  failedWith(decodeText(triangleDoc(dataUri(bin)).str(), {}, {}, small),
             asset::ResolveError::limit_exceeded, "an embedded buffer above max_resource_bytes");
  GltfDecodeOptions total;
  total.limits.max_total_resource_bytes = 16;
  failedWith(decodeText(triangleDoc(dataUri(bin)).str(), {}, {}, total),
             asset::ResolveError::limit_exceeded, "an embedded buffer above the total limit");
  // A buffer with neither uri nor GLB chunk.
  failedWith(decodeText(triangleDoc("").str()), GltfDecodeError::invalid_buffer,
             "a glTF buffer without uri");
  // A GLB whose BIN chunk is shorter than the buffer declares, and one without a BIN chunk.
  Bytes cut = bin;
  cut.resize(20);
  failedWith(decode(makeGlb(triangleDoc("").str(), cut)), GltfDecodeError::invalid_buffer,
             "a BIN chunk shorter than byteLength");
  failedWith(decode(makeGlb(triangleDoc("").str(), {}, false)), GltfDecodeError::invalid_buffer,
             "a GLB without BIN chunk but a buffer without uri");
  // A GLB with a larger BIN chunk (padding) is fine; extra bytes after the chunks are noted.
  Bytes padded = makeGlb(triangleDoc("").str(), bin);
  const auto ok = decode(padded);
  check(decodesValid(ok, "a GLB with a BIN chunk decodes"), "GLB with BIN");
  Bytes trailing = padded;
  trailing.push_back(std::byte{1});
  trailing.push_back(std::byte{2});
  const auto with_trailing = decode(trailing);
  if (decodesValid(with_trailing, "trailing bytes after the GLB length are ignored"))
    check(hasDiagnostic(*with_trailing, "gltf.glb.trailing_bytes", asset::DecodeSeverity::info),
          "trailing bytes are noted");
}

void imageDecoderCases() {
  const Box box = makeBox(true);
  // No image decoder: only when an image has to be decoded.
  failedWith(decode(box.glb), GltfDecodeError::missing_image_decoder,
             "images without an image decoder");
  check(decodesValid(decode(makeGlb(triangleDoc("").str(), triangleBin())),
                     "a model without images needs no image decoder"),
        "no image, no decoder");

  // The decoder's own error passes through unchanged.
  StubImages failing;
  failing.override_ = [](const ImageDecodeRequest &) -> terreate::Result<asset::ImageData> {
    return std::unexpected(
        terreate::Error{asset::make_error_code(asset::DecodeError::malformed_input), "stub"});
  };
  failedWith(decode(box.glb, {}, ImageDecoderView::bind(failing)),
             asset::DecodeError::malformed_input, "an image decoder error is returned unchanged");

  // An exception from the decoder is a named error.
  StubImages throwing;
  throwing.override_ = [](const ImageDecodeRequest &) -> terreate::Result<asset::ImageData> {
    throw std::runtime_error("decoder exploded");
  };
  failedWith(decode(box.glb, {}, ImageDecoderView::bind(throwing)),
             GltfDecodeError::image_decoder_failed, "an exception from the image decoder");

  // An invalid image from the decoder is refused with the ImageData error.
  StubImages invalid;
  invalid.override_ = [](const ImageDecodeRequest &request) -> terreate::Result<asset::ImageData> {
    auto image = StubImages::makeImage(request.interpretation, 2, 2);
    image.data.resize(3);
    return image;
  };
  failedWith(decode(box.glb, {}, ImageDecoderView::bind(invalid)),
             asset::ImageDataError::data_out_of_bounds, "an invalid ImageData is refused");

  // The image extent is checked against the limits again.
  StubImages big;
  big.override_ = [](const ImageDecodeRequest &request) -> terreate::Result<asset::ImageData> {
    return StubImages::makeImage(request.interpretation, 8, 8);
  };
  GltfDecodeOptions options;
  options.limits.max_dimension = 4;
  failedWith(decode(box.glb, {}, ImageDecoderView::bind(big), options),
             asset::DecodeError::dimension_exceeded, "an image above max_dimension");
  options = {};
  options.limits.max_pixels = 10;
  failedWith(decode(box.glb, {}, ImageDecoderView::bind(big), options),
             asset::DecodeError::pixel_count_exceeded, "an image above max_pixels");
  // The decoder is told the limits.
  StubImages seen_limits;
  asset::DecodeLimits limits_seen{};
  seen_limits.override_ = [&limits_seen](const ImageDecodeRequest &request) {
    limits_seen = request.limits;
    return terreate::Result<asset::ImageData>{StubImages::makeImage(request.interpretation, 2, 2)};
  };
  options = {};
  options.limits.max_dimension = 100;
  check(decode(box.glb, {}, ImageDecoderView::bind(seen_limits), options).has_value() &&
            limits_seen.max_dimension == 100,
        "the image decoder receives the limits");

  // The decoded size limit counts images too.
  options = {};
  options.max_decoded_bytes = 1000;
  failedWith(decode(box.glb, {}, ImageDecoderView::bind(big), options),
             GltfDecodeError::decoded_size_exceeded, "decoded bytes above max_decoded_bytes");

  // A decoder that returns another interpretation than asked is kept and noted.
  StubImages other;
  other.override_ = [](const ImageDecodeRequest &) -> terreate::Result<asset::ImageData> {
    return StubImages::makeImage(asset::ColorInterpretation::linear, 2, 2);
  };
  const auto kept = decode(box.glb, {}, ImageDecoderView::bind(other));
  if (decodesValid(kept, "a different interpretation is kept"))
    check(
        hasDiagnostic(*kept, "gltf.image.interpretation_mismatch", asset::DecodeSeverity::warning),
        "the mismatch is a warning");

  // One image used both as a color and as a normal map: decoded once, as the first use, and
  // diagnosed.
  Doc doc = triangleDoc("");
  doc.set(
      "materials",
      R"([{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"normalTexture":{"index":0}}])");
  doc.set("textures", R"([{"source":0}])");
  doc.set("images", R"([{"bufferView":1}])");
  doc.set("bufferViews",
          R"([{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":4}])");
  doc.set("buffers", R"([{"byteLength":40}])");
  Bytes bin = triangleBin();
  bin.resize(40, std::byte{9});
  StubImages conflict;
  const auto both = decode(makeGlb(doc.str(), bin), {}, ImageDecoderView::bind(conflict));
  if (decodesValid(both, "an image used two ways decodes")) {
    check(conflict.seen.size() == 1 &&
              conflict.seen[0].interpretation == asset::ColorInterpretation::srgb,
          "decoded once, as srgb (the first use)");
    check(
        hasDiagnostic(*both, "gltf.image.interpretation_conflict", asset::DecodeSeverity::warning),
        "the conflict is a warning");
  }
  // An image that no material uses is decoded as srgb and noted.
  Doc unused = triangleDoc("");
  unused.set("textures", R"([{"source":0}])");
  unused.set("images", R"([{"bufferView":1}])");
  unused.set("bufferViews",
             R"([{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":4}])");
  unused.set("buffers", R"([{"byteLength":40}])");
  StubImages plain;
  const auto defaulted = decode(makeGlb(unused.str(), bin), {}, ImageDecoderView::bind(plain));
  if (decodesValid(defaulted, "an unused image decodes")) {
    check(plain.seen.size() == 1 &&
              plain.seen[0].interpretation == asset::ColorInterpretation::srgb,
          "an unused image is srgb");
    check(hasDiagnostic(*defaulted, "gltf.image.interpretation_defaulted",
                        asset::DecodeSeverity::info),
          "the default is noted");
  }
  // An image with neither uri nor bufferView, a texture without source, a bad sampler enum.
  Doc empty_image = unused;
  empty_image.set("images", R"([{}])");
  failedWith(decode(makeGlb(empty_image.str(), bin), {}, ImageDecoderView::bind(plain)),
             GltfDecodeError::invalid_resource, "an image without a source");
  Doc no_source = unused;
  no_source.set("textures", R"([{}])");
  failedWith(decode(makeGlb(no_source.str(), bin), {}, ImageDecoderView::bind(plain)),
             GltfDecodeError::invalid_resource, "a texture without a source");
  Doc bad_sampler = unused;
  bad_sampler.set("samplers", R"([{"magFilter":97286}])");
  failedWith(decode(makeGlb(bad_sampler.str(), bin), {}, ImageDecoderView::bind(plain)),
             GltfDecodeError::invalid_resource, "a sampler with an unknown filter");
  // Sampler enumerators.
  Doc samplers = unused;
  samplers.set("samplers",
               R"([{"magFilter":9729,"minFilter":9728},{"minFilter":9984},{"minFilter":9985},)"
               R"({"minFilter":9986},{"minFilter":9987,"wrapS":10497,"wrapT":33071},{}])");
  const auto mapped = decode(makeGlb(samplers.str(), bin), {}, ImageDecoderView::bind(plain));
  if (decodesValid(mapped, "sampler enumerators decode")) {
    using F = asset::SamplerFilter;
    using M = asset::SamplerMipmapMode;
    const auto &s = mapped->model.samplers;
    check(s.size() == 6 && s[0].mag_filter == F::linear && s[0].min_filter == F::nearest &&
              s[0].mipmap_mode == M::none && s[1].min_filter == F::nearest &&
              s[1].mipmap_mode == M::nearest && s[2].min_filter == F::linear &&
              s[2].mipmap_mode == M::nearest && s[3].min_filter == F::nearest &&
              s[3].mipmap_mode == M::linear && s[4].min_filter == F::linear &&
              s[4].mipmap_mode == M::linear && s[4].wrap_u == asset::SamplerWrap::repeat &&
              s[4].wrap_v == asset::SamplerWrap::clamp_to_edge && s[5] == asset::SamplerData{},
          "filter and wrap mapping");
  }
}

// One file with many unsupported features (also a fuzzing seed).
std::string unsupportedDoc() {
  // One file with an animation, a skin with its attributes, a morph target, a camera, a punctual
  // light, a texture transform, extras, an unknown extension, a clearcoat material, a primitive
  // with a mode that is no glTF mode (skipped) next to a triangle primitive (kept) and a mesh that
  // has only such a primitive.
  BinBuilder bin;
  const std::array<float, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
  const std::array<float, 3> times{0, 1, 2};
  const std::array<std::uint8_t, 12> joints{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const std::array<float, 12> weights{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
  bin.floats(positions);
  bin.floats(times);
  bin.u8(joints);
  bin.floats(weights);
  const std::array<float, 6> uvs{0, 0, 1, 0, 0, 1};
  bin.floats(uvs);
  Doc doc;
  doc.set("asset", R"({"version":"2.0","extras":{"a":1}})");
  doc.set(
      "extensionsUsed",
      R"(["KHR_lights_punctual","KHR_texture_transform","KHR_materials_clearcoat","EXT_unknown_thing"])");
  doc.set("extras", R"({"hello":[1,2,3]})");
  doc.set("scene", "0");
  doc.set("scenes", R"([{"nodes":[0,1,2]}])");
  doc.set("nodes", R"([{"name":"skinned","mesh":0,"skin":0,"weights":[0.5],"extras":{"x":1}},)"
                   R"({"name":"cam","camera":0},)"
                   R"({"name":"lamp","extensions":{"KHR_lights_punctual":{"light":0}}},)"
                   R"({"name":"fan","mesh":1}])");
  doc.set("scenes", R"([{"nodes":[0,1,2,3]}])");
  doc.set("extensions",
          R"({"KHR_lights_punctual":{"lights":[{"name":"sun","type":"directional"}]}})");
  doc.set("cameras",
          R"([{"name":"main","type":"perspective","perspective":{"yfov":1,"znear":0.1}}])");
  doc.set("skins", R"([{"name":"rig","joints":[1]}])");
  doc.set("animations",
          R"([{"name":"Walk","channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}],)"
          R"("samplers":[{"input":1,"output":0}]}])");
  doc.set("meshes",
          R"([{"name":"m","weights":[0.5],"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":2,)"
          R"("WEIGHTS_0":3,"_CUSTOM":0,"TEXCOORD_0":4},"targets":[{"POSITION":0}],"material":0},)"
          R"({"attributes":{"POSITION":0},"mode":9}]},)"
          R"({"name":"fanmesh","primitives":[{"attributes":{"POSITION":0},"mode":9}]}])");
  doc.set("materials", R"([{"name":"fancy","pbrMetallicRoughness":{"baseColorTexture":{"index":0,)"
                       R"("extensions":{"KHR_texture_transform":{"scale":[2,2]}}}},)"
                       R"("extensions":{"KHR_materials_clearcoat":{"clearcoatFactor":1}}}])");
  doc.set("textures", R"([{"source":0}])");
  doc.set("images", R"([{"bufferView":4}])");
  doc.set("accessors", R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
                       R"({"bufferView":1,"componentType":5126,"count":3,"type":"SCALAR"},)"
                       R"({"bufferView":2,"componentType":5121,"count":3,"type":"VEC4"},)"
                       R"({"bufferView":3,"componentType":5126,"count":3,"type":"VEC4"},)"
                       R"({"bufferView":5,"componentType":5126,"count":3,"type":"VEC2"}])");
  doc.set(
      "bufferViews",
      R"([{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":12},)"
      R"({"buffer":0,"byteOffset":48,"byteLength":12},{"buffer":0,"byteOffset":60,"byteLength":48},)"
      R"({"buffer":0,"byteOffset":0,"byteLength":4},)"
      R"({"buffer":0,"byteOffset":108,"byteLength":24}])");
  doc.set("buffers", R"([{"byteLength":132,"uri":")" + dataUri(bin.data) + R"("}])");
  return doc.str();
}

void unsupportedFeatures() {
  StubImages images;
  const Bytes text = toBytes(unsupportedDoc());
  const auto result = decode(text, {}, ImageDecoderView::bind(images));
  if (!decodesValid(result, "a file with unsupported features still decodes"))
    return;
  const auto &model = result->model;
  for (const char *kind :
       {"animation", "skin", "morph-target", "camera", "light", "texture-transform", "extras",
        "extension", "material-extension", "attribute", "primitive-mode", "mesh"}) {
    check(hasUnsupported(model, kind), (std::string{"unsupported kind reported: "} + kind).c_str());
    check(hasDiagnostic(*result, std::string{"gltf.unsupported."} + kind,
                        asset::DecodeSeverity::warning),
          (std::string{"warning diagnostic: "} + kind).c_str());
  }
  check(hasUnsupported(model, "animation", "Walk") && hasUnsupported(model, "skin", "rig") &&
            hasUnsupported(model, "camera", "main") && hasUnsupported(model, "light", "sun") &&
            hasUnsupported(model, "attribute", "JOINTS_0") &&
            hasUnsupported(model, "attribute", "WEIGHTS_0") &&
            hasUnsupported(model, "attribute", "_CUSTOM") &&
            hasUnsupported(model, "extension", "KHR_lights_punctual") &&
            hasUnsupported(model, "extension", "EXT_unknown_thing") &&
            hasUnsupported(model, "material-extension", "KHR_materials_clearcoat") &&
            hasUnsupported(model, "primitive-mode", "unknown mode"),
        "the unsupported entries are named");
  // Kept: the triangle primitive of the first mesh; the primitive with the unknown mode and
  // the mesh that has only one are skipped (the second mesh has no representable primitive, so its
  // node has no mesh).
  check(model.meshes.size() == 1 && model.primitives.size() == 1 && model.model_meshes.size() == 1,
        "only the representable primitive is kept");
  check(model.nodes.size() == 4 && model.nodes[0].mesh == 0 && !model.nodes[3].mesh,
        "the node of the skipped mesh has no mesh");
  check(near(positionsOf(model.meshes[0]), {0, 0, 0, 1, 0, 0, 0, 1, 0}),
        "the base geometry of a morphed, skinned primitive is kept");
  check(model.meshes[0].attributes.size() == 2, "only the position and texcoord streams are kept");
  check(model.materials.size() == 1 && model.materials[0].base_color_texture.has_value(),
        "the material and its texture reference are kept despite the transform");
  check(model.primitives[0].material == 0, "the primitive keeps its material");
  check(customValue(model, "gltf.version") == "2.0" && result->native.extensions_used.size() == 4,
        "native info lists the extensions used");
}

// A document with 4 positions and primitives of every mode the Adapter maps to a topology, with
// the index accessors the primitives share: 1 is u16 {0,2,1,3}, 2 is u16 {0,1,2}, 3 is u32 {3,2,1}.
Doc modesDoc(const std::string &primitives) {
  BinBuilder bin;
  const std::array<float, 12> positions{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  bin.floats(positions);
  const std::array<std::uint16_t, 4> fan{0, 2, 1, 3};
  const std::size_t fan_at = bin.u16(fan);
  const std::array<std::uint16_t, 3> three{0, 1, 2};
  const std::size_t three_at = bin.u16(three);
  const std::array<std::uint32_t, 3> wide{3, 2, 1};
  const std::size_t wide_at = bin.u32(wide);
  Doc doc;
  doc.set("asset", R"({"version":"2.0"})");
  doc.set("scene", "0");
  doc.set("scenes", R"([{"nodes":[0]}])");
  doc.set("nodes", R"([{"mesh":0}])");
  doc.set("meshes", R"([{"primitives":[)" + primitives + R"(]}])");
  doc.set("accessors", R"([{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)"
                       R"({"bufferView":1,"componentType":5123,"count":4,"type":"SCALAR"},)"
                       R"({"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"},)"
                       R"({"bufferView":3,"componentType":5125,"count":3,"type":"SCALAR"}])");
  doc.set("bufferViews",
          R"([{"buffer":0,"byteLength":48},{"buffer":0,"byteOffset":)" + std::to_string(fan_at) +
              R"(,"byteLength":8},{"buffer":0,"byteOffset":)" + std::to_string(three_at) +
              R"(,"byteLength":6},{"buffer":0,"byteOffset":)" + std::to_string(wide_at) +
              R"(,"byteLength":12}])");
  doc.set("buffers", R"([{"byteLength":)" + std::to_string(bin.data.size()) + R"(,"uri":")" +
                         dataUri(bin.data) + R"("}])");
  return doc;
}

std::vector<std::uint32_t> indicesOf(const asset::MeshData &mesh) {
  std::vector<std::uint32_t> out(mesh.indexCount());
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = mesh.index(i).value_or(0);
  return out;
}

// #475: line strip, line loop and triangle fan are mapped, not skipped.
void lineAndFanModes() {
  using T = asset::PrimitiveTopology;
  using I = std::vector<std::uint32_t>;
  const std::string primitives = R"({"attributes":{"POSITION":0},"mode":6},)"
                                 R"({"attributes":{"POSITION":0},"mode":6,"indices":1},)"
                                 R"({"attributes":{"POSITION":0},"mode":3},)"
                                 R"({"attributes":{"POSITION":0},"mode":2},)"
                                 R"({"attributes":{"POSITION":0},"mode":2,"indices":2},)"
                                 R"({"attributes":{"POSITION":0},"mode":2,"indices":3},)"
                                 R"({"attributes":{"POSITION":0},"mode":5},)"
                                 R"({"attributes":{"POSITION":0},"mode":1},)"
                                 R"({"attributes":{"POSITION":0},"mode":0},)"
                                 R"({"attributes":{"POSITION":0},"mode":3,"indices":1})";
  const std::string text = modesDoc(primitives).str();
  const auto result = decodeText(text);
  if (!decodesValid(result, "line strip, line loop and fan primitives decode"))
    return;
  const auto &model = result->model;
  check(model.meshes.size() == 10 && model.primitives.size() == 10, "every primitive is kept");
  check(model.unsupported.empty() && !hasUnsupported(model, "primitive-mode") &&
            !hasDiagnostic(*result, "gltf.unsupported.primitive-mode"),
        "no mode is reported as unsupported");
  const auto &m = model.meshes;
  check(m[0].topology == T::triangle_fan && m[0].indexType() == asset::IndexType::none &&
            m[0].vertexCount() == 4,
        "a non-indexed fan stays non-indexed");
  check(m[1].topology == T::triangle_fan && indicesOf(m[1]) == I{0, 2, 1, 3}, "an indexed fan");
  check(m[2].topology == T::line_strip && m[2].indexType() == asset::IndexType::none,
        "a non-indexed line strip");
  check(m[3].topology == T::line_strip && m[3].indexType() == asset::IndexType::uint16 &&
            indicesOf(m[3]) == I{0, 1, 2, 3, 0},
        "a non-indexed loop gets indices 0..n-1, 0");
  check(m[4].topology == T::line_strip && indicesOf(m[4]) == I{0, 1, 2, 0},
        "an indexed loop repeats its first index");
  check(m[5].topology == T::line_strip && m[5].indexType() == asset::IndexType::uint32 &&
            indicesOf(m[5]) == I{3, 2, 1, 3},
        "a 32-bit indexed loop stays 32-bit");
  check(m[6].topology == T::triangle_strip && m[6].indexType() == asset::IndexType::none,
        "a strip is unchanged");
  check(m[7].topology == T::line_list && m[8].topology == T::point_list,
        "lines and points are unchanged");
  check(m[9].topology == T::line_strip && indicesOf(m[9]) == I{0, 2, 1, 3},
        "an indexed line strip is not closed");
  const auto closed =
      std::ranges::count_if(result->diagnostics, [](const asset::DecodeDiagnostic &d) {
        return d.code == "gltf.line_loop.closed" && d.severity == asset::DecodeSeverity::info &&
               d.message.find("LINE_LOOP") != std::string::npos;
      });
  check(closed == 3, "one info diagnostic per closed loop");

  // The fan converts explicitly to a list that matches the quad.
  const auto list = asset::convertTopology(m[0], T::triangle_list);
  check(list && indicesOf(*list) == I{0, 1, 2, 0, 2, 3} && asset::validate(*list).has_value(),
        "a decoded fan converts to the triangle list of the quad");
  const auto segments = asset::convertTopology(m[3], T::line_list);
  check(segments && indicesOf(*segments) == I{0, 1, 1, 2, 2, 3, 3, 0},
        "a decoded loop converts to the closed segment list");

  // Deterministic.
  const auto again = decodeText(text);
  bool same = again.has_value() && again->model.meshes.size() == m.size();
  for (std::size_t i = 0; same && i < m.size(); ++i)
    same = again->model.meshes[i].topology == m[i].topology &&
           again->model.meshes[i].indices == m[i].indices;
  check(same, "decoding twice gives the same meshes");
}

// Short or hostile primitives: a named error, never a crash.
void lineAndFanShortPrimitives() {
  const auto one = [](const char *mode, std::size_t count, const char *indices = "") {
    Doc doc = triangleDoc(dataUri(triangleBin()));
    doc.set("meshes", std::string{R"([{"primitives":[{"attributes":{"POSITION":0},"mode":)"} +
                          mode + indices + "}]}]");
    doc.set("accessors", R"([{"bufferView":0,"componentType":5126,"count":)" +
                             std::to_string(count) +
                             R"(,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":)" +
                             std::to_string(count) + R"(,"type":"SCALAR"}])");
    doc.set("bufferViews", R"([{"buffer":0,"byteLength":36},{"buffer":0,"byteLength":4}])");
    return decodeText(doc.str());
  };
  const auto named = [&](const Result &result, const char *message) {
    check(!result.has_value() &&
              result.error().code() ==
                  asset::make_error_code(asset::ModelDataError::invalid_mesh) &&
              result.error().detail().find("not valid for the topology") != std::string::npos,
          message);
  };
  named(one("2", 1), "a loop of 1 vertex is a named invalid_mesh error");
  named(one("3", 1), "a line strip of 1 vertex is a named invalid_mesh error");
  named(one("6", 2), "a fan of 2 vertices is a named invalid_mesh error");
  named(one("6", 1), "a fan of 1 vertex is a named invalid_mesh error");
  named(one("2", 1, R"(,"indices":1)"), "a loop with 1 index is a named invalid_mesh error");
  named(one("6", 2, R"(,"indices":1)"), "a fan with 2 indices is a named invalid_mesh error");
  const auto pair = one("2", 2);
  check(pair.has_value() && indicesOf(pair->model.meshes[0]) == std::vector<std::uint32_t>{0, 1, 0},
        "a loop of 2 vertices closes to 0 1 0");
  const auto triangle = one("6", 3);
  check(triangle.has_value(), "a fan of 3 vertices is valid");
}

// The index added to close a loop counts against max_decoded_bytes.
void lineLoopLimit() {
  const std::string loop = modesDoc(R"({"attributes":{"POSITION":0},"mode":2})").str();
  const std::string strip = modesDoc(R"({"attributes":{"POSITION":0},"mode":3})").str();
  const auto smallest = [](const std::string &text) {
    for (std::uint64_t limit = 0; limit < 4096; ++limit) {
      GltfDecodeOptions options;
      options.max_decoded_bytes = limit;
      if (decode(toBytes(text), {}, {}, options).has_value())
        return limit;
    }
    return std::uint64_t{0};
  };
  const std::uint64_t loop_limit = smallest(loop);
  const std::uint64_t strip_limit = smallest(strip);
  // Four vertices: the loop adds five 16-bit indices that the strip does not have.
  check(strip_limit != 0 && loop_limit == strip_limit + 5 * sizeof(std::uint16_t),
        "the closing index buffer is charged to the decoded size limit");
}

void requiredExtensionsAndVersions() {
  Doc doc = triangleDoc(dataUri(triangleBin()));
  doc.set("extensionsUsed", R"(["KHR_materials_unlit"])");
  doc.set("extensionsRequired", R"(["KHR_materials_unlit"])");
  const auto required = decodeText(doc.str());
  if (failedWith(required, GltfDecodeError::unsupported_required_extension,
                 "a required extension is refused"))
    check(required.error().detail().find("KHR_materials_unlit") != std::string::npos,
          "the error names the extension");
  Doc draco = triangleDoc(dataUri(triangleBin()));
  draco.set("extensionsRequired", R"(["KHR_draco_mesh_compression"])");
  failedWith(decodeText(draco.str()), GltfDecodeError::unsupported_required_extension,
             "a required Draco extension is refused");

  Doc old = triangleDoc(dataUri(triangleBin()));
  old.set("asset", R"({"version":"1.0"})");
  failedWith(decodeText(old.str()), GltfDecodeError::unsupported_version, "glTF 1.0");
  old.set("asset", R"({"version":"3.0"})");
  failedWith(decodeText(old.str()), GltfDecodeError::unsupported_version, "version 3.0");
  old.set("asset", R"({"version":"2.0","minVersion":"2.1"})");
  failedWith(decodeText(old.str()), GltfDecodeError::unsupported_version, "minVersion 2.1");
  old.set("asset", R"({"generator":"x"})");
  failedWith(decodeText(old.str()), GltfDecodeError::unsupported_version, "no version");
  old.set("asset", R"({"version":"2.1"})");
  check(decodesValid(decodeText(old.str()), "2.1 is accepted (the 2.x series)"), "version 2.1");

  // A GLB container of another version.
  Bytes glb = toBytes(triangle_glb);
  glb[4] = std::byte{3};
  failedWith(decode(glb), GltfDecodeError::unsupported_version, "GLB container version 3");
}

std::string nestedBrackets(std::size_t depth) {
  std::string text;
  for (std::size_t i = 0; i < depth; ++i)
    text += "[";
  for (std::size_t i = 0; i < depth; ++i)
    text += "]";
  return text;
}

void malformedInputs() {
  const auto malformed = [](const Bytes &bytes, const char *message) {
    failedWith(decode(bytes), GltfDecodeError::malformed_gltf, message);
  };
  malformed({}, "empty input");
  malformed(toBytes("not json at all, long enough to pass"), "garbage text");
  malformed(toBytes("{\"asset\":{\"version\":\"2.0\""), "truncated JSON");
  malformed(toBytes("[1,2,3,4,5,6,7,8,9,10,11,12]"), "a JSON array");
  malformed(toBytes("{\"nodes\": 5, \"asset\":{\"version\":\"2.0\"}}"), "a wrongly typed member");
  malformed(toBytes("{}"), "too short to be glTF");
  malformed(toBytes(std::string(64, '\0')), "NUL bytes");

  // Bad references inside a valid document.
  Doc bad_child = triangleDoc(dataUri(triangleBin()));
  bad_child.set("nodes", R"([{"mesh":0,"children":[5]}])");
  malformed(toBytes(bad_child.str()), "a child index out of range");
  Doc bad_mesh = triangleDoc(dataUri(triangleBin()));
  bad_mesh.set("nodes", R"([{"mesh":7}])");
  malformed(toBytes(bad_mesh.str()), "a mesh index out of range");
  Doc bad_accessor = triangleDoc(dataUri(triangleBin()));
  bad_accessor.set("meshes", R"([{"primitives":[{"attributes":{"POSITION":9}}]}])");
  malformed(toBytes(bad_accessor.str()), "an accessor index out of range");
  Doc bad_view = triangleDoc(dataUri(triangleBin()));
  bad_view.set("accessors", R"([{"bufferView":3,"componentType":5126,"count":3,"type":"VEC3"}])");
  malformed(toBytes(bad_view.str()), "a bufferView index out of range");
  Doc bad_scene_node = triangleDoc(dataUri(triangleBin()));
  bad_scene_node.set("scenes", R"([{"nodes":[4]}])");
  malformed(toBytes(bad_scene_node.str()), "a scene node out of range");
  Doc no_primitives = triangleDoc(dataUri(triangleBin()));
  no_primitives.set("meshes", R"([{"primitives":[]}])");
  malformed(toBytes(no_primitives.str()), "a mesh without primitives");
  Doc two_parents = triangleDoc(dataUri(triangleBin()));
  two_parents.set("nodes", R"([{"children":[2]},{"children":[2]},{"mesh":0}])");
  malformed(toBytes(two_parents.str()), "a node with two parents");
  Doc cycle = triangleDoc(dataUri(triangleBin()));
  cycle.set("nodes", R"([{"children":[1]},{"children":[0]}])");
  cycle.set("scenes", R"([])");
  cycle.drop("scene");
  malformed(toBytes(cycle.str()), "a cycle of two nodes");
  Doc self = cycle;
  self.set("nodes", R"([{"children":[0]}])");
  malformed(toBytes(self.str()), "a node that is its own child");
  Doc root_in_cycle = cycle;
  root_in_cycle.set("nodes", R"([{"children":[1]},{"children":[2]},{"children":[1]}])");
  root_in_cycle.set("scenes", R"([{"nodes":[0]}])");
  root_in_cycle.set("scene", "0");
  malformed(toBytes(root_in_cycle.str()), "a cycle below a root");

  // GLB structure.
  const Bytes glb = toBytes(triangle_glb);
  Bytes wrong_json_type = glb;
  wrong_json_type[16] = std::byte{'X'};
  malformed(wrong_json_type, "the first chunk is not JSON");
  Bytes bad_length = glb;
  bad_length[8] = std::byte{0xFF};
  bad_length[9] = std::byte{0xFF};
  malformed(bad_length, "the GLB length field is larger than the input");
  Bytes huge_chunk = glb;
  huge_chunk[12] = std::byte{0xFF};
  huge_chunk[13] = std::byte{0xFF};
  huge_chunk[14] = std::byte{0xFF};
  huge_chunk[15] = std::byte{0xFF};
  malformed(huge_chunk, "a chunk length of 4 GiB");
  Bytes other_chunk = glb;
  const std::size_t bin_header = 20 + 380;
  other_chunk[bin_header + 4] = std::byte{'Z'};
  malformed(other_chunk, "an unknown second chunk type");
  Bytes bin_too_long = glb;
  bin_too_long[bin_header] = std::byte{0xFF};
  bin_too_long[bin_header + 1] = std::byte{0xFF};
  malformed(bin_too_long, "a BIN chunk that runs past the GLB");
  malformed(Bytes(glb.begin(), glb.begin() + 11), "a GLB cut inside the header");
  malformed(Bytes(glb.begin(), glb.begin() + 19), "a GLB cut inside the chunk header");
  Bytes short_glb = Bytes(glb.begin(), glb.begin() + 100);
  short_glb[8] = std::byte{100};
  short_glb[9] = std::byte{0};
  malformed(short_glb, "a JSON chunk that runs past the GLB length");

  // Hostile nesting: refused before the parser recurses.
  malformed(toBytes(nestedBrackets(1'000'000)), "a million nested arrays");
  Doc deep = triangleDoc(dataUri(triangleBin()));
  deep.set("extras", std::string{"{\"a\":"} + nestedBrackets(200) + "}");
  malformed(toBytes(deep.str()), "extras nested 200 levels");
  // Legal nesting below the limit is accepted; the extras are reported.
  Doc shallow = triangleDoc(dataUri(triangleBin()));
  shallow.set("extras", std::string{"{\"a\":"} + nestedBrackets(100) + "}");
  const auto shallow_result = decodeText(shallow.str());
  if (decodesValid(shallow_result, "extras nested 100 levels decode"))
    check(hasUnsupported(shallow_result->model, "extras"), "the extras are reported");
  // Brackets inside strings do not count.
  Doc quoted = triangleDoc(dataUri(triangleBin()));
  quoted.set("extras", "\"" + std::string(500, '[') + "\"");
  check(decodesValid(decodeText(quoted.str()), "brackets in a string"), "string brackets");
}

void accessorErrors() {
  const std::string uri = dataUri(triangleBin());
  const auto accessors = [&](const std::string &accessor, const std::string &view,
                             GltfDecodeError code, const char *message) {
    Doc doc = triangleDoc(uri);
    doc.set("accessors", "[" + accessor + "]");
    if (!view.empty())
      doc.set("bufferViews", "[" + view + "]");
    failedWith(decodeText(doc.str()), code, message);
  };
  const std::string ok_view = R"({"buffer":0,"byteLength":36})";
  accessors(R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"})", ok_view,
            GltfDecodeError::invalid_accessor, "an accessor that runs past its view");
  accessors(R"({"bufferView":0,"byteOffset":4,"componentType":5126,"count":3,"type":"VEC3"})",
            ok_view, GltfDecodeError::invalid_accessor,
            "an accessor offset that runs past its view");
  accessors(
      R"({"bufferView":0,"byteOffset":18446744073709551615,"componentType":5126,"count":3,"type":"VEC3"})",
      ok_view, GltfDecodeError::invalid_accessor, "an offset near 2^64");
  accessors(R"({"bufferView":0,"componentType":5126,"count":4611686018427387904,"type":"VEC3"})",
            ok_view, GltfDecodeError::invalid_accessor, "a count near 2^62");
  accessors(R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"})",
            R"({"buffer":0,"byteOffset":8,"byteLength":36})", GltfDecodeError::invalid_accessor,
            "a buffer view that runs past its buffer");
  accessors(R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"})",
            R"({"buffer":0,"byteOffset":18446744073709551615,"byteLength":36})",
            GltfDecodeError::invalid_accessor, "a view offset near 2^64");
  accessors(R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"})",
            R"({"buffer":0,"byteLength":36,"byteStride":4})", GltfDecodeError::invalid_accessor,
            "a byteStride smaller than the element");
  accessors(R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"})",
            R"({"buffer":0,"byteLength":36,"byteStride":18446744073709551615})",
            GltfDecodeError::invalid_accessor, "a huge byteStride");
  accessors(R"({"bufferView":0,"componentType":5126,"count":0,"type":"VEC3"})", ok_view,
            GltfDecodeError::invalid_accessor, "an accessor without elements");
  accessors(R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC2"})", ok_view,
            GltfDecodeError::invalid_accessor, "a POSITION that is not VEC3");
  accessors(R"({"bufferView":0,"componentType":5126,"count":3,"type":"MAT4"})", ok_view,
            GltfDecodeError::invalid_accessor, "a matrix accessor");
  accessors(R"({"bufferView":0,"componentType":5122,"count":3,"type":"VEC3"})", ok_view,
            GltfDecodeError::invalid_accessor, "a non-normalized integer POSITION");
  accessors(R"({"bufferView":0,"componentType":5125,"normalized":true,"count":3,"type":"VEC3"})",
            ok_view, GltfDecodeError::invalid_accessor, "a normalized 32-bit integer POSITION");

  // A buffer view without a buffer is not valid glTF at all.
  Doc no_buffer = triangleDoc(uri);
  no_buffer.set("bufferViews", R"([{"byteLength":36}])");
  failedWith(decodeText(no_buffer.str()), GltfDecodeError::malformed_gltf, "a view without buffer");

  // Indices.
  const auto indices = [&](const std::string &accessor, const char *message) {
    Doc doc = triangleDoc(uri);
    doc.set("meshes", R"([{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}])");
    doc.set("accessors",
            R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)" + accessor + "]");
    failedWith(decodeText(doc.str()), GltfDecodeError::invalid_accessor, message);
  };
  indices(R"({"bufferView":0,"componentType":5126,"count":3,"type":"SCALAR"})", "float indices");
  indices(R"({"bufferView":0,"componentType":5123,"count":3,"type":"VEC2"})", "vec2 indices");
  indices(R"({"bufferView":0,"componentType":5123,"count":0,"type":"SCALAR"})", "no indices");
  indices(R"({"bufferView":0,"componentType":5123,"count":99,"type":"SCALAR"})",
          "indices past the view");
}

void indexRange() {
  // Valid indices, then one past the vertex count.
  BinBuilder bin;
  const std::array<float, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
  bin.floats(positions);
  const std::array<std::uint16_t, 3> good{0, 1, 2};
  const std::size_t good_at = bin.u16(good);
  const std::array<std::uint16_t, 3> bad{0, 1, 3};
  const std::size_t bad_at = bin.u16(bad);
  const std::array<std::uint32_t, 3> wide{2, 1, 0};
  const std::size_t wide_at = bin.u32(wide);
  const std::array<std::uint32_t, 3> wide_bad{0, 1, 4000000000U};
  const std::size_t wide_bad_at = bin.u32(wide_bad);
  const std::array<std::uint8_t, 3> bytes{2, 0, 1};
  const std::size_t bytes_at = bin.u8(bytes);
  bin.data.resize(bin.data.size() + 12);
  const auto make = [&](std::size_t at, const char *component) {
    Doc doc = triangleDoc(dataUri(bin.data));
    doc.set("buffers", R"([{"byteLength":)" + std::to_string(bin.data.size()) + R"(,"uri":")" +
                           dataUri(bin.data) + R"("}])");
    doc.set("bufferViews", R"([{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":)" +
                               std::to_string(at) + R"(,"byteLength":12}])");
    doc.set("meshes", R"([{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}])");
    doc.set("accessors", R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
                         R"({"bufferView":1,"componentType":)" +
                             std::string{component} + R"(,"count":3,"type":"SCALAR"}])");
    return decodeText(doc.str());
  };
  const auto ok16 = make(good_at, "5123");
  if (decodesValid(ok16, "16-bit indices decode")) {
    check(ok16->model.meshes[0].indexType() == asset::IndexType::uint16 &&
              ok16->model.meshes[0].index(2) == 2U,
          "uint16 indices");
  }
  failedWith(make(bad_at, "5123"), GltfDecodeError::invalid_accessor,
             "an index equal to the count");
  const auto ok32 = make(wide_at, "5125");
  if (decodesValid(ok32, "32-bit indices decode")) {
    check(ok32->model.meshes[0].indexType() == asset::IndexType::uint32 &&
              ok32->model.meshes[0].index(0) == 2U,
          "uint32 indices");
  }
  failedWith(make(wide_bad_at, "5125"), GltfDecodeError::invalid_accessor, "an index near 2^32");
  const auto ok8 = make(bytes_at, "5121");
  if (decodesValid(ok8, "8-bit indices decode")) {
    check(ok8->model.meshes[0].indexType() == asset::IndexType::uint16 &&
              ok8->model.meshes[0].index(0) == 2U && ok8->model.meshes[0].index(2) == 1U,
          "uint8 indices are widened to uint16");
    check(hasDiagnostic(*ok8, "gltf.index.widened", asset::DecodeSeverity::info),
          "the widening is noted");
  }
}

// A document with sparse, normalized and zero accessors (also a fuzzing seed).
Doc sparseDoc() {
  BinBuilder bin;
  const std::array<float, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
  bin.floats(positions);
  const std::array<std::uint8_t, 2> sparse_indices{2, 0};
  const std::size_t sparse_index_at = bin.u8(sparse_indices);
  const std::array<float, 6> sparse_values{5, 6, 7, 8, 9, 10};
  const std::size_t sparse_value_at = bin.floats(sparse_values);
  const std::array<std::uint16_t, 6> uvs{0, 65535, 32768, 0, 65535, 65535};
  const std::size_t uv_at = bin.u16(uvs);
  const std::array<std::uint8_t, 12> colors{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0};
  const std::size_t color_at = bin.u8(colors);
  const std::array<std::int8_t, 4> unused{};
  (void)unused;
  const auto num = [](std::size_t v) { return std::to_string(v); };
  Doc doc = triangleDoc("");
  doc.set("buffers", R"([{"byteLength":)" + num(bin.data.size()) + R"(,"uri":")" +
                         dataUri(bin.data) + R"("}])");
  doc.set("bufferViews",
          R"([{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":)" + num(sparse_index_at) +
              R"(,"byteLength":2},{"buffer":0,"byteOffset":)" + num(sparse_value_at) +
              R"(,"byteLength":24},{"buffer":0,"byteOffset":)" + num(uv_at) +
              R"(,"byteLength":12},{"buffer":0,"byteOffset":)" + num(color_at) +
              R"(,"byteLength":12}])");
  doc.set(
      "meshes",
      R"([{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"TEXCOORD_1":2,"COLOR_0":3,)"
      R"("COLOR_1":4}}]}])");
  doc.set("accessors",
          // 0: positions with a sparse override of vertices 2 and 0; 1: normalized ushort UV;
          // 2: UV with no buffer view (zeros) and a sparse override; 3: normalized ubyte RGBA
          // colors... (3 vertices x 4); 4: normalized ubyte RGB with a stride.
          R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","sparse":{"count":2,)"
          R"("indices":{"bufferView":1,"componentType":5121},"values":{"bufferView":2}}},)"
          R"({"bufferView":3,"componentType":5123,"normalized":true,"count":3,"type":"VEC2"},)"
          R"({"componentType":5126,"count":3,"type":"VEC2","sparse":{"count":1,)"
          R"("indices":{"bufferView":1,"byteOffset":1,"componentType":5121},)"
          R"("values":{"bufferView":2,"byteOffset":8}}},)"
          R"({"bufferView":4,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},)"
          R"({"bufferView":4,"componentType":5121,"normalized":true,"count":3,"type":"VEC3"}])");
  return doc;
}

void sparseAndNormalized() {
  const Doc doc = sparseDoc();
  const auto result = decodeText(doc.str());
  if (!decodesValid(result, "sparse and normalized accessors decode"))
    return;
  const auto &mesh = result->model.meshes[0];
  // Vertex 2 gets the first sparse value (5,6,7), vertex 0 the second (8,9,10).
  check(near(positionsOf(mesh), {8, 9, 10, 1, 0, 0, 5, 6, 7}), "sparse values override the base");
  check(near(floatsOf(mesh, {asset::AttributeSemantic::texcoord, 0, {}}),
             {0.0F, 1.0F, 32768.0F / 65535.0F, 0.0F, 1.0F, 1.0F}),
        "normalized unsigned short UVs");
  // Accessor 2 has no data; its sparse entry (index 0, from the second index byte) takes the
  // value at byteOffset 8 of the values view: floats (7, 8).
  check(near(floatsOf(mesh, {asset::AttributeSemantic::texcoord, 1, {}}), {7, 8, 0, 0, 0, 0}),
        "an accessor without buffer view is zero, with its sparse override");
  const auto colors0 = floatsOf(mesh, {asset::AttributeSemantic::color, 0, {}});
  check(colors0.size() == 12 && colors0[0] == 1.0F && colors0[1] == 0.0F &&
            std::fabs(colors0[7] - 128.0F / 255.0F) < 1e-6F,
        "normalized unsigned byte RGBA colors");
  const auto *color1 = mesh.find({asset::AttributeSemantic::color, 1, {}});
  check(color1 != nullptr && color1->format == asset::AttributeFormat::float32x3 &&
            color1->count == 3,
        "a COLOR_1 vec3 is float32x3");
  check(hasDiagnostic(*result, "gltf.attribute.normalized_converted", asset::DecodeSeverity::info),
        "the normalization is noted");

  // A sparse index outside the accessor, and a sparse count larger than the accessor.
  Doc out_of_range = doc;
  out_of_range.set(
      "accessors",
      R"([{"bufferView":0,"componentType":5126,"count":2,"type":"VEC3","sparse":{"count":2,)"
      R"("indices":{"bufferView":1,"componentType":5121},"values":{"bufferView":2}}}])");
  out_of_range.set("meshes", R"([{"primitives":[{"attributes":{"POSITION":0}}]}])");
  failedWith(decodeText(out_of_range.str()), GltfDecodeError::invalid_accessor,
             "a sparse index outside the accessor");
  Doc too_many = out_of_range;
  too_many.set(
      "accessors",
      R"([{"bufferView":0,"componentType":5126,"count":1,"type":"VEC3","sparse":{"count":2,)"
      R"("indices":{"bufferView":1,"componentType":5121},"values":{"bufferView":2}}}])");
  failedWith(decodeText(too_many.str()), GltfDecodeError::invalid_accessor,
             "a sparse count above the accessor count");
  Doc bad_type = out_of_range;
  bad_type.set(
      "accessors",
      R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","sparse":{"count":2,)"
      R"("indices":{"bufferView":1,"componentType":5126},"values":{"bufferView":2}}}])");
  failedWith(decodeText(bad_type.str()), GltfDecodeError::invalid_accessor, "float sparse indices");
  Doc past_values = out_of_range;
  past_values.set(
      "accessors",
      R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","sparse":{"count":2,)"
      R"("indices":{"bufferView":1,"componentType":5121},"values":{"bufferView":2,"byteOffset":16}}}])");
  failedWith(decodeText(past_values.str()), GltfDecodeError::invalid_accessor,
             "sparse values past their view");
  // Attribute accessors with different counts, and mismatching types.
  Doc mismatch = doc;
  mismatch.set("meshes", R"([{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":5}}]}])");
  mismatch.set(
      "accessors",
      R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
      R"({"bufferView":3,"componentType":5123,"normalized":true,"count":3,"type":"VEC2"},)"
      R"({"bufferView":3,"componentType":5123,"normalized":true,"count":3,"type":"VEC2"},)"
      R"({"bufferView":4,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},)"
      R"({"bufferView":4,"componentType":5121,"normalized":true,"count":3,"type":"VEC3"},)"
      R"({"bufferView":3,"componentType":5123,"normalized":true,"count":2,"type":"VEC2"}])");
  failedWith(decodeText(mismatch.str()), GltfDecodeError::invalid_accessor,
             "an attribute with another element count than the position");
  Doc wrong_shape = mismatch;
  wrong_shape.set("meshes", R"([{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":4}}]}])");
  failedWith(decodeText(wrong_shape.str()), GltfDecodeError::invalid_accessor,
             "a TEXCOORD that is not VEC2");
}

std::string chainDoc(std::size_t nodes, const std::string &uri) {
  Doc doc = triangleDoc(uri);
  std::string list = "[";
  for (std::size_t i = 0; i < nodes; ++i) {
    if (i != 0)
      list += ',';
    list += i + 1 < nodes ? "{\"children\":[" + std::to_string(i + 1) + "]}" : "{\"mesh\":0}";
  }
  doc.set("nodes", list + "]");
  return doc.str();
}

void limits() {
  const std::string uri = dataUri(triangleBin());
  // Counts.
  Doc two_nodes = triangleDoc(uri);
  two_nodes.set("nodes", R"([{"mesh":0},{"mesh":0}])");
  two_nodes.set("scenes", R"([{"nodes":[0,1]}])");
  GltfDecodeOptions options;
  options.limits.max_nodes = 1;
  failedWith(decodeText(two_nodes.str(), {}, {}, options), asset::DecodeError::count_exceeded,
             "nodes above max_nodes");
  check(decodesValid(decodeText(two_nodes.str()), "two nodes under the default limit"),
        "two nodes");
  options = {};
  options.limits.max_meshes = 0;
  failedWith(decodeText(triangleDoc(uri).str(), {}, {}, options),
             asset::DecodeError::count_exceeded, "meshes above max_meshes");
  options = {};
  options.limits.max_primitives = 0;
  failedWith(decodeText(triangleDoc(uri).str(), {}, {}, options),
             asset::DecodeError::count_exceeded, "primitives above max_primitives");
  options = {};
  options.limits.max_input_bytes = 10;
  failedWith(decodeText(triangleDoc(uri).str(), {}, {}, options),
             asset::DecodeError::input_too_large, "input above max_input_bytes");
  options = {};
  options.limits.max_external_resources = 0;
  {
    Files files;
    files.add("assets/t.bin", triangleBin());
    failedWith(decodeText(triangleDoc("t.bin").str(),
                          asset::ResourceResolverView::bind(files.memory), {}, options),
               asset::ResolveError::limit_exceeded,
               "external requests above max_external_resources");
    options = {};
    options.limits.max_resource_bytes = 16;
    failedWith(decodeText(triangleDoc("t.bin").str(),
                          asset::ResourceResolverView::bind(files.memory), {}, options),
               asset::ResolveError::limit_exceeded, "a buffer above max_resource_bytes");
  }

  // Depth: a chain at the limit decodes, one deeper fails, a hostile chain does not recurse.
  options = {};
  options.limits.max_hierarchy_depth = 8;
  check(decodesValid(decodeText(chainDoc(8, uri), {}, {}, options), "a chain exactly at the limit"),
        "depth at the limit");
  failedWith(decodeText(chainDoc(9, uri), {}, {}, options), asset::DecodeError::hierarchy_too_deep,
             "a chain one deeper than the limit");
  failedWith(decodeText(chainDoc(1000, uri)), asset::DecodeError::hierarchy_too_deep,
             "a 1000-deep chain under the default limit");
  options = {};
  options.limits.max_hierarchy_depth = 100'000;
  options.limits.max_nodes = 100'000;
  const auto deep = decodeText(chainDoc(50'000, uri), {}, {}, options);
  if (decodesValid(deep, "a 50000-deep chain decodes iteratively when the limits are raised"))
    check(deep->model.nodes.size() == 50'000, "all nodes of the chain");
  failedWith(decodeText(chainDoc(50'000, uri), {}, {}, GltfDecodeOptions{}),
             asset::DecodeError::hierarchy_too_deep, "the same chain under the default limit");

  // A model that describes far more data than the file holds: no buffer view (zeros).
  Doc zeros = triangleDoc(uri);
  zeros.set("accessors", R"([{"componentType":5126,"count":100000000,"type":"VEC3"}])");
  failedWith(decodeText(zeros.str()), GltfDecodeError::decoded_size_exceeded,
             "a huge accessor without data is refused before it allocates");
  Doc sparse_zero = zeros;
  sparse_zero.set("accessors", R"([{"componentType":5126,"count":5000000000,"type":"VEC3"}])");
  failedWith(decodeText(sparse_zero.str()), GltfDecodeError::invalid_accessor,
             "a position count above 32 bits");
  GltfDecodeOptions tiny;
  tiny.max_decoded_bytes = 20;
  failedWith(decodeText(triangleDoc(uri).str(), {}, {}, tiny),
             GltfDecodeError::decoded_size_exceeded, "decoded geometry above max_decoded_bytes");
  tiny.max_decoded_bytes = 36;
  check(decodesValid(decodeText(triangleDoc(uri).str(), {}, {}, tiny),
                     "exactly at max_decoded_bytes"),
        "decoded size at the limit");

  // The parser's own allocations are bounded: a short text that names a huge number of objects.
  std::string many = R"({"asset":{"version":"2.0"},"materials":[)";
  for (int i = 0; i < 200'000; ++i)
    many += i == 0 ? "{}" : ",{}";
  many += "]}";
  failedWith(decodeText(many), GltfDecodeError::out_of_memory,
             "a flood of empty materials is refused by the parser's memory budget");
  // The same text is within the budget when it is a normal share of the input.
  std::string few = R"({"asset":{"version":"2.0"},"materials":[)";
  for (int i = 0; i < 100; ++i)
    few += i == 0 ? "{}" : ",{}";
  few += "]}";
  const auto small_flood = decodeText(few);
  check(decodesValid(small_flood, "a hundred materials decode") &&
            small_flood->model.materials.size() == 100,
        "a hundred empty materials");
}

void reportingIsBounded() {
  // 100 animations: the first 64 are listed, the rest is one summary item with the exact count.
  Doc doc = triangleDoc(dataUri(triangleBin()));
  std::string animations = "[";
  for (int i = 0; i < 100; ++i) {
    if (i != 0)
      animations += ',';
    animations += R"({"name":"a)" + std::to_string(i) +
                  R"(","channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}],)"
                  R"("samplers":[{"input":0,"output":0}]})";
  }
  doc.set("animations", animations + "]");
  const auto result = decodeText(doc.str());
  if (!decodesValid(result, "many animations decode"))
    return;
  const auto count = std::ranges::count_if(result->model.unsupported,
                                           [](const auto &f) { return f.kind == "animation"; });
  check(count == 65, "64 listed animations and one summary entry");
  check(hasUnsupported(result->model, "animation", "(further)"), "the summary entry");
  const auto diagnostics = std::ranges::count_if(
      result->diagnostics, [](const auto &d) { return d.code == "gltf.unsupported.animation"; });
  check(diagnostics == 65, "64 listed warnings and one summary warning");
  const auto summary = std::ranges::find_if(result->model.unsupported, [](const auto &f) {
    return f.kind == "animation" && f.name == "(further)";
  });
  check(summary != result->model.unsupported.end() &&
            summary->detail.find("36") != std::string::npos,
        "the summary names the exact remaining count");
}

bool sameModel(const asset::ModelData &a, const asset::ModelData &b) {
  if (a.meshes.size() != b.meshes.size() || a.primitives != b.primitives ||
      a.model_meshes != b.model_meshes || a.samplers != b.samplers || a.textures != b.textures ||
      a.materials != b.materials || a.nodes != b.nodes || a.scenes != b.scenes ||
      a.default_scene != b.default_scene || a.custom != b.custom ||
      a.unsupported != b.unsupported || a.images.size() != b.images.size())
    return false;
  for (std::size_t i = 0; i < a.meshes.size(); ++i) {
    if (a.meshes[i].topology != b.meshes[i].topology ||
        a.meshes[i].indices != b.meshes[i].indices || a.meshes[i].bounds != b.meshes[i].bounds ||
        a.meshes[i].attributes.size() != b.meshes[i].attributes.size())
      return false;
    for (std::size_t s = 0; s < a.meshes[i].attributes.size(); ++s) {
      const auto &x = a.meshes[i].attributes[s];
      const auto &y = b.meshes[i].attributes[s];
      if (x.key != y.key || x.format != y.format || x.count != y.count || x.data != y.data)
        return false;
    }
  }
  for (std::size_t i = 0; i < a.images.size(); ++i) {
    if (a.images[i].data != b.images[i].data ||
        a.images[i].interpretation != b.images[i].interpretation)
      return false;
  }
  return true;
}

void determinismAndThreads() {
  const Box box = makeBox(false);
  Files files;
  boxFiles(files, box);
  StubImages first_images;
  StubImages second_images;
  const Bytes json = toBytes(box.json);
  const auto first = decode(json, asset::ResourceResolverView::bind(files.memory),
                            ImageDecoderView::bind(first_images), {}, "assets/box.gltf");
  const auto second = decode(json, asset::ResourceResolverView::bind(files.memory),
                             ImageDecoderView::bind(second_images), {}, "assets/box.gltf");
  if (first && second) {
    check(sameModel(first->model, second->model), "two decodes of one input are equal");
    check(first->diagnostics.size() == second->diagnostics.size() &&
              std::ranges::equal(first->diagnostics, second->diagnostics, {},
                                 &asset::DecodeDiagnostic::code, &asset::DecodeDiagnostic::code),
          "the diagnostics are the same, in the same order");
    check(first->provenance.resources.size() == second->provenance.resources.size(),
          "the provenance is the same");
  } else {
    check(false, "the box decodes twice");
  }
}

void errorCategory() {
  check(std::string_view{gltf_decode_error_category().name()} ==
            "terreate.adapter.asset_gltf_cgltf",
        "category name");
  for (const auto error :
       {GltfDecodeError::malformed_gltf, GltfDecodeError::unsupported_version,
        GltfDecodeError::unsupported_required_extension, GltfDecodeError::missing_resolver,
        GltfDecodeError::missing_image_decoder, GltfDecodeError::image_decoder_failed,
        GltfDecodeError::invalid_buffer, GltfDecodeError::invalid_accessor,
        GltfDecodeError::invalid_resource, GltfDecodeError::decoded_size_exceeded,
        GltfDecodeError::out_of_memory}) {
    check(!make_error_code(error).message().empty() &&
              make_error_code(error).message() != "unknown glTF decode error" &&
              make_error_code(error).category() == gltf_decode_error_category(),
          "named code with a message");
  }
  const std::error_code converted = GltfDecodeError::invalid_accessor;
  check(converted == make_error_code(GltfDecodeError::invalid_accessor), "implicit conversion");
  check(containerName(GltfContainer::gltf) == "gltf" && containerName(GltfContainer::glb) == "glb",
        "container names");
  // A default-bound view is unbound.
  check(!asset::ResourceResolverView{} && !ImageDecoderView{}, "unbound views");
}

// Every truncation and many byte flips of a valid GLB, under tight limits: always a valid model
// or a named error, never a crash (no undefined behavior; the sanitizers run this too).
void truncationsAndFlips() {
  const Box box = makeBox(true);
  GltfDecodeOptions tight;
  tight.limits.max_nodes = 16;
  tight.limits.max_meshes = 4;
  tight.limits.max_primitives = 4;
  tight.limits.max_hierarchy_depth = 4;
  tight.limits.max_input_bytes = 1 << 20;
  tight.limits.max_dimension = 64;
  tight.limits.max_pixels = 4096;
  tight.limits.max_external_resources = 2;
  tight.limits.max_resource_bytes = 1 << 16;
  tight.limits.max_total_resource_bytes = 1 << 17;
  tight.max_decoded_bytes = 1 << 20;
  const auto run = [&](const Bytes &bytes) {
    StubImages images;
    const auto result =
        decodeGltf(inputOf(bytes, "assets/fuzz.glb"), {}, ImageDecoderView::bind(images), tight);
    if (result.has_value())
      return asset::validate(result->model).has_value();
    return !result.error().code().message().empty();
  };
  bool ok = true;
  for (std::size_t length = 0; length <= box.glb.size(); ++length)
    ok = run(Bytes(box.glb.begin(), box.glb.begin() + static_cast<std::ptrdiff_t>(length))) && ok;
  check(ok, "every truncation of the GLB is a valid model or a named error");
  ok = true;
  for (std::size_t i = 0; i < box.glb.size(); ++i) {
    for (const std::uint8_t mask : {0x01U, 0x80U, 0xFFU}) {
      Bytes flipped = box.glb;
      flipped[i] ^= static_cast<std::byte>(mask);
      ok = run(flipped) && ok;
    }
  }
  check(ok, "byte flips of the GLB are a valid model or a named error");

  // The same for the glTF JSON text with an external buffer (flips of the JSON only).
  const Box external = makeBox(false);
  Files files;
  boxFiles(files, external);
  const Bytes json = toBytes(external.json);
  const auto run_json = [&](const Bytes &bytes) {
    StubImages images;
    const auto result = decodeGltf(inputOf(bytes, "assets/fuzz.gltf"),
                                   asset::ResourceResolverView::bind(files.memory),
                                   ImageDecoderView::bind(images), tight);
    if (result.has_value())
      return asset::validate(result->model).has_value();
    return !result.error().code().message().empty();
  };
  ok = true;
  for (std::size_t length = 0; length <= json.size(); ++length)
    ok = run_json(Bytes(json.begin(), json.begin() + static_cast<std::ptrdiff_t>(length))) && ok;
  for (std::size_t i = 0; i < json.size(); ++i) {
    for (const std::uint8_t mask : {0x01U, 0x20U, 0xFFU}) {
      Bytes flipped = json;
      flipped[i] ^= static_cast<std::byte>(mask);
      ok = run_json(flipped) && ok;
    }
  }
  check(ok, "truncations and flips of the glTF text are a valid model or a named error");

  // The feature-rich documents: every truncation, and a byte flip at every position.
  for (const std::string &seed : {unsupportedDoc(), sparseDoc().str(),
                                  modesDoc(R"({"attributes":{"POSITION":0},"mode":6,"indices":1},)"
                                           R"({"attributes":{"POSITION":0},"mode":2,"indices":2})")
                                      .str()}) {
    const Bytes text = toBytes(seed);
    ok = true;
    for (std::size_t length = 0; length <= text.size(); ++length)
      ok = run_json(Bytes(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(length))) && ok;
    for (std::size_t i = 0; i < text.size(); ++i) {
      Bytes flipped = text;
      flipped[i] ^= std::byte{0x04};
      ok = run_json(flipped) && ok;
    }
    check(ok,
          "truncations and flips of a feature-rich document are a valid model or a named error");
  }
}

void materialDefaults() {
  Doc doc = triangleDoc(dataUri(triangleBin()));
  doc.set("materials", R"([{}])");
  const auto result = decodeText(doc.str());
  if (decodesValid(result, "an empty material decodes")) {
    const auto &material = result->model.materials[0];
    check(material.normal_scale == 1.0F && material.occlusion_strength == 1.0F &&
              material.alpha_cutoff == 0.5F && material.metallic_factor == 1.0F,
          "the glTF defaults apply to a material without textures");
  }
  Doc bad_mag = triangleDoc(dataUri(triangleBin()));
  bad_mag.set("samplers", R"([{"magFilter":9987}])");
  bad_mag.set("textures", R"([{"source":0}])");
  bad_mag.set("images", R"([{"uri":"data:image/png;base64,YWJj"}])");
  StubImages images;
  failedWith(decodeText(bad_mag.str(), {}, ImageDecoderView::bind(images)),
             GltfDecodeError::invalid_resource, "a mipmap filter as magFilter");
}

void runAll() {
  materialDefaults();
  referenceTriangle();
  boxWithResolverAndImages();
  boxAsGlb();
  instancingAndTransforms();
  resolverFailures();
  embeddedUris();
  imageDecoderCases();
  unsupportedFeatures();
  lineAndFanModes();
  lineAndFanShortPrimitives();
  lineLoopLimit();
  requiredExtensionsAndVersions();
  malformedInputs();
  accessorErrors();
  indexRange();
  sparseAndNormalized();
  limits();
  reportingIsBounded();
  determinismAndThreads();
  errorCategory();
  truncationsAndFlips();
}

} // namespace

int main() noexcept {
  try {
    runAll();
  } catch (...) {
    std::fprintf(stderr, "%s: assertion=FAIL: an exception escaped a test\n", test_name);
    return EXIT_FAILURE;
  }
  if (failures != 0) {
    std::fprintf(stderr, "%s: assertion=FAIL: %d check(s) failed\n", test_name, failures);
    return EXIT_FAILURE;
  }
  std::printf("%s: gltf-cgltf=PASS\n", test_name);
  return EXIT_SUCCESS;
}

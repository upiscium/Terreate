// #278: the cgltf Adapter and the stb_image Adapter composed. The glTF Adapter does not depend on
// the image Adapter: the application passes the image decoder, and this test shows the wrapping
// that the architecture page documents. It links both Adapters (and so Asset) and nothing else:
// no Vulkan, no Graphics, no file system and no OS-, compiler- or linker-specific mechanism.
#include <terreate/adapter/asset_gltf_cgltf/decoder.hpp>
#include <terreate/adapter/asset_image_stb/decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace asset = terreate::asset;
namespace gltf = terreate::adapter::asset_gltf_cgltf;
namespace stb = terreate::adapter::asset_image_stb;

const char *test_name = "adapter.asset_gltf_cgltf_stb";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

// A 2x2 RGBA PNG: red, green (alpha 128), blue, white.
inline constexpr std::uint8_t png_rgba8_2x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
    0x24, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x08, 0x1b, 0x18, 0xc0, 0x34, 0x10, 0x00, 0x00, 0x3f, 0xd7, 0x08, 0x79, 0x5f, 0xc7, 0x6d,
    0xb1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

using Bytes = std::vector<std::byte>;

void putLe32(Bytes &out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}

// A GLB with one triangle (36 bytes) and the PNG in a second buffer view, used by one texture of
// one material in the slot `slot` (a glTF material member such as `"normalTexture"`).
Bytes makeGlb(const Bytes &png, const std::string &slot) {
  Bytes bin;
  const float geometry[15] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1};
  bin.resize(sizeof geometry);
  std::memcpy(bin.data(), geometry, sizeof geometry);
  bin.insert(bin.end(), png.begin(), png.end());
  while (bin.size() % 4 != 0)
    bin.push_back(std::byte{0});
  std::string json =
      R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
      R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],"materials":[{)" +
      slot +
      R"(:{"index":0}}],"textures":[{"source":0}],"images":[{"bufferView":2,"mimeType":"image/png"}],)"
      R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
      R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],)"
      R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24},)"
      R"({"buffer":0,"byteOffset":60,"byteLength":)" +
      std::to_string(png.size()) + R"(}],"buffers":[{"byteLength":)" + std::to_string(bin.size()) +
      "}]}";
  while (json.size() % 4 != 0)
    json.push_back(' ');
  Bytes out;
  putLe32(out, 0x46546C67U);
  putLe32(out, 2);
  putLe32(out, static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
  putLe32(out, static_cast<std::uint32_t>(json.size()));
  putLe32(out, 0x4E4F534AU);
  for (const char c : json)
    out.push_back(static_cast<std::byte>(c));
  putLe32(out, static_cast<std::uint32_t>(bin.size()));
  putLe32(out, 0x004E4942U);
  out.insert(out.end(), bin.begin(), bin.end());
  return out;
}

// The wrapping of the stb Adapter as the image decoder of the glTF Adapter, exactly as the
// architecture page shows it: the request carries the bytes, the limits and the interpretation
// the model needs; the callable forwards them and returns the ImageData. A four-channel result is
// asked for because the Asset-Graphics Integration uploads four-component images only.
struct StbImageDecoder {
  auto operator()(const gltf::ImageDecodeRequest &request) const
      -> terreate::Result<asset::ImageData> {
    stb::ImageDecodeOptions options;
    options.limits = request.limits;
    options.interpretation = request.interpretation;
    options.channels = stb::ChannelRequest::four;
    auto decoded = stb::decodeImage(request.input, options);
    if (!decoded)
      return std::unexpected(std::move(decoded.error()));
    return std::move(decoded->image);
  }
};

void embeddedPng() {
  Bytes png(sizeof png_rgba8_2x2);
  std::memcpy(png.data(), png_rgba8_2x2, sizeof png_rgba8_2x2);
  StbImageDecoder stb_decoder;
  const auto decoder = gltf::ImageDecoderView::bind(stb_decoder);

  const Bytes color = makeGlb(png, R"("emissiveTexture")");
  const auto result =
      gltf::decodeGltf(asset::DecodeInput{color, asset::makeSourceIdentity("m.glb")}, {}, decoder);
  if (check(result.has_value(), "a GLB with an embedded PNG decodes through the stb Adapter")) {
    check(asset::validate(result->model).has_value(), "the model validates");
    check(result->model.images.size() == 1, "one image");
    const auto &image = result->model.images[0];
    check(image.format == asset::PixelFormat::rgba8 && image.extent == asset::ImageExtent{2, 2, 1},
          "the PNG became a 2x2 rgba8 ImageData");
    check(image.interpretation == asset::ColorInterpretation::srgb,
          "an emissive texture is decoded as srgb");
    check(image.data.size() >= 4 && image.data[0] == std::byte{255} &&
              image.data[1] == std::byte{0},
          "the first pixel is the PNG's red");
    check(result->model.materials[0].emissive_texture.has_value(),
          "the material keeps the texture");
    check(result->provenance.resources.empty(), "the PNG came from the GLB, not the resolver");
  }

  const Bytes normal = makeGlb(png, R"("normalTexture")");
  const auto normal_result =
      gltf::decodeGltf(asset::DecodeInput{normal, asset::makeSourceIdentity("n.glb")}, {}, decoder);
  if (check(normal_result.has_value(), "a normal texture decodes")) {
    check(normal_result->model.images[0].interpretation == asset::ColorInterpretation::normal_map,
          "a normal texture is decoded as a normal map");
  }

  // The image Adapter's limit and error pass through unchanged.
  gltf::GltfDecodeOptions options;
  options.limits.max_dimension = 1;
  const auto too_big = gltf::decodeGltf(
      asset::DecodeInput{color, asset::makeSourceIdentity("m.glb")}, {}, decoder, options);
  check(!too_big && too_big.error().code() ==
                        asset::make_error_code(asset::DecodeError::dimension_exceeded),
        "the stb Adapter's limit error is returned unchanged");
  Bytes corrupt(png.begin(), png.begin() + 20);
  const Bytes broken = makeGlb(corrupt, R"("emissiveTexture")");
  const auto bad =
      gltf::decodeGltf(asset::DecodeInput{broken, asset::makeSourceIdentity("b.glb")}, {}, decoder);
  check(!bad && bad.error().code() == stb::make_error_code(stb::ImageDecodeError::malformed_image),
        "a truncated PNG is the stb Adapter's named error, unchanged");
}

} // namespace

int main() noexcept {
  embeddedPng();
  if (failures != 0) {
    std::fprintf(stderr, "%s: assertion=FAIL: %d check(s) failed\n", test_name, failures);
    return EXIT_FAILURE;
  }
  std::printf("%s: gltf-stb=PASS\n", test_name);
  return EXIT_SUCCESS;
}

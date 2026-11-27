// #276 part 2: the CPU-only ModelData contract. This test links Terreate::Asset
// alone; it needs no Vulkan, no Graphics, no Platform, no display and no OS- or
// linker-specific mechanism.
#include <terreate/asset/model_data.hpp>
#include <terreate/asset/primitives.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace terreate::asset;

const char *test_name = "asset.model_data";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

template <typename T> bool hasError(const terreate::Result<T> &result, ModelDataError error) {
  return !result && result.error().code() == make_error_code(error);
}

#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR)
#error "Asset public headers must not pull in Vulkan or GLFW"
#endif
static_assert(std::is_copy_constructible_v<ModelData> &&
              std::is_nothrow_move_constructible_v<ModelData>);
static_assert(std::is_error_code_enum_v<ModelDataError>);

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// An enumerator value that no declared enumerator has (all these enums are
// one byte wide). bit_cast avoids an out-of-range static_cast.
template <typename E> E invalidEnum() { return std::bit_cast<E>(std::uint8_t{200}); }

ImageData whiteImage() {
  ImageData image;
  image.format = PixelFormat::rgba8;
  image.extent = {2, 2, 1};
  auto layout = terreate::unwrap(packedLayout(image.format, image.extent, 1, 1));
  image.data.assign(layout.byte_size, std::byte{255});
  image.subresources = std::move(layout.subresources);
  return image;
}

// A box with a material that uses a base color texture, in a two-node
// hierarchy under one scene.
ModelData makeModel() {
  ModelData model;
  model.meshes.push_back(terreate::unwrap(box({})));
  model.images.push_back(whiteImage());
  model.samplers.emplace_back();
  model.textures.push_back({0, 0});
  MaterialData material;
  material.name = "paint";
  material.base_color_texture = TextureRef{0, 0};
  model.materials.push_back(material);
  model.primitives.push_back({0, 0});
  model.nodes.resize(2);
  model.nodes[0].name = "root";
  model.nodes[0].children = {1};
  model.nodes[1].name = "leaf";
  model.nodes[1].primitive = 0;
  model.nodes[1].transform = TrsTransform{{1.0F, 2.0F, 3.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, {2, 2, 2}};
  model.scenes.push_back({"main", {0}});
  model.default_scene = 0;
  return model;
}

template <typename Edit> void expect(ModelDataError error, const char *message, Edit edit) {
  ModelData model = makeModel();
  edit(model);
  check(hasError(validate(model), error), message);
}

void validModels() {
  check(validate(ModelData{}).has_value(), "an empty model is valid");
  const ModelData model = makeModel();
  auto result = validate(model);
  check(result.has_value(), "box with material, texture and two nodes is valid");

  ModelData matrix = makeModel();
  matrix.nodes[1].transform = MatrixTransform{};
  check(validate(matrix).has_value(), "a matrix transform is valid");
  check(std::holds_alternative<TrsTransform>(ModelNode{}.transform), "the default is TRS");
  check(std::get<TrsTransform>(ModelNode{}.transform).rotation[3] == 1.0F, "identity rotation");

  ModelData unused = makeModel();
  unused.meshes.push_back(terreate::unwrap(quad({})));
  unused.materials.emplace_back();
  unused.nodes.emplace_back();
  check(validate(unused).has_value(), "unused entries and detached nodes are valid");

  ModelData noMaterial = makeModel();
  noMaterial.primitives[0].material.reset();
  noMaterial.textures[0].sampler.reset();
  check(validate(noMaterial).has_value(), "no material and no sampler are valid");

  ModelData noUv = makeModel();
  noUv.meshes[0] = terreate::unwrap(box({.uvs = false}));
  noUv.materials[0].base_color_texture.reset();
  check(validate(noUv).has_value(), "a mesh without uv is valid without a texture");
  noUv.materials[0].base_color_texture = TextureRef{0, 0};
  noUv.primitives.clear();
  noUv.nodes[1].primitive.reset();
  check(validate(noUv).has_value(), "an unused material's texcoord is not checked");
}

void everyError() {
  using E = ModelDataError;
  expect(E::invalid_mesh, "invalid mesh", [](ModelData &m) { m.meshes[0].attributes.clear(); });
  {
    ModelData m = makeModel();
    m.meshes[0].attributes.clear();
    const auto r = validate(m);
    check(!r && r.error().detail().find("meshes[0]") != std::string::npos &&
              r.error().detail().find("terreate.asset.mesh_data") != std::string::npos,
          "the mesh error is wrapped with its position");
  }
  expect(E::invalid_image, "invalid image", [](ModelData &m) { m.images[0].data.pop_back(); });
  {
    ModelData m = makeModel();
    m.images[0].data.pop_back();
    const auto r = validate(m);
    check(!r && r.error().detail().find("images[0]") != std::string::npos &&
              r.error().detail().find("terreate.asset.image_data") != std::string::npos,
          "the image error is wrapped with its position");
  }
  expect(E::invalid_enum, "sampler enum",
         [](ModelData &m) { m.samplers[0].wrap_u = invalidEnum<SamplerWrap>(); });
  expect(E::invalid_enum, "sampler filter enum",
         [](ModelData &m) { m.samplers[0].min_filter = invalidEnum<SamplerFilter>(); });
  expect(E::invalid_enum, "sampler mipmap enum",
         [](ModelData &m) { m.samplers[0].mipmap_mode = invalidEnum<SamplerMipmapMode>(); });
  expect(E::invalid_enum, "alpha mode enum",
         [](ModelData &m) { m.materials[0].alpha_mode = invalidEnum<MaterialAlphaMode>(); });

  expect(E::index_out_of_range, "texture image", [](ModelData &m) { m.textures[0].image = 1; });
  expect(E::index_out_of_range, "texture sampler", [](ModelData &m) { m.textures[0].sampler = 1; });
  expect(E::index_out_of_range, "base color texture ref",
         [](ModelData &m) { m.materials[0].base_color_texture = TextureRef{1, 0}; });
  expect(E::index_out_of_range, "metallic roughness texture ref",
         [](ModelData &m) { m.materials[0].metallic_roughness_texture = TextureRef{5, 0}; });
  expect(E::index_out_of_range, "normal texture ref",
         [](ModelData &m) { m.materials[0].normal_texture = TextureRef{5, 0}; });
  expect(E::index_out_of_range, "emissive texture ref",
         [](ModelData &m) { m.materials[0].emissive_texture = TextureRef{5, 0}; });
  expect(E::index_out_of_range, "occlusion texture ref",
         [](ModelData &m) { m.materials[0].occlusion_texture = TextureRef{5, 0}; });
  expect(E::index_out_of_range, "primitive mesh", [](ModelData &m) { m.primitives[0].mesh = 1; });
  expect(E::index_out_of_range, "primitive material",
         [](ModelData &m) { m.primitives[0].material = 1; });
  expect(E::index_out_of_range, "node primitive", [](ModelData &m) { m.nodes[1].primitive = 1; });
  expect(E::index_out_of_range, "node child", [](ModelData &m) { m.nodes[1].children = {2}; });
  expect(E::index_out_of_range, "scene root", [](ModelData &m) { m.scenes[0].roots = {2}; });
  expect(E::index_out_of_range, "default scene", [](ModelData &m) { m.default_scene = 1; });
  expect(E::index_out_of_range, "default scene without scenes", [](ModelData &m) {
    m.scenes.clear();
    m.default_scene = 0;
  });

  expect(E::invalid_material_value, "base color above 1",
         [](ModelData &m) { m.materials[0].base_color_factor[3] = 1.5F; });
  expect(E::invalid_material_value, "base color NaN",
         [](ModelData &m) { m.materials[0].base_color_factor[0] = kNaN; });
  expect(E::invalid_material_value, "metallic below 0",
         [](ModelData &m) { m.materials[0].metallic_factor = -0.1F; });
  expect(E::invalid_material_value, "roughness infinite",
         [](ModelData &m) { m.materials[0].roughness_factor = kInf; });
  expect(E::invalid_material_value, "normal scale NaN",
         [](ModelData &m) { m.materials[0].normal_scale = kNaN; });
  expect(E::invalid_material_value, "emissive above 1",
         [](ModelData &m) { m.materials[0].emissive_factor[1] = 2.0F; });
  expect(E::invalid_material_value, "occlusion strength above 1",
         [](ModelData &m) { m.materials[0].occlusion_strength = 1.01F; });
  expect(E::invalid_material_value, "negative cutoff",
         [](ModelData &m) { m.materials[0].alpha_cutoff = -1.0F; });
  expect(E::invalid_material_value, "NaN cutoff",
         [](ModelData &m) { m.materials[0].alpha_cutoff = kNaN; });

  expect(E::invalid_transform, "NaN translation",
         [](ModelData &m) { std::get<TrsTransform>(m.nodes[1].transform).translation[1] = kNaN; });
  expect(E::invalid_transform, "infinite scale",
         [](ModelData &m) { std::get<TrsTransform>(m.nodes[1].transform).scale[0] = kInf; });
  expect(E::invalid_transform, "zero quaternion", [](ModelData &m) {
    std::get<TrsTransform>(m.nodes[1].transform).rotation = {0, 0, 0, 0};
  });
  expect(E::invalid_transform, "non-unit quaternion", [](ModelData &m) {
    std::get<TrsTransform>(m.nodes[1].transform).rotation = {0, 0, 0, 2};
  });
  expect(E::invalid_transform, "NaN quaternion", [](ModelData &m) {
    std::get<TrsTransform>(m.nodes[1].transform).rotation = {kNaN, 0, 0, 1};
  });
  expect(E::invalid_transform, "infinite matrix", [](ModelData &m) {
    MatrixTransform matrix;
    matrix.matrix[12] = kInf;
    m.nodes[1].transform = matrix;
  });

  expect(E::multiple_parents, "two parents", [](ModelData &m) {
    m.nodes.emplace_back();
    m.nodes[2].children = {1};
  });
  expect(E::multiple_parents, "a child listed twice",
         [](ModelData &m) { m.nodes[0].children = {1, 1}; });
  expect(E::node_cycle, "self child", [](ModelData &m) {
    m.nodes.emplace_back();
    m.nodes[2].children = {2};
  });
  expect(E::multiple_parents, "a child that is its own and another node's child",
         [](ModelData &m) { m.nodes[1].children = {1}; });
  expect(E::node_cycle, "two-node cycle", [](ModelData &m) { m.nodes[1].children = {0}; });
  expect(E::node_cycle, "a detached cycle", [](ModelData &m) {
    m.nodes.resize(4);
    m.nodes[2].children = {3};
    m.nodes[3].children = {2};
  });
  expect(E::node_cycle, "a node below a detached cycle", [](ModelData &m) {
    m.nodes.resize(4);
    m.nodes[2].children = {2, 3};
  });

  expect(E::invalid_scene_root, "a root with a parent",
         [](ModelData &m) { m.scenes[0].roots = {0, 1}; });
  expect(E::invalid_scene_root, "a root twice", [](ModelData &m) { m.scenes[0].roots = {0, 0}; });

  expect(E::missing_texcoord_set, "texcoord set missing on the mesh",
         [](ModelData &m) { m.materials[0].base_color_texture = TextureRef{0, 1}; });
  expect(E::missing_texcoord_set, "mesh without uv",
         [](ModelData &m) { m.meshes[0] = terreate::unwrap(box({.uvs = false})); });
  expect(E::missing_texcoord_set, "normal texture texcoord",
         [](ModelData &m) { m.materials[0].normal_texture = TextureRef{0, 3}; });
  expect(E::missing_texcoord_set, "metallic roughness texcoord",
         [](ModelData &m) { m.materials[0].metallic_roughness_texture = TextureRef{0, 3}; });
  expect(E::missing_texcoord_set, "emissive texcoord",
         [](ModelData &m) { m.materials[0].emissive_texture = TextureRef{0, 3}; });
  expect(E::missing_texcoord_set, "occlusion texcoord",
         [](ModelData &m) { m.materials[0].occlusion_texture = TextureRef{0, 3}; });

  expect(E::invalid_custom_entry, "empty custom key",
         [](ModelData &m) { m.custom.push_back({"", std::string("v")}); });
  expect(E::invalid_custom_entry, "repeated custom key", [](ModelData &m) {
    m.custom.push_back({"k", std::string("v")});
    m.custom.push_back({"k", std::vector<std::byte>{std::byte{1}}});
  });
  expect(E::invalid_unsupported_entry, "empty unsupported kind",
         [](ModelData &m) { m.unsupported.push_back({"", "Walk", ""}); });

  check(std::string(model_data_error_category().name()) == "terreate.asset.model_data",
        "category name");
  check(!model_data_error_category().message(static_cast<int>(ModelDataError::node_cycle)).empty(),
        "messages exist");
}

void hierarchy() {
  // A long chain: 300000 nodes, each the child of the previous one. The check
  // is iterative, so this neither overflows the stack nor takes long.
  constexpr std::size_t n = 300000;
  ModelData chain;
  chain.nodes.resize(n);
  for (std::size_t i = 0; i + 1 < n; ++i)
    chain.nodes[i].children = {i + 1};
  chain.scenes.push_back({"", {0}});
  check(validate(chain).has_value(), "a long chain is valid");
  chain.nodes[n - 1].children = {0};
  check(hasError(validate(chain), ModelDataError::node_cycle), "a long cycle is detected");
  chain.nodes[n - 1].children = {n / 2};
  check(hasError(validate(chain), ModelDataError::multiple_parents),
        "a long chain closing on a middle node has two parents");

  // A wide tree and a forest with several scenes.
  ModelData wide;
  wide.nodes.resize(1001);
  for (std::size_t i = 1; i < wide.nodes.size(); ++i)
    wide.nodes[0].children.push_back(i);
  wide.scenes.push_back({"a", {0}});
  wide.scenes.push_back({"b", {0}});
  check(validate(wide).has_value(), "a wide tree under two scenes is valid");
}

void copyIndependence() {
  const ModelData original = makeModel();
  ModelData copy = original;
  copy.meshes[0].attributes.clear();
  copy.images[0].data.clear();
  copy.nodes[0].children.clear();
  copy.materials[0].name = "changed";
  copy.custom.push_back({"k", std::string("v")});
  check(validate(original).has_value() && original.nodes[0].children.size() == 1 &&
            original.materials[0].name == "paint" && original.custom.empty(),
        "a copy owns its meshes, images, nodes, materials and lists");
  ModelData moved = makeModel();
  const ModelData taken = std::move(moved);
  check(validate(taken).has_value(), "a moved-to model is valid");
}

void metadata() {
  ModelData model = makeModel();
  model.custom.push_back({"generator", std::string("test")});
  model.custom.push_back({"blob", std::vector<std::byte>{std::byte{1}, std::byte{2}}});
  model.unsupported.push_back({"animation", "Walk", "3 channels"});
  model.unsupported.push_back({"skin", "", ""});
  model.unsupported.push_back({"camera", "main", ""});
  model.unsupported.push_back({"light", "sun", "KHR_lights_punctual"});
  check(validate(model).has_value(), "custom and unsupported entries are valid");
  const ModelData copy = model;
  check(copy.custom == model.custom && copy.unsupported == model.unsupported,
        "entries survive a copy unchanged");
  check(std::get<std::string>(copy.custom[0].value) == "test" &&
            std::get<std::vector<std::byte>>(copy.custom[1].value).size() == 2,
        "custom values keep their content");
  check(copy.unsupported[0].kind == "animation" && copy.unsupported[0].detail == "3 channels" &&
            copy.unsupported.size() == 4,
        "unsupported entries keep their content");
}

} // namespace

int main() noexcept {
  validModels();
  everyError();
  hierarchy();
  copyIndependence();
  metadata();
  if (failures != 0) {
    std::fprintf(stderr, "%s: assertion=FAIL: %d check(s) failed\n", test_name, failures);
    return EXIT_FAILURE;
  }
  std::printf("%s: model-data=PASS\n", test_name);
  return EXIT_SUCCESS;
}

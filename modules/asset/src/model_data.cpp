#include <terreate/asset/model_data.hpp>

#include <cmath>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace terreate::asset {
namespace {

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
constexpr const char *kOp = "validate(ModelData)";
constexpr double kQuaternionTolerance = 1e-3;

class ModelDataErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.asset.model_data"; }
  std::string message(int value) const override {
    switch (static_cast<ModelDataError>(value)) {
    case ModelDataError::index_out_of_range:
      return "an index refers past the end of its list";
    case ModelDataError::invalid_mesh:
      return "a mesh is not valid";
    case ModelDataError::invalid_image:
      return "an image is not valid";
    case ModelDataError::invalid_enum:
      return "an enumerator is not a declared value";
    case ModelDataError::invalid_material_value:
      return "a material value is not finite or is out of range";
    case ModelDataError::invalid_transform:
      return "a transform is not finite or its quaternion is not near unit length";
    case ModelDataError::node_cycle:
      return "the node hierarchy contains a cycle";
    case ModelDataError::multiple_parents:
      return "a node is the child of more than one parent";
    case ModelDataError::invalid_scene_root:
      return "a scene root has a parent or is listed twice";
    case ModelDataError::missing_texcoord_set:
      return "a material samples a texcoord set its mesh does not have";
    case ModelDataError::invalid_custom_entry:
      return "a custom entry has an empty or repeated key";
    case ModelDataError::invalid_unsupported_entry:
      return "an unsupported-feature entry has an empty kind";
    }
    return "unknown ModelData error";
  }
};

// Error construction allocates; if only the Error itself cannot allocate, a code-only Error is
// returned, which does not allocate.
Error makeError(ModelDataError error, const std::string &detail) {
  try {
    return Error{make_error_code(error), kOp, detail};
  } catch (const std::bad_alloc &) {
    return Error{make_error_code(error)};
  }
}

std::unexpected<Error> fail(ModelDataError error, const std::string &detail) {
  return std::unexpected(makeError(error, detail));
}

std::string at(const char *list, std::size_t index) {
  return std::string(list) + "[" + std::to_string(index) + "]";
}

// "meshes[2]: terreate.asset.mesh_data: <message> (<detail>)"
std::string wrapInner(const std::string &where, const Error &inner) {
  std::string text = where + ": " + inner.code().category().name() + ": " + inner.code().message();
  if (!inner.detail().empty())
    text += " (" + inner.detail() + ")";
  return text;
}

bool finite(float value) noexcept { return std::isfinite(value); }

template <std::size_t N> bool allFinite(const std::array<float, N> &values) noexcept {
  for (const float value : values)
    if (!finite(value))
      return false;
  return true;
}

bool inUnit(float value) noexcept { return finite(value) && value >= 0.0F && value <= 1.0F; }

template <std::size_t N> bool allInUnit(const std::array<float, N> &values) noexcept {
  for (const float value : values)
    if (!inUnit(value))
      return false;
  return true;
}

terreate::Result<void> checkTextureRef(const ModelData &model, const std::optional<TextureRef> &ref,
                                       std::size_t material, const char *field) {
  if (ref && ref->texture >= model.textures.size())
    return fail(ModelDataError::index_out_of_range,
                at("materials", material) + "." + field + ".texture " +
                    std::to_string(ref->texture) + " is not below " +
                    std::to_string(model.textures.size()) + " textures");
  return {};
}

terreate::Result<void> checkMaterial(const ModelData &model, std::size_t index) {
  const MaterialData &m = model.materials[index];
  const std::string where = at("materials", index);
  if (!allInUnit(m.base_color_factor))
    return fail(ModelDataError::invalid_material_value, where + ".base_color_factor");
  if (!inUnit(m.metallic_factor))
    return fail(ModelDataError::invalid_material_value, where + ".metallic_factor");
  if (!inUnit(m.roughness_factor))
    return fail(ModelDataError::invalid_material_value, where + ".roughness_factor");
  if (!finite(m.normal_scale))
    return fail(ModelDataError::invalid_material_value, where + ".normal_scale");
  if (!allInUnit(m.emissive_factor))
    return fail(ModelDataError::invalid_material_value, where + ".emissive_factor");
  if (!inUnit(m.occlusion_strength))
    return fail(ModelDataError::invalid_material_value, where + ".occlusion_strength");
  if (!finite(m.alpha_cutoff) || m.alpha_cutoff < 0.0F)
    return fail(ModelDataError::invalid_material_value, where + ".alpha_cutoff");
  switch (m.alpha_mode) {
  case MaterialAlphaMode::opaque:
  case MaterialAlphaMode::mask:
  case MaterialAlphaMode::blend:
    break;
  default:
    return fail(ModelDataError::invalid_enum, where + ".alpha_mode");
  }
  if (auto r = checkTextureRef(model, m.base_color_texture, index, "base_color_texture"); !r)
    return r;
  if (auto r =
          checkTextureRef(model, m.metallic_roughness_texture, index, "metallic_roughness_texture");
      !r)
    return r;
  if (auto r = checkTextureRef(model, m.normal_texture, index, "normal_texture"); !r)
    return r;
  if (auto r = checkTextureRef(model, m.emissive_texture, index, "emissive_texture"); !r)
    return r;
  return checkTextureRef(model, m.occlusion_texture, index, "occlusion_texture");
}

bool validFilter(SamplerFilter value) noexcept {
  return value == SamplerFilter::nearest || value == SamplerFilter::linear;
}
bool validMipmap(SamplerMipmapMode value) noexcept {
  return value == SamplerMipmapMode::none || value == SamplerMipmapMode::nearest ||
         value == SamplerMipmapMode::linear;
}
bool validWrap(SamplerWrap value) noexcept {
  return value == SamplerWrap::repeat || value == SamplerWrap::mirrored_repeat ||
         value == SamplerWrap::clamp_to_edge;
}

terreate::Result<void> checkTransform(const NodeTransform &transform, std::size_t node) {
  const std::string where = at("nodes", node) + ".transform";
  if (const auto *trs = std::get_if<TrsTransform>(&transform)) {
    if (!allFinite(trs->translation) || !allFinite(trs->scale))
      return fail(ModelDataError::invalid_transform, where + " is not finite");
    if (!allFinite(trs->rotation))
      return fail(ModelDataError::invalid_transform, where + ".rotation is not finite");
    // Components are finite floats, so the double sum cannot overflow.
    double sum = 0.0;
    for (const float c : trs->rotation)
      sum += static_cast<double>(c) * static_cast<double>(c);
    if (std::abs(std::sqrt(sum) - 1.0) > kQuaternionTolerance)
      return fail(ModelDataError::invalid_transform,
                  where + ".rotation is not within 1e-3 of unit length");
  } else if (!allFinite(std::get<MatrixTransform>(transform).matrix)) {
    return fail(ModelDataError::invalid_transform, where + ".matrix is not finite");
  }
  return {};
}

// Checks the child lists; fills `parents` with the parent count of each node
// (at most 1 on success).
terreate::Result<void> checkNodes(const ModelData &model, std::vector<std::uint8_t> &parents) {
  const std::size_t count = model.nodes.size();
  parents.assign(count, 0);
  for (std::size_t i = 0; i < count; ++i) {
    const ModelNode &node = model.nodes[i];
    if (node.primitive && *node.primitive >= model.primitives.size())
      return fail(ModelDataError::index_out_of_range,
                  at("nodes", i) + ".primitive " + std::to_string(*node.primitive) +
                      " is not below " + std::to_string(model.primitives.size()) + " primitives");
    if (auto r = checkTransform(node.transform, i); !r)
      return r;
    for (const std::size_t child : node.children) {
      if (child >= count)
        return fail(ModelDataError::index_out_of_range, at("nodes", i) + ".children has " +
                                                            std::to_string(child) + ", not below " +
                                                            std::to_string(count) + " nodes");
      if (parents[child] != 0)
        return fail(ModelDataError::multiple_parents,
                    "node " + std::to_string(child) + " is a child more than once");
      parents[child] = 1;
    }
  }
  return {};
}

// With at most one parent per node, a node is on or below a cycle exactly when
// no parentless node reaches it. The walk uses an explicit stack.
terreate::Result<void> checkAcyclic(const ModelData &model,
                                    const std::vector<std::uint8_t> &parents) {
  const std::size_t count = model.nodes.size();
  std::vector<std::size_t> stack;
  std::size_t reached = 0;
  for (std::size_t root = 0; root < count; ++root) {
    if (parents[root] != 0)
      continue;
    stack.push_back(root);
    while (!stack.empty()) {
      const std::size_t node = stack.back();
      stack.pop_back();
      ++reached;
      for (const std::size_t child : model.nodes[node].children)
        stack.push_back(child);
    }
  }
  if (reached == count)
    return {};
  return fail(ModelDataError::node_cycle,
              std::to_string(count - reached) + " node(s) are on or below a cycle");
}

terreate::Result<void> checkScenes(const ModelData &model,
                                   const std::vector<std::uint8_t> &parents) {
  for (std::size_t s = 0; s < model.scenes.size(); ++s) {
    std::unordered_set<std::size_t> seen;
    for (const std::size_t root : model.scenes[s].roots) {
      if (root >= model.nodes.size())
        return fail(ModelDataError::index_out_of_range,
                    at("scenes", s) + ".roots has " + std::to_string(root) + ", not below " +
                        std::to_string(model.nodes.size()) + " nodes");
      if (parents[root] != 0)
        return fail(ModelDataError::invalid_scene_root,
                    at("scenes", s) + " root " + std::to_string(root) + " has a parent");
      if (!seen.insert(root).second)
        return fail(ModelDataError::invalid_scene_root,
                    at("scenes", s) + " lists root " + std::to_string(root) + " twice");
    }
  }
  if (model.default_scene && *model.default_scene >= model.scenes.size())
    return fail(ModelDataError::index_out_of_range,
                "default_scene " + std::to_string(*model.default_scene) + " is not below " +
                    std::to_string(model.scenes.size()) + " scenes");
  return {};
}

terreate::Result<void> checkTexcoord(const ModelData &model, std::size_t primitive,
                                     std::size_t material, const std::optional<TextureRef> &ref,
                                     const char *field) {
  if (!ref)
    return {};
  const ModelPrimitive &p = model.primitives[primitive];
  const AttributeKey key{AttributeSemantic::texcoord, ref->texcoord, {}};
  if (model.meshes[p.mesh].find(key) == nullptr)
    return fail(ModelDataError::missing_texcoord_set,
                at("primitives", primitive) + ": material " + std::to_string(material) + " " +
                    field + " samples texcoord set " + std::to_string(ref->texcoord) +
                    " that meshes[" + std::to_string(p.mesh) + "] does not have");
  return {};
}

terreate::Result<void> checkTexcoords(const ModelData &model) {
  for (std::size_t i = 0; i < model.primitives.size(); ++i) {
    const ModelPrimitive &p = model.primitives[i];
    if (!p.material)
      continue;
    const std::size_t material = *p.material;
    const MaterialData &m = model.materials[material];
    if (auto r = checkTexcoord(model, i, material, m.base_color_texture, "base_color_texture"); !r)
      return r;
    if (auto r = checkTexcoord(model, i, material, m.metallic_roughness_texture,
                               "metallic_roughness_texture");
        !r)
      return r;
    if (auto r = checkTexcoord(model, i, material, m.normal_texture, "normal_texture"); !r)
      return r;
    if (auto r = checkTexcoord(model, i, material, m.emissive_texture, "emissive_texture"); !r)
      return r;
    if (auto r = checkTexcoord(model, i, material, m.occlusion_texture, "occlusion_texture"); !r)
      return r;
  }
  return {};
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace

auto model_data_error_category() noexcept -> const std::error_category & {
  static const ModelDataErrorCategory category;
  return category;
}

auto make_error_code(ModelDataError error) noexcept -> std::error_code {
  return {static_cast<int>(error), model_data_error_category()};
}

auto validate(const ModelData &model) -> terreate::Result<void> {
  for (std::size_t i = 0; i < model.meshes.size(); ++i)
    if (auto r = validate(model.meshes[i]); !r)
      return fail(ModelDataError::invalid_mesh, wrapInner(at("meshes", i), r.error()));
  for (std::size_t i = 0; i < model.images.size(); ++i)
    if (auto r = validate(model.images[i]); !r)
      return fail(ModelDataError::invalid_image, wrapInner(at("images", i), r.error()));

  for (std::size_t i = 0; i < model.samplers.size(); ++i) {
    const SamplerData &s = model.samplers[i];
    if (!validFilter(s.min_filter) || !validFilter(s.mag_filter) || !validMipmap(s.mipmap_mode) ||
        !validWrap(s.wrap_u) || !validWrap(s.wrap_v))
      return fail(ModelDataError::invalid_enum, at("samplers", i));
  }
  for (std::size_t i = 0; i < model.textures.size(); ++i) {
    const TextureData &t = model.textures[i];
    if (t.image >= model.images.size())
      return fail(ModelDataError::index_out_of_range,
                  at("textures", i) + ".image " + std::to_string(t.image) + " is not below " +
                      std::to_string(model.images.size()) + " images");
    if (t.sampler && *t.sampler >= model.samplers.size())
      return fail(ModelDataError::index_out_of_range,
                  at("textures", i) + ".sampler " + std::to_string(*t.sampler) + " is not below " +
                      std::to_string(model.samplers.size()) + " samplers");
  }
  for (std::size_t i = 0; i < model.materials.size(); ++i)
    if (auto r = checkMaterial(model, i); !r)
      return r;
  for (std::size_t i = 0; i < model.primitives.size(); ++i) {
    const ModelPrimitive &p = model.primitives[i];
    if (p.mesh >= model.meshes.size())
      return fail(ModelDataError::index_out_of_range,
                  at("primitives", i) + ".mesh " + std::to_string(p.mesh) + " is not below " +
                      std::to_string(model.meshes.size()) + " meshes");
    if (p.material && *p.material >= model.materials.size())
      return fail(ModelDataError::index_out_of_range,
                  at("primitives", i) + ".material " + std::to_string(*p.material) +
                      " is not below " + std::to_string(model.materials.size()) + " materials");
  }

  std::vector<std::uint8_t> parents;
  if (auto r = checkNodes(model, parents); !r)
    return r;
  if (auto r = checkAcyclic(model, parents); !r)
    return r;
  if (auto r = checkScenes(model, parents); !r)
    return r;
  if (auto r = checkTexcoords(model); !r)
    return r;

  std::unordered_set<std::string_view> keys;
  for (std::size_t i = 0; i < model.custom.size(); ++i) {
    if (model.custom[i].key.empty())
      return fail(ModelDataError::invalid_custom_entry, at("custom", i) + " has an empty key");
    if (!keys.insert(model.custom[i].key).second)
      return fail(ModelDataError::invalid_custom_entry,
                  at("custom", i) + " repeats the key \"" + model.custom[i].key + "\"");
  }
  for (std::size_t i = 0; i < model.unsupported.size(); ++i)
    if (model.unsupported[i].kind.empty())
      return fail(ModelDataError::invalid_unsupported_entry,
                  at("unsupported", i) + " has an empty kind");
  return {};
}

} // namespace terreate::asset

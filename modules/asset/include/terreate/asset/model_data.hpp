#ifndef TERREATE_ASSET_MODEL_DATA_HPP
#define TERREATE_ASSET_MODEL_DATA_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <terreate/asset/image_data.hpp>
#include <terreate/asset/mesh_data.hpp>
#include <terreate/core/result.hpp>

namespace terreate::asset {

/// How a sampler filters a texture when magnifying or minifying.
enum class SamplerFilter : std::uint8_t {
  /// The nearest texel.
  nearest,
  /// Linear interpolation of neighboring texels.
  linear,
};

/// How a sampler selects between mip levels.
enum class SamplerMipmapMode : std::uint8_t {
  /// Mip levels are not used (the base level is sampled).
  none,
  /// The nearest mip level.
  nearest,
  /// Linear interpolation between two mip levels.
  linear,
};

/// How a sampler treats texture coordinates outside 0..1.
enum class SamplerWrap : std::uint8_t {
  /// The coordinate wraps around.
  repeat,
  /// The coordinate wraps around, mirrored on every other repetition.
  mirrored_repeat,
  /// The coordinate is clamped to the edge texel.
  clamp_to_edge,
};

/// A CPU-side description of how a texture is sampled. It holds no GPU
/// object; the consumer maps the enumerators to its backend.
struct SamplerData {
  /// The filter used when the texture is minified.
  SamplerFilter min_filter = SamplerFilter::linear;
  /// The filter used when the texture is magnified.
  SamplerFilter mag_filter = SamplerFilter::linear;
  /// How mip levels are selected.
  SamplerMipmapMode mipmap_mode = SamplerMipmapMode::linear;
  /// Wrapping along u (the horizontal texture axis).
  SamplerWrap wrap_u = SamplerWrap::repeat;
  /// Wrapping along v (the vertical texture axis).
  SamplerWrap wrap_v = SamplerWrap::repeat;

  /// Samplers are equal when all members are equal.
  [[nodiscard]] friend bool operator==(const SamplerData &, const SamplerData &) = default;
};

/// A texture: an image with an optional sampler. Indices are into
/// ModelData::images and ModelData::samplers.
struct TextureData {
  /// Index into ModelData::images.
  std::size_t image = 0;
  /// Index into ModelData::samplers, or none for the consumer's default
  /// sampling.
  std::optional<std::size_t> sampler;

  /// Textures are equal when image and sampler are equal.
  [[nodiscard]] friend bool operator==(const TextureData &, const TextureData &) = default;
};

/// A reference from a material to a texture.
struct TextureRef {
  /// Index into ModelData::textures.
  std::size_t texture = 0;
  /// The texture coordinate set (AttributeKey::set of a `texcoord` attribute)
  /// of the mesh that is used to sample the texture.
  std::uint32_t texcoord = 0;

  /// References are equal when texture and texcoord are equal.
  [[nodiscard]] friend bool operator==(const TextureRef &, const TextureRef &) = default;
};

/// How a material's alpha is used. These are the glTF alpha modes.
enum class MaterialAlphaMode : std::uint8_t {
  /// The alpha is ignored; the surface is opaque.
  opaque,
  /// The surface is fully opaque or fully transparent, decided by comparing
  /// the alpha to MaterialData::alpha_cutoff.
  mask,
  /// The alpha blends the surface with what is behind it.
  blend,
};

/// A minimal glTF metallic-roughness material. Defaults are the glTF defaults.
/// All factors are linear. Texture images carry their own color
/// interpretation (ImageData::interpretation); a model does not repeat it.
struct MaterialData {
  /// A name for humans; may be empty.
  std::string name;
  /// Linear RGBA base color factor, each component in 0..1.
  std::array<float, 4> base_color_factor{1.0F, 1.0F, 1.0F, 1.0F};
  /// The base color texture (sRGB color, alpha), multiplied with the factor.
  std::optional<TextureRef> base_color_texture;
  /// The metalness factor, in 0..1.
  float metallic_factor = 1.0F;
  /// The roughness factor, in 0..1.
  float roughness_factor = 1.0F;
  /// The metallic-roughness texture (roughness in green, metalness in blue),
  /// multiplied with the factors.
  std::optional<TextureRef> metallic_roughness_texture;
  /// The tangent-space normal texture.
  std::optional<TextureRef> normal_texture;
  /// The scale of the normal texture's x and y; any finite value.
  float normal_scale = 1.0F;
  /// Linear RGB emissive factor, each component in 0..1.
  std::array<float, 3> emissive_factor{0.0F, 0.0F, 0.0F};
  /// The emissive texture (sRGB color), multiplied with the factor.
  std::optional<TextureRef> emissive_texture;
  /// The occlusion texture (occlusion in red).
  std::optional<TextureRef> occlusion_texture;
  /// How strongly the occlusion texture applies, in 0..1.
  float occlusion_strength = 1.0F;
  /// How alpha is used.
  MaterialAlphaMode alpha_mode = MaterialAlphaMode::opaque;
  /// The alpha threshold of MaterialAlphaMode::mask; finite and at least 0.
  float alpha_cutoff = 0.5F;
  /// Whether back faces are rendered too.
  bool double_sided = false;

  /// Materials are equal when all members are equal.
  [[nodiscard]] friend bool operator==(const MaterialData &, const MaterialData &) = default;
};

/// One drawable piece: a MeshData (the geometry of one primitive) with an
/// optional material. Indices are into ModelData::meshes and
/// ModelData::materials. A glTF primitive maps to one ModelPrimitive. Several
/// ModelMesh entries may refer to the same ModelPrimitive.
struct ModelPrimitive {
  /// Index into ModelData::meshes (a MeshData, not a ModelMesh).
  std::size_t mesh = 0;
  /// Index into ModelData::materials, or none for the consumer's default
  /// material.
  std::optional<std::size_t> material;

  /// Primitives are equal when mesh and material are equal.
  [[nodiscard]] friend bool operator==(const ModelPrimitive &, const ModelPrimitive &) = default;
};

/// A glTF mesh: a named group of primitives that a node places together. Not
/// to be confused with MeshData, which is the geometry of one primitive. Two
/// nodes may refer to the same ModelMesh (instancing).
struct ModelMesh {
  /// A name for humans; may be empty.
  std::string name;
  /// Indices into ModelData::primitives; at least one in a valid model. A
  /// primitive may be listed by several ModelMesh entries.
  std::vector<std::size_t> primitives;

  /// Meshes are equal when name and primitives are equal.
  [[nodiscard]] friend bool operator==(const ModelMesh &, const ModelMesh &) = default;
};

/// A node transform as translation, rotation and scale. The matrix is
/// `T * R * S`: scale first, then rotate, then translate.
struct TrsTransform {
  /// The translation (x, y, z).
  std::array<float, 3> translation{0.0F, 0.0F, 0.0F};
  /// The rotation as a quaternion (x, y, z, w); the identity is (0, 0, 0, 1).
  /// In a valid transform its length is within 1e-3 of 1.
  std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
  /// The scale (x, y, z).
  std::array<float, 3> scale{1.0F, 1.0F, 1.0F};

  /// Transforms are equal when all members are equal.
  [[nodiscard]] friend bool operator==(const TrsTransform &, const TrsTransform &) = default;
};

/// A node transform as a 4x4 matrix in column-major order: element `4 * c + r`
/// is row `r` of column `c`, and the translation is elements 12, 13 and 14.
struct MatrixTransform {
  /// The matrix elements, column-major; the default is the identity.
  std::array<float, 16> matrix{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                               0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};

  /// Transforms are equal when the matrices are equal.
  [[nodiscard]] friend bool operator==(const MatrixTransform &, const MatrixTransform &) = default;
};

/// A node transform: translation-rotation-scale or a matrix, like glTF. The
/// default is the identity TRS.
using NodeTransform = std::variant<TrsTransform, MatrixTransform>;

/// A node of a model's hierarchy. Its transform is relative to its parent.
struct ModelNode {
  /// A name for humans; may be empty.
  std::string name;
  /// Indices into ModelData::nodes of the children. A node has at most one
  /// parent in a valid model.
  std::vector<std::size_t> children;
  /// The transform relative to the parent.
  NodeTransform transform;
  /// Index into ModelData::model_meshes of the mesh this node places, or none.
  /// Several nodes may name the same mesh (instancing). This is the glTF
  /// node.mesh; no node is invented per primitive.
  std::optional<std::size_t> mesh;

  /// Nodes are equal when all members are equal.
  [[nodiscard]] friend bool operator==(const ModelNode &, const ModelNode &) = default;
};

/// A scene: the root nodes to place.
struct ModelScene {
  /// A name for humans; may be empty.
  std::string name;
  /// Indices into ModelData::nodes. Each is a node without a parent; a node
  /// appears at most once.
  std::vector<std::size_t> roots;

  /// Scenes are equal when all members are equal.
  [[nodiscard]] friend bool operator==(const ModelScene &, const ModelScene &) = default;
};

/// A reusable, backend-independent, CPU-side model: meshes, images, samplers,
/// textures, materials and a node hierarchy, the data a glTF or similar
/// decoder produces. The structure mirrors glTF:
/// `scene -> node -> ModelMesh -> ModelPrimitive -> MeshData + material`.
///
/// ModelData is an owning, copyable value (copying CPU data is explicit and
/// legitimate). It holds no GPU handle. Members are plain data; nothing is
/// guaranteed until validate() succeeds, and nothing is repaired. All
/// references are indices into the sibling lists of the same ModelData.
///
/// The space is right-handed with +Y up, like MeshData, and matrices are
/// column-major as in glTF. Animation, skins, cameras and lights are not
/// represented: a decoder that meets them lists them in `unsupported`.
struct ModelData {
  /// The geometry; each entry is one glTF primitive's worth of vertex data.
  std::vector<MeshData> meshes;
  /// The primitives: a MeshData with an optional material.
  std::vector<ModelPrimitive> primitives;
  /// The glTF meshes: groups of primitives that nodes place. Named
  /// `model_meshes` because `meshes` holds MeshData (one primitive's geometry).
  std::vector<ModelMesh> model_meshes;
  /// The decoded images.
  std::vector<ImageData> images;
  /// The sampler descriptions.
  std::vector<SamplerData> samplers;
  /// The textures: an image with an optional sampler.
  std::vector<TextureData> textures;
  /// The materials.
  std::vector<MaterialData> materials;
  /// The node hierarchy, in no required order.
  std::vector<ModelNode> nodes;
  /// The scenes.
  std::vector<ModelScene> scenes;
  /// Index into `scenes` of the scene to show by default, or none.
  std::optional<std::size_t> default_scene;
  /// Format-specific information the decoder chose to keep (the same entry
  /// type as ImageData::custom; keys are not empty and unique).
  std::vector<CustomEntry> custom;
  /// Source features the decoder could not represent, for example
  /// {"animation", "Walk", ""}, {"skin", ...}, {"camera", ...}, {"light", ...}
  /// (the same entry type as ImageData::unsupported; `kind` is not empty).
  std::vector<UnsupportedFeature> unsupported;
};

/// The failures of ModelData validation. Values start at 1; new values are
/// appended.
enum class ModelDataError : std::uint8_t {
  /// An index refers past the end of the list it indexes (including a default
  /// scene without scenes).
  index_out_of_range = 1,
  /// A MeshData in `meshes` is not valid; the Error's detail names the mesh
  /// and the MeshData error.
  invalid_mesh,
  /// An ImageData in `images` is not valid; the Error's detail names the image
  /// and the ImageData error.
  invalid_image,
  /// An enumerator is not one of the declared values.
  invalid_enum,
  /// A material factor is not finite or outside its range, or the cutoff or
  /// normal scale is not finite.
  invalid_material_value,
  /// A transform has a non-finite value, or a quaternion is not close to unit
  /// length.
  invalid_transform,
  /// A node is its own ancestor (the child lists contain a cycle).
  node_cycle,
  /// A node is listed as a child more than once (by two nodes, or twice by
  /// one).
  multiple_parents,
  /// A scene root has a parent, or is listed twice in one scene.
  invalid_scene_root,
  /// A material used by a primitive that a node's mesh reaches samples a
  /// texcoord set the primitive's MeshData does not have.
  missing_texcoord_set,
  /// A custom entry has an empty or repeated key.
  invalid_custom_entry,
  /// An unsupported-feature entry has an empty kind.
  invalid_unsupported_entry,
  /// A ModelMesh lists no primitive.
  empty_model_mesh,
};

/// The category of ModelDataError codes (name "terreate.asset.model_data").
[[nodiscard]] auto model_data_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of a ModelDataError.
[[nodiscard]] auto make_error_code(ModelDataError error) noexcept -> std::error_code;

/// Checks the invariants of a model and reports the first violation, with the
/// offending list and index in the Error's detail. Nothing is repaired.
///
/// The invariants, checked in this order: every mesh and image is valid
/// (reported as invalid_mesh or invalid_image, the inner error in the detail);
/// sampler enumerators are declared values; every texture's image and sampler
/// index is in range; every material has in-range finite factors, declared
/// enumerators and in-range texture references; every primitive's mesh and
/// material index is in range; every ModelMesh lists at least one primitive
/// (empty_model_mesh) and every listed primitive index is in range; every
/// node's children and mesh index are in range and its transform is finite (a quaternion within
/// 1e-3 of unit length); no node is listed as a child twice (multiple_parents); the child lists
/// contain no cycle (node_cycle); every scene root is in range, has no parent and is listed once;
/// the default scene is in range; every texcoord set that a material samples exists on the MeshData
/// of each primitive that a node's mesh reaches and that uses the material (anything no node
/// reaches is not checked for this); custom keys are non-empty and unique; unsupported kinds are
/// non-empty.
///
/// A matrix transform is only checked for finiteness; whether it can be
/// decomposed into translation, rotation and scale is not checked. Unused
/// meshes, images, samplers, textures, materials, primitives, model meshes and
/// nodes are valid. The check is iterative (no recursion), so a very long node chain
/// cannot overflow the stack, and its memory is linear in the node count.
/// Allocation failure throws std::bad_alloc.
[[nodiscard]] auto validate(const ModelData &model) -> terreate::Result<void>;

} // namespace terreate::asset

namespace std {
/// Lets ModelDataError convert to std::error_code.
template <> struct is_error_code_enum<terreate::asset::ModelDataError> : true_type {};
} // namespace std

#endif // TERREATE_ASSET_MODEL_DATA_HPP

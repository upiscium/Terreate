#ifndef TERREATE_ASSET_MESH_DATA_HPP
#define TERREATE_ASSET_MESH_DATA_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <terreate/core/result.hpp>

/// CPU-side geometry data shared by primitive generators, model decoders and
/// rendering code. Nothing in this namespace refers to Vulkan, Graphics or any
/// GPU handle.
///
/// Conventions of the geometry held by MeshData:
///
/// Positions, normals and tangents live in a right-handed space with +Y up.
/// A front-facing triangle winds counter-clockwise when seen from outside the
/// surface (the normal points toward the viewer). Texture coordinates are a
/// different space: their origin is the top-left of the image, u grows to the
/// right and v grows downward, like image rows. The application flips for its
/// own clip space or sampler convention. This component states these
/// conventions and never converts between them silently.
namespace terreate::asset {

/// How consecutive elements form primitives.
enum class PrimitiveTopology : std::uint8_t {
  /// Every three elements form one triangle.
  triangle_list,
  /// Every element after the first two forms a triangle with the previous two.
  triangle_strip,
  /// Every two elements form one line segment.
  line_list,
  /// Every element is one point.
  point_list,
};

/// The meaning of an attribute stream.
enum class AttributeSemantic : std::uint8_t {
  /// Vertex position, float32x3. Required in a valid mesh.
  position,
  /// Unit normal, float32x3.
  normal,
  /// Tangent with handedness in w, float32x4.
  tangent,
  /// Texture coordinate (origin top-left), float32x2. Several sets allowed.
  texcoord,
  /// Vertex color, float32x3 or float32x4. Several sets allowed.
  color,
  /// Format-specific data a decoder keeps instead of dropping it. Identified
  /// by AttributeKey::name; any float32 format is allowed.
  custom,
};

/// The storage format of one attribute element. Further formats (for example
/// normalized 8/16-bit integers) are added as enumerators later.
enum class AttributeFormat : std::uint8_t {
  /// One 32-bit IEEE 754 float, host byte order.
  float32x1,
  /// Two 32-bit floats.
  float32x2,
  /// Three 32-bit floats.
  float32x3,
  /// Four 32-bit floats.
  float32x4,
};

/// The type of the index buffer of a mesh.
enum class IndexType : std::uint8_t {
  /// The mesh is not indexed; elements are the vertices in order.
  none,
  /// 16-bit unsigned indices.
  uint16,
  /// 32-bit unsigned indices.
  uint32,
};

/// The number of components of a format (1 to 4).
[[nodiscard]] constexpr auto componentCount(AttributeFormat format) noexcept -> std::size_t {
  switch (format) {
  case AttributeFormat::float32x1:
    return 1;
  case AttributeFormat::float32x2:
    return 2;
  case AttributeFormat::float32x3:
    return 3;
  case AttributeFormat::float32x4:
    return 4;
  }
  return 0;
}

/// The size in bytes of one element of a format.
[[nodiscard]] constexpr auto byteSize(AttributeFormat format) noexcept -> std::size_t {
  return componentCount(format) * sizeof(float);
}

/// The size in bytes of one index of a type (0 for IndexType::none).
[[nodiscard]] constexpr auto byteSize(IndexType type) noexcept -> std::size_t {
  switch (type) {
  case IndexType::none:
    return 0;
  case IndexType::uint16:
    return sizeof(std::uint16_t);
  case IndexType::uint32:
    return sizeof(std::uint32_t);
  }
  return 0;
}

/// Identifies one attribute stream of a mesh: semantic, set index and, for
/// AttributeSemantic::custom, a name.
///
/// The set index distinguishes several streams of one semantic (TEXCOORD_0,
/// TEXCOORD_1). It must be 0 for position, normal and tangent. The name must
/// be empty except for custom attributes, where it must not be empty.
struct AttributeKey {
  /// The meaning of the stream.
  AttributeSemantic semantic = AttributeSemantic::position;
  /// The set index; 0 for semantics that have a single set.
  std::uint32_t set = 0;
  /// The name of a custom attribute; empty for every other semantic.
  std::string name;

  /// Keys are equal when semantic, set and name are equal.
  [[nodiscard]] friend bool operator==(const AttributeKey &, const AttributeKey &) = default;
};

/// One attribute of every vertex of a mesh, stored deinterleaved: all
/// elements of one attribute follow each other in `data`, tightly packed, in
/// host byte order.
///
/// `count` and `data` are plain members; the stream is valid only when
/// `data.size() == count * byteSize(format)` (validate() checks it).
struct AttributeStream {
  /// Which attribute this is.
  AttributeKey key;
  /// The element format.
  AttributeFormat format = AttributeFormat::float32x3;
  /// The number of elements (one per vertex).
  std::size_t count = 0;
  /// The packed element bytes.
  std::vector<std::byte> data;

  /// The size in bytes of one element.
  [[nodiscard]] auto elementSize() const noexcept -> std::size_t { return byteSize(format); }

  /// The number of float components per element.
  [[nodiscard]] auto components() const noexcept -> std::size_t { return componentCount(format); }

  /// Whether `data.size()` equals `count * elementSize()`.
  [[nodiscard]] auto sizeConsistent() const noexcept -> bool {
    return elementSize() != 0 && data.size() / elementSize() == count &&
           data.size() % elementSize() == 0;
  }

  /// The element at `vertex` as four floats; components the format does not
  /// have are 0. Returns nullopt when `vertex` is not below `count` or the
  /// data is shorter than the element needs. Reads through a byte copy, so no
  /// alignment is assumed.
  [[nodiscard]] auto get(std::size_t vertex) const noexcept -> std::optional<std::array<float, 4>>;

  /// All components of all elements as floats, `count * components()` values.
  /// Fails with MeshDataError::data_size_mismatch when the stream is not
  /// sizeConsistent().
  [[nodiscard]] auto toFloats() const -> terreate::Result<std::vector<float>>;
};

/// Builds a stream from float values.
///
/// `values.size()` must be a multiple of the format's component count;
/// otherwise the result is MeshDataError::data_size_mismatch. The stream's
/// `count` is `values.size() / componentCount(format)`. Allocation failure
/// throws std::bad_alloc.
[[nodiscard]] auto makeAttributeStream(AttributeKey key, AttributeFormat format,
                                       std::span<const float> values)
    -> terreate::Result<AttributeStream>;

/// The index buffer of a mesh: absent (std::monostate), 16-bit or 32-bit.
using IndexBuffer =
    std::variant<std::monostate, std::vector<std::uint16_t>, std::vector<std::uint32_t>>;

/// An axis-aligned bounding box in the mesh's position space.
struct Bounds {
  /// The component-wise minimum of the positions.
  std::array<float, 3> min{0.0F, 0.0F, 0.0F};
  /// The component-wise maximum of the positions.
  std::array<float, 3> max{0.0F, 0.0F, 0.0F};

  /// Bounds are equal when both corners are equal.
  [[nodiscard]] friend bool operator==(const Bounds &, const Bounds &) = default;
};

/// A reusable, backend-independent, CPU-side mesh: one glTF "primitive" or
/// one generated shape.
///
/// MeshData is an owning, copyable value (copying CPU data is explicit and
/// legitimate). It holds no GPU handle, no material and no submesh ranges;
/// those belong to a model or to the renderer. Members are plain data, so a
/// mesh can be assembled or edited freely; nothing is guaranteed until
/// validate() succeeds, and nothing is repaired.
struct MeshData {
  /// How elements form primitives.
  PrimitiveTopology topology = PrimitiveTopology::triangle_list;
  /// The attribute streams, deinterleaved, in no required order.
  std::vector<AttributeStream> attributes;
  /// The index buffer, or std::monostate for a non-indexed mesh.
  IndexBuffer indices;
  /// The bounding box when known. Present or absent, never guessed.
  std::optional<Bounds> bounds;

  /// The stream with `key`, or nullptr.
  [[nodiscard]] auto find(const AttributeKey &key) const noexcept -> const AttributeStream *;
  /// The stream with `key`, or nullptr.
  [[nodiscard]] auto find(const AttributeKey &key) noexcept -> AttributeStream *;
  /// The position stream (key `position`, set 0), or nullptr.
  [[nodiscard]] auto positions() const noexcept -> const AttributeStream *;

  /// The number of vertices: the count of the position stream, or 0 when
  /// there is none.
  [[nodiscard]] auto vertexCount() const noexcept -> std::size_t;
  /// The type of the index buffer.
  [[nodiscard]] auto indexType() const noexcept -> IndexType;
  /// The number of indices (0 when not indexed).
  [[nodiscard]] auto indexCount() const noexcept -> std::size_t;
  /// The index at `position` widened to 32 bits, or nullopt when not indexed
  /// or out of range.
  [[nodiscard]] auto index(std::size_t position) const noexcept -> std::optional<std::uint32_t>;
};

/// The failures of MeshData validation and conversion. Values start at 1; new
/// values are appended.
enum class MeshDataError : std::uint8_t {
  /// There is no position stream (key `position`, set 0), or, for
  /// computeBounds, it is empty.
  missing_position = 1,
  /// An attribute stream's count differs from the position count.
  count_mismatch,
  /// The format is not allowed for the semantic (for example a position that
  /// is not float32x3).
  format_not_allowed,
  /// An index is not below the vertex count.
  index_out_of_range,
  /// The number of indices (or of vertices when not indexed) is not valid for
  /// the topology.
  invalid_index_count,
  /// Two streams have the same semantic, set and name.
  duplicate_attribute,
  /// The bounds, when present, do not enclose every position.
  bounds_not_enclosing,
  /// A stream's byte size differs from `count * element size`, or float
  /// values do not fill whole elements.
  data_size_mismatch,
  /// A set index, or a name, is not valid for the semantic.
  invalid_attribute_key,
  /// computeBounds met a NaN or infinite position.
  non_finite_position,
  /// interleave() was asked for an attribute the mesh does not have.
  attribute_not_found,
};

/// The category of MeshDataError codes (name "terreate.asset.mesh_data").
[[nodiscard]] auto mesh_data_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of a MeshDataError.
[[nodiscard]] auto make_error_code(MeshDataError error) noexcept -> std::error_code;

/// Checks the invariants of a mesh and reports the first violation, with the
/// offending stream or index in the Error's detail. Nothing is repaired.
///
/// The invariants: every stream's key is valid, keys are unique, the format is
/// allowed for the semantic, `data.size()` matches `count`, a position stream
/// exists, every stream has the position count, the element count (indices,
/// or vertices when not indexed) suits the topology, every index is below the
/// vertex count, and present bounds enclose every position.
[[nodiscard]] auto validate(const MeshData &mesh) -> terreate::Result<void>;

/// The tight axis-aligned bounds of the position stream.
///
/// Fails with MeshDataError::missing_position when the mesh has no position
/// stream or it is empty, data_size_mismatch or format_not_allowed when it is
/// malformed, and non_finite_position for NaN or infinite values. The mesh is
/// not modified; assign the result to MeshData::bounds to record it.
[[nodiscard]] auto computeBounds(const MeshData &mesh) -> terreate::Result<Bounds>;

/// Where one attribute lives in a vertex buffer.
struct AttributeDescription {
  /// Which attribute.
  AttributeKey key;
  /// Its element format.
  AttributeFormat format = AttributeFormat::float32x3;
  /// Byte offset of the attribute inside one vertex; 0 when deinterleaved.
  std::uint32_t offset = 0;
  /// Byte distance between consecutive elements of this attribute: the common
  /// vertex stride when interleaved, the element size when deinterleaved.
  std::uint32_t stride = 0;
};

/// A description of a mesh's vertex data, enough for a later GPU conversion
/// without any Vulkan type.
struct MeshLayout {
  /// The mesh's topology.
  PrimitiveTopology topology = PrimitiveTopology::triangle_list;
  /// The mesh's index type.
  IndexType indexType = IndexType::none;
  /// The number of vertices.
  std::size_t vertexCount = 0;
  /// The number of indices (0 when not indexed).
  std::size_t indexCount = 0;
  /// True when every attribute shares one interleaved vertex buffer.
  bool interleaved = false;
  /// One entry per attribute, in stream order (or interleave order).
  std::vector<AttributeDescription> attributes;
};

/// Describes `mesh` as it is stored: deinterleaved, offsets 0, one stride
/// (element size) per attribute. Does not validate; it reports what is there.
[[nodiscard]] auto layoutOf(const MeshData &mesh) -> MeshLayout;

/// A mesh with its vertex attributes packed into one buffer.
struct InterleavedMesh {
  /// The matching description (interleaved is true, offsets and the common
  /// stride set, in the requested order).
  MeshLayout layout;
  /// `layout.vertexCount * stride` bytes: for each vertex, the attributes in
  /// order, tightly packed with no padding.
  std::vector<std::byte> vertices;
  /// A copy of the mesh's index buffer.
  IndexBuffer indices;
};

/// Interleaves the attributes listed in `order` (all of the mesh's streams, in
/// stream order, when `order` is empty) into one vertex buffer.
///
/// The mesh is validated first; its error is returned unchanged. An `order`
/// entry the mesh does not have yields MeshDataError::attribute_not_found, and
/// a repeated entry duplicate_attribute. Attributes absent from a non-empty
/// `order` are left out on purpose. The mesh is not modified. Allocation
/// failure throws std::bad_alloc, as for any standard container.
[[nodiscard]] auto interleave(const MeshData &mesh, std::span<const AttributeKey> order = {})
    -> terreate::Result<InterleavedMesh>;

} // namespace terreate::asset

namespace std {
/// Lets MeshDataError convert to std::error_code.
template <> struct is_error_code_enum<terreate::asset::MeshDataError> : true_type {};
} // namespace std

#endif // TERREATE_ASSET_MESH_DATA_HPP

#ifndef TERREATE_ASSET_PRIMITIVES_HPP
#define TERREATE_ASSET_PRIMITIVES_HPP

#include <cstdint>
#include <system_error>

#include <terreate/asset/mesh_data.hpp>
#include <terreate/core/result.hpp>

/// CPU-side generators for the primitive shapes an application needs for
/// tests, debugging and simple scenes (#346).
///
/// Every generator takes a plain description struct (designated initializers
/// work; the defaults are documented per member) and returns a MeshData that
/// passes validate(), has its bounds computed and uses PrimitiveTopology::
/// triangle_list with an index buffer. Which attributes exist is read from the
/// returned mesh: the position stream always, a normal stream (float32x3) when
/// `normals`, texcoord set 0 (float32x2) when `uvs`, a tangent stream
/// (float32x4) when `tangents`. Nothing is shared between meshes and no
/// generator touches a GPU.
///
/// Conventions, the same as for MeshData: right-handed, +Y up, triangles wind
/// counter-clockwise seen from outside (the geometric normal points outward and
/// agrees with the normal stream), texture coordinates have their origin at the
/// top-left of the image (v grows downward). Tangents point in the direction
/// of increasing u and have w = +1: `cross(normal, tangent.xyz) * w` points in
/// the direction of decreasing v (the glTF convention). Flat faces (triangle,
/// quad, box faces, plane, caps) use one normal per face; curved surfaces
/// (sphere, cylinder side) use smooth normals, with the seam and the poles
/// duplicated so that texture coordinates stay continuous.
///
/// Index type: `uint16` when the vertex count is at most 65536, otherwise
/// `uint32`. A generator fails with PrimitiveError::too_many_elements when the
/// vertex count or the index count would exceed 2^32 - 1, before it
/// allocates. Allocation failure throws std::bad_alloc.
namespace terreate::asset {

/// The failures of the primitive generators. Values start at 1; new values are
/// appended.
enum class PrimitiveError : std::uint8_t {
  /// A size or radius is zero, negative, NaN or infinite (a cylinder radius
  /// may be 0, but not both radii).
  invalid_size = 1,
  /// A segment or ring count is below the minimum of the shape.
  too_few_segments,
  /// The vertex count or the index count would not fit 32 bits.
  too_many_elements,
  /// `tangents` was requested without both `normals` and `uvs`.
  invalid_attribute_combination,
};

/// The category of PrimitiveError codes (name "terreate.asset.primitives").
[[nodiscard]] auto primitive_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of a PrimitiveError.
[[nodiscard]] auto make_error_code(PrimitiveError error) noexcept -> std::error_code;

/// An isosceles triangle in the XY plane facing +Z, centered on its bounding
/// box: vertices (-w/2,-h/2), (w/2,-h/2), (0,h/2) with UVs (0,1), (1,1),
/// (0.5,0). 3 vertices, 3 indices.
struct TriangleDesc {
  /// Base width along X; must be finite and positive.
  float width = 1.0F;
  /// Height along Y; must be finite and positive.
  float height = 1.0F;
  /// Generate the normal stream (+Z).
  bool normals = true;
  /// Generate texcoord set 0.
  bool uvs = true;
  /// Generate the tangent stream (needs `normals` and `uvs`).
  bool tangents = false;
};

/// A rectangle in the XY plane facing +Z, centered on the origin. UV (0,0) is
/// the top-left corner (+Y side), so the top edge has v = 0. 4 vertices, 6
/// indices.
struct QuadDesc {
  /// Extent along X; must be finite and positive.
  float width = 1.0F;
  /// Extent along Y; must be finite and positive.
  float height = 1.0F;
  /// Generate the normal stream (+Z).
  bool normals = true;
  /// Generate texcoord set 0.
  bool uvs = true;
  /// Generate the tangent stream (needs `normals` and `uvs`).
  bool tangents = false;
};

/// An axis-aligned box centered on the origin with six flat faces of four
/// vertices each (24 vertices, 36 indices). Every face maps the full 0..1 UV
/// square, seen from outside with u to the right and v downward; the top face
/// (+Y) has its top edge toward -Z.
struct BoxDesc {
  /// Extent along X; must be finite and positive.
  float width = 1.0F;
  /// Extent along Y; must be finite and positive.
  float height = 1.0F;
  /// Extent along Z; must be finite and positive.
  float depth = 1.0F;
  /// Generate the normal stream (flat, one per face).
  bool normals = true;
  /// Generate texcoord set 0.
  bool uvs = true;
  /// Generate the tangent stream (needs `normals` and `uvs`).
  bool tangents = false;
};

/// A grid in the XZ plane at y = 0 facing +Y, centered on the origin, with
/// `(segmentsX + 1) * (segmentsZ + 1)` vertices and `6 * segmentsX * segmentsZ`
/// indices. u grows toward +X and v toward +Z, so the top edge of the image is
/// the -Z edge seen from above.
struct PlaneDesc {
  /// Extent along X; must be finite and positive.
  float width = 1.0F;
  /// Extent along Z; must be finite and positive.
  float depth = 1.0F;
  /// Cells along X; at least 1.
  std::uint32_t segmentsX = 1;
  /// Cells along Z; at least 1.
  std::uint32_t segmentsZ = 1;
  /// Generate the normal stream (+Y).
  bool normals = true;
  /// Generate texcoord set 0.
  bool uvs = true;
  /// Generate the tangent stream (needs `normals` and `uvs`).
  bool tangents = false;
};

/// A sphere centered on the origin with its poles on the Y axis. Vertices are
/// laid out in `rings + 1` rows from the +Y pole (v = 0) to the -Y pole (v =
/// 1), each with `segments + 1` vertices; position is `(r sin(t) sin(p), r
/// cos(t), r sin(t) cos(p))` for polar angle t and azimuth p, so u = 0 and u =
/// 1 meet at the +Z meridian. `(segments + 1) * (rings + 1)` vertices and
/// `6 * segments * (rings - 1)` indices (the degenerate triangles at the poles
/// are not emitted).
struct UvSphereDesc {
  /// Radius; must be finite and positive.
  float radius = 0.5F;
  /// Divisions around the Y axis; at least 3.
  std::uint32_t segments = 32;
  /// Divisions from pole to pole; at least 2.
  std::uint32_t rings = 16;
  /// Generate the normal stream (smooth, pointing away from the center).
  bool normals = true;
  /// Generate texcoord set 0.
  bool uvs = true;
  /// Generate the tangent stream (needs `normals` and `uvs`).
  bool tangents = false;
};

/// A cylinder, or a cone or truncated cone, along the Y axis, centered on the
/// origin: the top is at y = +height/2 and the bottom at y = -height/2. A cone
/// is a cylinder whose `radiusTop` is 0 (or `radiusBottom`, pointing down).
///
/// The side has `heightSegments + 1` rows of `segments + 1` vertices (v = 0 at
/// the top, 1 at the bottom, same azimuth as UvSphereDesc) with smooth normals
/// that follow the slope. Each cap, when requested and its radius is not 0,
/// adds a center vertex and `segments + 1` rim vertices with a flat normal and
/// a planar UV disc (u grows toward +X; v toward +Z on the top cap and toward
/// -Z on the bottom cap). Triangles collapsed by a zero radius are not
/// emitted.
struct CylinderDesc {
  /// Radius at y = +height/2; finite and not negative (0 makes a cone).
  float radiusTop = 0.5F;
  /// Radius at y = -height/2; finite and not negative. At least one of the two
  /// radii must be positive.
  float radiusBottom = 0.5F;
  /// Extent along Y; must be finite and positive.
  float height = 1.0F;
  /// Divisions around the Y axis; at least 3.
  std::uint32_t segments = 32;
  /// Rows along Y; at least 1.
  std::uint32_t heightSegments = 1;
  /// Close the top with a cap (ignored when `radiusTop` is 0).
  bool capTop = true;
  /// Close the bottom with a cap (ignored when `radiusBottom` is 0).
  bool capBottom = true;
  /// Generate the normal stream.
  bool normals = true;
  /// Generate texcoord set 0.
  bool uvs = true;
  /// Generate the tangent stream (needs `normals` and `uvs`).
  bool tangents = false;
};

/// Generates a triangle; see TriangleDesc. Errors: invalid_size,
/// invalid_attribute_combination.
[[nodiscard]] auto triangle(const TriangleDesc &desc = {}) -> terreate::Result<MeshData>;
/// Generates a quad; see QuadDesc. Errors: invalid_size,
/// invalid_attribute_combination.
[[nodiscard]] auto quad(const QuadDesc &desc = {}) -> terreate::Result<MeshData>;
/// Generates a box; see BoxDesc. Errors: invalid_size,
/// invalid_attribute_combination.
[[nodiscard]] auto box(const BoxDesc &desc = {}) -> terreate::Result<MeshData>;
/// Generates a plane grid; see PlaneDesc. Errors: invalid_size,
/// invalid_attribute_combination, too_few_segments, too_many_elements.
[[nodiscard]] auto plane(const PlaneDesc &desc = {}) -> terreate::Result<MeshData>;
/// Generates a UV sphere; see UvSphereDesc. Errors: invalid_size,
/// invalid_attribute_combination, too_few_segments, too_many_elements.
[[nodiscard]] auto uvSphere(const UvSphereDesc &desc = {}) -> terreate::Result<MeshData>;
/// Generates a cylinder or cone; see CylinderDesc. Errors: invalid_size,
/// invalid_attribute_combination, too_few_segments, too_many_elements.
[[nodiscard]] auto cylinder(const CylinderDesc &desc = {}) -> terreate::Result<MeshData>;

} // namespace terreate::asset

namespace std {
/// Lets PrimitiveError convert to std::error_code.
template <> struct is_error_code_enum<terreate::asset::PrimitiveError> : true_type {};
} // namespace std

#endif // TERREATE_ASSET_PRIMITIVES_HPP

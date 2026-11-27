#include <terreate/asset/primitives.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace terreate::asset {
namespace {

using Vec3 = std::array<double, 3>;

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint64_t kMaxCount = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kUint16Vertices = std::uint64_t{1} << 16;

class PrimitiveErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.asset.primitives"; }
  std::string message(int value) const override {
    switch (static_cast<PrimitiveError>(value)) {
    case PrimitiveError::invalid_size:
      return "a size or radius is not finite and positive";
    case PrimitiveError::too_few_segments:
      return "a segment or ring count is below the minimum";
    case PrimitiveError::too_many_elements:
      return "the vertex or index count would not fit 32 bits";
    case PrimitiveError::invalid_attribute_combination:
      return "tangents need both normals and uvs";
    }
    return "unknown primitive error";
  }
};

// Error construction allocates; on exhaustion a code-only Error is returned.
Error makeError(PrimitiveError error, const char *operation, const char *detail) {
  try {
    return Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return Error{make_error_code(error)};
  }
}

bool positive(float value) noexcept { return std::isfinite(value) && value > 0.0F; }

struct Flags {
  bool normals;
  bool uvs;
  bool tangents;
};

std::optional<Error> checkFlags(const Flags &flags, const char *operation) {
  if (flags.tangents && !(flags.normals && flags.uvs))
    return makeError(PrimitiveError::invalid_attribute_combination, operation,
                     "tangents need normals and uvs");
  return std::nullopt;
}

// Multiplies without wrapping; nullopt on overflow of 64 bits.
std::optional<std::uint64_t> mul(std::uint64_t a, std::uint64_t b) noexcept {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
    return std::nullopt;
  return a * b;
}

std::optional<Error> checkCounts(std::optional<std::uint64_t> vertices,
                                 std::optional<std::uint64_t> indices, const char *operation) {
  if (!vertices || !indices || *vertices > kMaxCount || *indices > kMaxCount)
    return makeError(PrimitiveError::too_many_elements, operation,
                     "vertex or index count exceeds 2^32 - 1");
  return std::nullopt;
}

AttributeStream makeStream(AttributeSemantic semantic, AttributeFormat format,
                           const std::vector<float> &values) {
  AttributeStream stream;
  stream.key.semantic = semantic;
  stream.format = format;
  stream.count = values.size() / componentCount(format);
  stream.data.resize(values.size() * sizeof(float));
  if (!values.empty())
    std::memcpy(stream.data.data(), values.data(), stream.data.size());
  return stream;
}

struct Vertex {
  Vec3 position;
  Vec3 normal;
  double u;
  double v;
  Vec3 tangent;
};

// Collects vertices and triangles, then assembles the MeshData.
class Builder {
public:
  explicit Builder(const Flags &flags) : flags_(flags) {}

  void vertex(const Vertex &vtx) {
    for (const double c : vtx.position)
      positions_.push_back(static_cast<float>(c));
    if (flags_.normals)
      for (const double c : vtx.normal)
        normals_.push_back(static_cast<float>(c));
    if (flags_.uvs) {
      uvs_.push_back(static_cast<float>(vtx.u));
      uvs_.push_back(static_cast<float>(vtx.v));
    }
    if (flags_.tangents) {
      for (const double c : vtx.tangent)
        tangents_.push_back(static_cast<float>(c));
      tangents_.push_back(1.0F);
    }
  }

  void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    indices_.push_back(a);
    indices_.push_back(b);
    indices_.push_back(c);
  }

  std::uint32_t count() const noexcept { return static_cast<std::uint32_t>(positions_.size() / 3); }

  terreate::Result<MeshData> finish() {
    MeshData mesh;
    mesh.topology = PrimitiveTopology::triangle_list;
    mesh.attributes.push_back(
        makeStream(AttributeSemantic::position, AttributeFormat::float32x3, positions_));
    if (flags_.normals)
      mesh.attributes.push_back(
          makeStream(AttributeSemantic::normal, AttributeFormat::float32x3, normals_));
    if (flags_.uvs)
      mesh.attributes.push_back(
          makeStream(AttributeSemantic::texcoord, AttributeFormat::float32x2, uvs_));
    if (flags_.tangents)
      mesh.attributes.push_back(
          makeStream(AttributeSemantic::tangent, AttributeFormat::float32x4, tangents_));
    if (mesh.vertexCount() <= kUint16Vertices) {
      mesh.indices = std::vector<std::uint16_t>(indices_.begin(), indices_.end());
    } else {
      mesh.indices = std::move(indices_);
    }
    auto bounds = computeBounds(mesh);
    if (!bounds) {
      return std::unexpected(std::move(bounds.error()));
    }
    mesh.bounds = *bounds;
    return mesh;
  }

private:
  Flags flags_;
  std::vector<float> positions_;
  std::vector<float> normals_;
  std::vector<float> uvs_;
  std::vector<float> tangents_;
  std::vector<std::uint32_t> indices_;
};

Vec3 normalized(const Vec3 &v) {
  const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return {v[0] / length, v[1] / length, v[2] / length};
}

// A (segU x segV) grid of quads spanned by `u` (increasing u) and `v`
// (increasing v) from `origin`, the top-left corner seen from outside. The
// caller guarantees cross(u, v) = -normal, so winding is counter-clockwise
// seen from the normal side.
void addGrid(Builder &b, const Vec3 &origin, const Vec3 &u, const Vec3 &v, const Vec3 &normal,
             std::uint32_t segU, std::uint32_t segV) {
  const std::uint32_t base = b.count();
  const Vec3 tangent = normalized(u);
  for (std::uint32_t iv = 0; iv <= segV; ++iv) {
    const double fv = static_cast<double>(iv) / segV;
    for (std::uint32_t iu = 0; iu <= segU; ++iu) {
      const double fu = static_cast<double>(iu) / segU;
      b.vertex({{origin[0] + u[0] * fu + v[0] * fv, origin[1] + u[1] * fu + v[1] * fv,
                 origin[2] + u[2] * fu + v[2] * fv},
                normal,
                fu,
                fv,
                tangent});
    }
  }
  for (std::uint32_t iv = 0; iv < segV; ++iv) {
    for (std::uint32_t iu = 0; iu < segU; ++iu) {
      const std::uint32_t a = base + iv * (segU + 1) + iu;
      const std::uint32_t bb = a + (segU + 1);
      const std::uint32_t d = a + 1;
      const std::uint32_t c = bb + 1;
      b.triangle(a, bb, d);
      b.triangle(d, bb, c);
    }
  }
}

// One row of a surface of revolution around Y.
struct Ring {
  double y;
  double rho;
  double normalRho; // radial part of the unit normal
  double normalY;
  double v;
};

void addLathe(Builder &b, const std::vector<Ring> &rows, std::uint32_t segments) {
  const std::uint32_t base = b.count();
  for (const Ring &row : rows) {
    for (std::uint32_t j = 0; j <= segments; ++j) {
      // The last vertex of a row repeats the first one's position exactly so
      // the seam is watertight.
      const double phi = 2.0 * kPi * static_cast<double>(j == segments ? 0 : j) / segments;
      const double s = std::sin(phi);
      const double c = std::cos(phi);
      b.vertex({{row.rho * s, row.y, row.rho * c},
                {row.normalRho * s, row.normalY, row.normalRho * c},
                static_cast<double>(j) / segments,
                row.v,
                {c, 0.0, -s}});
    }
  }
  for (std::size_t r = 0; r + 1 < rows.size(); ++r) {
    const bool topCollapsed = rows[r].rho == 0.0;
    const bool bottomCollapsed = rows[r + 1].rho == 0.0;
    for (std::uint32_t j = 0; j < segments; ++j) {
      const std::uint32_t a = base + static_cast<std::uint32_t>(r) * (segments + 1) + j;
      const std::uint32_t bb = a + (segments + 1);
      const std::uint32_t d = a + 1;
      const std::uint32_t c = bb + 1;
      if (!topCollapsed)
        b.triangle(a, bb, d);
      if (!bottomCollapsed)
        b.triangle(d, bb, c);
    }
  }
}

void addCap(Builder &b, double y, double radius, bool top, std::uint32_t segments) {
  const std::uint32_t center = b.count();
  const Vec3 normal{0.0, top ? 1.0 : -1.0, 0.0};
  const Vec3 tangent{1.0, 0.0, 0.0};
  b.vertex({{0.0, y, 0.0}, normal, 0.5, 0.5, tangent});
  for (std::uint32_t j = 0; j <= segments; ++j) {
    const double phi = 2.0 * kPi * static_cast<double>(j == segments ? 0 : j) / segments;
    const double s = std::sin(phi);
    const double c = std::cos(phi);
    b.vertex({{radius * s, y, radius * c},
              normal,
              0.5 + 0.5 * s,
              top ? 0.5 + 0.5 * c : 0.5 - 0.5 * c,
              tangent});
  }
  for (std::uint32_t j = 0; j < segments; ++j) {
    const std::uint32_t a = center + 1 + j;
    if (top)
      b.triangle(center, a, a + 1);
    else
      b.triangle(center, a + 1, a);
  }
}

} // namespace

auto primitive_error_category() noexcept -> const std::error_category & {
  static const PrimitiveErrorCategory category;
  return category;
}

auto make_error_code(PrimitiveError error) noexcept -> std::error_code {
  return {static_cast<int>(error), primitive_error_category()};
}

auto triangle(const TriangleDesc &desc) -> terreate::Result<MeshData> {
  constexpr const char *op = "terreate::asset::triangle";
  if (!positive(desc.width) || !positive(desc.height))
    return std::unexpected(makeError(PrimitiveError::invalid_size, op, "width and height"));
  if (auto error = checkFlags({desc.normals, desc.uvs, desc.tangents}, op))
    return std::unexpected(std::move(*error));
  const double hw = desc.width / 2.0;
  const double hh = desc.height / 2.0;
  Builder b({desc.normals, desc.uvs, desc.tangents});
  const Vec3 n{0, 0, 1};
  const Vec3 t{1, 0, 0};
  b.vertex({{-hw, -hh, 0}, n, 0.0, 1.0, t});
  b.vertex({{hw, -hh, 0}, n, 1.0, 1.0, t});
  b.vertex({{0, hh, 0}, n, 0.5, 0.0, t});
  b.triangle(0, 1, 2);
  return b.finish();
}

auto quad(const QuadDesc &desc) -> terreate::Result<MeshData> {
  constexpr const char *op = "terreate::asset::quad";
  if (!positive(desc.width) || !positive(desc.height))
    return std::unexpected(makeError(PrimitiveError::invalid_size, op, "width and height"));
  if (auto error = checkFlags({desc.normals, desc.uvs, desc.tangents}, op))
    return std::unexpected(std::move(*error));
  const double w = desc.width;
  const double h = desc.height;
  Builder b({desc.normals, desc.uvs, desc.tangents});
  addGrid(b, {-w / 2, h / 2, 0}, {w, 0, 0}, {0, -h, 0}, {0, 0, 1}, 1, 1);
  return b.finish();
}

auto box(const BoxDesc &desc) -> terreate::Result<MeshData> {
  constexpr const char *op = "terreate::asset::box";
  if (!positive(desc.width) || !positive(desc.height) || !positive(desc.depth))
    return std::unexpected(makeError(PrimitiveError::invalid_size, op, "width, height and depth"));
  if (auto error = checkFlags({desc.normals, desc.uvs, desc.tangents}, op))
    return std::unexpected(std::move(*error));
  const double hx = desc.width / 2.0;
  const double hy = desc.height / 2.0;
  const double hz = desc.depth / 2.0;
  Builder b({desc.normals, desc.uvs, desc.tangents});
  // Each face: origin = top-left corner seen from outside, then the u and v
  // spans, then the outward normal. cross(u, v) = -normal for every face.
  addGrid(b, {-hx, hy, hz}, {2 * hx, 0, 0}, {0, -2 * hy, 0}, {0, 0, 1}, 1, 1);   // +Z
  addGrid(b, {hx, hy, -hz}, {-2 * hx, 0, 0}, {0, -2 * hy, 0}, {0, 0, -1}, 1, 1); // -Z
  addGrid(b, {hx, hy, hz}, {0, 0, -2 * hz}, {0, -2 * hy, 0}, {1, 0, 0}, 1, 1);   // +X
  addGrid(b, {-hx, hy, -hz}, {0, 0, 2 * hz}, {0, -2 * hy, 0}, {-1, 0, 0}, 1, 1); // -X
  addGrid(b, {-hx, hy, -hz}, {2 * hx, 0, 0}, {0, 0, 2 * hz}, {0, 1, 0}, 1, 1);   // +Y
  addGrid(b, {-hx, -hy, hz}, {2 * hx, 0, 0}, {0, 0, -2 * hz}, {0, -1, 0}, 1, 1); // -Y
  return b.finish();
}

auto plane(const PlaneDesc &desc) -> terreate::Result<MeshData> {
  constexpr const char *op = "terreate::asset::plane";
  if (!positive(desc.width) || !positive(desc.depth))
    return std::unexpected(makeError(PrimitiveError::invalid_size, op, "width and depth"));
  if (auto error = checkFlags({desc.normals, desc.uvs, desc.tangents}, op))
    return std::unexpected(std::move(*error));
  if (desc.segmentsX < 1 || desc.segmentsZ < 1)
    return std::unexpected(
        makeError(PrimitiveError::too_few_segments, op, "segmentsX and segmentsZ need at least 1"));
  const std::uint64_t sx = desc.segmentsX;
  const std::uint64_t sz = desc.segmentsZ;
  const auto cells = mul(sx, sz);
  if (auto error = checkCounts(mul(sx + 1, sz + 1), cells ? mul(*cells, 6) : std::nullopt, op))
    return std::unexpected(std::move(*error));
  const double w = desc.width;
  const double d = desc.depth;
  Builder b({desc.normals, desc.uvs, desc.tangents});
  addGrid(b, {-w / 2, 0, -d / 2}, {w, 0, 0}, {0, 0, d}, {0, 1, 0}, desc.segmentsX, desc.segmentsZ);
  return b.finish();
}

auto uvSphere(const UvSphereDesc &desc) -> terreate::Result<MeshData> {
  constexpr const char *op = "terreate::asset::uvSphere";
  if (!positive(desc.radius))
    return std::unexpected(makeError(PrimitiveError::invalid_size, op, "radius"));
  if (auto error = checkFlags({desc.normals, desc.uvs, desc.tangents}, op))
    return std::unexpected(std::move(*error));
  if (desc.segments < 3 || desc.rings < 2)
    return std::unexpected(makeError(PrimitiveError::too_few_segments, op,
                                     "segments need at least 3 and rings at least 2"));
  const std::uint64_t seg = desc.segments;
  const std::uint64_t rings = desc.rings;
  const auto indices = mul(seg, rings - 1);
  if (auto error =
          checkCounts(mul(seg + 1, rings + 1), indices ? mul(*indices, 6) : std::nullopt, op))
    return std::unexpected(std::move(*error));
  const double radius = desc.radius;
  std::vector<Ring> rows;
  rows.reserve(desc.rings + 1);
  for (std::uint32_t r = 0; r <= desc.rings; ++r) {
    const double theta = kPi * static_cast<double>(r) / desc.rings;
    if (r == 0)
      rows.push_back({radius, 0.0, 0.0, 1.0, 0.0});
    else if (r == desc.rings)
      rows.push_back({-radius, 0.0, 0.0, -1.0, 1.0});
    else
      rows.push_back({radius * std::cos(theta), radius * std::sin(theta), std::sin(theta),
                      std::cos(theta), static_cast<double>(r) / desc.rings});
  }
  Builder b({desc.normals, desc.uvs, desc.tangents});
  addLathe(b, rows, desc.segments);
  return b.finish();
}

auto cylinder(const CylinderDesc &desc) -> terreate::Result<MeshData> {
  constexpr const char *op = "terreate::asset::cylinder";
  const auto radiusOk = [](float r) { return std::isfinite(r) && r >= 0.0F; };
  if (!radiusOk(desc.radiusTop) || !radiusOk(desc.radiusBottom) ||
      (desc.radiusTop == 0.0F && desc.radiusBottom == 0.0F) || !positive(desc.height))
    return std::unexpected(makeError(PrimitiveError::invalid_size, op,
                                     "radii must be finite, not negative and not both 0; "
                                     "height must be positive"));
  if (auto error = checkFlags({desc.normals, desc.uvs, desc.tangents}, op))
    return std::unexpected(std::move(*error));
  if (desc.segments < 3 || desc.heightSegments < 1)
    return std::unexpected(makeError(PrimitiveError::too_few_segments, op,
                                     "segments need at least 3 and heightSegments at least 1"));
  const std::uint64_t seg = desc.segments;
  const std::uint64_t rows = desc.heightSegments;
  const bool topCap = desc.capTop && desc.radiusTop > 0.0F;
  const bool bottomCap = desc.capBottom && desc.radiusBottom > 0.0F;
  const std::uint64_t collapsed =
      (desc.radiusTop == 0.0F ? 1U : 0U) + (desc.radiusBottom == 0.0F ? 1U : 0U);
  const std::uint64_t capCount = (topCap ? 1U : 0U) + (bottomCap ? 1U : 0U);
  const auto sideVertices = mul(seg + 1, rows + 1);
  const auto sideTriangles = mul(seg, 2 * rows - collapsed);
  std::optional<std::uint64_t> vertices;
  std::optional<std::uint64_t> indices;
  if (sideVertices && sideTriangles) {
    vertices = *sideVertices + capCount * (seg + 2);
    indices = *sideTriangles * 3 + capCount * seg * 3;
  }
  if (auto error = checkCounts(vertices, indices, op))
    return std::unexpected(std::move(*error));

  const double rt = desc.radiusTop;
  const double rb = desc.radiusBottom;
  const double h = desc.height;
  const double slope = std::sqrt(h * h + (rb - rt) * (rb - rt));
  const double nrho = h / slope;
  const double ny = (rb - rt) / slope;
  std::vector<Ring> sideRows;
  sideRows.reserve(desc.heightSegments + 1);
  for (std::uint32_t r = 0; r <= desc.heightSegments; ++r) {
    const double f = static_cast<double>(r) / desc.heightSegments;
    const double y = r == desc.heightSegments ? -h / 2.0 : h / 2.0 - h * f;
    const double rho = r == 0 ? rt : (r == desc.heightSegments ? rb : rt + (rb - rt) * f);
    sideRows.push_back({y, rho, nrho, ny, f});
  }
  Builder b({desc.normals, desc.uvs, desc.tangents});
  addLathe(b, sideRows, desc.segments);
  if (topCap)
    addCap(b, h / 2.0, rt, true, desc.segments);
  if (bottomCap)
    addCap(b, -h / 2.0, rb, false, desc.segments);
  return b.finish();
}

} // namespace terreate::asset

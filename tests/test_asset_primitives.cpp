// #346: the CPU-only primitive generators. This test links Terreate::Asset
// alone; it needs no Vulkan, no Graphics, no Platform, no display and no OS- or
// linker-specific mechanism. Conventions are checked against the triangle data,
// not against prose.
#include <terreate/asset/primitives.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace terreate::asset;

const char *test_name = "asset.primitives";
int failures = 0;

bool check(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message.c_str());
  }
  return condition;
}

#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR)
#error "Asset public headers must not pull in Vulkan or GLFW"
#endif
static_assert(std::is_error_code_enum_v<PrimitiveError>);

using Vec3 = std::array<double, 3>;

Vec3 sub(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 cross(const Vec3 &a, const Vec3 &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
Vec3 scale(const Vec3 &a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }

template <typename T> bool hasError(const terreate::Result<T> &result, PrimitiveError error) {
  return !result && result.error().code() == make_error_code(error);
}

// The mesh read back through its public streams.
struct Decoded {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<std::array<double, 2>> uvs;
  std::vector<std::array<double, 4>> tangents;
  std::vector<std::uint32_t> indices;
};

Decoded decode(const MeshData &mesh) {
  Decoded d;
  const auto floats = [&](AttributeSemantic semantic) {
    const AttributeStream *s = mesh.find({semantic, 0, {}});
    return s ? terreate::unwrap(s->toFloats()) : std::vector<float>{};
  };
  const auto p = floats(AttributeSemantic::position);
  for (std::size_t i = 0; i + 2 < p.size(); i += 3)
    d.positions.push_back({p[i], p[i + 1], p[i + 2]});
  const auto n = floats(AttributeSemantic::normal);
  for (std::size_t i = 0; i + 2 < n.size(); i += 3)
    d.normals.push_back({n[i], n[i + 1], n[i + 2]});
  const auto uv = floats(AttributeSemantic::texcoord);
  for (std::size_t i = 0; i + 1 < uv.size(); i += 2)
    d.uvs.push_back({uv[i], uv[i + 1]});
  const auto t = floats(AttributeSemantic::tangent);
  for (std::size_t i = 0; i + 3 < t.size(); i += 4)
    d.tangents.push_back({t[i], t[i + 1], t[i + 2], t[i + 3]});
  for (std::size_t i = 0; i < mesh.indexCount(); ++i)
    d.indices.push_back(mesh.index(i).value_or(0));
  return d;
}

// A shape with a known inside point (the origin): triangles must face away
// from it. Planar shapes name the expected normal instead.
struct Expect {
  bool closed = false;        // outward = away from the origin
  Vec3 planarNormal{0, 0, 0}; // used when !closed
  double smoothness = 0.9;    // min dot(face normal, mean vertex normal)
};

void checkWindingNormalsUvs(const std::string &name, const MeshData &mesh, const Expect &expect) {
  const Decoded d = decode(mesh);
  const bool hasN = !d.normals.empty();
  const bool hasUv = !d.uvs.empty();
  const bool hasT = !d.tangents.empty();
  check(validate(mesh).has_value(), name + ": validates");
  check(mesh.topology == PrimitiveTopology::triangle_list, name + ": triangle list");
  check(d.indices.size() % 3 == 0 && !d.indices.empty(), name + ": whole triangles");

  for (std::size_t v = 0; v < d.positions.size(); ++v) {
    if (hasN) {
      check(std::fabs(length(d.normals[v]) - 1.0) < 1e-5, name + ": unit normal");
    }
    if (hasUv) {
      check(d.uvs[v][0] >= 0.0 && d.uvs[v][0] <= 1.0 && d.uvs[v][1] >= 0.0 && d.uvs[v][1] <= 1.0,
            name + ": uv within 0..1");
    }
    if (hasT) {
      const Vec3 t{d.tangents[v][0], d.tangents[v][1], d.tangents[v][2]};
      check(std::fabs(length(t) - 1.0) < 1e-5, name + ": unit tangent");
      check(std::fabs(dot(t, d.normals[v])) < 1e-5, name + ": tangent orthogonal to normal");
      check(d.tangents[v][3] == 1.0F, name + ": tangent handedness w = +1");
    }
  }

  for (std::size_t i = 0; i < d.indices.size(); i += 3) {
    const std::uint32_t ia = d.indices[i], ib = d.indices[i + 1], ic = d.indices[i + 2];
    const Vec3 &p0 = d.positions[ia];
    const Vec3 &p1 = d.positions[ib];
    const Vec3 &p2 = d.positions[ic];
    const Vec3 cr = cross(sub(p1, p0), sub(p2, p0));
    const double area2 = length(cr);
    if (!check(area2 > 1e-9, name + ": no degenerate triangle"))
      continue;
    const Vec3 gn = scale(cr, 1.0 / area2);
    if (expect.closed) {
      const Vec3 centroid{(p0[0] + p1[0] + p2[0]) / 3, (p0[1] + p1[1] + p2[1]) / 3,
                          (p0[2] + p1[2] + p2[2]) / 3};
      check(dot(gn, centroid) > 0.0, name + ": geometric normal points away from the center");
    } else {
      check(dot(gn, expect.planarNormal) > 0.999, name + ": geometric normal is the planar normal");
    }
    if (hasN) {
      const Vec3 mean{d.normals[ia][0] + d.normals[ib][0] + d.normals[ic][0],
                      d.normals[ia][1] + d.normals[ib][1] + d.normals[ic][1],
                      d.normals[ia][2] + d.normals[ib][2] + d.normals[ic][2]};
      check(dot(gn, scale(mean, 1.0 / length(mean))) > expect.smoothness,
            name + ": winding agrees with the normal attribute");
    }
    if (hasUv) {
      // dP/du and dP/dv of this triangle from its positions and uvs.
      const auto &u0 = d.uvs[ia];
      const auto &u1 = d.uvs[ib];
      const auto &u2 = d.uvs[ic];
      const double du1 = u1[0] - u0[0], dv1 = u1[1] - u0[1];
      const double du2 = u2[0] - u0[0], dv2 = u2[1] - u0[1];
      const double det = du1 * dv2 - du2 * dv1;
      if (!check(std::fabs(det) > 1e-12, name + ": uv triangle is not degenerate"))
        continue;
      const Vec3 e1 = sub(p1, p0), e2 = sub(p2, p0);
      Vec3 dPdu, dPdv;
      for (int k = 0; k < 3; ++k) {
        dPdu[k] = (e1[k] * dv2 - e2[k] * dv1) / det;
        dPdv[k] = (e2[k] * du1 - e1[k] * du2) / det;
      }
      // u grows to the right and v downward seen from outside: u x v points
      // inward, i.e. against the geometric normal.
      check(dot(cross(dPdu, dPdv), gn) < 0.0, name + ": uv orientation (u right, v down)");
      if (hasT) {
        const Vec3 tu = scale(dPdu, 1.0 / length(dPdu));
        for (const std::uint32_t idx : {ia, ib, ic}) {
          const Vec3 t{d.tangents[idx][0], d.tangents[idx][1], d.tangents[idx][2]};
          // The tangent follows u; cross(normal, tangent) * w follows -v.
          const Vec3 bt = scale(cross(d.normals[idx], t), d.tangents[idx][3]);
          check(dot(t, tu) > 0.9, name + ": tangent points toward increasing u");
          check(dot(bt, dPdv) < 0.0, name + ": bitangent points toward decreasing v");
        }
      }
    }
  }

  const Bounds tight = terreate::unwrap(computeBounds(mesh));
  check(mesh.bounds.has_value() && *mesh.bounds == tight, name + ": bounds are computed and tight");
}

// Every directed edge appears once and its reverse exists (after welding equal
// positions): closed and consistently oriented, so every edge has two faces.
bool closedManifold(const MeshData &mesh) {
  const Decoded d = decode(mesh);
  std::map<std::array<float, 3>, std::uint32_t> weld;
  std::vector<std::uint32_t> id(d.positions.size());
  for (std::size_t i = 0; i < d.positions.size(); ++i) {
    const std::array<float, 3> key{static_cast<float>(d.positions[i][0]),
                                   static_cast<float>(d.positions[i][1]),
                                   static_cast<float>(d.positions[i][2])};
    id[i] = weld.emplace(key, static_cast<std::uint32_t>(weld.size())).first->second;
  }
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> directed;
  for (std::size_t i = 0; i < d.indices.size(); i += 3) {
    for (int k = 0; k < 3; ++k) {
      const auto a = id[d.indices[i + k]];
      const auto b = id[d.indices[i + (k + 1) % 3]];
      ++directed[{a, b}];
    }
  }
  for (const auto &[edge, count] : directed) {
    if (count != 1)
      return false;
    const auto reverse = directed.find({edge.second, edge.first});
    if (reverse == directed.end() || reverse->second != 1)
      return false;
  }
  return true;
}

bool sameMesh(const MeshData &a, const MeshData &b) {
  if (a.topology != b.topology || a.indices != b.indices || a.bounds != b.bounds ||
      a.attributes.size() != b.attributes.size())
    return false;
  for (std::size_t i = 0; i < a.attributes.size(); ++i) {
    const auto &x = a.attributes[i];
    const auto &y = b.attributes[i];
    if (!(x.key == y.key) || x.format != y.format || x.count != y.count || x.data != y.data)
      return false;
  }
  return true;
}

constexpr Expect kClosed{true, {0, 0, 0}, 0.9};
constexpr Expect planar(double x, double y, double z) { return {false, {x, y, z}, 0.999}; }

void withAllAttributes() {
  const auto tri = terreate::unwrap(triangle({.width = 2.0F, .height = 4.0F, .tangents = true}));
  checkWindingNormalsUvs("triangle", tri, planar(0, 0, 1));
  check(tri.bounds && tri.bounds->min == std::array<float, 3>{-1, -2, 0} &&
            tri.bounds->max == std::array<float, 3>{1, 2, 0},
        "triangle bounds");
  check(tri.vertexCount() == 3 && tri.indexCount() == 3, "triangle counts");

  const auto q = terreate::unwrap(quad({.width = 2.0F, .height = 4.0F, .tangents = true}));
  checkWindingNormalsUvs("quad", q, planar(0, 0, 1));
  check(q.vertexCount() == 4 && q.indexCount() == 6, "quad counts");

  const auto b =
      terreate::unwrap(box({.width = 2.0F, .height = 4.0F, .depth = 6.0F, .tangents = true}));
  checkWindingNormalsUvs("box", b, {true, {0, 0, 0}, 0.999});
  check(b.vertexCount() == 24 && b.indexCount() == 36, "box counts");
  check(closedManifold(b), "box is closed and consistently oriented");
  check(b.bounds && b.bounds->min == std::array<float, 3>{-1, -2, -3} &&
            b.bounds->max == std::array<float, 3>{1, 2, 3},
        "box bounds are the half extents");

  const auto p = terreate::unwrap(
      plane({.width = 4.0F, .depth = 2.0F, .segmentsX = 4, .segmentsZ = 3, .tangents = true}));
  checkWindingNormalsUvs("plane", p, planar(0, 1, 0));
  check(p.bounds && p.bounds->min == std::array<float, 3>{-2, 0, -1} &&
            p.bounds->max == std::array<float, 3>{2, 0, 1},
        "plane bounds");
  check(p.vertexCount() == 20 && p.indexCount() == 72, "plane counts");

  const auto s = terreate::unwrap(uvSphere({.radius = 2.0F, .tangents = true}));
  checkWindingNormalsUvs("sphere", s, kClosed);
  check(s.vertexCount() == std::size_t{33} * 17 && s.indexCount() == std::size_t{6} * 32 * 15,
        "sphere counts");
  check(closedManifold(s), "sphere is closed and consistently oriented");
  {
    const Decoded d = decode(s);
    bool radial = true;
    for (std::size_t i = 0; i < d.positions.size(); ++i) {
      const Vec3 expected = scale(d.positions[i], 0.5);
      radial = radial && length(sub(expected, d.normals[i])) < 1e-5 &&
               std::fabs(length(d.positions[i]) - 2.0) < 1e-5;
    }
    check(radial, "sphere normals are position / radius");
    check(s.bounds && std::fabs(s.bounds->max[1] - 2.0F) < 1e-6F &&
              std::fabs(s.bounds->min[1] + 2.0F) < 1e-6F &&
              std::fabs(s.bounds->max[0] - 2.0F) < 1e-5F,
          "sphere bounds");
  }

  const auto c = terreate::unwrap(cylinder({.radiusTop = 1.0F,
                                            .radiusBottom = 2.0F,
                                            .height = 3.0F,
                                            .segments = 16,
                                            .heightSegments = 2,
                                            .tangents = true}));
  checkWindingNormalsUvs("cylinder", c, kClosed);
  check(c.vertexCount() == 17 * 3 + 2 * 18 && c.indexCount() == 16 * 4 * 3 + 2 * 16 * 3,
        "cylinder counts");
  check(closedManifold(c), "capped cylinder is closed and consistently oriented");
  check(c.bounds && c.bounds->max[1] == 1.5F && c.bounds->min[1] == -1.5F, "cylinder bounds in y");

  const auto cone = terreate::unwrap(cylinder(
      {.radiusTop = 0.0F, .radiusBottom = 1.0F, .height = 2.0F, .segments = 12, .tangents = true}));
  checkWindingNormalsUvs("cone", cone, {true, {0, 0, 0}, 0.8});
  check(cone.vertexCount() == 13 * 2 + 14 && cone.indexCount() == 12 * 1 * 3 + 12 * 3,
        "cone counts (apex triangles collapsed, one cap)");
  check(closedManifold(cone), "cone is closed and consistently oriented");

  const auto openCylinder =
      terreate::unwrap(cylinder({.segments = 8, .capTop = false, .capBottom = false}));
  check(openCylinder.vertexCount() == 18 && openCylinder.indexCount() == 48,
        "uncapped cylinder counts");
  check(validate(openCylinder).has_value(), "uncapped cylinder validates");
}

void faceConventions() {
  // Quad: the top edge (+Y) has v = 0, the left edge (-X) has u = 0.
  const Decoded q = decode(terreate::unwrap(quad({.width = 2.0F, .height = 2.0F})));
  bool ok = true;
  for (std::size_t i = 0; i < q.positions.size(); ++i) {
    ok = ok && (q.positions[i][1] > 0) == (q.uvs[i][1] == 0.0) &&
         (q.positions[i][0] < 0) == (q.uvs[i][0] == 0.0);
  }
  check(ok, "quad: top edge has v = 0, left edge has u = 0");
  // Plane: u follows +X, v follows +Z.
  const Decoded p = decode(
      terreate::unwrap(plane({.width = 2.0F, .depth = 2.0F, .segmentsX = 2, .segmentsZ = 2})));
  ok = true;
  for (std::size_t i = 0; i < p.positions.size(); ++i)
    ok = ok && std::fabs(p.uvs[i][0] - (p.positions[i][0] + 1.0) / 2.0) < 1e-6 &&
         std::fabs(p.uvs[i][1] - (p.positions[i][2] + 1.0) / 2.0) < 1e-6 &&
         p.positions[i][1] == 0.0;
  check(ok, "plane: u follows +X, v follows +Z, y = 0");
  // Box: on every face the vertex with uv (0,0) is the top-left corner seen from outside.
  {
    const MeshData bx = terreate::unwrap(box({.width = 2.0F, .height = 4.0F, .depth = 6.0F}));
    const Decoded bd = decode(bx);
    struct Face {
      Vec3 normal;
      Vec3 topLeft;
    };
    const Face faces[] = {{{0, 0, 1}, {-1, 2, 3}},  {{0, 0, -1}, {1, 2, -3}},
                          {{1, 0, 0}, {1, 2, 3}},   {{-1, 0, 0}, {-1, 2, -3}},
                          {{0, 1, 0}, {-1, 2, -3}}, {{0, -1, 0}, {-1, -2, 3}}};
    for (const Face &face : faces) {
      bool found = false;
      for (std::size_t i = 0; i < bd.positions.size(); ++i)
        if (length(sub(bd.normals[i], face.normal)) < 1e-6 && bd.uvs[i][0] == 0.0 &&
            bd.uvs[i][1] == 0.0)
          found = length(sub(bd.positions[i], face.topLeft)) < 1e-6;
      check(found, "box: uv (0,0) is the top-left corner of each face");
    }
  }
  // Cylinder: v = 0 on the +Y end; u = 0 on the +Z meridian; cap uv follow +X and +Z/-Z.
  {
    const Decoded cd =
        decode(terreate::unwrap(cylinder({.height = 2.0F, .segments = 8, .capBottom = false})));
    bool good = true;
    for (std::size_t i = 0; i < 18; ++i) {
      good = good && (cd.positions[i][1] == 1.0) == (cd.uvs[i][1] == 0.0) &&
             (cd.uvs[i][0] != 0.0 || (cd.positions[i][0] == 0.0 && cd.positions[i][2] > 0.0)) &&
             (cd.uvs[i][0] != 1.0 || (cd.positions[i][0] == 0.0 && cd.positions[i][2] > 0.0));
    }
    check(good, "cylinder side: v = 0 at +Y, u = 0 at the +Z meridian");
    good = true;
    for (std::size_t i = 18 + 1; i < cd.positions.size(); ++i) {
      good = good && std::fabs(cd.uvs[i][0] - (0.5 + 0.5 * cd.positions[i][0] / 0.5)) < 1e-6 &&
             std::fabs(cd.uvs[i][1] - (0.5 + 0.5 * cd.positions[i][2] / 0.5)) < 1e-6;
    }
    check(good, "top cap: planar disc, u follows +X and v follows +Z");
    const Decoded bc = decode(terreate::unwrap(cylinder({.segments = 8, .capTop = false})));
    good = true;
    for (std::size_t i = 18 + 1; i < bc.positions.size(); ++i)
      good = good && std::fabs(bc.uvs[i][1] - (0.5 - 0.5 * bc.positions[i][2] / 0.5)) < 1e-6;
    check(good, "bottom cap: v follows -Z");
  }
  // Sphere: v = 0 at the +Y pole, 1 at the -Y pole.
  const Decoded s = decode(terreate::unwrap(uvSphere({.radius = 1.0F, .segments = 8, .rings = 4})));
  ok = true;
  for (std::size_t i = 0; i < s.positions.size(); ++i)
    ok = ok && std::fabs(s.uvs[i][1] - (1.0 - s.positions[i][1]) / 2.0) < 0.26 &&
         (s.positions[i][1] == 1.0) == (s.uvs[i][1] == 0.0);
  check(ok, "sphere: v = 0 at +Y");
}

void attributeSelection() {
  const auto plain = terreate::unwrap(box({.normals = false, .uvs = false}));
  check(plain.attributes.size() == 1 && plain.positions() != nullptr, "positions only");
  check(plain.find({AttributeSemantic::normal, 0, {}}) == nullptr, "no normals when not requested");
  const auto normalsOnly = terreate::unwrap(uvSphere({.uvs = false}));
  check(normalsOnly.attributes.size() == 2 &&
            normalsOnly.find({AttributeSemantic::normal, 0, {}}) &&
            !normalsOnly.find({AttributeSemantic::texcoord, 0, {}}),
        "normals only");
  const auto full = terreate::unwrap(quad({.tangents = true}));
  const AttributeStream *t = full.find({AttributeSemantic::tangent, 0, {}});
  check(full.attributes.size() == 4 && t && t->format == AttributeFormat::float32x4 &&
            t->count == 4,
        "tangents are float32x4");
  const auto defaults = terreate::unwrap(quad());
  check(defaults.attributes.size() == 3 && !defaults.find({AttributeSemantic::tangent, 0, {}}),
        "defaults: normals and uvs, no tangents");
  checkWindingNormalsUvs("quad-defaults", defaults, planar(0, 0, 1));
  checkWindingNormalsUvs("sphere-no-tangents",
                         terreate::unwrap(uvSphere({.segments = 5, .rings = 3})),
                         {true, {0, 0, 0}, 0.7});
}

void indexTypeRule() {
  const auto small = terreate::unwrap(plane({.segmentsX = 255, .segmentsZ = 255}));
  check(small.vertexCount() == 65536 && small.indexType() == IndexType::uint16,
        "65536 vertices use uint16");
  check(validate(small).has_value(), "the largest uint16 plane validates");
  const auto large = terreate::unwrap(plane({.segmentsX = 256, .segmentsZ = 255}));
  check(large.vertexCount() == 65792 && large.indexType() == IndexType::uint32,
        "more than 65536 vertices use uint32");
  check(validate(large).has_value(), "a uint32 plane validates");
  check(terreate::unwrap(box()).indexType() == IndexType::uint16, "box uses uint16");
}

void everyError() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  check(std::string(primitive_error_category().name()) == "terreate.asset.primitives",
        "category name");
  check(make_error_code(PrimitiveError::invalid_size).category() == primitive_error_category(),
        "error code category");

  for (const float bad : {0.0F, -1.0F, nan, inf, -inf}) {
    const auto e = PrimitiveError::invalid_size;
    check(hasError(triangle({.width = bad}), e) && hasError(triangle({.height = bad}), e),
          "triangle: invalid size");
    check(hasError(quad({.width = bad}), e) && hasError(quad({.height = bad}), e),
          "quad: invalid size");
    check(hasError(box({.width = bad}), e) && hasError(box({.height = bad}), e) &&
              hasError(box({.depth = bad}), e),
          "box: invalid size");
    check(hasError(plane({.width = bad}), e) && hasError(plane({.depth = bad}), e),
          "plane: invalid size");
    check(hasError(uvSphere({.radius = bad}), e), "sphere: invalid radius");
    check(hasError(cylinder({.height = bad}), e), "cylinder: invalid height");
  }
  const auto size = PrimitiveError::invalid_size;
  check(hasError(cylinder({.radiusTop = -0.5F}), size) &&
            hasError(cylinder({.radiusBottom = nan}), size) &&
            hasError(cylinder({.radiusTop = inf}), size) &&
            hasError(cylinder({.radiusTop = 0.0F, .radiusBottom = 0.0F}), size),
        "cylinder: invalid radii (both zero included)");
  check(cylinder({.radiusTop = 0.0F, .radiusBottom = 1.0F}).has_value() &&
            cylinder({.radiusTop = 1.0F, .radiusBottom = 0.0F}).has_value(),
        "cylinder: one zero radius is a cone");

  const auto few = PrimitiveError::too_few_segments;
  check(hasError(plane({.segmentsX = 0}), few) && hasError(plane({.segmentsZ = 0}), few),
        "plane: segments");
  check(hasError(uvSphere({.segments = 2}), few) && hasError(uvSphere({.rings = 1}), few) &&
            uvSphere({.segments = 3, .rings = 2}).has_value(),
        "sphere: segments and rings");
  check(hasError(cylinder({.segments = 2}), few) &&
            hasError(cylinder({.heightSegments = 0}), few) &&
            cylinder({.segments = 3, .heightSegments = 1}).has_value(),
        "cylinder: segments");

  const auto many = PrimitiveError::too_many_elements;
  constexpr std::uint32_t max32 = std::numeric_limits<std::uint32_t>::max();
  check(hasError(plane({.segmentsX = 65536, .segmentsZ = 65536}), many),
        "plane: vertices exceed 32 bits");
  check(hasError(plane({.segmentsX = max32, .segmentsZ = max32}), many),
        "plane: 64-bit product overflow");
  check(hasError(plane({.segmentsX = 40000, .segmentsZ = 20000}), many),
        "plane: indices exceed 32 bits while vertices fit");
  check(hasError(uvSphere({.segments = 100000, .rings = 100000}), many), "sphere: too many");
  check(hasError(uvSphere({.segments = max32, .rings = max32}), many), "sphere: overflow");
  check(hasError(cylinder({.segments = 100000, .heightSegments = 100000}), many),
        "cylinder: too many");
  check(hasError(cylinder({.segments = max32, .heightSegments = max32}), many),
        "cylinder: overflow");

  const auto combo = PrimitiveError::invalid_attribute_combination;
  check(hasError(triangle({.uvs = false, .tangents = true}), combo) &&
            hasError(quad({.normals = false, .tangents = true}), combo) &&
            hasError(box({.normals = false, .uvs = false, .tangents = true}), combo) &&
            hasError(plane({.uvs = false, .tangents = true}), combo) &&
            hasError(uvSphere({.normals = false, .tangents = true}), combo) &&
            hasError(cylinder({.uvs = false, .tangents = true}), combo),
        "tangents need normals and uvs");
  check(!hasError(quad({.tangents = true}), combo), "tangents with normals and uvs are fine");

  const auto error = triangle({.width = 0.0F});
  check(!error && !error.error().context().empty() && !error.error().detail().empty() &&
            !make_error_code(PrimitiveError::invalid_size).message().empty(),
        "errors name the operation and the detail");
}

void determinism() {
  check(sameMesh(terreate::unwrap(uvSphere({.tangents = true})),
                 terreate::unwrap(uvSphere({.tangents = true}))),
        "sphere is deterministic");
  check(sameMesh(terreate::unwrap(cylinder({.radiusTop = 0.0F, .tangents = true})),
                 terreate::unwrap(cylinder({.radiusTop = 0.0F, .tangents = true}))),
        "cylinder is deterministic");
  check(sameMesh(terreate::unwrap(box()), terreate::unwrap(box())), "box is deterministic");
  check(sameMesh(terreate::unwrap(plane({.segmentsX = 3, .segmentsZ = 2})),
                 terreate::unwrap(plane({.segmentsX = 3, .segmentsZ = 2}))),
        "plane is deterministic");
  check(!sameMesh(terreate::unwrap(box({.width = 1.0F})), terreate::unwrap(box({.width = 2.0F}))),
        "different descriptions give different meshes");
}

} // namespace

int main() noexcept {
  withAllAttributes();
  faceConventions();
  attributeSelection();
  indexTypeRule();
  everyError();
  determinism();
  if (failures != 0) {
    std::fprintf(stderr, "%s: assertion=FAIL: %d check(s) failed\n", test_name, failures);
    return EXIT_FAILURE;
  }
  std::printf("%s: primitives=PASS\n", test_name);
  return EXIT_SUCCESS;
}

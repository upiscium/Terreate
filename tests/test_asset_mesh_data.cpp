// #345: the CPU-only MeshData contract. This test links Terreate::Asset alone;
// it needs no Vulkan, no Graphics, no Platform, no display and no OS- or
// linker-specific mechanism.
#include <terreate/asset/mesh_data.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace terreate::asset;

const char *test_name = "asset.mesh_data";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

template <typename T> bool hasError(const terreate::Result<T> &result, MeshDataError error) {
  return !result && result.error().code() == make_error_code(error);
}

// The consumer must see no GPU or windowing type through the Asset headers.
#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR)
#error "Asset public headers must not pull in Vulkan or GLFW"
#endif
static_assert(std::is_copy_constructible_v<MeshData> &&
              std::is_nothrow_move_constructible_v<MeshData>);
static_assert(std::is_error_code_enum_v<MeshDataError>);

AttributeKey key(AttributeSemantic semantic, std::uint32_t set = 0, std::string name = {}) {
  return AttributeKey{semantic, set, std::move(name)};
}

AttributeStream stream(AttributeKey k, AttributeFormat format, std::vector<float> values) {
  return terreate::unwrap(makeAttributeStream(std::move(k), format, values));
}

// A valid indexed quad: 4 vertices, 2 triangles, with normals and two UV sets.
MeshData quad() {
  MeshData mesh;
  mesh.topology = PrimitiveTopology::triangle_list;
  mesh.attributes.push_back(stream(key(AttributeSemantic::position), AttributeFormat::float32x3,
                                   {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0}));
  mesh.attributes.push_back(stream(key(AttributeSemantic::normal), AttributeFormat::float32x3,
                                   {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}));
  mesh.attributes.push_back(stream(key(AttributeSemantic::texcoord, 0), AttributeFormat::float32x2,
                                   {0, 1, 1, 1, 1, 0, 0, 0}));
  mesh.attributes.push_back(stream(key(AttributeSemantic::texcoord, 1), AttributeFormat::float32x2,
                                   {0, 0, 1, 0, 1, 1, 0, 1}));
  mesh.indices = std::vector<std::uint16_t>{0, 1, 2, 0, 2, 3};
  return mesh;
}

void accessors() {
  const MeshData mesh = quad();
  check(mesh.vertexCount() == 4, "vertexCount is the position count");
  check(mesh.indexType() == IndexType::uint16, "indexType uint16");
  check(mesh.indexCount() == 6, "indexCount");
  check(mesh.index(4) == 2U && !mesh.index(6), "index widens and is range checked");
  check(MeshData{}.vertexCount() == 0 && MeshData{}.indexType() == IndexType::none &&
            MeshData{}.indexCount() == 0 && !MeshData{}.index(0),
        "an empty mesh has no vertices or indices");

  const AttributeStream *uv1 = mesh.find(key(AttributeSemantic::texcoord, 1));
  check(uv1 != nullptr && uv1->components() == 2 && uv1->elementSize() == 8, "find by set");
  check(mesh.find(key(AttributeSemantic::texcoord, 2)) == nullptr, "missing set not found");
  check(mesh.find(key(AttributeSemantic::custom, 0, "x")) == nullptr, "missing custom not found");
  check(mesh.positions() != nullptr, "positions()");

  const auto p2 = mesh.positions()->get(2);
  check(p2 && (*p2)[0] == 1.0F && (*p2)[1] == 1.0F && (*p2)[2] == 0.0F && (*p2)[3] == 0.0F,
        "get pads missing components with 0");
  check(!mesh.positions()->get(4), "get out of range is nullopt");
  const auto floats = mesh.positions()->toFloats();
  check(floats && floats->size() == 12 && (*floats)[3] == 1.0F, "toFloats");

  AttributeStream broken = *mesh.positions();
  broken.data.pop_back();
  check(!broken.sizeConsistent() && hasError(broken.toFloats(), MeshDataError::data_size_mismatch),
        "toFloats rejects an inconsistent stream");
  check(!broken.get(3), "get does not read a truncated element");

  const std::vector<float> odd{1, 2, 3, 4};
  check(hasError(
            makeAttributeStream(key(AttributeSemantic::position), AttributeFormat::float32x3, odd),
            MeshDataError::data_size_mismatch),
        "makeAttributeStream rejects partial elements");

  check(componentCount(AttributeFormat::float32x4) == 4 &&
            byteSize(AttributeFormat::float32x2) == 8,
        "format sizes");
  check(byteSize(IndexType::none) == 0 && byteSize(IndexType::uint16) == 2 &&
            byteSize(IndexType::uint32) == 4,
        "index sizes");
}

void validMeshes() {
  check(validate(quad()).has_value(), "the quad is valid");

  MeshData wide = quad();
  wide.indices = std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3};
  check(validate(wide).has_value() && wide.indexType() == IndexType::uint32, "uint32 indices");

  MeshData unindexed = quad();
  unindexed.indices = std::monostate{};
  unindexed.attributes.resize(1);
  unindexed.attributes[0] = stream(key(AttributeSemantic::position), AttributeFormat::float32x3,
                                   {0, 0, 0, 1, 0, 0, 0, 1, 0});
  check(validate(unindexed).has_value(), "non-indexed triangle list of 3 vertices");

  MeshData custom = quad();
  custom.attributes.push_back(stream(key(AttributeSemantic::custom, 0, "_weights"),
                                     AttributeFormat::float32x1, {1, 2, 3, 4}));
  custom.attributes.push_back(stream(key(AttributeSemantic::color), AttributeFormat::float32x4,
                                     {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}));
  custom.attributes.push_back(stream(key(AttributeSemantic::tangent), AttributeFormat::float32x4,
                                     {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1}));
  check(validate(custom).has_value(), "custom, color x4 and tangent are valid");

  MeshData strip = quad();
  strip.topology = PrimitiveTopology::triangle_strip;
  strip.indices = std::vector<std::uint16_t>{0, 1, 2, 3};
  check(validate(strip).has_value(), "a strip of 4 indices");
  strip.indices = std::monostate{};
  check(validate(strip).has_value(), "a non-indexed strip of 4 vertices");

  MeshData points = quad();
  points.topology = PrimitiveTopology::point_list;
  points.indices = std::monostate{};
  check(validate(points).has_value(), "a point list of any count");
}

void everyError() {
  MeshData noPosition = quad();
  noPosition.attributes.erase(noPosition.attributes.begin());
  check(hasError(validate(noPosition), MeshDataError::missing_position), "missing_position");
  check(hasError(validate(MeshData{}), MeshDataError::missing_position),
        "an empty mesh has no position");

  MeshData countMismatch = quad();
  countMismatch.attributes[1] = stream(key(AttributeSemantic::normal), AttributeFormat::float32x3,
                                       {0, 0, 1, 0, 0, 1, 0, 0, 1});
  check(hasError(validate(countMismatch), MeshDataError::count_mismatch), "count_mismatch");

  MeshData badFormat = quad();
  badFormat.attributes[0] = stream(key(AttributeSemantic::position), AttributeFormat::float32x2,
                                   {0, 0, 1, 0, 1, 1, 0, 1});
  check(hasError(validate(badFormat), MeshDataError::format_not_allowed),
        "format_not_allowed (position x2)");
  MeshData badUv = quad();
  badUv.attributes[2] = stream(key(AttributeSemantic::texcoord), AttributeFormat::float32x3,
                               {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0});
  check(hasError(validate(badUv), MeshDataError::format_not_allowed),
        "format_not_allowed (texcoord x3)");

  MeshData outOfRange = quad();
  outOfRange.indices = std::vector<std::uint16_t>{0, 1, 2, 0, 2, 4};
  check(hasError(validate(outOfRange), MeshDataError::index_out_of_range),
        "index_out_of_range (uint16)");
  outOfRange.indices = std::vector<std::uint32_t>{0, 1, 2, 0, 2, 0xFFFFFFFFU};
  check(hasError(validate(outOfRange), MeshDataError::index_out_of_range),
        "index_out_of_range (uint32)");

  MeshData badIndexCount = quad();
  badIndexCount.indices = std::vector<std::uint16_t>{0, 1, 2, 3};
  check(hasError(validate(badIndexCount), MeshDataError::invalid_index_count),
        "invalid_index_count (triangle list)");
  badIndexCount.topology = PrimitiveTopology::line_list;
  badIndexCount.indices = std::vector<std::uint16_t>{0, 1, 2};
  check(hasError(validate(badIndexCount), MeshDataError::invalid_index_count),
        "invalid_index_count (line list)");
  badIndexCount.topology = PrimitiveTopology::triangle_strip;
  badIndexCount.indices = std::vector<std::uint16_t>{0, 1};
  check(hasError(validate(badIndexCount), MeshDataError::invalid_index_count),
        "invalid_index_count (strip of 2)");
  badIndexCount.topology = PrimitiveTopology::triangle_list;
  badIndexCount.indices = std::monostate{};
  check(hasError(validate(badIndexCount), MeshDataError::invalid_index_count),
        "invalid_index_count (4 vertices, no indices, triangle list)");

  MeshData duplicate = quad();
  duplicate.attributes.push_back(duplicate.attributes[2]);
  check(hasError(validate(duplicate), MeshDataError::duplicate_attribute), "duplicate_attribute");

  MeshData bounded = quad();
  bounded.bounds = Bounds{{0, 0, 0}, {1, 1, 0}};
  check(validate(bounded).has_value(), "enclosing bounds are valid");
  bounded.bounds = Bounds{{0, 0, 0}, {0.5F, 1, 0}};
  check(hasError(validate(bounded), MeshDataError::bounds_not_enclosing), "bounds_not_enclosing");
  MeshData nan = quad();
  nan.attributes[0].data.assign(nan.attributes[0].data.size(), std::byte{0xFF}); // NaN floats
  nan.bounds = Bounds{{-1, -1, -1}, {1, 1, 1}};
  check(hasError(validate(nan), MeshDataError::bounds_not_enclosing),
        "a NaN position is not enclosed");

  MeshData size = quad();
  size.attributes[1].data.pop_back();
  check(hasError(validate(size), MeshDataError::data_size_mismatch), "data_size_mismatch");
  MeshData countOnly = quad();
  countOnly.attributes[1].count = 5;
  check(hasError(validate(countOnly), MeshDataError::data_size_mismatch),
        "data_size_mismatch is reported before count_mismatch");

  MeshData badKey = quad();
  badKey.attributes[1].key.set = 1;
  check(hasError(validate(badKey), MeshDataError::invalid_attribute_key),
        "invalid_attribute_key (normal set 1)");
  MeshData badName = quad();
  badName.attributes.push_back(
      stream(key(AttributeSemantic::custom), AttributeFormat::float32x1, {1, 2, 3, 4}));
  check(hasError(validate(badName), MeshDataError::invalid_attribute_key),
        "invalid_attribute_key (custom without name)");
  MeshData namedPosition = quad();
  namedPosition.attributes[2].key.name = "x";
  check(hasError(validate(namedPosition), MeshDataError::invalid_attribute_key),
        "invalid_attribute_key (named texcoord)");

  MeshData badEnum = quad();
  badEnum.attributes.push_back(
      stream(key(AttributeSemantic::custom, 0, "x"), AttributeFormat::float32x1, {1, 2, 3, 4}));
  badEnum.attributes.back().format = static_cast<AttributeFormat>(9);
  check(hasError(validate(badEnum), MeshDataError::format_not_allowed),
        "an out-of-range format value is rejected, not divided by");
  check(!badEnum.attributes.back().get(0) && !badEnum.attributes.back().sizeConsistent(),
        "an out-of-range format reads nothing");

  const auto error = validate(noPosition).error();
  check(std::string{error.code().category().name()} == "terreate.asset.mesh_data" &&
            !error.code().message().empty() && !error.context().empty(),
        "the error carries the MeshData category, a message and a context");
}

void bounds() {
  MeshData mesh = quad();
  check(!mesh.bounds, "bounds are absent unless set");
  const auto computed = computeBounds(mesh);
  check(computed && computed->min == std::array<float, 3>{0, 0, 0} &&
            computed->max == std::array<float, 3>{1, 1, 0},
        "computeBounds is the tight box");
  mesh.bounds = *computed;
  check(validate(mesh).has_value(), "computed bounds enclose the positions");

  mesh.attributes[0] =
      stream(key(AttributeSemantic::position), AttributeFormat::float32x3, {-2, 5, 1, 3, -1, 0});
  const auto negative = computeBounds(mesh);
  check(negative && negative->min == std::array<float, 3>{-2, -1, 0} &&
            negative->max == std::array<float, 3>{3, 5, 1},
        "computeBounds handles negative and mixed values");

  check(hasError(computeBounds(MeshData{}), MeshDataError::missing_position),
        "computeBounds without positions");
  MeshData empty;
  empty.attributes.push_back(
      stream(key(AttributeSemantic::position), AttributeFormat::float32x3, {}));
  check(hasError(computeBounds(empty), MeshDataError::missing_position),
        "computeBounds of an empty position stream");
  MeshData infinite = quad();
  infinite.attributes[0] = stream(key(AttributeSemantic::position), AttributeFormat::float32x3,
                                  {0, 0, std::numeric_limits<float>::infinity()});
  check(hasError(computeBounds(infinite), MeshDataError::non_finite_position),
        "computeBounds rejects infinity");
  MeshData wrongFormat = quad();
  wrongFormat.attributes[0].format = AttributeFormat::float32x2;
  check(hasError(computeBounds(wrongFormat), MeshDataError::format_not_allowed),
        "computeBounds rejects a wrong position format");
}

void layouts() {
  const MeshData mesh = quad();
  const MeshLayout layout = layoutOf(mesh);
  check(!layout.interleaved && layout.vertexCount == 4 && layout.indexCount == 6 &&
            layout.indexType == IndexType::uint16 &&
            layout.topology == PrimitiveTopology::triangle_list && layout.attributes.size() == 4,
        "layoutOf describes the mesh");
  check(layout.attributes[0].stride == 12 && layout.attributes[0].offset == 0 &&
            layout.attributes[2].stride == 8,
        "deinterleaved: offset 0, stride is the element size");
}

void interleaving() {
  const MeshData mesh = quad();
  const auto packed = interleave(mesh);
  check(packed.has_value(), "interleave of a valid mesh");
  if (!packed)
    return;
  // position(12) + normal(12) + uv0(8) + uv1(8)
  check(packed->layout.interleaved && packed->vertices.size() == std::size_t{160} &&
            packed->layout.attributes[1].offset == 12 &&
            packed->layout.attributes[2].offset == 24 &&
            packed->layout.attributes[3].offset == 32 && packed->layout.attributes[3].stride == 40,
        "interleave offsets and stride in stream order");
  check(std::holds_alternative<std::vector<std::uint16_t>>(packed->indices) &&
            packed->layout.indexCount == 6,
        "interleave keeps the indices");

  // Round trip: reading each attribute back from the packed bytes equals the source.
  for (std::size_t a = 0; a < mesh.attributes.size(); ++a) {
    const AttributeStream &source = mesh.attributes[a];
    const AttributeDescription &description = packed->layout.attributes[a];
    bool same = description.key == source.key && description.format == source.format;
    for (std::size_t v = 0; v < source.count && same; ++v)
      same = std::memcmp(packed->vertices.data() + v * description.stride + description.offset,
                         source.data.data() + v * source.elementSize(), source.elementSize()) == 0;
    check(same, "interleave round trip per attribute");
  }

  // An explicit order: reordered, and a dropped attribute is left out on purpose.
  const std::array<AttributeKey, 2> order{key(AttributeSemantic::texcoord, 1),
                                          key(AttributeSemantic::position)};
  const auto reordered = interleave(mesh, order);
  check(reordered && reordered->layout.attributes.size() == 2 &&
            reordered->layout.attributes[0].offset == 0 &&
            reordered->layout.attributes[1].offset == 8 &&
            reordered->layout.attributes[1].stride == 20 &&
            reordered->vertices.size() == std::size_t{80},
        "interleave honors the requested order");
  if (reordered) {
    float uv[2];
    float pos[3];
    std::memcpy(uv, reordered->vertices.data() + std::size_t{40}, sizeof uv);
    std::memcpy(pos, reordered->vertices.data() + std::size_t{48}, sizeof pos);
    check(uv[0] == 1.0F && uv[1] == 1.0F && pos[0] == 1.0F && pos[1] == 1.0F,
          "interleave order content");
  }

  const std::array<AttributeKey, 1> missing{key(AttributeSemantic::custom, 0, "nope")};
  check(hasError(interleave(mesh, missing), MeshDataError::attribute_not_found),
        "attribute_not_found");
  const std::array<AttributeKey, 2> repeated{key(AttributeSemantic::position),
                                             key(AttributeSemantic::position)};
  check(hasError(interleave(mesh, repeated), MeshDataError::duplicate_attribute),
        "a repeated order entry");

  MeshData invalid = quad();
  invalid.indices = std::vector<std::uint16_t>{0, 1, 9};
  check(hasError(interleave(invalid), MeshDataError::index_out_of_range),
        "interleave returns the validation error");
}

void copyIndependence() {
  const MeshData original = quad();
  MeshData copy = original;
  copy.attributes[0].data[0] = std::byte{0x7F};
  copy.attributes.pop_back();
  std::get<std::vector<std::uint16_t>>(copy.indices)[0] = 3;
  copy.bounds = Bounds{};
  check(original.attributes.size() == 4 && original.attributes[0].data[0] == std::byte{0} &&
            std::get<std::vector<std::uint16_t>>(original.indices)[0] == 0 && !original.bounds,
        "a copy owns its streams, indices and bounds");
  check(validate(original).has_value(), "the original stays valid after editing a copy");

  MeshData moved = quad();
  const MeshData taken = std::move(moved);
  check(validate(taken).has_value(), "a moved-to mesh is valid");
}

std::vector<std::uint32_t> indicesOf(const MeshData &mesh) {
  std::vector<std::uint32_t> out(mesh.indexCount());
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = mesh.index(i).value_or(0);
  return out;
}

// A mesh of `vertices` vertices on a line, with the topology and (when given) the indices.
MeshData shape(PrimitiveTopology topology, std::size_t vertices,
               std::vector<std::uint16_t> indices = {}, bool indexed = true) {
  MeshData mesh;
  mesh.topology = topology;
  std::vector<float> positions;
  for (std::size_t i = 0; i < vertices; ++i) {
    positions.push_back(static_cast<float>(i));
    positions.push_back(0.0F);
    positions.push_back(0.0F);
  }
  mesh.attributes.push_back(
      stream(key(AttributeSemantic::position), AttributeFormat::float32x3, positions));
  if (indexed)
    mesh.indices = std::move(indices);
  return mesh;
}

using Indices = std::vector<std::uint32_t>;

// A value that is no enumerator, made through its bytes.
PrimitiveTopology forgedTopology(std::uint8_t value) {
  PrimitiveTopology topology{};
  std::memcpy(&topology, &value, sizeof topology);
  return topology;
}

void newTopologies() {
  check(static_cast<int>(PrimitiveTopology::triangle_list) == 0 &&
            static_cast<int>(PrimitiveTopology::triangle_strip) == 1 &&
            static_cast<int>(PrimitiveTopology::line_list) == 2 &&
            static_cast<int>(PrimitiveTopology::point_list) == 3 &&
            static_cast<int>(PrimitiveTopology::line_strip) == 4 &&
            static_cast<int>(PrimitiveTopology::triangle_fan) == 5,
        "the earlier topology values are stable, the new ones are appended");

  check(validate(shape(PrimitiveTopology::triangle_fan, 4, {0, 1, 2, 3})).has_value(),
        "a fan of 4 indices");
  check(validate(shape(PrimitiveTopology::triangle_fan, 3, {}, false)).has_value(),
        "a non-indexed fan of 3 vertices");
  check(validate(shape(PrimitiveTopology::triangle_fan, 3, {})).has_value(),
        "a fan with zero indices");
  check(hasError(validate(shape(PrimitiveTopology::triangle_fan, 4, {0, 1})),
                 MeshDataError::invalid_index_count),
        "a fan of 2 indices is invalid");
  check(hasError(validate(shape(PrimitiveTopology::triangle_fan, 2, {}, false)),
                 MeshDataError::invalid_index_count),
        "a non-indexed fan of 2 vertices is invalid");
  check(validate(shape(PrimitiveTopology::line_strip, 2, {0, 1})).has_value(),
        "a line strip of 2 indices");
  check(validate(shape(PrimitiveTopology::line_strip, 5, {}, false)).has_value(),
        "a non-indexed line strip");
  check(validate(shape(PrimitiveTopology::line_strip, 2, {})).has_value(),
        "a line strip with zero indices");
  check(hasError(validate(shape(PrimitiveTopology::line_strip, 2, {0})),
                 MeshDataError::invalid_index_count),
        "a line strip of 1 index is invalid");
  check(hasError(validate(shape(PrimitiveTopology::line_strip, 1, {}, false)),
                 MeshDataError::invalid_index_count),
        "a non-indexed line strip of 1 vertex is invalid");
  check(hasError(validate(shape(PrimitiveTopology::line_strip, 2, {0, 5})),
                 MeshDataError::index_out_of_range),
        "indices of a strip are range checked");
}

void convertTopologyRules() {
  // A strip of 5 vertices: triangles (0,1,2), (2,1,3) [odd: swapped], (2,3,4).
  const auto strip5 = convertTopology(shape(PrimitiveTopology::triangle_strip, 5, {}, false),
                                      PrimitiveTopology::triangle_list);
  check(strip5.has_value() && strip5->topology == PrimitiveTopology::triangle_list &&
            strip5->indexType() == IndexType::uint16 &&
            indicesOf(*strip5) == Indices{0, 1, 2, 2, 1, 3, 2, 3, 4} &&
            validate(*strip5).has_value(),
        "a non-indexed strip becomes the alternating-winding list (uint16)");

  const auto indexedStrip = convertTopology(
      shape(PrimitiveTopology::triangle_strip, 6, {5, 4, 3, 2}), PrimitiveTopology::triangle_list);
  check(indexedStrip && indicesOf(*indexedStrip) == Indices{5, 4, 3, 3, 4, 2} &&
            validate(*indexedStrip).has_value(),
        "an indexed strip uses its indices in strip order");

  // Degenerate joins are dropped and the parity counts input positions. Strip 0 1 2 2 3 4:
  // k0 (0,1,2) kept; k1 (2,1,2) and k2 (2,2,3) are degenerate; k3 is odd: (e4,e3,e5) = (3,2,4).
  const auto joined =
      convertTopology(shape(PrimitiveTopology::triangle_strip, 5, {0, 1, 2, 2, 3, 4}),
                      PrimitiveTopology::triangle_list);
  check(joined && indicesOf(*joined) == Indices{0, 1, 2, 3, 2, 4} && validate(*joined).has_value(),
        "degenerate triangles of a strip are dropped, the winding parity is kept");

  const auto onlyDegenerate = convertTopology(
      shape(PrimitiveTopology::triangle_strip, 2, {0, 0, 0, 1}), PrimitiveTopology::triangle_list);
  check(onlyDegenerate && onlyDegenerate->indexCount() == 0 &&
            onlyDegenerate->indexType() == IndexType::uint16 &&
            validate(*onlyDegenerate).has_value(),
        "a strip with only degenerate triangles gives an indexed list of zero indices");

  const auto shortStrip = convertTopology(shape(PrimitiveTopology::triangle_strip, 2, {}),
                                          PrimitiveTopology::triangle_list);
  check(shortStrip && shortStrip->indexCount() == 0, "an empty strip gives an empty list");

  // A fan of a quad: (0,1,2), (0,2,3).
  const auto fan = convertTopology(shape(PrimitiveTopology::triangle_fan, 4, {}, false),
                                   PrimitiveTopology::triangle_list);
  check(fan && indicesOf(*fan) == Indices{0, 1, 2, 0, 2, 3} && validate(*fan).has_value(),
        "a non-indexed fan keeps its winding");
  const auto indexedFan = convertTopology(
      shape(PrimitiveTopology::triangle_fan, 5, {4, 3, 2, 1, 0}), PrimitiveTopology::triangle_list);
  check(indexedFan && indicesOf(*indexedFan) == Indices{4, 3, 2, 4, 2, 1, 4, 1, 0},
        "an indexed fan");
  const auto fanDegenerate = convertTopology(
      shape(PrimitiveTopology::triangle_fan, 4, {0, 1, 1, 2, 3}), PrimitiveTopology::triangle_list);
  check(fanDegenerate && indicesOf(*fanDegenerate) == Indices{0, 1, 2, 0, 2, 3}, "a fan drops "
                                                                                 "degenerate "
                                                                                 "triangles");

  // Lines.
  const auto lines = convertTopology(shape(PrimitiveTopology::line_strip, 4, {}, false),
                                     PrimitiveTopology::line_list);
  check(lines && lines->topology == PrimitiveTopology::line_list &&
            indicesOf(*lines) == Indices{0, 1, 1, 2, 2, 3} && validate(*lines).has_value(),
        "a non-indexed line strip becomes a line list");
  const auto closed = convertTopology(shape(PrimitiveTopology::line_strip, 3, {0, 1, 2, 0}),
                                      PrimitiveTopology::line_list);
  check(closed && indicesOf(*closed) == Indices{0, 1, 1, 2, 2, 0}, "a closed strip");
  const auto repeated = convertTopology(shape(PrimitiveTopology::line_strip, 3, {0, 0, 1}),
                                        PrimitiveTopology::line_list);
  check(repeated && indicesOf(*repeated) == Indices{0, 1}, "a zero-length segment is dropped");

  // Index type: 32-bit stays 32-bit; many non-indexed vertices need 32-bit.
  MeshData wide = shape(PrimitiveTopology::triangle_fan, 4);
  wide.indices = std::vector<std::uint32_t>{0, 1, 2, 3};
  const auto wideOut = convertTopology(wide, PrimitiveTopology::triangle_list);
  check(wideOut && wideOut->indexType() == IndexType::uint32, "a uint32 input stays uint32");
  const auto many = convertTopology(shape(PrimitiveTopology::triangle_fan, 70000, {}, false),
                                    PrimitiveTopology::triangle_list);
  check(many && many->indexType() == IndexType::uint32 &&
            many->indexCount() == std::size_t{3} * 69998 && validate(*many).has_value(),
        "a non-indexed fan over 65535 vertices gets uint32 indices");

  // Same topology: a copy, vertex data and bounds untouched.
  MeshData withBounds = quad();
  withBounds.bounds = computeBounds(withBounds).value();
  const auto same = convertTopology(withBounds, PrimitiveTopology::triangle_list);
  check(same && same->indices == withBounds.indices && same->bounds == withBounds.bounds &&
            same->attributes.size() == 4,
        "the same topology returns a copy");
  MeshData stripWithBounds = shape(PrimitiveTopology::triangle_strip, 4, {}, false);
  stripWithBounds.bounds = computeBounds(stripWithBounds).value();
  const auto kept = convertTopology(stripWithBounds, PrimitiveTopology::triangle_list);
  check(kept && kept->bounds == stripWithBounds.bounds &&
            kept->attributes[0].data == stripWithBounds.attributes[0].data,
        "the vertex streams and bounds are copied unchanged");

  // Determinism and non-mutation.
  const MeshData input = shape(PrimitiveTopology::triangle_strip, 6, {}, false);
  const auto a = convertTopology(input, PrimitiveTopology::triangle_list);
  const auto b = convertTopology(input, PrimitiveTopology::triangle_list);
  check(a && b && a->indices == b->indices && input.indexType() == IndexType::none,
        "the conversion is deterministic and leaves the input alone");
}

void convertTopologyErrors() {
  using T = PrimitiveTopology;
  const auto refused = [](T from, T to) {
    const MeshData mesh = shape(from, 6, {}, false);
    return hasError(convertTopology(mesh, to), MeshDataError::unsupported_conversion);
  };
  check(refused(T::triangle_list, T::triangle_strip), "a list to a strip is refused");
  check(refused(T::triangle_list, T::triangle_fan), "a list to a fan is refused");
  check(refused(T::triangle_strip, T::triangle_fan), "a strip to a fan is refused");
  check(refused(T::triangle_strip, T::line_list), "triangles to lines are refused");
  check(refused(T::triangle_fan, T::triangle_strip), "a fan to a strip is refused");
  check(refused(T::triangle_fan, T::line_strip), "a fan to a line strip is refused");
  check(refused(T::line_list, T::line_strip), "a line list to a strip is refused");
  check(refused(T::line_strip, T::triangle_list), "lines to triangles are refused");
  check(refused(T::line_strip, T::point_list), "lines to points are refused");
  check(refused(T::point_list, T::line_list), "points to lines are refused");
  check(refused(T::triangle_strip, forgedTopology(200)), "a forged target is refused");
  check(hasError(convertTopology(shape(T::triangle_fan, 4, {0, 1}), T::triangle_list),
                 MeshDataError::invalid_index_count),
        "an invalid mesh returns the validate error");
  check(hasError(convertTopology(MeshData{}, T::triangle_list), MeshDataError::missing_position),
        "an empty mesh returns the validate error");
  const auto copy = convertTopology(shape(T::triangle_list, 3, {0, 1, 2}), T::triangle_list);
  check(copy && indicesOf(*copy) == Indices{0, 1, 2}, "a list to a list is a copy");
}

} // namespace

int main() noexcept {
  accessors();
  validMeshes();
  everyError();
  bounds();
  layouts();
  interleaving();
  copyIndependence();
  newTopologies();
  convertTopologyRules();
  convertTopologyErrors();
  if (failures != 0) {
    std::fprintf(stderr, "%s: assertion=FAIL: %d check(s) failed\n", test_name, failures);
    return EXIT_FAILURE;
  }
  std::printf("%s: mesh-data=PASS\n", test_name);
  return EXIT_SUCCESS;
}

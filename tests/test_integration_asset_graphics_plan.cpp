// #279 part 1: planMeshUpload is CPU only. It builds the observable plan from a
// MeshData and an upload description: the interleaved bytes, the native vertex
// input structs, the buffer descriptions, the unmapped attributes and its named
// errors. No Vulkan Instance or device is created; no OS- or linker-specific
// mechanism is used.
#include <terreate/integration/asset_graphics/mesh_upload.hpp>

#include <terreate/asset/primitives.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace asset = terreate::asset;
namespace graphics = terreate::graphics;
namespace ag = terreate::integration::asset_graphics;

const char *test_name = "integration.asset_graphics.plan";
int failures = 0;

bool check(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message.c_str());
  }
  return condition;
}

static_assert(std::is_error_code_enum_v<ag::MeshUploadError>);
static_assert(!std::is_copy_constructible_v<ag::MeshBuffers>);
static_assert(std::is_move_constructible_v<ag::MeshBuffers>);

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
  return !result && result.error().code() == make_error_code(error);
}

const asset::AttributeKey position{asset::AttributeSemantic::position, 0, {}};
const asset::AttributeKey normal{asset::AttributeSemantic::normal, 0, {}};
const asset::AttributeKey tangent{asset::AttributeSemantic::tangent, 0, {}};
const asset::AttributeKey texcoord{asset::AttributeSemantic::texcoord, 0, {}};

ag::MeshUploadDescription
describe(std::initializer_list<std::pair<asset::AttributeKey, std::uint32_t>> bindings) {
  ag::MeshUploadDescription description;
  for (const auto &[key, location] : bindings)
    description.attributes.push_back({key, location});
  return description;
}

std::vector<std::byte> bytesOf(const std::vector<std::uint16_t> &values) {
  std::vector<std::byte> bytes(values.size() * sizeof(std::uint16_t));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}
std::vector<std::byte> bytesOf(const std::vector<std::uint32_t> &values) {
  std::vector<std::byte> bytes(values.size() * sizeof(std::uint32_t));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

asset::MeshData
make(std::initializer_list<std::pair<asset::AttributeKey, asset::AttributeFormat>> streams,
     std::size_t vertices, asset::PrimitiveTopology topology) {
  asset::MeshData mesh;
  mesh.topology = topology;
  for (const auto &[key, format] : streams) {
    const std::vector<float> values(vertices * asset::componentCount(format), 0.5F);
    mesh.attributes.push_back(terreate::unwrap(asset::makeAttributeStream(key, format, values)));
  }
  return mesh;
}

void boxPlan() {
  const auto box = terreate::unwrap(asset::box({}));
  const auto description = describe({{position, 0}, {normal, 1}, {texcoord, 2}});
  const auto result = ag::planMeshUpload(box, description);
  if (!check(bool(result), "a generated box plans"))
    return;
  const auto &plan = *result;

  const std::array<asset::AttributeKey, 3> order{position, normal, texcoord};
  const auto packed = terreate::unwrap(asset::interleave(box, order));
  check(plan.vertex_bytes == packed.vertices, "vertex bytes equal asset::interleave");
  check(std::get<std::vector<std::uint16_t>>(box.indices).size() == 36 &&
            plan.index_bytes == bytesOf(std::get<std::vector<std::uint16_t>>(box.indices)),
        "index bytes are the mesh's uint16 indices");
  check(plan.binding.binding == 0 && plan.binding.stride == 32 &&
            plan.binding.inputRate == vk::VertexInputRate::eVertex,
        "binding 0, stride 3+3+2 floats, per-vertex rate");
  if (check(plan.attributes.size() == 3, "three attribute descriptions")) {
    const auto at = [&](std::size_t i, std::uint32_t location, vk::Format format,
                        std::uint32_t offset) {
      return plan.attributes[i].location == location && plan.attributes[i].binding == 0 &&
             plan.attributes[i].format == format && plan.attributes[i].offset == offset;
    };
    check(at(0, 0, vk::Format::eR32G32B32Sfloat, 0), "position: location 0, RGB32, offset 0");
    check(at(1, 1, vk::Format::eR32G32B32Sfloat, 12), "normal: location 1, RGB32, offset 12");
    check(at(2, 2, vk::Format::eR32G32Sfloat, 24), "texcoord: location 2, RG32, offset 24");
  }
  check(plan.topology == vk::PrimitiveTopology::eTriangleList, "triangle list");
  check(plan.vertex_count == 24 && plan.index_count == 36 && plan.indexed() &&
            plan.index_type == vk::IndexType::eUint16,
        "24 vertices, 36 uint16 indices");
  check(plan.vertex_buffer.size == std::size_t{24} * 32 &&
            plan.vertex_bytes.size() == std::size_t{24} * 32,
        "vertex buffer size is the byte count");
  check(plan.vertex_buffer.usage ==
            (vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer),
        "vertex usage is TransferDst + Vertex");
  check(plan.index_buffer && plan.index_buffer->size == 72 &&
            plan.index_buffer->usage ==
                (vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer),
        "index buffer is 72 bytes, TransferDst + Index");
  const auto device_local =
      graphics::bufferAllocationFor(graphics::BufferMemoryIntent::device_local);
  const auto same = [&](const graphics::BufferAllocationDescription &a) {
    return a.usage == device_local.usage && a.flags == device_local.flags &&
           a.required_flags == device_local.required_flags &&
           a.preferred_flags == device_local.preferred_flags &&
           a.memory_type_bits == device_local.memory_type_bits;
  };
  check(same(plan.vertex_buffer.allocation) && plan.index_buffer &&
            same(plan.index_buffer->allocation),
        "memory defaults to the device_local intent");
  check(plan.unmapped.empty(), "every box attribute is bound, nothing unmapped");

  // applyTo fills the native fields and only those.
  graphics::GraphicsPipelineDescription pipeline;
  pipeline.vertex_entry_point = "main";
  pipeline.cull_mode = vk::CullModeFlagBits::eBack;
  pipeline.topology = vk::PrimitiveTopology::ePointList;
  pipeline.vertex_bindings = {
      vk::VertexInputBindingDescription{7, 4, vk::VertexInputRate::eInstance},
      vk::VertexInputBindingDescription{8, 4, vk::VertexInputRate::eInstance}};
  pipeline.vertex_attributes = {
      vk::VertexInputAttributeDescription{9, 7, vk::Format::eR32Sfloat, 0}};
  plan.applyTo(pipeline);
  check(pipeline.vertex_bindings.size() == 1 && pipeline.vertex_bindings[0] == plan.binding &&
            pipeline.vertex_attributes == plan.attributes &&
            pipeline.topology == vk::PrimitiveTopology::eTriangleList,
        "applyTo replaces bindings and attributes and sets the topology");
  check(pipeline.vertex_entry_point == "main" && pipeline.cull_mode == vk::CullModeFlagBits::eBack,
        "applyTo leaves the other pipeline members alone");
}

void unmappedAndBinding() {
  const auto box = terreate::unwrap(asset::box({}));
  auto description = describe({{texcoord, 5}, {position, 2}});
  description.binding = 3;
  const auto result = ag::planMeshUpload(box, description);
  if (!check(bool(result), "a partial binding plans"))
    return;
  const auto &plan = *result;
  check(plan.unmapped.size() == 1 && plan.unmapped[0] == normal,
        "the unbound normal is reported as unmapped, not dropped silently");
  check(plan.binding.binding == 3 && plan.binding.stride == 20 && plan.attributes.size() == 2 &&
            plan.attributes[0].location == 5 && plan.attributes[0].binding == 3 &&
            plan.attributes[0].offset == 0 &&
            plan.attributes[0].format == vk::Format::eR32G32Sfloat &&
            plan.attributes[1].location == 2 && plan.attributes[1].offset == 8,
        "the order of the description decides the offsets; locations are the application's");
  const std::array<asset::AttributeKey, 2> order{texcoord, position};
  check(plan.vertex_bytes == terreate::unwrap(asset::interleave(box, order)).vertices,
        "the unbound stream is not in the bytes");
}

void spherePlan() {
  const auto sphere = terreate::unwrap(asset::uvSphere({.tangents = true}));
  const auto result = ag::planMeshUpload(
      sphere, describe({{position, 0}, {normal, 1}, {texcoord, 2}, {tangent, 3}}));
  if (!check(bool(result), "a generated sphere with tangents plans"))
    return;
  const auto &plan = *result;
  const std::array<asset::AttributeKey, 4> order{position, normal, texcoord, tangent};
  check(plan.vertex_bytes == terreate::unwrap(asset::interleave(sphere, order)).vertices,
        "sphere bytes equal asset::interleave");
  check(plan.binding.stride == 48 && plan.vertex_count == 561 && plan.index_count == 2880 &&
            plan.index_type == vk::IndexType::eUint16 && plan.unmapped.empty(),
        "stride 12+12+8+16, 561 vertices, 2880 uint16 indices");
  check(plan.attributes.size() == 4 &&
            plan.attributes[3].format == vk::Format::eR32G32B32A32Sfloat &&
            plan.attributes[3].offset == 32,
        "tangent is RGBA32 at offset 32");
  check(plan.index_buffer && plan.index_buffer->size == std::size_t{2880} * 2,
        "index buffer is 5760 bytes");
}

void wideIndices() {
  // 257 x 257 vertices exceed 65536, so the generator picks uint32 indices.
  const auto plane = terreate::unwrap(asset::plane({.segmentsX = 256, .segmentsZ = 256}));
  const auto result = ag::planMeshUpload(plane, describe({{position, 0}}));
  if (!check(bool(result), "a large plane plans"))
    return;
  const auto &plan = *result;
  check(plan.index_type == vk::IndexType::eUint32 && plan.vertex_count == 257 * 257 &&
            plan.index_count == 6 * 256 * 256 &&
            plan.index_bytes == bytesOf(std::get<std::vector<std::uint32_t>>(plane.indices)) &&
            plan.index_buffer && plan.index_buffer->size == std::size_t{plan.index_count} * 4U,
        "uint32 indices are used as stored, never narrowed");
  check(plan.unmapped.size() == 2, "normal and texcoord of the plane are unmapped");
}

void formatsAndTopologies() {
  const asset::AttributeKey custom1{asset::AttributeSemantic::custom, 0, "weight"};
  const asset::AttributeKey custom2{asset::AttributeSemantic::custom, 0, "pair"};
  const asset::AttributeKey custom3{asset::AttributeSemantic::custom, 0, "triple"};
  const asset::AttributeKey custom4{asset::AttributeSemantic::custom, 0, "quad"};
  auto mesh = make({{position, asset::AttributeFormat::float32x3},
                    {custom1, asset::AttributeFormat::float32x1},
                    {custom2, asset::AttributeFormat::float32x2},
                    {custom3, asset::AttributeFormat::float32x3},
                    {custom4, asset::AttributeFormat::float32x4}},
                   4, asset::PrimitiveTopology::line_list);
  const auto result = ag::planMeshUpload(
      mesh, describe({{position, 0}, {custom1, 1}, {custom2, 2}, {custom3, 3}, {custom4, 4}}));
  if (!check(bool(result), "float32 x1..x4 plan"))
    return;
  const auto &plan = *result;
  const std::array<vk::Format, 5> formats{vk::Format::eR32G32B32Sfloat, vk::Format::eR32Sfloat,
                                          vk::Format::eR32G32Sfloat, vk::Format::eR32G32B32Sfloat,
                                          vk::Format::eR32G32B32A32Sfloat};
  bool all = plan.attributes.size() == formats.size();
  for (std::size_t i = 0; all && i < formats.size(); ++i)
    all = plan.attributes[i].format == formats[i];
  check(all, "float32 with 1 to 4 components map to R32..R32G32B32A32 Sfloat");
  check(plan.topology == vk::PrimitiveTopology::eLineList && !plan.indexed() &&
            plan.index_bytes.empty() && plan.index_count == 0 &&
            plan.index_type == vk::IndexType::eNoneKHR && !plan.index_buffer,
        "a non-indexed mesh has no index bytes, count or buffer");

  const struct {
    std::size_t vertices;
    vk::PrimitiveTopology expected;
    asset::PrimitiveTopology asset_topology;
  } topologies[] = {
      {3, vk::PrimitiveTopology::eTriangleList, asset::PrimitiveTopology::triangle_list},
      {4, vk::PrimitiveTopology::eTriangleStrip, asset::PrimitiveTopology::triangle_strip},
      {2, vk::PrimitiveTopology::eLineList, asset::PrimitiveTopology::line_list},
      {1, vk::PrimitiveTopology::ePointList, asset::PrimitiveTopology::point_list},
  };
  for (const auto &entry : topologies) {
    const auto point =
        make({{position, asset::AttributeFormat::float32x3}}, entry.vertices, entry.asset_topology);
    const auto planned = ag::planMeshUpload(point, describe({{position, 0}}));
    check(planned && planned->topology == entry.expected,
          "topology maps one to one: " + std::to_string(static_cast<int>(entry.asset_topology)));
  }
}

void extraUsageAndAllocation() {
  const auto box = terreate::unwrap(asset::box({}));
  auto description = describe({{position, 0}});
  description.extra_vertex_usage = vk::BufferUsageFlagBits::eTransferSrc;
  description.extra_index_usage = vk::BufferUsageFlagBits::eStorageBuffer;
  description.vertex_allocation =
      graphics::bufferAllocationFor(graphics::BufferMemoryIntent::host_sequential_write);
  const auto result = ag::planMeshUpload(box, description);
  if (!check(bool(result), "extra usage plans"))
    return;
  check(result->vertex_buffer.usage ==
            (vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer |
             vk::BufferUsageFlagBits::eTransferSrc),
        "the caller's vertex usage is added, the required usage stays");
  check(result->index_buffer &&
            result->index_buffer->usage ==
                (vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer |
                 vk::BufferUsageFlagBits::eStorageBuffer),
        "the caller's index usage is added, the required usage stays");
  check(result->vertex_buffer.allocation.flags ==
                graphics::bufferAllocationFor(graphics::BufferMemoryIntent::host_sequential_write)
                    .flags &&
            result->vertex_buffer.allocation.required_flags ==
                vk::MemoryPropertyFlagBits::eHostVisible,
        "the caller's allocation is used as given");
}

void errors() {
  const auto box = terreate::unwrap(asset::box({}));

  // An invalid mesh: its own error, unchanged.
  auto broken = box;
  std::get<std::vector<std::uint16_t>>(broken.indices)[0] = 9999;
  const auto invalid = ag::planMeshUpload(broken, describe({{position, 0}}));
  check(hasError(invalid, asset::MeshDataError::index_out_of_range) &&
            invalid.error().code().category() == asset::mesh_data_error_category(),
        "an invalid mesh returns its MeshDataError unchanged");

  check(hasError(ag::planMeshUpload(box, {}), ag::MeshUploadError::no_attributes),
        "an empty attribute list is no_attributes, not 'everything'");

  const asset::AttributeKey absent{asset::AttributeSemantic::color, 0, {}};
  const auto missing = ag::planMeshUpload(box, describe({{position, 0}, {absent, 1}}));
  check(hasError(missing, ag::MeshUploadError::attribute_not_found) &&
            missing.error().detail().find("color") != std::string::npos,
        "a binding for an attribute the mesh lacks is attribute_not_found, naming it");

  check(hasError(ag::planMeshUpload(box, describe({{position, 0}, {position, 1}})),
                 ag::MeshUploadError::duplicate_attribute),
        "binding one attribute twice is duplicate_attribute");
  check(hasError(ag::planMeshUpload(box, describe({{position, 4}, {normal, 4}})),
                 ag::MeshUploadError::duplicate_location),
        "two attributes at one location are duplicate_location");

  // A mesh without vertices.
  asset::MeshData empty;
  empty.attributes.push_back(terreate::unwrap(
      asset::makeAttributeStream(position, asset::AttributeFormat::float32x3, {})));
  const auto empty_result = ag::planMeshUpload(empty, describe({{position, 0}}));
  check(hasError(empty_result, ag::MeshUploadError::empty_mesh),
        "a mesh without vertices is refused, never turned into a zero-sized buffer");

  // An enumerator Asset does not define is refused by validate before any
  // mapping, so the Integration's unsupported_format/unsupported_topology are
  // reachable only when Asset adds formats or topologies this Integration lacks.
  auto forged_format = box;
  forged_format.attributes[0].format = static_cast<asset::AttributeFormat>(200);
  check(!ag::planMeshUpload(forged_format, describe({{position, 0}})),
        "a format outside the enumerators is refused");
  auto forged_topology = box;
  forged_topology.topology = static_cast<asset::PrimitiveTopology>(200);
  check(!ag::planMeshUpload(forged_topology, describe({{position, 0}})),
        "a topology outside the enumerators is refused");

  // The category is named and messages are defined for every value.
  const auto category_name = std::string{ag::mesh_upload_error_category().name()};
  check(category_name == "terreate.integration.asset_graphics", "the category name");
  for (int value = 1; value <= 10; ++value)
    check(!ag::mesh_upload_error_category().message(value).empty() &&
              ag::mesh_upload_error_category().message(value) != "unknown mesh upload error",
          "every MeshUploadError has a message");
}

} // namespace

int main() {
  boxPlan();
  unmappedAndBinding();
  spherePlan();
  wideIndices();
  formatsAndTopologies();
  extraUsageAndAllocation();
  errors();
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("integration.asset_graphics.plan: plan=PASS");
  return EXIT_SUCCESS;
}

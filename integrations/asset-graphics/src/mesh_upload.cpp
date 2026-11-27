#include <terreate/integration/asset_graphics/mesh_upload.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <variant>

namespace terreate::integration::asset_graphics {
namespace {

class MeshUploadErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.integration.asset_graphics"; }
  std::string message(int value) const override {
    switch (static_cast<MeshUploadError>(value)) {
    case MeshUploadError::no_attributes:
      return "the upload description binds no attribute";
    case MeshUploadError::attribute_not_found:
      return "a binding names an attribute the mesh does not have";
    case MeshUploadError::duplicate_attribute:
      return "two bindings name the same mesh attribute";
    case MeshUploadError::duplicate_location:
      return "two bindings use the same shader location";
    case MeshUploadError::unsupported_format:
      return "the attribute format has no Vulkan mapping";
    case MeshUploadError::unsupported_topology:
      return "the mesh topology has no Vulkan mapping";
    case MeshUploadError::empty_mesh:
      return "the mesh has no vertices or no indices to upload";
    case MeshUploadError::size_overflow:
      return "a count, stride or size does not fit its 32-bit Vulkan field";
    case MeshUploadError::plan_mismatch:
      return "the plan or the buffers are not consistent with each other";
    case MeshUploadError::wait_timeout:
      return "the upload did not complete within the timeout";
    }
    return "unknown mesh upload error";
  }
};

// Error construction allocates; on exhaustion a code-only Error is returned,
// which does not allocate.
terreate::Error fail(MeshUploadError error, const char *operation, const std::string &detail) {
  try {
    return terreate::Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{make_error_code(error)};
  }
}

const char *semanticName(asset::AttributeSemantic semantic) noexcept {
  switch (semantic) {
  case asset::AttributeSemantic::position:
    return "position";
  case asset::AttributeSemantic::normal:
    return "normal";
  case asset::AttributeSemantic::tangent:
    return "tangent";
  case asset::AttributeSemantic::texcoord:
    return "texcoord";
  case asset::AttributeSemantic::color:
    return "color";
  case asset::AttributeSemantic::custom:
    return "custom";
  }
  return "unknown";
}

std::string describe(const asset::AttributeKey &key) {
  std::string text = semanticName(key.semantic);
  if (key.semantic == asset::AttributeSemantic::custom)
    text += " '" + key.name + "'";
  text += " set " + std::to_string(key.set);
  return text;
}

std::optional<vk::Format> toVulkan(asset::AttributeFormat format) noexcept {
  switch (format) {
  case asset::AttributeFormat::float32x1:
    return vk::Format::eR32Sfloat;
  case asset::AttributeFormat::float32x2:
    return vk::Format::eR32G32Sfloat;
  case asset::AttributeFormat::float32x3:
    return vk::Format::eR32G32B32Sfloat;
  case asset::AttributeFormat::float32x4:
    return vk::Format::eR32G32B32A32Sfloat;
  }
  return std::nullopt;
}

std::optional<vk::PrimitiveTopology> toVulkan(asset::PrimitiveTopology topology) noexcept {
  switch (topology) {
  case asset::PrimitiveTopology::triangle_list:
    return vk::PrimitiveTopology::eTriangleList;
  case asset::PrimitiveTopology::triangle_strip:
    return vk::PrimitiveTopology::eTriangleStrip;
  case asset::PrimitiveTopology::line_list:
    return vk::PrimitiveTopology::eLineList;
  case asset::PrimitiveTopology::point_list:
    return vk::PrimitiveTopology::ePointList;
  }
  return std::nullopt;
}

constexpr std::uint64_t max32 = std::numeric_limits<std::uint32_t>::max();

template <class Index> std::vector<std::byte> toBytes(const std::vector<Index> &indices) {
  std::vector<std::byte> bytes(indices.size() * sizeof(Index));
  if (!indices.empty())
    std::memcpy(bytes.data(), indices.data(), bytes.size());
  return bytes;
}

bool hasUsage(vk::BufferUsageFlags usage, vk::BufferUsageFlags needed) noexcept {
  return (usage & needed) == needed;
}

// A description of what is wrong with `plan`, or an empty string when it is
// consistent: sizes equal the bytes, the always-present usage is set and the
// index buffer exists exactly when there are index bytes.
std::string planProblem(const MeshUploadPlan &plan) {
  if (plan.vertex_bytes.empty())
    return "the plan has no vertex bytes";
  if (plan.vertex_buffer.size != plan.vertex_bytes.size())
    return "vertex_buffer.size differs from the number of vertex bytes";
  if (!hasUsage(plan.vertex_buffer.usage,
                vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer))
    return "vertex_buffer.usage lacks eTransferDst or eVertexBuffer";
  if (plan.index_buffer.has_value() == plan.index_bytes.empty())
    return "index_buffer is present exactly when there are index bytes";
  if (plan.index_buffer) {
    if (plan.index_buffer->size != plan.index_bytes.size())
      return "index_buffer.size differs from the number of index bytes";
    if (!hasUsage(plan.index_buffer->usage,
                  vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer))
      return "index_buffer.usage lacks eTransferDst or eIndexBuffer";
  }
  return {};
}

bool sizeMatches(const graphics::Buffer &buffer, vk::DeviceSize size) noexcept {
  const auto *configuration = buffer.configuration();
  return buffer.valid() && configuration != nullptr && configuration->requested.size == size;
}

} // namespace

const std::error_category &mesh_upload_error_category() noexcept {
  static const MeshUploadErrorCategory category;
  return category;
}
std::error_code make_error_code(MeshUploadError error) noexcept {
  return {static_cast<int>(error), mesh_upload_error_category()};
}

void MeshUploadPlan::applyTo(graphics::GraphicsPipelineDescription &description) const {
  description.vertex_bindings = {binding};
  description.vertex_attributes = attributes;
  description.topology = topology;
}

auto planMeshUpload(const asset::MeshData &mesh, const MeshUploadDescription &description)
    -> terreate::Result<MeshUploadPlan> {
  constexpr const char *operation = "planMeshUpload";
  if (const auto valid = asset::validate(mesh); !valid)
    return std::unexpected(valid.error());
  if (description.attributes.empty())
    return std::unexpected(fail(MeshUploadError::no_attributes, operation,
                                "MeshUploadDescription::attributes is empty"));

  MeshUploadPlan plan;
  const auto topology = toVulkan(mesh.topology);
  if (!topology)
    return std::unexpected(
        fail(MeshUploadError::unsupported_topology, operation, "the mesh topology"));
  plan.topology = *topology;

  std::vector<asset::AttributeKey> order;
  order.reserve(description.attributes.size());
  std::vector<vk::Format> formats;
  formats.reserve(description.attributes.size());
  for (std::size_t i = 0; i < description.attributes.size(); ++i) {
    const auto &bound = description.attributes[i];
    const auto *stream = mesh.find(bound.key);
    if (stream == nullptr)
      return std::unexpected(fail(MeshUploadError::attribute_not_found, operation,
                                  "the mesh has no " + describe(bound.key)));
    for (std::size_t j = 0; j < i; ++j) {
      if (description.attributes[j].key == bound.key)
        return std::unexpected(fail(MeshUploadError::duplicate_attribute, operation,
                                    describe(bound.key) + " is bound twice"));
      if (description.attributes[j].location == bound.location)
        return std::unexpected(
            fail(MeshUploadError::duplicate_location, operation,
                 "location " + std::to_string(bound.location) + " is used by two bindings"));
    }
    const auto format = toVulkan(stream->format);
    if (!format)
      return std::unexpected(
          fail(MeshUploadError::unsupported_format, operation, describe(bound.key)));
    order.push_back(bound.key);
    formats.push_back(*format);
  }

  const bool indexed = mesh.indexType() != asset::IndexType::none;
  if (mesh.vertexCount() == 0 || (indexed && mesh.indexCount() == 0))
    return std::unexpected(fail(MeshUploadError::empty_mesh, operation,
                                mesh.vertexCount() == 0 ? "the mesh has no vertices"
                                                        : "the mesh is indexed with no indices"));
  if (mesh.vertexCount() > max32 || mesh.indexCount() > max32)
    return std::unexpected(fail(MeshUploadError::size_overflow, operation,
                                "the vertex or index count exceeds 2^32 - 1"));

  auto packed = asset::interleave(mesh, order);
  if (!packed)
    return std::unexpected(packed.error());
  const auto &layout = packed->layout;
  if (layout.attributes.size() != order.size())
    return std::unexpected(fail(MeshUploadError::plan_mismatch, operation,
                                "the interleaved layout lost an attribute"));
  const std::uint64_t stride = layout.attributes.front().stride;
  if (stride == 0 || stride > max32)
    return std::unexpected(
        fail(MeshUploadError::size_overflow, operation, "the vertex stride does not fit 32 bits"));

  plan.vertex_bytes = std::move(packed->vertices);
  plan.binding = vk::VertexInputBindingDescription{
      description.binding, static_cast<std::uint32_t>(stride), vk::VertexInputRate::eVertex};
  plan.attributes.reserve(order.size());
  for (std::size_t i = 0; i < order.size(); ++i)
    plan.attributes.emplace_back(description.attributes[i].location, description.binding,
                                 formats[i], layout.attributes[i].offset);
  plan.vertex_count = static_cast<std::uint32_t>(mesh.vertexCount());
  plan.vertex_buffer.size = plan.vertex_bytes.size();
  plan.vertex_buffer.usage = vk::BufferUsageFlagBits::eTransferDst |
                             vk::BufferUsageFlagBits::eVertexBuffer |
                             description.extra_vertex_usage;
  plan.vertex_buffer.allocation = description.vertex_allocation;

  if (const auto *narrow = std::get_if<std::vector<std::uint16_t>>(&mesh.indices)) {
    plan.index_bytes = toBytes(*narrow);
    plan.index_type = vk::IndexType::eUint16;
  } else if (const auto *wide = std::get_if<std::vector<std::uint32_t>>(&mesh.indices)) {
    plan.index_bytes = toBytes(*wide);
    plan.index_type = vk::IndexType::eUint32;
  }
  if (indexed) {
    plan.index_count = static_cast<std::uint32_t>(mesh.indexCount());
    graphics::BufferDescription index;
    index.size = plan.index_bytes.size();
    index.usage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer |
                  description.extra_index_usage;
    index.allocation = description.index_allocation;
    plan.index_buffer = index;
  }

  for (const auto &stream : mesh.attributes) {
    const bool bound = std::ranges::any_of(
        description.attributes, [&](const auto &binding) { return binding.key == stream.key; });
    if (!bound)
      plan.unmapped.push_back(stream.key);
  }
  return plan;
}

auto createMeshBuffers(const graphics::Allocator &allocator, const MeshUploadPlan &plan)
    -> terreate::Result<MeshBuffers> {
  if (const auto problem = planProblem(plan); !problem.empty())
    return std::unexpected(fail(MeshUploadError::plan_mismatch, "createMeshBuffers", problem));
  auto vertex = graphics::createBuffer(allocator, plan.vertex_buffer);
  if (!vertex)
    return std::unexpected(vertex.error());
  std::optional<graphics::Buffer> index;
  if (plan.index_buffer) {
    auto created = graphics::createBuffer(allocator, *plan.index_buffer);
    if (!created)
      return std::unexpected(created.error());
    index.emplace(std::move(*created));
  }
  return MeshBuffers{std::move(*vertex), std::move(index)};
}

auto meshUploadItems(const MeshUploadPlan &plan, const MeshBuffers &buffers)
    -> terreate::Result<std::vector<graphics::BufferUploadItem>> {
  constexpr const char *operation = "meshUploadItems";
  if (const auto problem = planProblem(plan); !problem.empty())
    return std::unexpected(fail(MeshUploadError::plan_mismatch, operation, problem));
  if (!sizeMatches(buffers.vertex, plan.vertex_buffer.size))
    return std::unexpected(fail(MeshUploadError::plan_mismatch, operation,
                                "the vertex buffer is invalid or its size differs from the plan"));
  if (plan.index_buffer.has_value() != buffers.index.has_value())
    return std::unexpected(fail(MeshUploadError::plan_mismatch, operation,
                                "the index buffer is present in only one of the plan and buffers"));
  if (buffers.index && !sizeMatches(*buffers.index, plan.index_buffer->size))
    return std::unexpected(fail(MeshUploadError::plan_mismatch, operation,
                                "the index buffer is invalid or its size differs from the plan"));

  std::vector<graphics::BufferUploadItem> items;
  items.reserve(2);
  items.push_back({&buffers.vertex, 0, plan.vertex_bytes});
  if (buffers.index)
    items.push_back({&*buffers.index, 0, plan.index_bytes});
  return items;
}

} // namespace terreate::integration::asset_graphics

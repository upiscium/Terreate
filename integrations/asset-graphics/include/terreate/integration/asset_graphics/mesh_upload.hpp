#ifndef TERREATE_INTEGRATION_ASSET_GRAPHICS_MESH_UPLOAD_HPP
#define TERREATE_INTEGRATION_ASSET_GRAPHICS_MESH_UPLOAD_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <vector>

#include <terreate/asset/mesh_data.hpp>
#include <terreate/core/result.hpp>
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/graphics_pipeline.hpp>
#include <terreate/graphics/upload_batch.hpp>

/// Connects Asset and Graphics for meshes, through their public APIs only
/// (#279, part 1). Asset stays free of Vulkan and Graphics stays free of Asset;
/// neither depends on this Integration.
///
/// Getting a `MeshData` onto the GPU takes three explicit steps, and nothing in
/// this namespace records a command, submits work or waits:
///
/// 1. `planMeshUpload` describes the conversion on the CPU. The result,
///    `MeshUploadPlan`, is the observable effective state: the interleaved bytes,
///    the native vertex input structs, the buffer descriptions.
/// 2. `createMeshBuffers` creates the vertex (and index) `graphics::Buffer`.
/// 3. `meshUploadItems` gives the `graphics::BufferUploadItem` values the
///    application hands to `graphics::prepareUploadBatch`.
///
/// The application keeps the timing, the queue, the barriers and the retirement
/// (`graphics::recordUploadBatch`, a submission, `graphics::RetirementQueue`).
namespace terreate::integration::asset_graphics {

/// The application's own decision that one mesh attribute is read by the shader
/// at one location. Nothing is guessed from the semantic.
struct MeshAttributeBinding {
  /// The mesh attribute (for example `{AttributeSemantic::position}` or
  /// `{AttributeSemantic::texcoord, 0}`).
  asset::AttributeKey key;
  /// The shader input location it feeds.
  std::uint32_t location = 0;
};

/// What to upload and how to create the buffers. Plain data with documented
/// defaults.
struct MeshUploadDescription {
  /// The attributes placed in the vertex stream, in this order. Required and
  /// not empty: an empty list is an error, not "everything". Mesh attributes
  /// that are not listed are reported by `MeshUploadPlan::unmapped`.
  std::vector<MeshAttributeBinding> attributes;
  /// The vertex binding number of the stream (per-vertex rate).
  std::uint32_t binding = 0;
  /// Usage added to the vertex buffer. `eTransferDst` and `eVertexBuffer` are
  /// always set; add for example `eTransferSrc` to read it back.
  vk::BufferUsageFlags extra_vertex_usage{};
  /// Usage added to the index buffer. `eTransferDst` and `eIndexBuffer` are
  /// always set.
  vk::BufferUsageFlags extra_index_usage{};
  /// Memory of the vertex buffer. Defaults to the `device_local` intent.
  graphics::BufferAllocationDescription vertex_allocation =
      graphics::bufferAllocationFor(graphics::BufferMemoryIntent::device_local);
  /// Memory of the index buffer. Defaults to the `device_local` intent.
  graphics::BufferAllocationDescription index_allocation =
      graphics::bufferAllocationFor(graphics::BufferMemoryIntent::device_local);
};

/// The result of `planMeshUpload`: everything that will be created and
/// uploaded, as plain values the application can inspect, copy and edit before
/// the next step. No Vulkan object exists yet.
///
/// The plan owns the bytes. `meshUploadItems` hands out spans into
/// `vertex_bytes` and `index_bytes`, so the plan must outlive
/// `graphics::prepareUploadBatch` (which copies them into staging) and must not
/// be modified in between.
struct MeshUploadPlan {
  /// The attributes of `MeshUploadDescription::attributes` interleaved and
  /// tightly packed in that order (`asset::interleave`), `vertex_count * stride`
  /// bytes.
  std::vector<std::byte> vertex_bytes;
  /// The indices as stored in the mesh (`uint16` or `uint32`, host byte order);
  /// empty when the mesh is not indexed.
  std::vector<std::byte> index_bytes;
  /// The vertex binding: number, stride and per-vertex rate.
  vk::VertexInputBindingDescription binding{};
  /// One description per bound attribute: location, binding, format and offset.
  std::vector<vk::VertexInputAttributeDescription> attributes;
  /// The Vulkan topology of the mesh.
  vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
  /// The index type; `vk::IndexType::eNoneKHR` when the mesh is not indexed.
  vk::IndexType index_type = vk::IndexType::eNoneKHR;
  /// The number of vertices.
  std::uint32_t vertex_count = 0;
  /// The number of indices; 0 when the mesh is not indexed.
  std::uint32_t index_count = 0;
  /// The vertex buffer `createMeshBuffers` will create: size, usage, memory.
  graphics::BufferDescription vertex_buffer;
  /// The index buffer `createMeshBuffers` will create; empty when the mesh is
  /// not indexed.
  std::optional<graphics::BufferDescription> index_buffer;
  /// Mesh attributes that have no binding and so are not in the vertex stream.
  /// Never dropped silently: the application decides whether that is intended.
  std::vector<asset::AttributeKey> unmapped;

  /// Whether the mesh has an index buffer.
  [[nodiscard]] auto indexed() const noexcept -> bool { return index_buffer.has_value(); }

  /// Replaces `vertex_bindings` and `vertex_attributes` of `description` with
  /// this plan's binding and attributes, and sets its `topology`. Every other
  /// member (primitive restart, culling, depth, formats) is left as it is.
  void applyTo(graphics::GraphicsPipelineDescription &description) const;
};

/// The failures of this Integration. A mesh that fails `asset::validate` is not
/// in this list: its `asset::MeshDataError` is returned unchanged. Errors of
/// `graphics::createBuffer` (`graphics::BufferError`, native failures) are also
/// returned unchanged. Values start at 1; new values are appended.
enum class MeshUploadError : std::uint8_t {
  /// `MeshUploadDescription::attributes` is empty.
  no_attributes = 1,
  /// A binding names an attribute the mesh does not have. The Error's detail
  /// names it.
  attribute_not_found,
  /// Two bindings name the same mesh attribute.
  duplicate_attribute,
  /// Two bindings use the same shader location.
  duplicate_location,
  /// A bound attribute has a format with no Vulkan mapping here.
  unsupported_format,
  /// The mesh topology has no Vulkan mapping here.
  unsupported_topology,
  /// The mesh has no vertices, or it is indexed with no indices: a zero-sized
  /// buffer cannot be created.
  empty_mesh,
  /// A count, a stride or a size does not fit its 32-bit Vulkan field.
  size_overflow,
  /// A plan is not self-consistent (a size, usage or index buffer that does not
  /// match its bytes), or the buffers do not match the plan.
  plan_mismatch,
};

/// The category of MeshUploadError codes (name
/// "terreate.integration.asset_graphics").
[[nodiscard]] auto mesh_upload_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of a MeshUploadError.
[[nodiscard]] auto make_error_code(MeshUploadError error) noexcept -> std::error_code;

/// Step 1, CPU only: describes how `mesh` becomes a vertex buffer and an
/// optional index buffer. Nothing is created and no Vulkan function is called.
///
/// Checks run in this order: `asset::validate(mesh)` (its error is returned
/// unchanged), `no_attributes`, `unsupported_topology`, then each binding in
/// order (`attribute_not_found`, `duplicate_attribute`, `duplicate_location`,
/// `unsupported_format`), `empty_mesh`, `size_overflow`. Nothing is repaired or
/// guessed: shader locations are the application's, and attributes without a
/// binding are listed in `MeshUploadPlan::unmapped`.
///
/// Formats map float32 with 1 to 4 components to `vk::Format::eR32Sfloat`,
/// `eR32G32Sfloat`, `eR32G32B32Sfloat` and `eR32G32B32A32Sfloat`. Topologies
/// map one to one (list and strip triangles, line list, point list). Buffer
/// usage is always `eTransferDst` plus `eVertexBuffer` (or `eIndexBuffer`) plus
/// the application's extra usage. Allocation failure throws `std::bad_alloc`.
[[nodiscard]] auto planMeshUpload(const asset::MeshData &mesh,
                                  const MeshUploadDescription &description)
    -> terreate::Result<MeshUploadPlan>;

/// The buffers of one mesh. Move-only owners; the application keeps them alive
/// through every GPU use (retire them with a `graphics::RetirementQueue`).
struct MeshBuffers {
  /// The vertex buffer.
  graphics::Buffer vertex;
  /// The index buffer; empty when the plan is not indexed.
  std::optional<graphics::Buffer> index;
};

/// Step 2: creates the buffers the plan describes with `graphics::createBuffer`,
/// exactly as `plan.vertex_buffer` and `plan.index_buffer` say. No data is
/// copied and no GPU work is recorded. A plan that is not self-consistent gives
/// `MeshUploadError::plan_mismatch`; creation errors are returned unchanged. If
/// the index buffer fails, the vertex buffer (not yet used by the GPU) is
/// destroyed.
[[nodiscard]] auto createMeshBuffers(const graphics::Allocator &allocator,
                                     const MeshUploadPlan &plan) -> terreate::Result<MeshBuffers>;

/// Step 3: the upload items for `graphics::prepareUploadBatch`, one for the
/// vertex buffer and, when the plan is indexed, one for the index buffer, each
/// at offset 0.
///
/// The items borrow from both arguments: the bytes point into `plan` and the
/// destinations are `&buffers.vertex` and `&*buffers.index`. Keep the plan alive
/// and unchanged until `prepareUploadBatch` has returned (it copies the bytes into
/// staging), and keep the buffers alive and in place through every GPU use.
/// `MeshUploadError::plan_mismatch` when the buffers are not the ones created
/// from this plan (an invalid buffer, a missing or extra index buffer, or a size
/// that differs).
[[nodiscard]] auto meshUploadItems(const MeshUploadPlan &plan, const MeshBuffers &buffers)
    -> terreate::Result<std::vector<graphics::BufferUploadItem>>;

} // namespace terreate::integration::asset_graphics

namespace std {
/// Lets MeshUploadError convert to std::error_code.
template <>
struct is_error_code_enum<terreate::integration::asset_graphics::MeshUploadError> : true_type {};
} // namespace std

#endif // TERREATE_INTEGRATION_ASSET_GRAPHICS_MESH_UPLOAD_HPP

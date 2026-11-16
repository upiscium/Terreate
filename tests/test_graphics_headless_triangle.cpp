// #283: headless offscreen triangle through the public Graphics APIs only.
// Upload (#338) -> descriptors (#266) -> pipeline (#267) -> barriers (#264) ->
// Dynamic Rendering with explicit binding state (#265, #403) -> submit (#258) ->
// image readback (#339) -> retirement (#259). No Window, Surface or Swapchain.
//
// Native Vulkan is used only where the public contract asks for it: the Fence
// is a caller-owned sync primitive (#258, docs/architecture/gpu_submission.md),
// barrier/copy regions are native structs taken by the public recorders, and
// the Debug Utils probe proves the validation sink is live.
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/descriptor_pool.hpp>
#include <terreate/graphics/descriptor_set_layout.hpp>
#include <terreate/graphics/descriptor_write.hpp>
#include <terreate/graphics/graphics_pipeline.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/pipeline_commands.hpp>
#include <terreate/graphics/pipeline_layout.hpp>
#include <terreate/graphics/readback_batch.hpp>
#include <terreate/graphics/rendering_commands.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/shader_module.hpp>
#include <terreate/graphics/spirv_binary.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/transfer_commands.hpp>
#include <terreate/graphics/upload_batch.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace {
using namespace terreate::graphics;

// Offline-compiled fixtures validated with spirv-val --target-env vulkan1.3;
// no compiler or reflection at runtime.
//   glslangValidator -V -g0 --target-env vulkan1.3 -S vert -o quad.vert.spv quad.vert
//   glslangValidator -V -g0 --target-env vulkan1.3 -S frag -o color.frag.spv color.frag
// quad.vert (SHA-256 7b43439e895d55c89e3d4d50098ce2f574854ba38b5fb485238b615037bf48a0;
// identical to the graphics.rendering_commands fixture):
//   #version 450
//   layout(location = 0) in vec2 position;
//   void main() { gl_Position = vec4(position, 0.5, 1.0); }
// color.frag (SHA-256 9bc768970751a2637060c661c0013432fe61d811f8143b080e9340fa4a070006):
//   #version 450
//   layout(set = 0, binding = 0) uniform Color { vec4 value; } color_ubo;
//   layout(location = 0) out vec4 color;
//   void main() { color = color_ubo.value; }
constexpr std::array<std::uint32_t, 158> vertex_words{
    0x07230203, 0x00010600, 0x0008000b, 0x0000001b, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000012, 0x00030047,
    0x0000000b, 0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x0000000b, 0x00000000, 0x00050048,
    0x0000000b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000000b, 0x00000002, 0x0000000b,
    0x00000003, 0x00050048, 0x0000000b, 0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x00000012,
    0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040015, 0x00000008,
    0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c, 0x0000000a,
    0x00000006, 0x00000009, 0x0006001e, 0x0000000b, 0x00000007, 0x00000006, 0x0000000a, 0x0000000a,
    0x00040020, 0x0000000c, 0x00000003, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000003,
    0x00040015, 0x0000000e, 0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000,
    0x00040017, 0x00000010, 0x00000006, 0x00000002, 0x00040020, 0x00000011, 0x00000001, 0x00000010,
    0x0004003b, 0x00000011, 0x00000012, 0x00000001, 0x0004002b, 0x00000006, 0x00000014, 0x3f000000,
    0x0004002b, 0x00000006, 0x00000015, 0x3f800000, 0x00040020, 0x00000019, 0x00000003, 0x00000007,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d,
    0x00000010, 0x00000013, 0x00000012, 0x00050051, 0x00000006, 0x00000016, 0x00000013, 0x00000000,
    0x00050051, 0x00000006, 0x00000017, 0x00000013, 0x00000001, 0x00070050, 0x00000007, 0x00000018,
    0x00000016, 0x00000017, 0x00000014, 0x00000015, 0x00050041, 0x00000019, 0x0000001a, 0x0000000d,
    0x0000000f, 0x0003003e, 0x0000001a, 0x00000018, 0x000100fd, 0x00010038,
};
constexpr std::array<std::uint32_t, 110> fragment_words{
    0x07230203, 0x00010600, 0x0008000b, 0x00000012, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000c, 0x00030010,
    0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00030047, 0x0000000a,
    0x00000002, 0x00050048, 0x0000000a, 0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x0000000c,
    0x00000021, 0x00000000, 0x00040047, 0x0000000c, 0x00000022, 0x00000000, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000003, 0x0003001e, 0x0000000a, 0x00000007, 0x00040020, 0x0000000b, 0x00000002,
    0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000002, 0x00040015, 0x0000000d, 0x00000020,
    0x00000001, 0x0004002b, 0x0000000d, 0x0000000e, 0x00000000, 0x00040020, 0x0000000f, 0x00000002,
    0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x00050041, 0x0000000f, 0x00000010, 0x0000000c, 0x0000000e, 0x0004003d, 0x00000007, 0x00000011,
    0x00000010, 0x0003003e, 0x00000009, 0x00000011, 0x000100fd, 0x00010038,
};

constexpr std::uint32_t size = 64;
constexpr vk::Format color_format = vk::Format::eR8G8B8A8Unorm;

int failures = 0;
std::size_t submit_calls = 0;
std::size_t draw_calls = 0;
std::size_t bind_descriptor_calls = 0;
std::size_t idle_calls = 0;
std::size_t destroy_buffer_calls = 0;
VkFence forced_pending_fence = VK_NULL_HANDLE;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "graphics.headless_triangle: assertion=FAIL: %s\n", message);
  }
  return condition;
}

int blocked(const char *message) {
  std::fprintf(stderr, "graphics.headless_triangle: validation=BLOCKED: %s\n", message);
  std::fflush(stderr);
  return EXIT_FAILURE;
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    // The Graphics diagnostic translator publishes uppercase native categories.
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

// Caller-owned sync primitive: #258 borrows the Fence, it never creates one.
struct NativeFence {
  VkDevice device = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  NativeFence() = default;
  NativeFence(const NativeFence &) = delete;
  NativeFence &operator=(const NativeFence &) = delete;
  ~NativeFence() {
    if (fence != VK_NULL_HANDLE)
      vkDestroyFence(device, fence, nullptr);
  }
};
} // namespace

// GNU linker wrappers count real native calls; every wrap forwards to the driver.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vkQueueSubmit2(VkQueue, std::uint32_t, const VkSubmitInfo2 *, VkFence);
extern "C" VkResult __real_vkGetFenceStatus(VkDevice, VkFence);
extern "C" VkResult __real_vkDeviceWaitIdle(VkDevice);
extern "C" void __real_vkCmdDraw(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t,
                                 std::uint32_t);
extern "C" void __real_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint,
                                               VkPipelineLayout, std::uint32_t, std::uint32_t,
                                               const VkDescriptorSet *, std::uint32_t,
                                               const std::uint32_t *);
extern "C" void __real_vmaDestroyBuffer(VmaAllocator, VkBuffer, VmaAllocation);

extern "C" VkResult __wrap_vkQueueSubmit2(VkQueue queue, std::uint32_t count,
                                          const VkSubmitInfo2 *info, VkFence fence) {
  ++submit_calls;
  return __real_vkQueueSubmit2(queue, count, info, fence);
}
extern "C" VkResult __wrap_vkGetFenceStatus(VkDevice device, VkFence fence) {
  if (fence != VK_NULL_HANDLE && forced_pending_fence == fence)
    return VK_NOT_READY;
  return __real_vkGetFenceStatus(device, fence);
}
extern "C" VkResult __wrap_vkDeviceWaitIdle(VkDevice device) {
  ++idle_calls;
  return __real_vkDeviceWaitIdle(device);
}
extern "C" void __wrap_vkCmdDraw(VkCommandBuffer command, std::uint32_t vertices,
                                 std::uint32_t instances, std::uint32_t first_vertex,
                                 std::uint32_t first_instance) {
  ++draw_calls;
  __real_vkCmdDraw(command, vertices, instances, first_vertex, first_instance);
}
extern "C" void __wrap_vkCmdBindDescriptorSets(VkCommandBuffer command, VkPipelineBindPoint point,
                                               VkPipelineLayout layout, std::uint32_t first,
                                               std::uint32_t count, const VkDescriptorSet *sets,
                                               std::uint32_t dynamic_count,
                                               const std::uint32_t *dynamic_offsets) {
  ++bind_descriptor_calls;
  __real_vkCmdBindDescriptorSets(command, point, layout, first, count, sets, dynamic_count,
                                 dynamic_offsets);
}
extern "C" void __wrap_vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer,
                                        VmaAllocation allocation) {
  ++destroy_buffer_calls;
  __real_vmaDestroyBuffer(allocator, buffer, allocation);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
VkImageMemoryBarrier2 colorBarrier(const Image &image) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.nativeImage();
  barrier.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return barrier;
}

int exercise(const Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             Queue queue) {
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("VMA Allocator");

  // Offscreen RGBA8 color target: the format #339 image readback supports.
  ImageDescription image_description;
  image_description.format = color_format;
  image_description.extent = vk::Extent3D{size, size, 1};
  image_description.usage =
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
  auto target = createImage(*allocator, image_description);
  if (!target)
    return blocked("offscreen color Image");
  ImageViewDescription view_description;
  view_description.format = color_format;
  view_description.subresource_range =
      vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
  auto target_view = createImageView(device, *target, view_description);
  if (!target_view)
    return blocked("offscreen color ImageView");

  // Device-local vertex and uniform Buffers, filled only through #338 staging.
  constexpr std::array<float, 6> vertices{-0.5F, -0.5F, 0.5F, -0.5F, 0.0F, 0.5F};
  constexpr std::array<float, 4> uniform_color{1.0F, 0.0F, 1.0F, 1.0F};
  BufferDescription vertex_description;
  vertex_description.size = sizeof(vertices);
  vertex_description.usage =
      vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
  BufferDescription uniform_description;
  uniform_description.size = sizeof(uniform_color);
  uniform_description.usage =
      vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eTransferDst;
  auto vertex_buffer = createBuffer(*allocator, vertex_description);
  auto uniform_buffer = createBuffer(*allocator, uniform_description);
  if (!vertex_buffer || !uniform_buffer)
    return blocked("vertex and uniform Buffers");
  const std::array<BufferUploadItem, 2> uploads{{
      {&*vertex_buffer, 0, std::as_bytes(std::span{vertices})},
      {&*uniform_buffer, 0, std::as_bytes(std::span{uniform_color})},
  }};
  auto upload = prepareUploadBatch(*allocator, UploadBatchDescription{.buffers = uploads});
  if (!upload)
    return blocked("prepare vertex/uniform Upload");

  // One uniform buffer at set 0, binding 0, read by the fragment shader.
  DescriptorSetLayoutDescription set_description;
  set_description.bindings = {
      {0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment}};
  auto set_layout = createDescriptorSetLayout(device, set_description);
  if (!set_layout)
    return blocked("DescriptorSetLayout");
  DescriptorPoolDescription pool_description;
  pool_description.max_sets = 1;
  pool_description.sizes = {{vk::DescriptorType::eUniformBuffer, 1}};
  auto descriptor_pool = createDescriptorPool(device, pool_description);
  if (!descriptor_pool)
    return blocked("DescriptorPool");
  auto descriptor_set = descriptor_pool->allocate(*set_layout);
  if (!descriptor_set)
    return blocked("allocate DescriptorSet");
  const std::array<DescriptorWriteDescription, 1> writes{{
      {0, 0, vk::DescriptorType::eUniformBuffer,
       std::vector<DescriptorBufferWriteElement>{{&*uniform_buffer, 0, sizeof(uniform_color)}}},
  }};
  if (!updateDescriptorSet(device, *set_layout, *descriptor_set, writes))
    return blocked("write the uniform Buffer descriptor");

  const auto vertex_binary = SpirvBinary::fromWords(vertex_words);
  const auto fragment_binary = SpirvBinary::fromWords(fragment_words);
  if (!vertex_binary || !fragment_binary)
    return blocked("precompiled SPIR-V fixtures");
  auto vertex_module = createShaderModule(device, *vertex_binary);
  auto fragment_module = createShaderModule(device, *fragment_binary);
  if (!vertex_module || !fragment_module)
    return blocked("ShaderModules");
  const std::array<const DescriptorSetLayout *, 1> set_layouts{&*set_layout};
  auto pipeline_layout = createPipelineLayout(device, set_layouts, {});
  if (!pipeline_layout)
    return blocked("PipelineLayout with the uniform set");
  GraphicsPipelineDescription pipeline_description;
  pipeline_description.vertex_entry_point = "main";
  pipeline_description.fragment_entry_point = "main";
  pipeline_description.vertex_bindings = {
      vk::VertexInputBindingDescription{0, 2 * sizeof(float), vk::VertexInputRate::eVertex}};
  pipeline_description.vertex_attributes = {
      vk::VertexInputAttributeDescription{0, 0, vk::Format::eR32G32Sfloat, 0}};
  pipeline_description.color_attachment_formats = {color_format};
  vk::PipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  pipeline_description.color_blend_attachments = {blend};
  auto pipeline = createGraphicsPipeline(device, *pipeline_layout, *vertex_module, *fragment_module,
                                         pipeline_description);
  if (!pipeline)
    return blocked("Dynamic Rendering GraphicsPipeline");

  CommandPoolDescription command_pool_description;
  command_pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  auto command_pool = createCommandPool(device, command_pool_description);
  if (!command_pool)
    return blocked("CommandPool");
  auto command = command_pool->allocatePrimary();
  if (!command || !command->begin())
    return blocked("begin primary CommandBuffer");

  // Explicit caller-owned dependencies; no recorder transitions or tracks state.
  VkMemoryBarrier2 uploaded{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  uploaded.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  uploaded.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  uploaded.dstStageMask =
      VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  uploaded.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
  auto to_attachment = colorBarrier(*target);
  to_attachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_attachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  if (!recordUploadBatch(device, *command, *upload) ||
      !recordBarriers(device, *command,
                      {.memory = std::span{&uploaded, 1}, .images = std::span{&to_attachment, 1}}))
    return blocked("record Upload and attachment transition");

  const vk::ClearValue clear{vk::ClearColorValue{std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F}}};
  RenderingDescription rendering;
  rendering.render_area = vk::Rect2D{{0, 0}, {size, size}};
  rendering.color_attachments = {{&*target, &*target_view, vk::ImageLayout::eColorAttachmentOptimal,
                                  vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                                  clear}};
  auto scope = recordBeginRendering(device, *command, rendering);
  if (!scope)
    return blocked("begin Dynamic Rendering");
  const std::array<const DescriptorSet *, 1> sets{&*descriptor_set};
  const std::array<VertexBufferBinding, 1> vertex_bindings{{{&*vertex_buffer, 0}}};
  GraphicsBindingState binding_state;
  if (!recordSetViewport(device, *command,
                         vk::Viewport{0.0F, 0.0F, float(size), float(size), 0.0F, 1.0F}) ||
      !recordSetScissor(device, *command, vk::Rect2D{{0, 0}, {size, size}}) ||
      !recordBindPipeline(device, *command, *pipeline, binding_state) ||
      !recordBindDescriptorSets(device, *command, vk::PipelineBindPoint::eGraphics,
                                *pipeline_layout, 0, sets, {}) ||
      !recordBindVertexBuffers(device, *command, 0, vertex_bindings, binding_state))
    return blocked("dynamic state, pipeline, descriptor set and vertex buffer binds");
  // The explicit binding state proves the draw fits the bound vertex buffer.
  const auto draws_before = draw_calls;
  const auto too_long = recordDraw(device, *command, *scope, binding_state, 4, 1, 0, 0);
  check(!too_long &&
            too_long.error().code() ==
                make_error_code(RenderingCommandError::vertex_range_out_of_bounds) &&
            draw_calls == draws_before,
        "a draw past the 3 uploaded vertices is rejected before the native call");
  if (!recordDraw(device, *command, *scope, binding_state, 3, 1, 0, 0) ||
      !recordEndRendering(device, *command, std::move(*scope)))
    return blocked("draw the triangle and end rendering");

  auto to_transfer = colorBarrier(*target);
  to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_transfer.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  to_transfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
  region.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = VkExtent3D{size, size, 1};
  const std::array<ImageReadbackItem, 1> reads{
      {{&*target, vk::ImageLayout::eTransferSrcOptimal, region}}};
  auto readback = prepareReadbackBatch(*allocator, ReadbackBatchDescription{.images = reads});
  if (!readback)
    return blocked("prepare image Readback");
  VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  if (!recordBarriers(device, *command, {.images = std::span{&to_transfer, 1}}) ||
      !recordReadbackBatch(device, *command, *readback) ||
      !recordBarriers(device, *command, {.memory = std::span{&to_host, 1}}) || !command->end())
    return blocked("record readback and end the CommandBuffer");
  check(draw_calls == draws_before + 1 && bind_descriptor_calls == 1 && submit_calls == 0,
        "one native draw and one descriptor bind recorded; recording never submits");

  NativeFence fence;
  fence.device = static_cast<VkDevice>(device.nativeHandle());
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(fence.device, &fence_info, nullptr, &fence.fence) != VK_SUCCESS)
    return blocked("caller-owned native Fence");
  const std::array<const CommandBuffer *, 1> commands{&*command};
  int identity_domain = 0;
  CompletionDescription completion;
  completion.device = &device;
  completion.queue = queue;
  completion.requested = {&identity_domain, 1};
  completion.effective = {&identity_domain, 1};
  completion.condition = FenceCompletionCondition{vk::Fence{fence.fence}};
  SubmissionDescription submission;
  submission.command_buffers = commands;
  submission.fence = vk::Fence{fence.fence};
  submission.completion = completion;

  auto no_receipt = tryCopyCompletedReadbackBatch(device, *readback, {});
  check(!no_receipt &&
            no_receipt.error().code() == make_error_code(ReadbackError::missing_completion),
        "readback refuses host access without completion evidence");
  // The application submits once; recorders never submit or wait.
  auto receipt = submit(device, queue, submission);
  if (!receipt)
    std::_Exit(blocked("caller Queue submission"));
  const std::span<const SubmissionReceipt> uses{&*receipt, 1};
  forced_pending_fence = fence.fence;
  const auto pending = tryCopyCompletedReadbackBatch(device, *readback, uses);
  forced_pending_fence = VK_NULL_HANDLE;
  check(!pending && pending.error().code() == make_error_code(ReadbackError::completion_pending),
        "a pending Fence refuses host access to the readback");
  const auto waited = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!waited || waited->state != SubmissionCompletionState::completed)
    std::_Exit(blocked("Fence did not prove GPU completion"));

  auto copied = tryCopyCompletedReadbackBatch(device, *readback, uses);
  if (!copied || copied->images().size() != 1)
    return blocked("copy the completed image Readback");
  const auto pitch = copied->images()[0].row_pitch_bytes;
  constexpr std::size_t row_bytes = std::size_t{size} * 4;
  if (!check(copied->images()[0].format == color_format && pitch >= row_bytes &&
                 copied->imageDataFor(0).size() >= (pitch * (std::size_t{size} - 1)) + row_bytes,
             "readback is RGBA8 and covers every row"))
    return EXIT_FAILURE;
  const auto pixel = [&copied, pitch](std::uint32_t x, std::uint32_t y) {
    std::array<std::uint8_t, 4> rgba{};
    std::memcpy(rgba.data(),
                copied->imageDataFor(0).data() + (std::size_t{y} * pitch) + (std::size_t{x} * 4),
                rgba.size());
    return rgba;
  };
  constexpr std::array<std::uint8_t, 4> magenta{255, 0, 255, 255};
  constexpr std::array<std::uint8_t, 4> black{0, 0, 0, 255};
  // NDC y points down: the base spans x in [16, 48) on row 16 and the apex is at (32, 48).
  check(pixel(size / 2, size / 2) == magenta && pixel(20, 18) == magenta &&
            pixel(44, 18) == magenta,
        "the triangle has the uniform color at its center and near both base corners");
  check(pixel(0, 0) == black && pixel(size - 1, 0) == black && pixel(0, size - 1) == black &&
            pixel(size - 1, size - 1) == black && pixel(8, size / 2) == black &&
            pixel(size / 2, 56) == black,
        "pixels outside the triangle keep the clear color");

  // Staging is retired with the genuine receipt after the CPU copy.
  const auto destroyed_before = destroy_buffer_calls;
  RetirementQueue retirement;
  if (!retirement.retire(*upload, uses) || !retirement.retire(*readback, uses))
    std::_Exit(blocked("retire Upload and Readback staging"));
  auto collected = retirement.collect();
  if (!collected || !retirement.requireEmpty())
    std::_Exit(blocked("collect completed staging"));
  check(collected->collected == 2 && destroy_buffer_calls == destroyed_before + 2 &&
            pixel(size / 2, size / 2) == magenta,
        "both staging Buffers are destroyed once and the CPU result outlives them");
  check(submit_calls == 1 && idle_calls == 0, "one caller submit and no hidden device wait");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  bool has_validation = false;
  bool has_debug_utils = false;
  for (const auto &layer : capabilities->available_layers)
    has_validation |= layer == "VK_LAYER_KHRONOS_validation";
  for (const auto &extension : capabilities->available_extensions)
    has_debug_utils |= extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (has_validation && !has_debug_utils)
    return blocked("Validation Layer available but Debug Utils observation unavailable");

  InstanceDescription instance_description;
  instance_description.api_version = VK_API_VERSION_1_3;
  instance_description.application_name = "terreate-headless-triangle";
  instance_description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  if (has_debug_utils)
    instance_description.debug_utils = DebugUtilsMode::required;
  const auto instance_plan = resolveInstance(instance_description, *capabilities);
  if (!instance_plan)
    return blocked("Vulkan 1.3 Instance resolution");
  if (has_validation) {
    bool layer_enabled = false;
    for (const auto &layer : instance_plan->enabledLayers())
      layer_enabled |= layer == "VK_LAYER_KHRONOS_validation";
    if (!layer_enabled || !instance_plan->debugUtilsEnabled())
      return blocked("Validation Layer/Debug Utils available but not effectively enabled");
  }
  ValidationRecorder recorder;
  auto instance = createInstance(*instance_plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("headless Vulkan Instance");

  if (has_validation) {
    // Prove that validation messages reach the recorder, so a silent sink
    // cannot produce a false validation PASS.
    const auto native_instance = static_cast<VkInstance>(instance->nativeHandle());
    const auto emit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
        vkGetInstanceProcAddr(native_instance, "vkSubmitDebugUtilsMessageEXT"));
    if (!emit)
      return blocked("Debug Utils callback cannot be probed");
    VkDebugUtilsMessengerCallbackDataEXT probe{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    probe.pMessageIdName = "terreate-headless-triangle-validation-probe";
    probe.messageIdNumber = 283;
    probe.pMessage = "test-only validation warning sent through Vulkan Debug Utils";
    const auto before_probe = recorder.warnings_or_errors.load(std::memory_order_relaxed);
    emit(native_instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &probe);
    if (!check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == before_probe + 1,
               "validation sink receives the native probe"))
      return EXIT_FAILURE;
    // Remove only the synthetic probe; real warnings stay counted.
    recorder.warnings_or_errors.fetch_sub(1, std::memory_order_relaxed);
    std::puts("graphics.headless_triangle: validation-callback-probe=PASS");
  }

  const auto candidates = queryPhysicalDevices(*instance);
  if (!candidates || candidates->empty())
    return blocked("headless PhysicalDevice inventory");
  for (const auto &candidate : *candidates) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t index = 0; index < families.size(); ++index) {
      if (!families[index].queueCount ||
          !(families[index].queueFlags & vk::QueueFlagBits::eGraphics))
        continue;
      DeviceDescription request;
      request.required_features_13.synchronization2 = VK_TRUE;
      request.required_features_13.dynamicRendering = VK_TRUE;
      DeviceQueueRequest queue_request;
      queue_request.caller_id = "headless-triangle";
      queue_request.required_flags = vk::QueueFlagBits::eGraphics;
      queue_request.allowed_family_indices = {static_cast<std::uint32_t>(index)};
      request.queue_requests.push_back(queue_request);
      const auto plan = resolveDevice(candidate, request);
      if (!plan)
        continue;
      int status = EXIT_FAILURE;
      {
        auto device = createDevice(*instance, *plan);
        if (!device)
          return blocked("graphics logical Device with dynamicRendering");
        status = exercise(*instance, candidate, *device, device->queue("headless-triangle"));
      }
      if (status != EXIT_SUCCESS)
        return status;
      if (has_validation) {
        if (!check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == 0,
                   "no validation warnings/errors including teardown"))
          return EXIT_FAILURE;
        std::puts("graphics.headless_triangle: validation-layer=PASS");
      } else {
        std::puts("graphics.headless_triangle: validation-layer=SKIP "
                  "reason=VK_LAYER_KHRONOS_validation unavailable");
      }
      std::puts("graphics.headless_triangle: headless-triangle-readback=PASS");
      return EXIT_SUCCESS;
    }
  }
  return blocked("no graphics queue with dynamicRendering and synchronization2");
}
} // namespace

int main() { return run(); }

#include <terreate/graphics/rendering_commands.hpp>

#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/graphics_pipeline.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/pipeline_commands.hpp>
#include <terreate/graphics/pipeline_layout.hpp>
#include <terreate/graphics/readback_batch.hpp>
#include <terreate/graphics/shader_module.hpp>
#include <terreate/graphics/spirv_binary.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/transfer_commands.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace terreate::graphics;
using Error = RenderingCommandError;

// Offline-compiled fixtures validated with spirv-val --target-env vulkan1.3;
// no compiler or reflection at runtime.
//   glslangValidator -V -g0 --target-env vulkan1.3 -S vert -o tri.vert.spv tri.vert
//   glslangValidator -V -g0 --target-env vulkan1.3 -S frag -o tri.frag.spv tri.frag
// tri.vert (SHA-256 3bcdc5bce9a2b454e8a638d2296dade372f458714316dbea7416a8b85e06dd0d):
//   #version 450
//   const vec2 positions[3] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.0, 0.5));
//   void main() { gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1.0); }
// tri.frag (SHA-256 f88f709962cc2746e383cae765cb5142b1bebfce400cf9d0a4e0b7f252db3f33):
//   #version 450
//   layout(location = 0) out vec4 color;
//   void main() { color = vec4(0.0, 1.0, 0.0, 1.0); }
constexpr std::array<std::uint32_t, 220> vertex_words{
    0x07230203, 0x00010600, 0x0008000b, 0x00000028, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x0000001b, 0x00030047,
    0x0000000b, 0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x0000000b, 0x00000000, 0x00050048,
    0x0000000b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000000b, 0x00000002, 0x0000000b,
    0x00000003, 0x00050048, 0x0000000b, 0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x0000001b,
    0x0000000b, 0x0000002a, 0x00030047, 0x0000001e, 0x00000018, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040015, 0x00000008, 0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000009,
    0x00000001, 0x0004001c, 0x0000000a, 0x00000006, 0x00000009, 0x0006001e, 0x0000000b, 0x00000007,
    0x00000006, 0x0000000a, 0x0000000a, 0x00040020, 0x0000000c, 0x00000003, 0x0000000b, 0x0004003b,
    0x0000000c, 0x0000000d, 0x00000003, 0x00040015, 0x0000000e, 0x00000020, 0x00000001, 0x0004002b,
    0x0000000e, 0x0000000f, 0x00000000, 0x00040017, 0x00000010, 0x00000006, 0x00000002, 0x0004002b,
    0x00000008, 0x00000011, 0x00000003, 0x0004001c, 0x00000012, 0x00000010, 0x00000011, 0x0004002b,
    0x00000006, 0x00000013, 0xbf000000, 0x0005002c, 0x00000010, 0x00000014, 0x00000013, 0x00000013,
    0x0004002b, 0x00000006, 0x00000015, 0x3f000000, 0x0005002c, 0x00000010, 0x00000016, 0x00000015,
    0x00000013, 0x0004002b, 0x00000006, 0x00000017, 0x00000000, 0x0005002c, 0x00000010, 0x00000018,
    0x00000017, 0x00000015, 0x0006002c, 0x00000012, 0x00000019, 0x00000014, 0x00000016, 0x00000018,
    0x00040020, 0x0000001a, 0x00000001, 0x0000000e, 0x0004003b, 0x0000001a, 0x0000001b, 0x00000001,
    0x00040020, 0x0000001d, 0x00000007, 0x00000012, 0x00040020, 0x0000001f, 0x00000007, 0x00000010,
    0x0004002b, 0x00000006, 0x00000022, 0x3f800000, 0x00040020, 0x00000026, 0x00000003, 0x00000007,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0005003b,
    0x0000001d, 0x0000001e, 0x00000007, 0x00000019, 0x0004003d, 0x0000000e, 0x0000001c, 0x0000001b,
    0x00050041, 0x0000001f, 0x00000020, 0x0000001e, 0x0000001c, 0x0004003d, 0x00000010, 0x00000021,
    0x00000020, 0x00050051, 0x00000006, 0x00000023, 0x00000021, 0x00000000, 0x00050051, 0x00000006,
    0x00000024, 0x00000021, 0x00000001, 0x00070050, 0x00000007, 0x00000025, 0x00000023, 0x00000024,
    0x00000015, 0x00000022, 0x00050041, 0x00000026, 0x00000027, 0x0000000d, 0x0000000f, 0x0003003e,
    0x00000027, 0x00000025, 0x000100fd, 0x00010038,
};
constexpr std::array<std::uint32_t, 76> fragment_words{
    0x07230203, 0x00010600, 0x0008000b, 0x0000000d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009,
    0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x00000000, 0x0004002b, 0x00000006, 0x0000000b,
    0x3f800000, 0x0007002c, 0x00000007, 0x0000000c, 0x0000000a, 0x0000000b, 0x0000000a, 0x0000000b,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0003003e,
    0x00000009, 0x0000000c, 0x000100fd, 0x00010038,
};

constexpr std::uint32_t size = 64;
constexpr vk::Format color_format = vk::Format::eR8G8B8A8Unorm;
constexpr vk::Format depth_format = vk::Format::eD16Unorm;

const char *test_name = "graphics.rendering_commands.cpu";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

int blocked(const char *message) {
  std::fprintf(stderr, "%s: validation=BLOCKED: %s\n", test_name, message);
  std::fflush(stderr);
  return EXIT_FAILURE;
}

template <typename T> bool hasError(const terreate::Result<T> &result, Error error) {
  return !result && result.error().code() == make_error_code(error);
}

// Observed native recording calls; every wrap forwards to the real driver.
struct Recorded {
  std::size_t begin = 0;
  VkRect2D render_area{};
  std::uint32_t layer_count = 0;
  std::uint32_t view_mask = 1;
  VkRenderingFlags flags = 1;
  std::uint32_t color_count = 0;
  std::vector<VkRenderingAttachmentInfo> colors{};
  bool has_depth = false;
  VkRenderingAttachmentInfo depth{};
  bool has_stencil = true;
  std::size_t end = 0;
  std::size_t viewport = 0;
  VkViewport viewport_value{};
  std::size_t scissor = 0;
  VkRect2D scissor_value{};
  std::size_t draw = 0;
  std::array<std::uint32_t, 4> draw_args{};
};
Recorded recorded;

std::size_t nativeCalls() {
  return recorded.begin + recorded.end + recorded.viewport + recorded.scissor + recorded.draw;
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

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

// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void __real_vkCmdBeginRendering(VkCommandBuffer, const VkRenderingInfo *);
extern "C" void __real_vkCmdEndRendering(VkCommandBuffer);
extern "C" void __real_vkCmdSetViewport(VkCommandBuffer, std::uint32_t, std::uint32_t,
                                        const VkViewport *);
extern "C" void __real_vkCmdSetScissor(VkCommandBuffer, std::uint32_t, std::uint32_t,
                                       const VkRect2D *);
extern "C" void __real_vkCmdDraw(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t,
                                 std::uint32_t);

extern "C" void __wrap_vkCmdBeginRendering(VkCommandBuffer command, const VkRenderingInfo *info) {
  ++recorded.begin;
  recorded.render_area = info->renderArea;
  recorded.layer_count = info->layerCount;
  recorded.view_mask = info->viewMask;
  recorded.flags = info->flags;
  recorded.color_count = info->colorAttachmentCount;
  recorded.colors.assign(info->pColorAttachments,
                         info->pColorAttachments + info->colorAttachmentCount);
  recorded.has_depth = info->pDepthAttachment != nullptr;
  if (info->pDepthAttachment)
    recorded.depth = *info->pDepthAttachment;
  recorded.has_stencil = info->pStencilAttachment != nullptr;
  __real_vkCmdBeginRendering(command, info);
}
extern "C" void __wrap_vkCmdEndRendering(VkCommandBuffer command) {
  ++recorded.end;
  __real_vkCmdEndRendering(command);
}
extern "C" void __wrap_vkCmdSetViewport(VkCommandBuffer command, std::uint32_t first,
                                        std::uint32_t count, const VkViewport *viewports) {
  ++recorded.viewport;
  if (first == 0 && count == 1)
    recorded.viewport_value = *viewports;
  __real_vkCmdSetViewport(command, first, count, viewports);
}
extern "C" void __wrap_vkCmdSetScissor(VkCommandBuffer command, std::uint32_t first,
                                       std::uint32_t count, const VkRect2D *scissors) {
  ++recorded.scissor;
  if (first == 0 && count == 1)
    recorded.scissor_value = *scissors;
  __real_vkCmdSetScissor(command, first, count, scissors);
}
extern "C" void __wrap_vkCmdDraw(VkCommandBuffer command, std::uint32_t vertices,
                                 std::uint32_t instances, std::uint32_t first_vertex,
                                 std::uint32_t first_instance) {
  ++recorded.draw;
  recorded.draw_args = {vertices, instances, first_vertex, first_instance};
  __real_vkCmdDraw(command, vertices, instances, first_vertex, first_instance);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
int cpuTest() {
  static_assert(std::is_error_code_enum_v<RenderingCommandError>);
  static_assert(!std::is_default_constructible_v<RenderingScope>);
  static_assert(!std::is_copy_constructible_v<RenderingScope>);
  static_assert(!std::is_copy_assignable_v<RenderingScope>);
  static_assert(std::is_nothrow_move_constructible_v<RenderingScope>);
  static_assert(!std::is_move_assignable_v<RenderingScope>);
  const std::error_code first = Error::invalid_device;
  const std::error_code last = Error::allocation_failed;
  check(first.category() == rendering_command_error_category() && first != last &&
            std::string_view{rendering_command_error_category().name()} ==
                "terreate.graphics.rendering_commands" &&
            !first.message().empty() && !last.message().empty(),
        "typed error category and distinct codes");
  check(RenderingAttachmentDescription{}.layout == vk::ImageLayout::eUndefined &&
            RenderingDescription{}.layer_count == 1,
        "default attachment layout is deliberately invalid");

  vk::PhysicalDeviceLimits limits{};
  limits.maxColorAttachments = 2;
  limits.maxFramebufferWidth = 128;
  limits.maxFramebufferHeight = 64;
  limits.maxFramebufferLayers = 4;
  limits.maxViewportDimensions = std::array<std::uint32_t, 2>{128, 64};
  limits.viewportBoundsRange = std::array<float, 2>{-256.0F, 255.0F};

  RenderingDescription description;
  description.render_area = vk::Rect2D{{0, 0}, {128, 64}};
  description.color_attachments.resize(2);
  check(validateRenderingDescription(description, limits).has_value(),
        "two colors and a render area at the framebuffer limits accepted");
  description.color_attachments.resize(3);
  check(hasError(validateRenderingDescription(description, limits), Error::invalid_attachment),
        "color count above maxColorAttachments rejected");
  description.color_attachments.resize(1);
  const auto area = [&](vk::Rect2D rect) {
    auto copy = description;
    copy.render_area = rect;
    return validateRenderingDescription(copy, limits);
  };
  check(hasError(area({{0, 0}, {0, 8}}), Error::invalid_render_area) &&
            hasError(area({{-1, 0}, {8, 8}}), Error::invalid_render_area) &&
            hasError(area({{1, 0}, {128, 8}}), Error::invalid_render_area) &&
            hasError(area({{0, 1}, {8, 64}}), Error::invalid_render_area) &&
            hasError(area({{std::numeric_limits<std::int32_t>::max(), 0}, {1, 1}}),
                     Error::invalid_render_area),
        "empty, negative, framebuffer-exceeding and int32-overflowing areas rejected");
  auto layers = description;
  layers.layer_count = 0;
  check(hasError(validateRenderingDescription(layers, limits), Error::invalid_layer_count),
        "zero layer count rejected");
  layers.layer_count = 5;
  check(hasError(validateRenderingDescription(layers, limits), Error::invalid_layer_count),
        "layer count above maxFramebufferLayers rejected");

  const vk::Viewport viewport{0.0F, 0.0F, 128.0F, 64.0F, 0.0F, 1.0F};
  check(validateViewport(viewport, limits).has_value(), "viewport at the limits accepted");
  auto flipped = viewport;
  flipped.y = 64.0F;
  flipped.height = -64.0F;
  check(validateViewport(flipped, limits).has_value(), "negative-height viewport accepted");
  const auto bad_viewport = [&](auto mutate) {
    auto copy = viewport;
    mutate(copy);
    return hasError(validateViewport(copy, limits), Error::invalid_viewport);
  };
  check(bad_viewport([](vk::Viewport &v) { v.width = 0.0F; }) &&
            bad_viewport([](vk::Viewport &v) { v.height = 0.0F; }) &&
            bad_viewport([](vk::Viewport &v) { v.width = 129.0F; }) &&
            bad_viewport([](vk::Viewport &v) { v.height = 65.0F; }) &&
            bad_viewport([](vk::Viewport &v) { v.x = 200.0F; }) &&
            bad_viewport([](vk::Viewport &v) { v.minDepth = -0.5F; }) &&
            bad_viewport([](vk::Viewport &v) { v.maxDepth = 1.5F; }) &&
            bad_viewport([](vk::Viewport &v) { v.width = std::nanf(""); }),
        "zero, oversized, out-of-bounds, bad depth and NaN viewports rejected");

  check(validateScissor({{0, 0}, {64, 64}}).has_value() &&
            hasError(validateScissor({{-1, 0}, {1, 1}}), Error::invalid_scissor) &&
            hasError(validateScissor({{std::numeric_limits<std::int32_t>::max(), 0}, {1, 1}}),
                     Error::invalid_scissor),
        "negative and int32-overflowing scissors rejected");

  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.rendering_commands.cpu: cpu=PASS");
  return EXIT_SUCCESS;
}

struct Target {
  Image image;
  ImageView view;
};

std::optional<Target> makeTarget(const Device &device, const Allocator &allocator,
                                 vk::Format format, vk::ImageUsageFlags usage,
                                 vk::ImageAspectFlags aspect) {
  ImageDescription description;
  description.format = format;
  description.extent = vk::Extent3D{size, size, 1};
  description.usage = usage;
  auto image = createImage(allocator, description);
  if (!image)
    return std::nullopt;
  ImageViewDescription view_description;
  view_description.format = format;
  view_description.subresource_range = vk::ImageSubresourceRange{aspect, 0, 1, 0, 1};
  auto view = createImageView(device, *image, view_description);
  if (!view)
    return std::nullopt;
  return Target{std::move(*image), std::move(*view)};
}

VkImageMemoryBarrier2 imageBarrier(const Image &image, VkImageAspectFlags aspect) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.nativeImage();
  barrier.subresourceRange = VkImageSubresourceRange{aspect, 0, 1, 0, 1};
  return barrier;
}

int exercise(const Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             Device &other, Device &disabled, Queue queue) {
  auto allocator = createAllocator(instance, candidate, device);
  auto other_allocator = createAllocator(instance, candidate, other);
  if (!allocator || !other_allocator)
    return blocked("VMA allocators");
  constexpr auto color_usage =
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
  auto color =
      makeTarget(device, *allocator, color_format, color_usage, vk::ImageAspectFlagBits::eColor);
  auto depth =
      makeTarget(device, *allocator, depth_format, vk::ImageUsageFlagBits::eDepthStencilAttachment,
                 vk::ImageAspectFlagBits::eDepth);
  auto second_depth =
      makeTarget(device, *allocator, depth_format, vk::ImageUsageFlagBits::eDepthStencilAttachment,
                 vk::ImageAspectFlagBits::eDepth);
  auto sampled = makeTarget(device, *allocator, color_format, vk::ImageUsageFlagBits::eSampled,
                            vk::ImageAspectFlagBits::eColor);
  auto foreign = makeTarget(other, *other_allocator, color_format, color_usage,
                            vk::ImageAspectFlagBits::eColor);
  if (!color || !depth || !second_depth || !sampled || !foreign)
    return blocked("offscreen color/depth images and views");

  const auto vertex_binary = SpirvBinary::fromWords(vertex_words);
  const auto fragment_binary = SpirvBinary::fromWords(fragment_words);
  if (!vertex_binary || !fragment_binary)
    return blocked("SPIR-V fixtures");
  auto vertex = createShaderModule(device, *vertex_binary);
  auto fragment = createShaderModule(device, *fragment_binary);
  auto layout = createPipelineLayout(device, {});
  if (!vertex || !fragment || !layout)
    return blocked("shader modules and PipelineLayout");
  GraphicsPipelineDescription pipeline_description;
  pipeline_description.vertex_entry_point = "main";
  pipeline_description.fragment_entry_point = "main";
  pipeline_description.depth_test_enable = true;
  pipeline_description.depth_write_enable = true;
  pipeline_description.color_attachment_formats = {color_format};
  vk::PipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  pipeline_description.color_blend_attachments = {blend};
  pipeline_description.depth_attachment_format = depth_format;
  auto pipeline = createGraphicsPipeline(device, *layout, *vertex, *fragment, pipeline_description);
  if (!pipeline)
    return blocked("Dynamic Rendering GraphicsPipeline");

  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  auto pool = createCommandPool(device, pool_description);
  auto disabled_pool = createCommandPool(disabled, pool_description);
  if (!pool || !disabled_pool)
    return blocked("CommandPools");
  auto command = pool->allocatePrimary();
  auto spare = pool->allocatePrimary();
  auto disabled_command = disabled_pool->allocatePrimary();
  if (!command || !spare || !disabled_command || !spare->begin() || !disabled_command->begin())
    return blocked("primary CommandBuffers");

  const vk::ClearValue clear_color{
      vk::ClearColorValue{std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F}}};
  const vk::ClearValue clear_depth{vk::ClearDepthStencilValue{1.0F, 0}};
  RenderingDescription description;
  description.render_area = vk::Rect2D{{0, 0}, {size, size}};
  description.color_attachments = {
      {&color->image, &color->view, vk::ImageLayout::eColorAttachmentOptimal,
       vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, clear_color}};
  description.depth_attachment =
      RenderingAttachmentDescription{&depth->image,
                                     &depth->view,
                                     vk::ImageLayout::eDepthAttachmentOptimal,
                                     vk::AttachmentLoadOp::eClear,
                                     vk::AttachmentStoreOp::eDontCare,
                                     clear_depth};
  const vk::Viewport viewport{0.0F, 0.0F, float(size), float(size), 0.0F, 1.0F};
  const vk::Rect2D scissor{{0, 0}, {size, size}};

  check(hasError(recordBeginRendering(device, *command, description), Error::not_recording) &&
            nativeCalls() == 0,
        "initial CommandBuffer rejects rendering without native calls");
  if (!command->begin())
    return blocked("begin primary CommandBuffer");

  const auto with = [&](auto mutate) {
    auto copy = description;
    mutate(copy);
    return recordBeginRendering(device, *command, copy);
  };
  check(hasError(recordBeginRendering(disabled, *disabled_command, description),
                 Error::feature_not_enabled) &&
            hasError(recordBeginRendering(other, *command, description), Error::parent_mismatch) &&
            hasError(with([&](RenderingDescription &d) {
                       d.color_attachments[0].image = &foreign->image;
                       d.color_attachments[0].view = &foreign->view;
                     }),
                     Error::parent_mismatch) &&
            hasError(with([&](RenderingDescription &d) {
                       d.color_attachments[0].image = &sampled->image;
                       d.color_attachments[0].view = &sampled->view;
                     }),
                     Error::invalid_attachment) &&
            hasError(with([&](RenderingDescription &d) {
                       d.color_attachments[0].view = &sampled->view;
                     }),
                     Error::invalid_attachment) &&
            hasError(with([](RenderingDescription &d) {
                       d.color_attachments[0].layout = vk::ImageLayout::eUndefined;
                     }),
                     Error::invalid_attachment) &&
            hasError(with([](RenderingDescription &d) {
                       d.color_attachments[0].layout = vk::ImageLayout::eShaderReadOnlyOptimal;
                     }),
                     Error::invalid_attachment) &&
            hasError(with([](RenderingDescription &d) { d.color_attachments[0].view = nullptr; }),
                     Error::invalid_attachment) &&
            hasError(with([](RenderingDescription &d) {
                       d.render_area = vk::Rect2D{{1, 0}, {size, size}};
                     }),
                     Error::invalid_render_area) &&
            hasError(with([](RenderingDescription &d) { d.layer_count = 2; }),
                     Error::invalid_layer_count) &&
            hasError(with([&](RenderingDescription &d) {
                       d.stencil_attachment = RenderingAttachmentDescription{
                           &second_depth->image, &second_depth->view,
                           vk::ImageLayout::eStencilAttachmentOptimal};
                     }),
                     Error::invalid_attachment) &&
            hasError(
                with([&](RenderingDescription &d) { d.stencil_attachment = d.depth_attachment; }),
                Error::invalid_attachment) &&
            hasError(recordSetViewport(device, *command,
                                       vk::Viewport{0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F}),
                     Error::invalid_viewport) &&
            hasError(recordSetScissor(device, *command, vk::Rect2D{{-1, 0}, {1, 1}}),
                     Error::invalid_scissor) &&
            nativeCalls() == 0,
        "feature, Device, attachment Device, usage, view pairing, layout, area, layers, "
        "depth/stencil view and dynamic state rejected before native calls");
  if (!disabled_command->end())
    return blocked("end disabled-Device CommandBuffer");

  // Explicit caller-owned layout transitions; recorders never transition.
  auto to_color = imageBarrier(color->image, VK_IMAGE_ASPECT_COLOR_BIT);
  to_color.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  auto to_depth = imageBarrier(depth->image, VK_IMAGE_ASPECT_DEPTH_BIT);
  to_depth.dstStageMask =
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  to_depth.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  to_depth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_depth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  const std::array<VkImageMemoryBarrier2, 2> to_attachments{to_color, to_depth};
  if (!recordBarriers(device, *command, {.images = to_attachments}))
    return blocked("explicit attachment layout transitions");

  auto scope = recordBeginRendering(device, *command, description);
  if (!scope)
    return blocked("begin Dynamic Rendering");
  check(scope->active() && scope->nativeCommandBuffer() == command->nativeHandle(),
        "scope is active on the begun CommandBuffer");
  const auto &state = scope->state();
  check(state.render_area == description.render_area && state.layer_count == 1 &&
            state.color_attachments.size() == 1 &&
            state.color_attachments[0].view == color->view.nativeImageView() &&
            state.color_attachments[0].format == color_format &&
            state.color_attachments[0].extent == vk::Extent2D{size, size} &&
            state.color_attachments[0].layout == vk::ImageLayout::eColorAttachmentOptimal &&
            state.color_attachments[0].load_op == vk::AttachmentLoadOp::eClear &&
            state.depth_attachment && state.depth_attachment->format == depth_format &&
            state.depth_attachment->view == depth->view.nativeImageView() &&
            !state.stencil_attachment,
        "RenderingScope exposes the effective attachment state");
  check(recorded.begin == 1 && recorded.flags == 0 && recorded.view_mask == 0 &&
            recorded.layer_count == 1 && recorded.render_area.extent.width == size &&
            recorded.render_area.extent.height == size && recorded.color_count == 1 &&
            recorded.colors.size() == 1 &&
            recorded.colors[0].imageView == color->view.nativeImageView() &&
            recorded.colors[0].imageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            recorded.colors[0].resolveMode == VK_RESOLVE_MODE_NONE &&
            recorded.colors[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
            recorded.colors[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
            recorded.colors[0].clearValue.color.float32[2] == 1.0F && recorded.has_depth &&
            recorded.depth.imageView == depth->view.nativeImageView() &&
            recorded.depth.clearValue.depthStencil.depth == 1.0F && !recorded.has_stencil,
        "one vkCmdBeginRendering with the exact VkRenderingInfo");

  if (!recordBindPipeline(device, *command, *pipeline) ||
      !recordSetViewport(device, *command, viewport) ||
      !recordSetScissor(device, *command, scissor))
    return blocked("bind pipeline and set dynamic state");
  const auto calls = nativeCalls();
  // A scope begun elsewhere or already moved from is not proof of rendering.
  // Moving transfers the only end capability; the source becomes inactive.
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  auto moved = std::move(*scope);
  check(
      !scope->active() && scope->state().color_attachments.empty() && moved.active() &&
          hasError(recordDraw(device, *command, *scope, 3, 1, 0, 0),
                   Error::invalid_rendering_scope) &&
          hasError(recordEndRendering(device, *command, std::move(*scope)),
                   Error::invalid_rendering_scope) &&
          hasError(recordDraw(device, *spare, moved, 3, 1, 0, 0), Error::invalid_rendering_scope) &&
          hasError(recordEndRendering(device, *spare, std::move(moved)),
                   Error::invalid_rendering_scope) &&
          moved.active() && nativeCalls() == calls,
      "inactive or foreign-CommandBuffer scopes rejected without native calls; a failed "
      "end leaves the scope active");
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  if (!recordDraw(device, *command, moved, 3, 1, 0, 0) ||
      !recordEndRendering(device, *command, std::move(moved)))
    return blocked("draw and end Dynamic Rendering");
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  check(!moved.active() && moved.state().color_attachments.empty(),
        "recordEndRendering consumes the scope");
  check(recorded.viewport == 1 && recorded.viewport_value.width == float(size) &&
            recorded.scissor == 1 && recorded.scissor_value.extent.width == size &&
            recorded.draw == 1 && recorded.draw_args == std::array<std::uint32_t, 4>{3, 1, 0, 0} &&
            recorded.end == 1 && recorded.begin == 1,
        "exactly one viewport, scissor, draw and end with exact arguments");

  auto to_transfer = imageBarrier(color->image, VK_IMAGE_ASPECT_COLOR_BIT);
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
      {{&color->image, vk::ImageLayout::eTransferSrcOptimal, region}}};
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
      !recordBarriers(device, *command, {.memory = std::span{&to_host, 1}}) || !command->end() ||
      !spare->end())
    return blocked("explicit transfer barriers, readback and end");

  NativeFence fence;
  fence.device = static_cast<VkDevice>(device.nativeHandle());
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(fence.device, &fence_info, nullptr, &fence.fence) != VK_SUCCESS)
    return blocked("native Fence");
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
  // The application submits; recorders never submit or wait.
  auto receipt = submit(device, queue, submission);
  if (!receipt)
    std::_Exit(blocked("caller Queue submission"));
  const auto waited = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!waited || waited->state != SubmissionCompletionState::completed)
    std::_Exit(blocked("Fence did not prove GPU completion"));
  const std::span<const SubmissionReceipt> uses{&*receipt, 1};
  auto copied = tryCopyCompletedReadbackBatch(device, *readback, uses);
  if (!copied || copied->images().size() != 1)
    return blocked("copy completed image Readback");
  const auto pixels = copied->imageDataFor(0);
  const auto pitch = copied->images()[0].row_pitch_bytes;
  constexpr std::size_t row_bytes = std::size_t{size} * 4;
  if (!check(pitch >= row_bytes && pixels.size() >= (pitch * (std::size_t{size} - 1)) + row_bytes,
             "readback covers every row"))
    return EXIT_FAILURE;
  const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
    std::array<std::uint8_t, 4> rgba{};
    std::memcpy(rgba.data(), pixels.data() + (std::size_t{y} * pitch) + (std::size_t{x} * 4),
                rgba.size());
    return rgba;
  };
  constexpr std::array<std::uint8_t, 4> green{0, 255, 0, 255};
  constexpr std::array<std::uint8_t, 4> blue{0, 0, 255, 255};
  check(pixel(size / 2, size / 2) == green, "triangle covers the center pixel");
  check(pixel(0, 0) == blue && pixel(size - 1, 0) == blue && pixel(0, size - 1) == blue &&
            pixel(size - 1, size - 1) == blue,
        "corners keep the clear color");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int nativeTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  bool has_validation = false;
  bool has_debug_utils = false;
  for (const auto &layer : capabilities->available_layers)
    has_validation |= layer == "VK_LAYER_KHRONOS_validation";
  for (const auto &extension : capabilities->available_extensions)
    has_debug_utils |= extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (!has_validation || !has_debug_utils)
    return blocked("VK_LAYER_KHRONOS_validation and Debug Utils required");

  InstanceDescription instance_description;
  instance_description.api_version = VK_API_VERSION_1_3;
  instance_description.application_name = "terreate-rendering-commands";
  instance_description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  instance_description.debug_utils = DebugUtilsMode::required;
  const auto instance_plan = resolveInstance(instance_description, *capabilities);
  if (!instance_plan || !instance_plan->debugUtilsEnabled())
    return blocked("Vulkan 1.3 Instance with Debug Utils");
  ValidationRecorder recorder;
  auto instance = createInstance(*instance_plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("headless Vulkan Instance");

  // Prove that validation messages actually reach the recorder.
  const auto native_instance = static_cast<VkInstance>(instance->nativeHandle());
  const auto emit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
      vkGetInstanceProcAddr(native_instance, "vkSubmitDebugUtilsMessageEXT"));
  if (!emit)
    return blocked("Debug Utils callback cannot be probed");
  VkDebugUtilsMessengerCallbackDataEXT probe{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
  probe.pMessageIdName = "terreate-rendering-commands-validation-probe";
  probe.pMessage = "test-only validation warning sent through Vulkan Debug Utils";
  emit(native_instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &probe);
  if (!check(recorder.warnings_or_errors.load() == 1, "validation sink receives native probe"))
    return EXIT_FAILURE;
  recorder.warnings_or_errors.store(0);

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
      queue_request.caller_id = "rendering-commands";
      queue_request.required_flags = vk::QueueFlagBits::eGraphics;
      queue_request.allowed_family_indices = {static_cast<std::uint32_t>(index)};
      request.queue_requests.push_back(queue_request);
      auto without_rendering = request;
      without_rendering.required_features_13.dynamicRendering = VK_FALSE;
      const auto plan = resolveDevice(candidate, request);
      const auto disabled_plan = resolveDevice(candidate, without_rendering);
      if (!plan || !disabled_plan)
        continue;
      int status = EXIT_FAILURE;
      {
        auto device = createDevice(*instance, *plan);
        auto other = createDevice(*instance, *plan);
        auto disabled = createDevice(*instance, *disabled_plan);
        if (!device || !other || !disabled)
          return blocked("three graphics logical Devices");
        check(device->plan()->enabledFeatures13().dynamicRendering &&
                  !disabled->plan()->enabledFeatures13().dynamicRendering,
              "dynamicRendering explicitly enabled only where requested");
        status = exercise(*instance, candidate, *device, *other, *disabled,
                          device->queue("rendering-commands"));
      }
      if (status != EXIT_SUCCESS)
        return status;
      if (!check(recorder.warnings_or_errors.load() == 0,
                 "no validation warnings/errors including teardown"))
        return EXIT_FAILURE;
      std::puts("graphics.rendering_commands: validation-layer=PASS");
      std::puts("graphics.rendering_commands: headless-triangle=PASS");
      return EXIT_SUCCESS;
    }
  }
  return blocked("no graphics queue with dynamicRendering");
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify cpu or gpu mode");
  const std::string_view mode{argv[1]};
  if (mode == "cpu")
    return cpuTest();
  if (mode == "gpu") {
    test_name = "graphics.rendering_commands";
    return nativeTest();
  }
  return blocked("unknown mode");
}

// #275 (part 1): rendering into Swapchain images with the Dynamic Rendering API.
//
// A Swapchain image is a color attachment through
// RenderingAttachmentDescription::swapchain / swapchain_image. The GPU test
// drives an application-owned loop on a VK_EXT_headless_surface (no window
// system needed): acquire, record a clear with recordBeginRendering into the
// acquired image, submit natively, present. It also checks the rejections that
// need no GPU work. If no device can present to a headless surface, the device
// lacks dynamic rendering, or the extension is missing, the test reports SKIP,
// never PASS. The validation layer must be available. No linker or platform
// mechanism is used.
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/presentation.hpp>
#include <terreate/graphics/rendering_commands.hpp>
#include <terreate/graphics/surface.hpp>
#include <terreate/graphics/swapchain.hpp>
#include <terreate/graphics/swapchain_frame.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace terreate::graphics;
using Error = RenderingCommandError;

const char *test_name = "graphics.swapchain_rendering";
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

bool hasName(const std::vector<std::string> &names, std::string_view wanted) {
  return std::ranges::any_of(names, [&](const auto &name) { return name == wanted; });
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        std::fprintf(stderr, "graphics.swapchain_rendering: validation message: %s\n",
                     event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

VkResult createHeadless(VkInstance instance, VkSurfaceKHR *surface, void *) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
      vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
  if (create == nullptr)
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  VkHeadlessSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
  return create(instance, &info, nullptr, surface);
}

int skip(const char *reason) {
  std::printf("graphics.swapchain_rendering: rendering=SKIP reason=%s\n", reason);
  return EXIT_SUCCESS;
}

// The application's own native synchronization objects.
struct SyncObjects {
  VkDevice device = VK_NULL_HANDLE;
  VkSemaphore acquire = VK_NULL_HANDLE;
  VkFence acquire_fence = VK_NULL_HANDLE;
  VkFence submit_fence = VK_NULL_HANDLE;
  std::vector<VkSemaphore> render_done{};

  SyncObjects() = default;
  SyncObjects(const SyncObjects &) = delete;
  SyncObjects &operator=(const SyncObjects &) = delete;
  void release() {
    if (device == VK_NULL_HANDLE)
      return;
    for (const auto semaphore : render_done)
      vkDestroySemaphore(device, semaphore, nullptr);
    vkDestroyFence(device, submit_fence, nullptr);
    vkDestroyFence(device, acquire_fence, nullptr);
    vkDestroySemaphore(device, acquire, nullptr);
    device = VK_NULL_HANDLE;
  }
  ~SyncObjects() { release(); }
};

bool makeSync(SyncObjects &sync, VkDevice device, std::size_t image_count) {
  sync.device = device;
  const VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateSemaphore(device, &semaphore_info, nullptr, &sync.acquire) != VK_SUCCESS ||
      vkCreateFence(device, &fence_info, nullptr, &sync.acquire_fence) != VK_SUCCESS ||
      vkCreateFence(device, &fence_info, nullptr, &sync.submit_fence) != VK_SUCCESS)
    return false;
  for (std::size_t i = 0; i < image_count; ++i) {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS)
      return false;
    sync.render_done.push_back(semaphore);
  }
  return true;
}

struct LayoutChange {
  VkImageLayout from = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout to = VK_IMAGE_LAYOUT_UNDEFINED;
};

void barrier(VkCommandBuffer commands, VkImage image, const LayoutChange &change,
             VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = change.from;
  b.newLayout = change.to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, dst_stage, 0, 0,
                       nullptr, 0, nullptr, 1, &b);
}

RenderingAttachmentDescription clearTo(const Swapchain &chain, const SwapchainImage &image) {
  RenderingAttachmentDescription attachment;
  attachment.layout = vk::ImageLayout::eColorAttachmentOptimal;
  attachment.load_op = vk::AttachmentLoadOp::eClear;
  attachment.store_op = vk::AttachmentStoreOp::eStore;
  attachment.clear_value = vk::ClearValue{vk::ClearColorValue{std::array<float, 4>{0, 0, 1, 1}}};
  attachment.swapchain = &chain;
  attachment.swapchain_image = &image;
  return attachment;
}

int gpuTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  if (!hasName(capabilities->available_extensions, VK_KHR_SURFACE_EXTENSION_NAME) ||
      !hasName(capabilities->available_extensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
    return skip("VK_EXT_headless_surface unavailable");
  if (!hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation") ||
      !hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    return blocked("the validation layer and VK_EXT_debug_utils are required");

  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-swapchain-rendering";
  description.required_extensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                     VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance with surface extensions");
  ValidationRecorder recorder;
  auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("Instance with surface extensions");

  auto inventory = queryPhysicalDevices(*instance);
  if (!inventory)
    return blocked("physical device enumeration");
  auto surface = createSurface(*instance, SurfaceCreator{&createHeadless, nullptr, {}});
  if (!surface)
    return blocked("headless surface");

  const PhysicalDeviceCandidate *presenting = nullptr;
  SurfaceSupport presenting_support;
  std::optional<DevicePlan> device_plan;
  std::optional<PresentationResolution> presentation;
  for (const auto &candidate : *inventory) {
    const auto observed = querySurfaceSupport(*instance, candidate, *surface);
    if (!observed || !observed->canPresent())
      continue;
    const auto resolved = resolvePresentation(*observed, {});
    if (!resolved)
      continue;
    DeviceDescription device_description;
    device_description.required_features_13.dynamicRendering = VK_TRUE;
    if (!applyPresentation(device_description, *resolved))
      continue;
    auto resolved_plan = resolveDevice(candidate, device_description);
    if (!resolved_plan)
      continue;
    presenting = &candidate;
    presenting_support = *observed;
    presentation = *resolved;
    device_plan = std::move(*resolved_plan);
    break;
  }
  if (presenting == nullptr || !device_plan || !presentation)
    return skip("no device can present to a headless surface with dynamic rendering");

  auto device = createDevice(*instance, *device_plan);
  if (!check(device.has_value(), "createDevice with VK_KHR_swapchain and dynamic rendering"))
    return EXIT_FAILURE;
  const Queue queue = device->queue("present");
  if (!check(queue.valid(), "the present queue exists"))
    return EXIT_FAILURE;

  SwapchainRequest request;
  request.extent = vk::Extent2D{64, 48};
  const auto swapchain_description = resolveSwapchain(presenting_support, *presentation, request);
  if (!check(swapchain_description.has_value(), "resolveSwapchain on the real observation"))
    return EXIT_FAILURE;
  auto first = createSwapchain(*instance, *device, *surface, *swapchain_description);
  if (!check(first.has_value(), "createSwapchain on a headless surface"))
    return EXIT_FAILURE;

  SyncObjects sync;
  const auto native_device = static_cast<VkDevice>(device->nativeHandle());
  if (!check(makeSync(sync, native_device, first->images().size()), "native sync objects"))
    return EXIT_FAILURE;

  {
    CommandPoolDescription pool_description;
    pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
    auto pool = createCommandPool(*device, pool_description);
    if (!check(pool.has_value(), "createCommandPool"))
      return EXIT_FAILURE;

    // Frames: acquire, clear the acquired Swapchain image through
    // recordBeginRendering, submit natively, present.
    const std::size_t frame_count = first->images().size() * 2;
    std::size_t presented = 0;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
      AcquireDescription acquire;
      acquire.timeout_ns = UINT64_MAX;
      acquire.semaphore = vk::Semaphore{sync.acquire};
      acquire.fence = vk::Fence{sync.acquire_fence};
      const auto acquired = acquireNextImage(*device, *first, acquire);
      if (!acquired.has_value() || !acquired->image.has_value()) {
        check(false, "acquire returns a usable image");
        break;
      }
      const SwapchainImage image = *acquired->image;
      check(vkWaitForFences(native_device, 1, &sync.acquire_fence, VK_TRUE, UINT64_MAX) ==
                VK_SUCCESS,
            "the acquire fence is signaled");
      vkResetFences(native_device, 1, &sync.acquire_fence);

      auto command = pool->allocatePrimary();
      if (!check(command.has_value() && command->begin().has_value(), "begin a CommandBuffer"))
        break;
      const auto native_command = static_cast<VkCommandBuffer>(command->nativeHandle());
      const auto native_image = static_cast<VkImage>(image.nativeImage());
      barrier(native_command, native_image,
              LayoutChange{VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
      RenderingDescription rendering;
      rendering.render_area = vk::Rect2D{{0, 0}, image.extent()};
      rendering.color_attachments = {clearTo(*first, image)};
      auto scope = recordBeginRendering(*device, *command, rendering);
      if (!check(scope.has_value(), "recordBeginRendering into a Swapchain image"))
        break;
      check(scope->state().color_attachments.size() == 1 &&
                scope->state().color_attachments[0].format == first->configuration()->format &&
                scope->state().color_attachments[0].extent.width == image.extent().width &&
                scope->state().color_attachments[0].view ==
                    static_cast<VkImageView>(image.nativeImageView()),
            "the effective attachment state comes from the Swapchain image");
      if (!check(recordEndRendering(*device, *command, std::move(*scope)).has_value(),
                 "recordEndRendering"))
        break;
      barrier(
          native_command, native_image,
          LayoutChange{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      if (!check(command->end().has_value(), "end the CommandBuffer"))
        break;

      const VkSemaphore render_done = sync.render_done[image.index() % sync.render_done.size()];
      const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.waitSemaphoreCount = 1;
      submit.pWaitSemaphores = &sync.acquire;
      submit.pWaitDstStageMask = &wait_stage;
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &native_command;
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &render_done;
      if (!check(vkQueueSubmit(static_cast<VkQueue>(queue.nativeHandle()), 1, &submit,
                               sync.submit_fence) == VK_SUCCESS,
                 "native submit"))
        break;
      const std::array<vk::Semaphore, 1> waits{vk::Semaphore{render_done}};
      const auto result = presentImage(*device, queue, *first, PresentDescription{image, waits});
      if (!check(result.has_value(), "present"))
        break;
      ++presented;
      check(vkWaitForFences(native_device, 1, &sync.submit_fence, VK_TRUE, UINT64_MAX) ==
                VK_SUCCESS,
            "the submission completes");
      vkResetFences(native_device, 1, &sync.submit_fence);
    }
    check(presented == frame_count, "every frame rendered into a Swapchain image was presented");

    // Rejections: all happen before any native call, on a recording buffer.
    auto second = createSwapchain(*instance, *device, *surface, [&] {
      auto next = *swapchain_description;
      next.old_swapchain = &*first;
      return next;
    }());
    auto probe = pool->allocatePrimary();
    if (check(second.has_value() && probe.has_value() && probe->begin().has_value(),
              "a recreated Swapchain and a recording CommandBuffer for the rejections")) {
      const SwapchainImage &good = first->images().front();
      RenderingDescription rendering;
      rendering.render_area = vk::Rect2D{{0, 0}, good.extent()};

      rendering.color_attachments = {clearTo(*first, good)};
      auto valid = recordBeginRendering(*device, *probe, rendering);
      check(valid.has_value(), "a retired Swapchain's own image is still a valid attachment");
      if (valid)
        check(recordEndRendering(*device, *probe, std::move(*valid)).has_value(),
              "recordEndRendering after the probe");

      auto only_swapchain = clearTo(*first, good);
      only_swapchain.swapchain_image = nullptr;
      rendering.color_attachments = {only_swapchain};
      check(hasError(recordBeginRendering(*device, *probe, rendering), Error::invalid_attachment),
            "a Swapchain without an image is invalid_attachment");
      auto only_image = clearTo(*first, good);
      only_image.swapchain = nullptr;
      rendering.color_attachments = {only_image};
      check(hasError(recordBeginRendering(*device, *probe, rendering), Error::invalid_attachment),
            "a Swapchain image without its Swapchain is invalid_attachment");
      rendering.color_attachments = {clearTo(*second, good)};
      check(hasError(recordBeginRendering(*device, *probe, rendering), Error::invalid_attachment),
            "an image of another generation is invalid_attachment");
      auto bad_layout = clearTo(*first, good);
      bad_layout.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
      rendering.color_attachments = {bad_layout};
      check(hasError(recordBeginRendering(*device, *probe, rendering), Error::invalid_attachment),
            "a layout that is not an attachment layout is invalid_attachment");
      rendering.color_attachments = {clearTo(*first, good)};
      rendering.depth_attachment = clearTo(*first, good);
      check(hasError(recordBeginRendering(*device, *probe, rendering), Error::invalid_attachment),
            "a Swapchain image is never a depth attachment");
      rendering.depth_attachment.reset();
      rendering.render_area = vk::Rect2D{{0, 0}, vk::Extent2D{good.extent().width + 1, 1}};
      check(hasError(recordBeginRendering(*device, *probe, rendering), Error::invalid_render_area),
            "a render area larger than the image is invalid_render_area");
      rendering.render_area = vk::Rect2D{{0, 0}, good.extent()};
      rendering.layer_count = 2;
      check(hasError(recordBeginRendering(*device, *probe, rendering), Error::invalid_layer_count),
            "more than one layer is invalid_layer_count (a Swapchain image view has one layer)");
      rendering.layer_count = 1;
      rendering.color_attachments = {clearTo(*first, good)};
      rendering.stencil_attachment = clearTo(*first, good);
      check(hasError(recordBeginRendering(*device, *probe, rendering), Error::invalid_attachment),
            "a Swapchain image is never a stencil attachment");
      rendering.stencil_attachment.reset();
      // A transfer-only Swapchain has no image view and no color attachment usage.
      if (presenting_support.capabilities.supportedUsageFlags &
          vk::ImageUsageFlagBits::eTransferDst) {
        auto transfer_request = request;
        transfer_request.usage = vk::ImageUsageFlagBits::eTransferDst;
        auto transfer_description =
            resolveSwapchain(presenting_support, *presentation, transfer_request);
        if (check(transfer_description.has_value(), "resolveSwapchain for transfer usage")) {
          transfer_description->old_swapchain = &*second;
          auto third = createSwapchain(*instance, *device, *surface, *transfer_description);
          if (check(third.has_value(), "createSwapchain for transfer usage")) {
            rendering.color_attachments = {clearTo(*third, third->images().front())};
            check(hasError(recordBeginRendering(*device, *probe, rendering),
                           Error::invalid_attachment),
                  "a Swapchain without color attachment usage and views is invalid_attachment");
            const SwapchainImage kept = third->images().front();
            const Swapchain moved_to{std::move(*third)};
            // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is the test.
            rendering.color_attachments = {clearTo(*third, kept)};
            check(hasError(recordBeginRendering(*device, *probe, rendering),
                           Error::invalid_attachment),
                  "a moved-from Swapchain is invalid_attachment");
            vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
          }
        }
      }
      check(probe->end().has_value(), "end the probe CommandBuffer");
    }
    vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
    // `second` goes out of scope with the pool, before the Device.
  }

  vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
  sync.release();
  // Destroy in lifetime order: Swapchain, Device, then Surface.
  {
    const auto gone = std::move(*first);
  }
  {
    const auto gone = std::move(*device);
  }
  {
    const auto gone = std::move(*surface);
  }

  check(recorder.warnings_or_errors.load() == 0, "no validation warnings or errors");
  std::puts("graphics.swapchain_rendering: validation-layer=PASS");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.swapchain_rendering: rendering=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2 || std::string_view{argv[1]} != "gpu")
    return blocked("specify gpu mode");
  return gpuTest();
}

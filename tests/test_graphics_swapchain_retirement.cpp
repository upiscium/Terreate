// #275 (part 3): retiring the old Swapchain generation with present fences.
//
// The GPU test enables the swapchainMaintenance1 device feature, presents with
// a present fence per frame, recreates the Swapchain, hands the retired
// Swapchain and its present fences to a SwapchainRetirementQueue, and collects
// it by non-blocking polls only: there is no wait-idle between the recreation and
// the destruction of the old generation. It also checks the rejections that
// need no GPU work, including a present fence on a Device without the feature.
// If the Instance or a presenting device lacks VK_KHR_swapchain_maintenance1
// (or the surface-maintenance Instance extensions), the test reports SKIP, never
// PASS. The validation layer must be available. No linker or platform mechanism
// is used.
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/presentation.hpp>
#include <terreate/graphics/surface.hpp>
#include <terreate/graphics/swapchain.hpp>
#include <terreate/graphics/swapchain_frame.hpp>
#include <terreate/graphics/swapchain_retirement.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace terreate::graphics;

const char *test_name = "graphics.swapchain_retirement";
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

template <typename T, typename E> bool hasError(const terreate::Result<T> &result, E error) {
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
        std::fprintf(stderr, "graphics.swapchain_retirement: validation message: %s\n",
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
  std::printf("graphics.swapchain_retirement: retirement=SKIP reason=%s\n", reason);
  return EXIT_SUCCESS;
}

// The application's native objects for one Swapchain's frames: acquire
// semaphore and fence, submit fence, one present semaphore per image index, and
// the present fences of every present (owned by the application; they must stay
// untouched until the Swapchain that was presented is collected).
struct WindowObjects {
  VkDevice device = VK_NULL_HANDLE;
  VkSemaphore acquire = VK_NULL_HANDLE;
  VkFence acquire_fence = VK_NULL_HANDLE;
  VkFence submit_fence = VK_NULL_HANDLE;
  std::vector<VkSemaphore> render_done{};
  std::vector<VkFence> present_fences{};

  WindowObjects() = default;
  WindowObjects(const WindowObjects &) = delete;
  WindowObjects &operator=(const WindowObjects &) = delete;
  void release() {
    if (device == VK_NULL_HANDLE)
      return;
    for (const auto fence : present_fences)
      vkDestroyFence(device, fence, nullptr);
    for (const auto semaphore : render_done)
      vkDestroySemaphore(device, semaphore, nullptr);
    vkDestroyFence(device, submit_fence, nullptr);
    vkDestroyFence(device, acquire_fence, nullptr);
    vkDestroySemaphore(device, acquire, nullptr);
    device = VK_NULL_HANDLE;
  }
  ~WindowObjects() { release(); }
};

bool makeObjects(WindowObjects &objects, VkDevice device, std::size_t image_count) {
  objects.device = device;
  const VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateSemaphore(device, &semaphore_info, nullptr, &objects.acquire) != VK_SUCCESS ||
      vkCreateFence(device, &fence_info, nullptr, &objects.acquire_fence) != VK_SUCCESS ||
      vkCreateFence(device, &fence_info, nullptr, &objects.submit_fence) != VK_SUCCESS)
    return false;
  for (std::size_t i = 0; i < image_count; ++i) {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS)
      return false;
    objects.render_done.push_back(semaphore);
  }
  return true;
}

struct Commands {
  VkDevice device = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer buffer = VK_NULL_HANDLE;

  Commands() = default;
  Commands(const Commands &) = delete;
  Commands &operator=(const Commands &) = delete;
  void release() {
    if (device == VK_NULL_HANDLE)
      return;
    vkDestroyCommandPool(device, pool, nullptr);
    device = VK_NULL_HANDLE;
  }
  ~Commands() { release(); }
};

bool makeCommands(Commands &commands, VkDevice device, std::uint32_t family) {
  commands.device = device;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = family;
  if (vkCreateCommandPool(device, &pool_info, nullptr, &commands.pool) != VK_SUCCESS)
    return false;
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = commands.pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  return vkAllocateCommandBuffers(device, &alloc, &commands.buffer) == VK_SUCCESS;
}

bool recordToPresent(VkCommandBuffer buffer, VkImage image) {
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(buffer, &begin) != VK_SUCCESS)
    return false;
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);
  return vkEndCommandBuffer(buffer) == VK_SUCCESS;
}

int gpuTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  const auto &available = capabilities->available_extensions;
  if (!hasName(available, VK_KHR_SURFACE_EXTENSION_NAME) ||
      !hasName(available, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
    return skip("VK_EXT_headless_surface unavailable");
  if (!hasName(available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) ||
      !hasName(available, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME))
    return skip("VK_KHR_surface_maintenance1 unavailable");
  if (!hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation") ||
      !hasName(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    return blocked("the validation layer and VK_EXT_debug_utils are required");

  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-swapchain-retirement";
  description.required_extensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                     VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
                                     VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                                     VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance with the surface maintenance extensions");
  ValidationRecorder recorder;
  auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("Instance with the surface maintenance extensions");

  auto inventory = queryPhysicalDevices(*instance);
  if (!inventory)
    return blocked("physical device enumeration");
  const SurfaceCreator creator{&createHeadless, nullptr, {}};
  auto surface_a = createSurface(*instance, creator);
  auto surface_c = createSurface(*instance, creator);
  if (!surface_a || !surface_c)
    return blocked("two headless surfaces");

  // A presenting device with the swapchainMaintenance1 feature.
  const PhysicalDeviceCandidate *presenting = nullptr;
  SurfaceSupport support_a;
  std::optional<DevicePlan> fence_plan;
  std::optional<DevicePlan> plain_plan;
  std::optional<PresentationResolution> presentation;
  for (const auto &candidate : *inventory) {
    if (!candidate.capabilities().swapchain_maintenance1_available ||
        candidate.capabilities().swapchain_maintenance1.swapchainMaintenance1 != VK_TRUE)
      continue;
    const auto observed = querySurfaceSupport(*instance, candidate, *surface_a);
    if (!observed || !observed->canPresent())
      continue;
    const auto resolved = resolvePresentation(*observed, {});
    if (!resolved)
      continue;
    DeviceDescription with_fences;
    with_fences.required_extensions = {VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
    with_fences.required_swapchain_maintenance1.swapchainMaintenance1 = VK_TRUE;
    DeviceDescription plain;
    if (!applyPresentation(with_fences, *resolved) || !applyPresentation(plain, *resolved))
      continue;
    auto fence_resolved = resolveDevice(candidate, with_fences);
    auto plain_resolved = resolveDevice(candidate, plain);
    if (!fence_resolved || !plain_resolved)
      continue;
    presenting = &candidate;
    support_a = *observed;
    presentation = *resolved;
    fence_plan = std::move(*fence_resolved);
    plain_plan = std::move(*plain_resolved);
    break;
  }
  if (presenting == nullptr || !fence_plan || !plain_plan || !presentation)
    return skip("no presenting device with the swapchainMaintenance1 feature");

  auto device = createDevice(*instance, *fence_plan);
  auto plain_device = createDevice(*instance, *plain_plan);
  if (!check(device.has_value() && plain_device.has_value(),
             "a Device with and a Device without swapchainMaintenance1"))
    return EXIT_FAILURE;
  check(device->plan()->enabledSwapchainMaintenance1().swapchainMaintenance1 == VK_TRUE &&
            plain_device->plan()->enabledSwapchainMaintenance1().swapchainMaintenance1 == VK_FALSE,
        "the plans record whether the feature is enabled");
  const Queue queue = device->queue("present");
  const Queue plain_queue = plain_device->queue("present");
  if (!check(queue.valid() && plain_queue.valid(), "the present queues exist"))
    return EXIT_FAILURE;

  SwapchainRequest request;
  request.extent = vk::Extent2D{64, 48};
  const auto description_a = resolveSwapchain(support_a, *presentation, request);
  const auto support_c = querySurfaceSupport(*instance, *presenting, *surface_c);
  if (!check(description_a.has_value() && support_c.has_value() && support_c->canPresent(),
             "resolve the first Swapchain and observe the second surface"))
    return EXIT_FAILURE;
  const auto description_c = resolveSwapchain(*support_c, *presentation, request);
  auto window = createSwapchain(*instance, *device, *surface_a, *description_a);
  if (!check(description_c.has_value() && window.has_value(), "the first Swapchain"))
    return EXIT_FAILURE;
  auto plain_window = createSwapchain(*instance, *plain_device, *surface_c, *description_c);
  if (!check(plain_window.has_value(), "a Swapchain on the Device without the feature"))
    return EXIT_FAILURE;

  // A present fence on a Device without the feature is rejected before any native call.
  {
    const std::array<vk::Semaphore, 0> no_waits{};
    VkFence real_fence = VK_NULL_HANDLE;
    const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (check(vkCreateFence(static_cast<VkDevice>(plain_device->nativeHandle()), &fence_info,
                            nullptr, &real_fence) == VK_SUCCESS,
              "a fence on the Device without the feature")) {
      const PresentDescription with_fence{plain_window->images().front(), no_waits,
                                          vk::Fence{real_fence}};
      check(hasError(presentImage(*plain_device, plain_queue, *plain_window, with_fence),
                     SwapchainFrameError::present_fence_not_supported),
            "a present fence without the feature is present_fence_not_supported");
      vkDestroyFence(static_cast<VkDevice>(plain_device->nativeHandle()), real_fence, nullptr);
    }
  }

  const auto native_device = static_cast<VkDevice>(device->nativeHandle());
  Commands commands;
  WindowObjects objects;
  if (!check(
          makeCommands(commands, native_device, static_cast<std::uint32_t>(queue.familyIndex())) &&
              makeObjects(objects, native_device, window->images().size()),
          "native frame objects"))
    return EXIT_FAILURE;

  // One application-owned frame that presents with a present fence of its own.
  const auto runFrame = [&](const Swapchain &chain, WindowObjects &frame_objects) -> bool {
    AcquireDescription acquire;
    acquire.timeout_ns = UINT64_MAX;
    acquire.semaphore = vk::Semaphore{frame_objects.acquire};
    acquire.fence = vk::Fence{frame_objects.acquire_fence};
    const auto acquired = acquireNextImage(*device, chain, acquire);
    if (!acquired.has_value() || !acquired->image.has_value()) {
      check(false, "acquire returns a usable image");
      return false;
    }
    const SwapchainImage image = *acquired->image;
    check(vkWaitForFences(native_device, 1, &frame_objects.acquire_fence, VK_TRUE, UINT64_MAX) ==
              VK_SUCCESS,
          "the acquire fence is signaled");
    vkResetFences(native_device, 1, &frame_objects.acquire_fence);
    vkResetCommandPool(native_device, commands.pool, 0);
    if (!check(recordToPresent(commands.buffer, static_cast<VkImage>(image.nativeImage())),
               "record the frame commands"))
      return false;
    const VkSemaphore render_done =
        frame_objects.render_done[image.index() % frame_objects.render_done.size()];
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame_objects.acquire;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands.buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done;
    if (!check(vkQueueSubmit(static_cast<VkQueue>(queue.nativeHandle()), 1, &submit,
                             frame_objects.submit_fence) == VK_SUCCESS,
               "native submit"))
      return false;
    VkFence present_fence = VK_NULL_HANDLE;
    const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!check(vkCreateFence(native_device, &fence_info, nullptr, &present_fence) == VK_SUCCESS,
               "create a present fence"))
      return false;
    frame_objects.present_fences.push_back(present_fence);
    const std::array<vk::Semaphore, 1> waits{vk::Semaphore{render_done}};
    const auto result = presentImage(*device, queue, chain,
                                     PresentDescription{image, waits, vk::Fence{present_fence}});
    if (!check(result.has_value(), "present with a present fence"))
      return false;
    check(vkWaitForFences(native_device, 1, &frame_objects.submit_fence, VK_TRUE, UINT64_MAX) ==
              VK_SUCCESS,
          "the submission completes");
    vkResetFences(native_device, 1, &frame_objects.submit_fence);
    return true;
  };

  bool frames_ok = true;
  for (std::size_t frame = 0; frame < window->images().size() * 2 && frames_ok; ++frame)
    frames_ok = runFrame(*window, objects);
  check(frames_ok, "frames present with a present fence each");

  // Recreate; the old generation is retired and handed to the queue with its
  // present fences. No wait-idle happens from here to its destruction.
  SwapchainRequest bigger;
  bigger.extent = vk::Extent2D{96, 64};
  auto recreated =
      recreateSwapchain(*instance, *device, *presenting, *surface_a, *window, {}, bigger);
  if (!recreated.has_value() || !recreated->swapchain.has_value()) {
    check(false, "recreateSwapchain");
    return EXIT_FAILURE;
  }
  Swapchain &next = *recreated->swapchain;
  check(window->retired(), "the old Swapchain is retired");

  SwapchainRetirementQueue retirement;
  {
    SwapchainRetirementUses none;
    check(hasError(retirement.retire(*device, *window, none),
                   SwapchainRetirementError::missing_evidence),
          "no evidence and no_pending_present is missing_evidence");
    const std::array<vk::Fence, 1> null_fence{vk::Fence{}};
    SwapchainRetirementUses with_null;
    with_null.present_fences = null_fence;
    check(hasError(retirement.retire(*device, *window, with_null),
                   SwapchainRetirementError::invalid_fence),
          "a null present fence is invalid_fence");
    std::vector<vk::Fence> fences;
    fences.reserve(objects.present_fences.size());
    for (const auto fence : objects.present_fences)
      fences.emplace_back(fence);
    SwapchainRetirementUses foreign;
    foreign.present_fences = fences;
    check(hasError(retirement.retire(*device, *plain_window, foreign),
                   SwapchainRetirementError::parent_mismatch),
          "a Swapchain of another Device is a parent_mismatch");
    check(window->valid() && plain_window->valid(),
          "failed retirements leave the Swapchains with the caller");

    SwapchainRetirementUses uses;
    uses.present_fences = fences;
    if (!check(retirement.retire(*device, *window, uses).has_value(),
               "retire the old Swapchain with its present fences"))
      return EXIT_FAILURE;
    check(!window->valid() && retirement.pendingCount() == 1,
          "the queue owns the Swapchain after a successful retirement");
  }

  // The new generation keeps rendering while the old one is collected by polls.
  WindowObjects next_objects;
  if (!check(makeObjects(next_objects, native_device, next.images().size()),
             "native objects for the new Swapchain"))
    return EXIT_FAILURE;
  bool collected = false;
  bool next_ok = true;
  for (std::size_t attempt = 0; attempt < 2000 && !collected; ++attempt) {
    if (attempt < next.images().size() * 2)
      next_ok = next_ok && runFrame(next, next_objects);
    const auto report = retirement.collect(*device);
    if (!report.has_value()) {
      check(false, "collect observes the present fences");
      break;
    }
    collected = report->pending == 0 && report->collected == 1;
    if (!collected)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(next_ok, "the new Swapchain renders while the old one waits for retirement");
  check(collected && retirement.pendingCount() == 0 && retirement.requireEmpty().has_value(),
        "the old Swapchain is destroyed once its present fences are signaled, with no wait-idle");

  vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
  objects.release();
  next_objects.release();
  commands.release();
  // Destroy in lifetime order: Swapchains, Devices, then Surfaces.
  {
    const auto gone = std::move(next);
  }
  {
    const auto gone = std::move(*plain_window);
  }
  {
    const auto gone = std::move(*device);
  }
  {
    const auto gone = std::move(*plain_device);
  }
  {
    const auto gone = std::move(*surface_a);
  }
  {
    const auto gone = std::move(*surface_c);
  }

  check(recorder.warnings_or_errors.load() == 0, "no validation warnings or errors");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.swapchain_retirement: validation-layer=PASS");
  std::puts("graphics.swapchain_retirement: retirement=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2 || std::string_view{argv[1]} != "gpu")
    return blocked("specify gpu mode");
  return gpuTest();
}

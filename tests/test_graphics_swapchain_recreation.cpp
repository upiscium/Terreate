// #275 (part 2): explicit Swapchain recreation, and independence of several
// Swapchains on one shared Device.
//
// The GPU test drives two windows (two VK_EXT_headless_surface surfaces, no
// window system needed) from one application-owned loop, recreates one
// Swapchain with another extent, and checks that the other keeps running
// untouched. It also checks the zero-extent contract: no Swapchain is created,
// the old one stays usable, and nothing retries by itself. If no device can
// present to a headless surface, or the extension is missing, the test reports
// SKIP, never PASS. The validation layer must be available. No linker or
// platform mechanism is used.
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/presentation.hpp>
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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace terreate::graphics;

const char *test_name = "graphics.swapchain_recreation";
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
        std::fprintf(stderr, "graphics.swapchain_recreation: validation message: %s\n",
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
  std::printf("graphics.swapchain_recreation: recreation=SKIP reason=%s\n", reason);
  return EXIT_SUCCESS;
}

// One window's application-owned native objects: its own acquire semaphore,
// fences and one present semaphore per image index. Nothing is shared between
// windows.
struct WindowSync {
  VkDevice device = VK_NULL_HANDLE;
  VkSemaphore acquire = VK_NULL_HANDLE;
  VkFence acquire_fence = VK_NULL_HANDLE;
  VkFence submit_fence = VK_NULL_HANDLE;
  std::vector<VkSemaphore> render_done{};

  WindowSync() = default;
  WindowSync(const WindowSync &) = delete;
  WindowSync &operator=(const WindowSync &) = delete;
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
  ~WindowSync() { release(); }
};

bool makeSync(WindowSync &sync, VkDevice device, std::size_t image_count) {
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

// The shared Device's native command resources, used by every window in turn.
struct SharedCommands {
  VkDevice device = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;

  SharedCommands() = default;
  SharedCommands(const SharedCommands &) = delete;
  SharedCommands &operator=(const SharedCommands &) = delete;
  void release() {
    if (device == VK_NULL_HANDLE)
      return;
    vkDestroyCommandPool(device, pool, nullptr);
    device = VK_NULL_HANDLE;
  }
  ~SharedCommands() { release(); }
};

bool makeCommands(SharedCommands &shared, VkDevice device, std::uint32_t family) {
  shared.device = device;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = family;
  if (vkCreateCommandPool(device, &pool_info, nullptr, &shared.pool) != VK_SUCCESS)
    return false;
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = shared.pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  return vkAllocateCommandBuffers(device, &alloc, &shared.commands) == VK_SUCCESS;
}

bool recordToPresent(VkCommandBuffer commands, VkImage image) {
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS)
    return false;
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);
  return vkEndCommandBuffer(commands) == VK_SUCCESS;
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
  description.application_name = "terreate-swapchain-recreation";
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
  const SurfaceCreator creator{&createHeadless, nullptr, {}};
  auto surface_a = createSurface(*instance, creator);
  auto surface_b = createSurface(*instance, creator);
  if (!surface_a || !surface_b)
    return blocked("two headless surfaces");

  const PhysicalDeviceCandidate *presenting = nullptr;
  SurfaceSupport support_a;
  for (const auto &candidate : *inventory) {
    const auto observed = querySurfaceSupport(*instance, candidate, *surface_a);
    if (!observed || !observed->canPresent() || !resolvePresentation(*observed, {}))
      continue;
    presenting = &candidate;
    support_a = *observed;
    break;
  }
  if (presenting == nullptr)
    return skip("no device can present to a headless surface");
  const auto support_b = querySurfaceSupport(*instance, *presenting, *surface_b);
  if (!check(support_b.has_value() && support_b->canPresent(), "the second surface presents"))
    return EXIT_FAILURE;

  const auto presentation = resolvePresentation(support_a, {});
  DeviceDescription device_description;
  if (!check(presentation.has_value() &&
                 applyPresentation(device_description, *presentation).has_value(),
             "presentation requirements apply to a DeviceDescription"))
    return EXIT_FAILURE;
  const auto device_plan = resolveDevice(*presenting, device_description);
  if (!check(device_plan.has_value(), "resolveDevice with the presentation requirements"))
    return EXIT_FAILURE;
  auto device = createDevice(*instance, *device_plan);
  if (!check(device.has_value(), "one shared Device for both windows"))
    return EXIT_FAILURE;
  const Queue queue = device->queue("present");
  if (!check(queue.valid(), "the present queue exists"))
    return EXIT_FAILURE;

  SwapchainRequest request;
  request.extent = vk::Extent2D{64, 48};
  const auto description_a = resolveSwapchain(support_a, *presentation, request);
  const auto description_b = resolveSwapchain(*support_b, *presentation, request);
  if (!check(description_a.has_value() && description_b.has_value(), "resolveSwapchain twice"))
    return EXIT_FAILURE;
  auto window_a = createSwapchain(*instance, *device, *surface_a, *description_a);
  auto window_b = createSwapchain(*instance, *device, *surface_b, *description_b);
  if (!check(window_a.has_value() && window_b.has_value(), "an independent Swapchain per window"))
    return EXIT_FAILURE;
  check(window_a->generation() != window_b->generation() && !window_a->correlatedWith(*surface_b) &&
            !window_b->correlatedWith(*surface_a),
        "each window has its own Swapchain, generation and Surface");

  const auto native_device = static_cast<VkDevice>(device->nativeHandle());
  SharedCommands commands;
  if (!check(makeCommands(commands, native_device, static_cast<std::uint32_t>(queue.familyIndex())),
             "native command resources"))
    return EXIT_FAILURE;

  // One application-owned frame on one window's Swapchain.
  const auto runFrame = [&](const Swapchain &chain, WindowSync &sync) -> bool {
    AcquireDescription acquire;
    acquire.timeout_ns = UINT64_MAX;
    acquire.semaphore = vk::Semaphore{sync.acquire};
    acquire.fence = vk::Fence{sync.acquire_fence};
    const auto acquired = acquireNextImage(*device, chain, acquire);
    if (!acquired.has_value() || !acquired->image.has_value()) {
      check(false, "acquire returns a usable image");
      return false;
    }
    const SwapchainImage image = *acquired->image;
    check(vkWaitForFences(native_device, 1, &sync.acquire_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS,
          "the acquire fence is signaled");
    vkResetFences(native_device, 1, &sync.acquire_fence);
    vkResetCommandPool(native_device, commands.pool, 0);
    if (!check(recordToPresent(commands.commands, static_cast<VkImage>(image.nativeImage())),
               "record the frame commands"))
      return false;
    const VkSemaphore render_done = sync.render_done[image.index() % sync.render_done.size()];
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &sync.acquire;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands.commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done;
    if (!check(vkQueueSubmit(static_cast<VkQueue>(queue.nativeHandle()), 1, &submit,
                             sync.submit_fence) == VK_SUCCESS,
               "native submit"))
      return false;
    const std::array<vk::Semaphore, 1> waits{vk::Semaphore{render_done}};
    const auto result = presentImage(*device, queue, chain, PresentDescription{image, waits});
    if (!check(result.has_value(), "present"))
      return false;
    check(vkWaitForFences(native_device, 1, &sync.submit_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS,
          "the submission completes");
    vkResetFences(native_device, 1, &sync.submit_fence);
    return true;
  };

  WindowSync sync_a;
  WindowSync sync_b;
  if (!check(makeSync(sync_a, native_device, window_a->images().size()) &&
                 makeSync(sync_b, native_device, window_b->images().size()),
             "native sync objects per window"))
    return EXIT_FAILURE;

  // Both windows run from the same loop on the shared Device.
  bool both_ok = true;
  for (std::size_t frame = 0; frame < window_a->images().size() * 2 && both_ok; ++frame)
    both_ok = runFrame(*window_a, sync_a) && runFrame(*window_b, sync_b);
  check(both_ok, "both windows render and present from one loop");

  // Zero extent: nothing is created, the old Swapchain stays usable, and there
  // is no retry inside the library.
  {
    SwapchainRequest zero;
    zero.extent = vk::Extent2D{0, 0};
    const auto deferred =
        recreateSwapchain(*instance, *device, *presenting, *surface_a, *window_a, {}, zero);
    check(deferred.has_value() && deferred->status == RecreationStatus::deferred_zero_extent &&
              !deferred->swapchain.has_value(),
          "a zero extent defers the recreation and creates nothing");
    check(!window_a->retired() && runFrame(*window_a, sync_a),
          "the old Swapchain is untouched and still renders after a deferred recreation");
  }

  // Failures before any native creation leave the old Swapchain untouched.
  {
    check(hasError(recreateSwapchain(*instance, *device, *presenting, *surface_b, *window_a, {},
                                     request),
                   SwapchainError::parent_mismatch),
          "an old Swapchain of another Surface is a parent_mismatch");
    PresentationRequest impossible;
    impossible.preferred_formats = {
        vk::SurfaceFormatKHR{vk::Format::eR64Sfloat, vk::ColorSpaceKHR::eSrgbNonlinear}};
    impossible.require_preferred_format = true;
    check(hasError(recreateSwapchain(*instance, *device, *presenting, *surface_a, *window_a,
                                     impossible, request),
                   PresentationError::no_supported_format),
          "an unsatisfiable required format fails before any native creation");
    check(!window_a->retired(), "those failures left the old Swapchain untouched");
  }

  // Recreate window A with another extent; window B must not notice.
  const auto generation_b = window_b->generation();
  const auto images_b = window_b->images().size();
  const auto old_a_generation = window_a->generation();
  SwapchainRequest bigger;
  bigger.extent = vk::Extent2D{96, 64};
  auto recreated =
      recreateSwapchain(*instance, *device, *presenting, *surface_a, *window_a, {}, bigger);
  if (!recreated.has_value() || !recreated->swapchain.has_value()) {
    check(false, "recreateSwapchain with a new extent");
    return EXIT_FAILURE;
  }
  check(recreated->status == RecreationStatus::recreated,
        "the status of a successful recreation is recreated");
  Swapchain &next_a = *recreated->swapchain;
  check(next_a.configuration()->extent.width == 96 && next_a.configuration()->extent.height == 64 &&
            next_a.lineage() == 2 && next_a.generation() != old_a_generation,
        "the new Swapchain has the new extent, lineage 2 and its own generation");
  check(window_a->retired() && !next_a.retired(), "the old Swapchain is retired, the new is not");
  {
    AcquireDescription acquire;
    acquire.semaphore = vk::Semaphore{sync_a.acquire};
    check(hasError(acquireNextImage(*device, *window_a, acquire),
                   SwapchainFrameError::swapchain_retired),
          "nothing can be acquired from the retired Swapchain");
  }
  check(window_b->generation() == generation_b && window_b->images().size() == images_b &&
            !window_b->retired() && window_b->configuration()->extent.width == 64,
        "recreating window A leaves window B's Swapchain untouched");
  check(hasError(
            recreateSwapchain(*instance, *device, *presenting, *surface_a, *window_a, {}, bigger),
            SwapchainError::old_swapchain_retired),
        "a retired Swapchain cannot be recreated twice");

  // The documented rule for a recreation: the old generation's synchronization is
  // reused or destroyed only once the Queue is idle. The new Swapchain gets its own
  // objects; window B keeps its own and continues without any wait on A.
  vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
  sync_a.release();
  WindowSync sync_next_a;
  if (!check(makeSync(sync_next_a, native_device, next_a.images().size()),
             "native sync objects for the new Swapchain"))
    return EXIT_FAILURE;
  bool after_ok = true;
  for (std::size_t frame = 0; frame < 3 * next_a.images().size() && after_ok; ++frame)
    after_ok = runFrame(next_a, sync_next_a) && runFrame(*window_b, sync_b);
  check(after_ok, "the new Swapchain and window B keep rendering from one loop");

  vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
  sync_next_a.release();
  sync_b.release();
  commands.release();
  // Destroy in lifetime order: Swapchains, Device, then Surfaces.
  {
    const auto gone = std::move(*window_a);
  }
  {
    const auto gone = std::move(next_a);
  }
  {
    const auto gone = std::move(*window_b);
  }
  {
    const auto gone = std::move(*device);
  }
  {
    const auto gone = std::move(*surface_a);
  }
  {
    const auto gone = std::move(*surface_b);
  }

  check(recorder.warnings_or_errors.load() == 0, "no validation warnings or errors");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.swapchain_recreation: validation-layer=PASS");
  std::puts("graphics.swapchain_recreation: recreation=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2 || std::string_view{argv[1]} != "gpu")
    return blocked("specify gpu mode");
  return gpuTest();
}

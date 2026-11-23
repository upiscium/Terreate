// #274: explicit swapchain image acquire and present.
//
// `cpu` mode checks the result types. `gpu` mode drives an application-owned
// loop on a VK_EXT_headless_surface (no window system needed): acquire, a
// native submission that waits for the acquire semaphore and signals one
// present semaphore per image index, present, and reuse of each semaphore only
// once its image index is acquired again. If no device can present to a
// headless surface, or the extension is missing, the GPU test reports SKIP,
// never PASS. No linker or platform mechanism is used.
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
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace terreate::graphics;
using Error = SwapchainFrameError;

const char *test_name = "graphics.swapchain_frame.cpu";
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

int cpuTest() {
  check(std::string{swapchain_frame_error_category().name()} ==
                "terreate.graphics.swapchain_frame" &&
            make_error_code(Error::stale_image).message() ==
                "the image does not belong to this Swapchain",
        "error category name and message");

  // A status alone never implies an image: only `image` does.
  const AcquireResult none;
  check(!none.usable() && !none.recreationNeeded() && none.status == AcquireStatus::not_ready,
        "a default acquire result has no image");
  AcquireResult out_of_date;
  out_of_date.status = AcquireStatus::out_of_date;
  check(!out_of_date.usable() && out_of_date.recreationNeeded(),
        "out_of_date has no image and needs recreation");
  AcquireResult timeout;
  timeout.status = AcquireStatus::timeout;
  check(!timeout.usable() && !timeout.recreationNeeded(),
        "a timeout has no image and needs no recreation");
  AcquireResult suboptimal;
  suboptimal.status = AcquireStatus::acquired_suboptimal;
  suboptimal.image = SwapchainImage{};
  check(suboptimal.usable() && suboptimal.recreationNeeded(),
        "suboptimal has an image and needs recreation");
  AcquireResult acquired;
  acquired.status = AcquireStatus::acquired;
  acquired.image = SwapchainImage{};
  check(acquired.usable() && !acquired.recreationNeeded(), "acquired has an image");

  PresentResult presented;
  check(!presented.recreationNeeded(), "presented needs no recreation");
  presented.status = PresentStatus::presented_suboptimal;
  check(presented.recreationNeeded(), "presented_suboptimal needs recreation");
  presented.status = PresentStatus::out_of_date;
  check(presented.recreationNeeded(), "out_of_date needs recreation");

  // The native-to-status mappings (headless surfaces cannot produce every result).
  check(classifyAcquireResult(vk::Result::eSuccess) == AcquireStatus::acquired &&
            classifyAcquireResult(vk::Result::eSuboptimalKHR) ==
                AcquireStatus::acquired_suboptimal &&
            classifyAcquireResult(vk::Result::eErrorOutOfDateKHR) == AcquireStatus::out_of_date &&
            classifyAcquireResult(vk::Result::eTimeout) == AcquireStatus::timeout &&
            classifyAcquireResult(vk::Result::eNotReady) == AcquireStatus::not_ready,
        "every acquire status maps from its native result");
  check(!classifyAcquireResult(vk::Result::eErrorSurfaceLostKHR) &&
            !classifyAcquireResult(vk::Result::eErrorDeviceLost) &&
            !classifyAcquireResult(vk::Result::eErrorOutOfHostMemory),
        "a native failure is not an acquire status");
  check(classifyPresentResult(vk::Result::eSuccess) == PresentStatus::presented &&
            classifyPresentResult(vk::Result::eSuboptimalKHR) ==
                PresentStatus::presented_suboptimal &&
            classifyPresentResult(vk::Result::eErrorOutOfDateKHR) == PresentStatus::out_of_date,
        "every present status maps from its native result");
  check(!classifyPresentResult(vk::Result::eErrorSurfaceLostKHR) &&
            !classifyPresentResult(vk::Result::eErrorDeviceLost) &&
            !classifyPresentResult(vk::Result::eTimeout),
        "a native failure is not a present status");

  // The default AcquireDescription never waits.
  const AcquireDescription description;
  check(description.timeout_ns == 0 && !description.semaphore && !description.fence,
        "the default acquire does not block and names nothing");

  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.swapchain_frame.cpu: cpu=PASS");
  return EXIT_SUCCESS;
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
  std::printf("graphics.swapchain_frame: frames=SKIP reason=%s\n", reason);
  return EXIT_SUCCESS;
}

// The application's own native objects for the loop (the library takes borrowed
// handles; it creates no semaphores or fences).
struct FrameObjects {
  VkDevice device = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  VkSemaphore acquire = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkFence acquire_fence = VK_NULL_HANDLE;
  std::vector<VkSemaphore> render_done{};

  // Destroy the objects while the Device is still alive; the destructor is a
  // safety net only.
  void release() {
    if (device == VK_NULL_HANDLE)
      return;
    for (const auto semaphore : render_done)
      vkDestroySemaphore(device, semaphore, nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyFence(device, acquire_fence, nullptr);
    vkDestroySemaphore(device, acquire, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    device = VK_NULL_HANDLE;
  }
  FrameObjects() = default;
  FrameObjects(const FrameObjects &) = delete;
  FrameObjects &operator=(const FrameObjects &) = delete;
  ~FrameObjects() { release(); }
};

struct FrameSetup {
  VkDevice device = VK_NULL_HANDLE;
  std::uint32_t queue_family = 0;
  std::size_t image_count = 0;
};

bool makeObjects(FrameObjects &objects, const FrameSetup &setup) {
  const VkDevice device = setup.device;
  const std::size_t image_count = setup.image_count;
  objects.device = device;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = setup.queue_family;
  if (vkCreateCommandPool(device, &pool_info, nullptr, &objects.pool) != VK_SUCCESS)
    return false;
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = objects.pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device, &alloc, &objects.commands) != VK_SUCCESS)
    return false;
  const VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  if (vkCreateSemaphore(device, &semaphore_info, nullptr, &objects.acquire) != VK_SUCCESS)
    return false;
  const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(device, &fence_info, nullptr, &objects.fence) != VK_SUCCESS)
    return false;
  if (vkCreateFence(device, &fence_info, nullptr, &objects.acquire_fence) != VK_SUCCESS)
    return false;
  for (std::size_t i = 0; i < image_count; ++i) {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS)
      return false;
    objects.render_done.push_back(semaphore);
  }
  return true;
}

// Record: move the image to PRESENT_SRC (its contents are not needed).
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
  const bool has_validation =
      hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation");
  const bool has_debug_utils =
      hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-swapchain-frame";
  description.required_extensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                     VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
  description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  if (has_validation && has_debug_utils)
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
  for (const auto &candidate : *inventory) {
    const auto observed = querySurfaceSupport(*instance, candidate, *surface);
    if (!observed || !observed->canPresent() || !resolvePresentation(*observed, {}))
      continue;
    presenting = &candidate;
    presenting_support = *observed;
    break;
  }
  if (presenting == nullptr)
    return skip("no device can present to a headless surface");

  const auto presentation = resolvePresentation(presenting_support, {});
  DeviceDescription device_description;
  if (!check(presentation.has_value() &&
                 applyPresentation(device_description, *presentation).has_value(),
             "presentation requirements apply to a DeviceDescription"))
    return EXIT_FAILURE;
  const auto device_plan = resolveDevice(*presenting, device_description);
  if (!check(device_plan.has_value(), "resolveDevice with the presentation requirements"))
    return EXIT_FAILURE;
  auto device = createDevice(*instance, *device_plan);
  if (!check(device.has_value(), "createDevice with VK_KHR_swapchain"))
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

  FrameObjects objects;
  const auto native_device = static_cast<VkDevice>(device->nativeHandle());
  if (!check(makeObjects(objects,
                         FrameSetup{native_device, static_cast<std::uint32_t>(queue.familyIndex()),
                                    first->images().size()}),
             "native frame objects"))
    return EXIT_FAILURE;

  // Description errors come before any native call.
  {
    check(hasError(acquireNextImage(*device, *first, AcquireDescription{}),
                   Error::invalid_description),
          "an acquire without a semaphore or fence is invalid_description");
    check(hasError(presentImage(*device, queue, *first, PresentDescription{}), Error::stale_image),
          "presenting an empty image is stale_image");
  }

  // The application-owned loop: more frames than images, so every image index
  // is acquired again and its present semaphore is reused only then.
  const std::size_t frame_count = first->images().size() * 3;
  std::set<std::uint32_t> seen;
  std::size_t presented = 0;
  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    AcquireDescription acquire;
    acquire.timeout_ns = UINT64_MAX;
    acquire.semaphore = vk::Semaphore{objects.acquire};
    acquire.fence = vk::Fence{objects.acquire_fence};
    const auto acquired = acquireNextImage(*device, *first, acquire);
    if (!acquired.has_value() || !acquired->image.has_value()) {
      check(false, "acquire returns a usable image");
      break;
    }
    const SwapchainImage image = *acquired->image;
    // The acquire fence is signaled once the image is ready; reset it for reuse.
    check(vkWaitForFences(native_device, 1, &objects.acquire_fence, VK_TRUE, UINT64_MAX) ==
              VK_SUCCESS,
          "the acquire fence is signaled");
    vkResetFences(native_device, 1, &objects.acquire_fence);
    if (frame == 0) {
      const std::array<vk::Semaphore, 1> null_wait{vk::Semaphore{}};
      check(hasError(presentImage(*device, queue, *first, PresentDescription{image, null_wait}),
                     Error::invalid_description),
            "a null wait semaphore is invalid_description");
    }
    check(first->owns(image) && (acquired->status == AcquireStatus::acquired ||
                                 acquired->status == AcquireStatus::acquired_suboptimal),
          "the acquired image belongs to this Swapchain");
    seen.insert(image.index());

    // Render (here: only the layout transition), waiting for the acquire semaphore.
    vkResetCommandPool(native_device, objects.pool, 0);
    if (!check(recordToPresent(objects.commands, static_cast<VkImage>(image.nativeImage())),
               "record the frame commands")) {
      break;
    }
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &objects.acquire;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &objects.commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &objects.render_done[image.index()];
    if (!check(vkQueueSubmit(static_cast<VkQueue>(queue.nativeHandle()), 1, &submit,
                             objects.fence) == VK_SUCCESS,
               "native submit")) {
      break;
    }

    const std::array<vk::Semaphore, 1> waits{vk::Semaphore{objects.render_done[image.index()]}};
    const auto result = presentImage(*device, queue, *first, PresentDescription{image, waits});
    if (!result.has_value()) {
      check(false, "present succeeds");
      break;
    }
    check(result->status == PresentStatus::presented ||
              result->status == PresentStatus::presented_suboptimal,
          "present reports presented or suboptimal");
    ++presented;
    // The acquire semaphore is reused by the next acquire only after the
    // submission that waited on it has completed.
    check(vkWaitForFences(native_device, 1, &objects.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS,
          "the submission completes");
    vkResetFences(native_device, 1, &objects.fence);
  }
  check(presented == frame_count, "every frame was presented");
  check(!seen.empty() && seen.size() <= first->images().size(),
        "acquired indices stay inside the image list");

  // A retired Swapchain cannot acquire; an image of another generation is stale.
  {
    auto next_description = *swapchain_description;
    next_description.old_swapchain = &*first;
    auto second = createSwapchain(*instance, *device, *surface, next_description);
    if (check(second.has_value(), "recreate with an old Swapchain")) {
      AcquireDescription acquire;
      acquire.semaphore = vk::Semaphore{objects.acquire};
      check(hasError(acquireNextImage(*device, *first, acquire), Error::swapchain_retired),
            "acquiring from a retired Swapchain is swapchain_retired");
      const std::array<vk::Semaphore, 0> no_waits{};
      check(hasError(presentImage(*device, queue, *second,
                                  PresentDescription{first->images()[0], no_waits}),
                     Error::stale_image),
            "an image of the old generation is stale_image on the new Swapchain");
    }
  }

  vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
  objects.release();
  // Destroy in lifetime order: native objects, Swapchain, Device, then Surface.
  {
    const auto gone = std::move(*first);
  }
  {
    const auto gone = std::move(*device);
  }
  {
    const auto gone = std::move(*surface);
  }

  if (has_validation && has_debug_utils) {
    check(recorder.warnings_or_errors.load() == 0, "no validation warnings or errors");
    std::puts("graphics.swapchain_frame: validation-layer=PASS");
  } else {
    std::puts("graphics.swapchain_frame: validation-layer=SKIP "
              "reason=VK_LAYER_KHRONOS_validation or VK_EXT_debug_utils unavailable");
  }
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.swapchain_frame: frames=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify cpu or gpu mode");
  const std::string_view mode{argv[1]};
  if (mode == "cpu")
    return cpuTest();
  if (mode == "gpu") {
    test_name = "graphics.swapchain_frame";
    return gpuTest();
  }
  return blocked("unknown mode");
}

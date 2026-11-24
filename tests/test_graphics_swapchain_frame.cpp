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
#include <terreate/graphics/synchronization.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
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
        std::fprintf(stderr, "graphics.swapchain_frame: validation message: %s\n",
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
  std::printf("graphics.swapchain_frame: frames=SKIP reason=%s\n", reason);
  return EXIT_SUCCESS;
}

// The application's objects for the loop. Semaphores and fences are Terreate's
// Fence and Semaphore owners; the command pool stays native (no Terreate
// command-pool use is needed for this test's one raw barrier).
struct FrameObjects {
  VkDevice device = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  std::optional<Semaphore> acquire{};
  std::optional<Fence> fence{};
  std::optional<Fence> acquire_fence{};
  std::vector<Semaphore> render_done{};

  // Destroy the objects while the Device is still alive; the destructor is a
  // safety net only.
  void release() {
    render_done.clear();
    fence.reset();
    acquire_fence.reset();
    acquire.reset();
    if (device == VK_NULL_HANDLE)
      return;
    vkDestroyCommandPool(device, pool, nullptr);
    device = VK_NULL_HANDLE;
  }
  FrameObjects() = default;
  FrameObjects(const FrameObjects &) = delete;
  FrameObjects &operator=(const FrameObjects &) = delete;
  ~FrameObjects() { release(); }
};

struct FrameSetup {
  const Device *device = nullptr;
  std::uint32_t queue_family = 0;
  std::size_t image_count = 0;
};

bool makeObjects(FrameObjects &objects, const FrameSetup &setup) {
  const auto device = static_cast<VkDevice>(setup.device->nativeHandle());
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
  auto acquire = createSemaphore(*setup.device, {});
  auto fence = createFence(*setup.device, {});
  auto acquire_fence = createFence(*setup.device, {});
  if (!acquire || !fence || !acquire_fence)
    return false;
  objects.acquire.emplace(std::move(*acquire));
  objects.fence.emplace(std::move(*fence));
  objects.acquire_fence.emplace(std::move(*acquire_fence));
  for (std::size_t i = 0; i < setup.image_count; ++i) {
    auto semaphore = createSemaphore(*setup.device, {});
    if (!semaphore)
      return false;
    objects.render_done.push_back(std::move(*semaphore));
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
  if (!check(
          makeObjects(objects, FrameSetup{&*device, static_cast<std::uint32_t>(queue.familyIndex()),
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

  // One application-owned frame on `chain`: acquire (semaphore and fence), a
  // native submission that waits for the acquire semaphore and signals the
  // present semaphore of this image index, present, and wait for the submission
  // before the acquire semaphore is reused. `slot_offset` selects which
  // present semaphore a given image index uses.
  const auto runFrame = [&](const Swapchain &chain, bool probe_null_wait,
                            std::set<std::uint32_t> *seen_indices) -> bool {
    AcquireDescription acquire;
    acquire.timeout_ns = UINT64_MAX;
    acquire.semaphore = objects.acquire->nativeHandle();
    acquire.fence = objects.acquire_fence->nativeHandle();
    const auto acquired = acquireNextImage(*device, chain, acquire);
    if (!acquired.has_value() || !acquired->image.has_value()) {
      check(false, "acquire returns a usable image");
      return false;
    }
    const SwapchainImage image = *acquired->image;
    // The acquire fence is signaled once the image is ready; reset it for reuse.
    check(objects.acquire_fence->wait(UINT64_MAX).value_or(WaitStatus::timeout) ==
              WaitStatus::satisfied,
          "the acquire fence is signaled");
    check(objects.acquire_fence->reset().has_value(), "reset the acquire fence");
    if (probe_null_wait) {
      const std::array<vk::Semaphore, 1> null_wait{vk::Semaphore{}};
      check(hasError(presentImage(*device, queue, chain, PresentDescription{image, null_wait}),
                     Error::invalid_description),
            "a null wait semaphore is invalid_description");
    }
    check(chain.owns(image) && (acquired->status == AcquireStatus::acquired ||
                                acquired->status == AcquireStatus::acquired_suboptimal),
          "the acquired image belongs to this Swapchain");
    if (seen_indices != nullptr)
      seen_indices->insert(image.index());

    // Render (here: only the layout transition), waiting for the acquire semaphore.
    vkResetCommandPool(native_device, objects.pool, 0);
    if (!check(recordToPresent(objects.commands, static_cast<VkImage>(image.nativeImage())),
               "record the frame commands"))
      return false;
    const Semaphore &render_done_semaphore =
        objects.render_done[image.index() % objects.render_done.size()];
    const auto render_done = static_cast<VkSemaphore>(render_done_semaphore.nativeHandle());
    const auto acquire_native = static_cast<VkSemaphore>(objects.acquire->nativeHandle());
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquire_native;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &objects.commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done;
    if (!check(vkQueueSubmit(static_cast<VkQueue>(queue.nativeHandle()), 1, &submit,
                             static_cast<VkFence>(objects.fence->nativeHandle())) == VK_SUCCESS,
               "native submit"))
      return false;

    const std::array<vk::Semaphore, 1> waits{render_done_semaphore.nativeHandle()};
    const auto result = presentImage(*device, queue, chain, PresentDescription{image, waits});
    if (!result.has_value()) {
      check(false, "present succeeds");
      return false;
    }
    check(result->status == PresentStatus::presented ||
              result->status == PresentStatus::presented_suboptimal,
          "present reports presented or suboptimal");
    // The acquire semaphore is reused by the next acquire only after the
    // submission that waited on it has completed.
    check(objects.fence->wait(UINT64_MAX).value_or(WaitStatus::timeout) == WaitStatus::satisfied,
          "the submission completes");
    check(objects.fence->reset().has_value(), "reset the submission fence");
    return true;
  };

  // The loop: more frames than images, so every image index is acquired again
  // and its present semaphore is reused only then (the documented rule).
  const std::size_t frame_count = first->images().size() * 3;
  std::set<std::uint32_t> seen;
  std::size_t presented = 0;
  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    if (!runFrame(*first, frame == 0, &seen))
      break;
    ++presented;
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
      acquire.semaphore =
          objects.acquire.has_value() ? objects.acquire->nativeHandle() : vk::Semaphore{};
      check(hasError(acquireNextImage(*device, *first, acquire), Error::swapchain_retired),
            "acquiring from a retired Swapchain is swapchain_retired");
      const std::array<vk::Semaphore, 0> no_waits{};
      check(hasError(presentImage(*device, queue, *second,
                                  PresentDescription{first->images()[0], no_waits}),
                     Error::stale_image),
            "an image of the old generation is stale_image on the new Swapchain");
      // The documented rule for a recreation: the old generation's present
      // semaphores are reused for the new Swapchain only once the Queue is idle
      // (the same-index proof does not carry over to a retired Swapchain).
      vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
      std::set<std::uint32_t> second_seen;
      std::size_t second_presented = 0;
      for (std::size_t frame = 0; frame < second->images().size() * 2; ++frame) {
        if (!runFrame(*second, false, &second_seen))
          break;
        ++second_presented;
      }
      check(second_presented == second->images().size() * 2,
            "frames run on the new Swapchain with the old semaphores after a queue idle");
      // Destroying a Swapchain needs its presented images to be done with: make the
      // Queue idle before `second` goes out of scope.
      vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
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

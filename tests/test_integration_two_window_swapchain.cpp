// #351: two real windows, each with an independent Surface and Swapchain, driven
// from one application-owned loop on one shared Device.
//
// The test needs a window system (DISPLAY or WAYLAND_DISPLAY on Linux) and
// reports SKIP, never PASS, without one, when the Vulkan loader lacks the
// surface extensions GLFW needs, or when no device can present to both windows.
// It checks: both windows render and present; resizing one window and
// recreating its Swapchain leaves the other untouched; the second window keeps
// rendering after the first is marked to close and its Swapchain, Surface and
// Window are destroyed (the close request that a window manager sends is not
// injected here: GLFW offers no portable way to send it, so only the
// destruction of one window is tested); and the contract that does not prevent a later multi-Device
// application (a Swapchain belongs to its own Device, a second Device refuses
// it). Resizing uses the GLFW window handle (the escape hatch), like the
// platform tests. The validation layer must be available. No linker or platform
// mechanism is used.
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/presentation.hpp>
#include <terreate/graphics/surface.hpp>
#include <terreate/graphics/swapchain.hpp>
#include <terreate/graphics/swapchain_frame.hpp>
#include <terreate/integration/platform_graphics/surface.hpp>
#include <terreate/platform/events.hpp>
#include <terreate/platform/runtime.hpp>
#include <terreate/platform/window.hpp>

#include <GLFW/glfw3.h>

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
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace integration = terreate::integration::platform_graphics;
namespace graphics = terreate::graphics;
namespace platform = terreate::platform;

const char *test_name = "integration.two_window_swapchain";
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

int skip(const char *reason) {
  std::printf("integration.two_window_swapchain: two-window=SKIP reason=%s\n", reason);
  return EXIT_SUCCESS;
}

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
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
        std::fprintf(stderr, "integration.two_window_swapchain: validation message: %s\n",
                     event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

// One window's application-owned native objects.
struct WindowObjects {
  VkDevice device = VK_NULL_HANDLE;
  VkSemaphore acquire = VK_NULL_HANDLE;
  VkFence acquire_fence = VK_NULL_HANDLE;
  VkFence submit_fence = VK_NULL_HANDLE;
  std::vector<VkSemaphore> render_done{};

  WindowObjects() = default;
  WindowObjects(const WindowObjects &) = delete;
  WindowObjects &operator=(const WindowObjects &) = delete;
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

// What one frame reported, so the test can tell a clean frame from one that asks
// for a recreation (the real out_of_date / suboptimal path of a resized window).
struct FrameOutcome {
  bool ok = false;
  bool recreation_needed = false;
  // Which status asked for it, for the log (empty when none did).
  const char *reason = "";
};

int nativeTest() {
  const char *display = std::getenv("DISPLAY");
  const char *wayland = std::getenv("WAYLAND_DISPLAY");
  const bool has_display =
      (display != nullptr && *display != '\0') || (wayland != nullptr && *wayland != '\0');
#if defined(_WIN32) || defined(__APPLE__)
  const bool needs_display_variable = false;
#else
  const bool needs_display_variable = true;
#endif
  if (needs_display_variable && !has_display)
    return skip("no DISPLAY or WAYLAND_DISPLAY");

  auto runtime = platform::createRuntime();
  if (!runtime)
    return blocked("automatic-backend Runtime with a configured display");
  auto window_a =
      platform::createWindow(*runtime, {.width = 320, .height = 240, .title = "terreate-a"});
  auto window_b =
      platform::createWindow(*runtime, {.width = 240, .height = 160, .title = "terreate-b"});
  if (!check(window_a.has_value() && window_b.has_value(), "two Windows can exist together"))
    return EXIT_FAILURE;
  check(window_a->id() != window_b->id() && window_a->nativeHandle() != window_b->nativeHandle(),
        "the two Windows have different identities");

  const auto extensions = runtime->requiredInstanceExtensions();
  if (!extensions)
    return skip("GLFW names no surface extensions");
  const auto capabilities = graphics::queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  for (const auto &name : *extensions)
    if (!hasName(capabilities->available_extensions, name))
      return skip("the Vulkan loader lacks an extension GLFW needs");
  if (!hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation") ||
      !hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    return blocked("the validation layer and VK_EXT_debug_utils are required");

  graphics::InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-two-window";
  description.required_extensions = *extensions;
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = graphics::DebugUtilsMode::required;
  const auto plan = graphics::resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance with GLFW's extensions");
  ValidationRecorder recorder;
  auto instance = graphics::createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("Vulkan Instance with GLFW's extensions");

  auto surface_a = integration::createWindowSurface(*instance, *runtime, *window_a);
  auto surface_b = integration::createWindowSurface(*instance, *runtime, *window_b);
  if (!check(surface_a.has_value() && surface_b.has_value(), "an independent Surface per Window"))
    return EXIT_FAILURE;
  check(surface_a->nativeSurface() != surface_b->nativeSurface(),
        "the two Surfaces are different native objects");

  auto inventory = graphics::queryPhysicalDevices(*instance);
  if (!inventory)
    return blocked("physical device enumeration");
  const graphics::PhysicalDeviceCandidate *presenting = nullptr;
  graphics::SurfaceSupport support_a;
  graphics::SurfaceSupport support_b;
  for (const auto &candidate : *inventory) {
    const auto observed_a = graphics::querySurfaceSupport(*instance, candidate, *surface_a);
    const auto observed_b = graphics::querySurfaceSupport(*instance, candidate, *surface_b);
    if (!observed_a || !observed_b || !observed_a->canPresent() || !observed_b->canPresent() ||
        !graphics::resolvePresentation(*observed_a, {}))
      continue;
    presenting = &candidate;
    support_a = *observed_a;
    support_b = *observed_b;
    break;
  }
  if (presenting == nullptr)
    return skip("no device can present to both windows");

  const auto presentation = graphics::resolvePresentation(support_a, {});
  graphics::DeviceDescription device_description;
  if (!check(presentation.has_value() &&
                 graphics::applyPresentation(device_description, *presentation).has_value(),
             "presentation requirements apply to a DeviceDescription"))
    return EXIT_FAILURE;
  const auto device_plan = graphics::resolveDevice(*presenting, device_description);
  if (!check(device_plan.has_value(), "resolveDevice with the presentation requirements"))
    return EXIT_FAILURE;
  auto device = graphics::createDevice(*instance, *device_plan);
  if (!check(device.has_value(), "one shared Device for both windows"))
    return EXIT_FAILURE;
  const graphics::Queue queue = device->queue("present");
  if (!check(queue.valid(), "the present queue exists"))
    return EXIT_FAILURE;

  const auto make_swapchain =
      [&](const platform::Window &window, const graphics::Surface &surface,
          const graphics::SurfaceSupport &support) -> std::optional<graphics::Swapchain> {
    const auto size = window.framebufferSize();
    if (!size)
      return std::nullopt;
    graphics::SwapchainRequest request;
    request.extent = vk::Extent2D{size->width, size->height};
    const auto resolved = graphics::resolveSwapchain(support, *presentation, request);
    if (!resolved)
      return std::nullopt;
    auto created = graphics::createSwapchain(*instance, *device, surface, *resolved);
    if (!created)
      return std::nullopt;
    return std::move(*created);
  };
  auto swapchain_a = make_swapchain(*window_a, *surface_a, support_a);
  auto swapchain_b = make_swapchain(*window_b, *surface_b, support_b);
  if (!check(swapchain_a.has_value() && swapchain_b.has_value(),
             "an independent Swapchain per Window"))
    return EXIT_FAILURE;
  check(swapchain_a->generation() != swapchain_b->generation() &&
            swapchain_a->correlatedWith(*surface_a) && !swapchain_a->correlatedWith(*surface_b) &&
            swapchain_b->correlatedWith(*surface_b) && !swapchain_b->correlatedWith(*surface_a),
        "each Swapchain belongs to its own Surface and has its own generation");

  const auto native_device = static_cast<VkDevice>(device->nativeHandle());
  Commands commands;
  if (!check(makeCommands(commands, native_device, static_cast<std::uint32_t>(queue.familyIndex())),
             "native command resources"))
    return EXIT_FAILURE;
  WindowObjects objects_a;
  WindowObjects objects_b;
  if (!check(makeObjects(objects_a, native_device, swapchain_a->images().size()) &&
                 makeObjects(objects_b, native_device, swapchain_b->images().size()),
             "native objects per window"))
    return EXIT_FAILURE;

  // One application-owned frame of one window. A status that needs a
  // recreation (out_of_date, suboptimal) is reported to the caller.
  const auto runFrame = [&](const graphics::Swapchain &chain,
                            WindowObjects &objects) -> FrameOutcome {
    FrameOutcome outcome;
    graphics::AcquireDescription acquire;
    acquire.timeout_ns = UINT64_MAX;
    acquire.semaphore = vk::Semaphore{objects.acquire};
    acquire.fence = vk::Fence{objects.acquire_fence};
    const auto acquired = graphics::acquireNextImage(*device, chain, acquire);
    if (!acquired.has_value())
      return outcome;
    outcome.recreation_needed = acquired->recreationNeeded();
    if (outcome.recreation_needed)
      outcome.reason = acquired->status == graphics::AcquireStatus::out_of_date
                           ? "acquire:out_of_date"
                           : "acquire:acquired_suboptimal";
    if (!acquired->image.has_value()) {
      // out_of_date: nothing was acquired; recreate before the next frame.
      outcome.ok = acquired->status == graphics::AcquireStatus::out_of_date;
      return outcome;
    }
    const graphics::SwapchainImage image = *acquired->image;
    if (vkWaitForFences(native_device, 1, &objects.acquire_fence, VK_TRUE, UINT64_MAX) !=
        VK_SUCCESS)
      return outcome;
    vkResetFences(native_device, 1, &objects.acquire_fence);
    vkResetCommandPool(native_device, commands.pool, 0);
    if (!recordToPresent(commands.buffer, static_cast<VkImage>(image.nativeImage())))
      return outcome;
    const VkSemaphore render_done = objects.render_done[image.index() % objects.render_done.size()];
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &objects.acquire;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands.buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done;
    if (vkQueueSubmit(static_cast<VkQueue>(queue.nativeHandle()), 1, &submit,
                      objects.submit_fence) != VK_SUCCESS)
      return outcome;
    const std::array<vk::Semaphore, 1> waits{vk::Semaphore{render_done}};
    const auto presented =
        graphics::presentImage(*device, queue, chain, graphics::PresentDescription{image, waits});
    const bool waited =
        vkWaitForFences(native_device, 1, &objects.submit_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    vkResetFences(native_device, 1, &objects.submit_fence);
    if (!presented.has_value() || !waited)
      return outcome;
    outcome.ok = true;
    if (presented->recreationNeeded() && !outcome.recreation_needed)
      outcome.reason = presented->status == graphics::PresentStatus::out_of_date
                           ? "present:out_of_date"
                           : "present:presented_suboptimal";
    outcome.recreation_needed = outcome.recreation_needed || presented->recreationNeeded();
    return outcome;
  };

  // Both windows run from one loop, events processed in between.
  bool both_ok = true;
  for (std::size_t frame = 0; frame < 6 && both_ok; ++frame) {
    if (!check(runtime->pollEvents().has_value(), "pollEvents"))
      return EXIT_FAILURE;
    both_ok = runFrame(*swapchain_a, objects_a).ok && runFrame(*swapchain_b, objects_b).ok;
  }
  check(both_ok, "both windows render and present from one loop");

  // Resize window A only; recreate only A's Swapchain. B must not notice.
  const auto generation_b = swapchain_b->generation();
  const auto extent_b = swapchain_b->configuration()->extent;
  // Drop events that arrived while the windows were created and shown, so a stale
  // resize event is not taken for the one asked for below.
  if (!check(runtime->pollEvents().has_value(), "drain the creation events"))
    return EXIT_FAILURE;
  const auto old_size_a = window_a->framebufferSize();
  if (!check(old_size_a.has_value(), "window A framebuffer size before the resize"))
    return EXIT_FAILURE;
  glfwSetWindowSize(window_a->nativeHandle(), 400, 300);
  bool resize_seen = false;
  bool resize_seen_on_b = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!(resize_seen && window_a->framebufferSize().value_or(*old_size_a) != *old_size_a) &&
         std::chrono::steady_clock::now() < deadline) {
    const auto batch = runtime->waitEvents(std::chrono::milliseconds(100));
    if (!batch)
      break;
    for (const auto &event : batch->events) {
      if (std::holds_alternative<platform::FramebufferResized>(event.data)) {
        resize_seen = resize_seen || event.window == window_a->id();
        resize_seen_on_b = resize_seen_on_b || event.window == window_b->id();
      }
    }
  }
  if (!check(resize_seen && window_a->framebufferSize().value_or(*old_size_a) != *old_size_a,
             "window A reports its resize as an event and its framebuffer size changed"))
    return EXIT_FAILURE;
  check(!resize_seen_on_b, "window B reports no resize");

  // The application sees what the resized window does: out_of_date or
  // suboptimal on the next frames, or neither. Whether the platform reports it is
  // platform-dependent, so it is logged, not asserted (a present that returned
  // out_of_date is not given another turn: its present semaphore is not assumed
  // consumed, and the old objects are released only after a queue idle below).
  bool recreation_requested = false;
  const char *recreation_reason = "none";
  for (std::size_t frame = 0; frame < 4 && !recreation_requested; ++frame) {
    const auto outcome = runFrame(*swapchain_a, objects_a);
    recreation_requested = outcome.recreation_needed;
    if (recreation_requested)
      recreation_reason = outcome.reason;
    check(outcome.ok, "window A frames still complete after its resize");
  }
  std::printf("integration.two_window_swapchain: observed-recreation-request=%s (%s)\n",
              recreation_requested ? "yes" : "no", recreation_reason);

  const auto new_size = window_a->framebufferSize();
  if (!check(new_size.has_value(), "window A framebuffer size"))
    return EXIT_FAILURE;
  graphics::SwapchainRequest bigger;
  bigger.extent = vk::Extent2D{new_size->width, new_size->height};
  auto recreated = graphics::recreateSwapchain(*instance, *device, *presenting, *surface_a,
                                               *swapchain_a, {}, bigger);
  if (!recreated.has_value() || !recreated->swapchain.has_value()) {
    check(false, "recreateSwapchain for the resized window");
    return EXIT_FAILURE;
  }
  graphics::Swapchain &next_a = *recreated->swapchain;
  check(next_a.configuration()->extent.width == new_size->width &&
            next_a.configuration()->extent.height == new_size->height,
        "the new Swapchain of window A has the resized extent");
  check(swapchain_b->generation() == generation_b && !swapchain_b->retired() &&
            swapchain_b->configuration()->extent.width == extent_b.width &&
            swapchain_b->configuration()->extent.height == extent_b.height,
        "recreating window A leaves window B's Swapchain untouched");

  // The old generation's synchronization goes with a queue idle here: the
  // documented conservative path (the retirement queue needs present fences).
  vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
  objects_a.release();
  WindowObjects objects_next_a;
  if (!check(makeObjects(objects_next_a, native_device, next_a.images().size()),
             "native objects for the new Swapchain"))
    return EXIT_FAILURE;
  bool after_resize_ok = true;
  for (std::size_t frame = 0; frame < 6 && after_resize_ok; ++frame) {
    after_resize_ok = runFrame(next_a, objects_next_a).ok && runFrame(*swapchain_b, objects_b).ok;
  }
  check(after_resize_ok, "the resized window and window B keep rendering from one loop");

  // The multi-Device contract: a Swapchain belongs to its own Device, a second
  // Device refuses it.
  {
    auto second_device = graphics::createDevice(*instance, *device_plan);
    if (check(second_device.has_value(), "a second Device on the same physical device")) {
      const graphics::Queue second_queue = second_device->queue("present");
      check(second_queue.valid() && !swapchain_b->correlatedWith(*second_device) &&
                swapchain_b->correlatedWith(*device),
            "a Swapchain belongs only to the Device it was created on");
      const std::array<vk::Semaphore, 0> no_waits{};
      check(hasError(graphics::presentImage(
                         *second_device, second_queue, *swapchain_b,
                         graphics::PresentDescription{swapchain_b->images().front(), no_waits}),
                     graphics::SwapchainFrameError::parent_mismatch),
            "a second Device refuses a Swapchain of the first");
    }
  }

  // Close window B: its Swapchain, Surface and Window go (queue idle first),
  // and window A keeps rendering.
  check(window_b->setShouldClose(true).has_value() && window_b->shouldClose().value_or(false),
        "window B is marked to close");
  vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
  objects_b.release();
  {
    const auto gone = std::move(*swapchain_b);
  }
  {
    const auto gone = std::move(*surface_b);
  }
  {
    const auto gone = std::move(*window_b);
  }
  bool after_close_ok = true;
  for (std::size_t frame = 0; frame < 6 && after_close_ok; ++frame) {
    if (!check(runtime->pollEvents().has_value(), "pollEvents after closing window B"))
      return EXIT_FAILURE;
    after_close_ok = runFrame(next_a, objects_next_a).ok;
  }
  check(after_close_ok, "window A keeps rendering after window B is closed and destroyed");

  vkQueueWaitIdle(static_cast<VkQueue>(queue.nativeHandle()));
  objects_next_a.release();
  commands.release();
  // Destroy in lifetime order: Swapchains, Device, Surfaces, Windows (Runtime last).
  {
    const auto gone = std::move(*swapchain_a);
  }
  {
    const auto gone = std::move(next_a);
  }
  {
    const auto gone = std::move(*device);
  }
  {
    const auto gone = std::move(*surface_a);
  }
  {
    const auto gone = std::move(*window_a);
  }
  {
    // The Instance goes before the count, so a message from its destruction counts.
    const auto gone = std::move(*instance);
  }

  check(recorder.warnings_or_errors.load() == 0, "no validation warnings or errors");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("integration.two_window_swapchain: validation-layer=PASS");
  std::puts("integration.two_window_swapchain: two-window=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2 || std::string_view{argv[1]} != "native")
    return blocked("specify native mode");
  return nativeTest();
}

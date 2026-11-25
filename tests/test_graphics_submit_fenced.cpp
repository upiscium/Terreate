// fenceCompletion, timelineCompletion and submitFenced: the builders put every argument in
// the right place, submitFenced makes one submit() with the fence as native fence and
// completion condition, and a resource retired with its receipt is collected once the fence
// is signaled.
//
// GPU test on a headless Instance (no window system). If no device has the timeline
// semaphore and synchronization2 features, the test reports SKIP, never PASS. The validation
// layer must be available. No linker or platform mechanism is used.
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/completion.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/synchronization.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace terreate::graphics;

const char *test_name = "graphics.submit_fenced";
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
        std::fprintf(stderr, "graphics.submit_fenced: validation message: %s\n",
                     event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

DeviceQueueRequest graphicsQueue() {
  DeviceQueueRequest request;
  request.caller_id = "main";
  request.strength = RequirementStrength::required;
  request.required_flags = vk::QueueFlagBits::eGraphics;
  return request;
}

int skip(const char *reason) {
  std::printf("graphics.submit_fenced: submit_fenced=SKIP reason=%s\n", reason);
  return EXIT_SUCCESS;
}

int exercise(const Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             ValidationRecorder &recorder) {
  const Queue queue = device.queue("main");
  if (!queue.valid())
    return blocked("graphics Queue");
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("Allocator");

  int domain = 0;
  const CompletionIdentity identity{&domain, 7};

  // The identity is valid iff the domain is set and the serial is not zero.
  check(identity.valid() && !CompletionIdentity{nullptr, 7}.valid() &&
            !CompletionIdentity{&domain, 0}.valid(),
        "an identity is valid iff its domain is set and its serial is not zero");

  auto fence = createFence(device, {});
  auto timeline = createSemaphore(device, {.kind = SubmissionSemaphoreKind::timeline});
  if (!fence || !timeline)
    return blocked("Fence and timeline Semaphore");

  // The builders put every argument in the right member; requested == effective.
  const CompletionDescription by_fence = fenceCompletion(device, queue, *fence, identity);
  const auto *fence_condition = std::get_if<FenceCompletionCondition>(&by_fence.condition);
  check(by_fence.device == &device && by_fence.queue.nativeHandle() == queue.nativeHandle() &&
            by_fence.requested == identity && by_fence.effective == identity &&
            fence_condition != nullptr && fence_condition->fence == fence->nativeHandle(),
        "fenceCompletion maps the Device, Queue, identity and Fence");
  const CompletionDescription by_timeline =
      timelineCompletion(device, queue, *timeline, 42, identity);
  const auto *timeline_condition = std::get_if<TimelineCompletionCondition>(&by_timeline.condition);
  check(by_timeline.device == &device && by_timeline.requested == identity &&
            by_timeline.effective == identity && timeline_condition != nullptr &&
            timeline_condition->semaphore == timeline->nativeHandle() &&
            timeline_condition->signal_value == 42,
        "timelineCompletion maps the Device, identity, Semaphore and value");

  // submitFenced: an empty command buffer, one submission, the fence as the condition.
  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  auto pool = createCommandPool(device, pool_description);
  if (!pool)
    return blocked("CommandPool");
  auto command = pool->allocatePrimary();
  if (!command || !command->begin() || !command->end())
    return blocked("an empty CommandBuffer");
  const std::array<const CommandBuffer *, 1> buffers{&*command};

  // A resource whose lifetime the submission covers, retired with the receipt below.
  BufferDescription buffer_description;
  buffer_description.size = 64;
  buffer_description.usage = vk::BufferUsageFlagBits::eTransferDst;
  auto resource = createBuffer(*allocator, buffer_description);
  if (!resource)
    return blocked("test Buffer");

  const auto receipt = submitFenced(device, queue, *fence, identity, {.command_buffers = buffers});
  if (!check(bool(receipt), "submitFenced submits"))
    return EXIT_FAILURE;
  const auto *receipt_condition =
      std::get_if<FenceCompletionCondition>(&receipt->completion().condition);
  check(receipt_condition != nullptr && receipt_condition->fence == fence->nativeHandle() &&
            receipt->completion().requested == identity,
        "the receipt's condition is the given fence and identity");

  RetirementQueue retirement;
  const std::array<SubmissionReceipt, 1> uses{*receipt};
  check(bool(retirement.retire(*resource, uses)) && retirement.pendingCount() == 1,
        "the receipt retires the resource");

  // The fence is the one wait: signaled, then the resource is collected.
  const auto waited = fence->wait(UINT64_MAX);
  check(waited && *waited == WaitStatus::satisfied, "the fence is signaled");
  const auto collected = retirement.collect();
  check(collected && collected->collected == 1 && collected->pending == 0 &&
            retirement.pendingCount() == 0,
        "the resource is collected once the fence is signaled");

  // A moved-from Fence gives a null native fence, which submit refuses.
  auto spare = createFence(device, {});
  if (!spare)
    return blocked("second Fence");
  Fence taken = std::move(*spare);
  check(!submitFenced(device, queue, *spare, {&domain, 8}, {.command_buffers = buffers}),
        "a moved-from Fence is refused");
  check(taken.valid(), "the Fence it was moved to is intact");

  check(recorder.warnings_or_errors.load() == 0, "the validation layer reports nothing");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int gpuTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  if (!hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation") ||
      !hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    return blocked("the validation layer and VK_EXT_debug_utils are required");
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-submit-fenced";
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance");
  ValidationRecorder recorder;
  auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("Vulkan Instance");
  auto inventory = queryPhysicalDevices(*instance);
  if (!inventory)
    return blocked("physical device enumeration");

  for (const auto &candidate : *inventory) {
    DeviceDescription request;
    request.queue_requests = {graphicsQueue()};
    request.required_features_12.timelineSemaphore = VK_TRUE;
    request.required_features_13.synchronization2 = VK_TRUE;
    const auto device_plan = resolveDevice(candidate, request);
    if (!device_plan)
      continue;
    int status = EXIT_FAILURE;
    {
      auto device = createDevice(*instance, *device_plan);
      if (!device)
        return blocked("Device with timelineSemaphore and synchronization2");
      status = exercise(*instance, candidate, *device, recorder);
    }
    if (status != EXIT_SUCCESS)
      return status;
    check(recorder.warnings_or_errors.load() == 0,
          "the validation layer reports nothing after destruction");
    if (failures == 0)
      std::puts("graphics.submit_fenced: submit_fenced=PASS validation-layer=PASS");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  return skip("no device with timelineSemaphore and synchronization2");
}

} // namespace

int main() { return gpuTest(); }

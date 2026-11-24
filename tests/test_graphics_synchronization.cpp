// Fence and Semaphore: creation, the single-call operations, the timeline
// feature rule and the helpers that fill submission descriptions.
//
// GPU test on a headless Instance (no window system, no Surface). If no device
// supports timeline semaphores, the test reports SKIP, never PASS. The
// validation layer must be available. No linker or platform mechanism is used.
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/synchronization.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using namespace terreate::graphics;
using Error = SynchronizationError;

const char *test_name = "graphics.synchronization";
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
        std::fprintf(stderr, "graphics.synchronization: validation message: %s\n",
                     event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

// The owners fit the retirement queues: move-only, nothrow move and destroy,
// with `valid()`.
template <typename T>
constexpr bool retirable = !std::is_copy_constructible_v<T> && !std::is_copy_assignable_v<T> &&
                           std::is_nothrow_move_constructible_v<T> &&
                           std::is_nothrow_destructible_v<T> && requires(const T &t) {
                             { t.valid() } -> std::convertible_to<bool>;
                           };
static_assert(retirable<Fence> && retirable<Semaphore>);

int skip(const char *reason) {
  std::printf("graphics.synchronization: synchronization=SKIP reason=%s\n", reason);
  return EXIT_SUCCESS;
}

DeviceQueueRequest graphicsQueue() {
  DeviceQueueRequest request;
  request.caller_id = "main";
  request.strength = RequirementStrength::required;
  request.required_flags = vk::QueueFlagBits::eGraphics;
  return request;
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
  description.application_name = "terreate-synchronization";
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

  const PhysicalDeviceCandidate *chosen = nullptr;
  std::optional<DevicePlan> timeline_plan;
  std::optional<DevicePlan> plain_plan;
  for (const auto &candidate : *inventory) {
    DeviceDescription with_timeline;
    with_timeline.required_features_12.timelineSemaphore = VK_TRUE;
    with_timeline.queue_requests = {graphicsQueue()};
    DeviceDescription plain;
    plain.queue_requests = {graphicsQueue()};
    auto timeline_resolved = resolveDevice(candidate, with_timeline);
    auto plain_resolved = resolveDevice(candidate, plain);
    if (!timeline_resolved || !plain_resolved)
      continue;
    chosen = &candidate;
    timeline_plan = std::move(*timeline_resolved);
    plain_plan = std::move(*plain_resolved);
    break;
  }
  if (chosen == nullptr || !timeline_plan || !plain_plan)
    return skip("no device supports timeline semaphores");
  auto device = createDevice(*instance, *timeline_plan);
  auto plain_device = createDevice(*instance, *plain_plan);
  if (!check(device.has_value() && plain_device.has_value(),
             "a Device with and a Device without timelineSemaphore"))
    return EXIT_FAILURE;
  const Queue queue = device->queue("main");
  if (!check(queue.valid(), "the queue exists"))
    return EXIT_FAILURE;
  const auto native_queue = static_cast<VkQueue>(queue.nativeHandle());

  // ---- Fence ----
  {
    auto signaled = createFence(*device, FenceDescription{.signaled = true});
    auto unsignaled = createFence(*device, FenceDescription{.signaled = false});
    if (!check(signaled.has_value() && unsignaled.has_value(), "createFence"))
      return EXIT_FAILURE;
    check(signaled->valid() && signaled->nativeHandle() && signaled->correlatedWith(*device) &&
              !signaled->correlatedWith(*plain_device) && signaled->configuration() != nullptr &&
              signaled->configuration()->signaled,
          "a Fence is valid, correlated with its own Device only, and keeps its description");
    check(signaled->status().value_or(FenceStatus::unsignaled) == FenceStatus::signaled &&
              unsignaled->status().value_or(FenceStatus::signaled) == FenceStatus::unsignaled,
          "status reports the creation state without waiting");
    check(signaled->wait(0).value_or(WaitStatus::timeout) == WaitStatus::satisfied,
          "wait on a signaled Fence is satisfied at once");
    check(unsignaled->wait(1'000'000).value_or(WaitStatus::satisfied) == WaitStatus::timeout,
          "wait on an unsignaled Fence times out");
    check(signaled->reset().has_value() &&
              signaled->status().value_or(FenceStatus::signaled) == FenceStatus::unsignaled,
          "reset unsignals the Fence");
    // An empty submission signals the Fence once the queue's earlier work is done.
    check(vkQueueSubmit(native_queue, 0, nullptr,
                        static_cast<VkFence>(unsignaled->nativeHandle())) == VK_SUCCESS &&
              unsignaled->wait(UINT64_MAX).value_or(WaitStatus::timeout) == WaitStatus::satisfied,
          "a submission signals the Fence and wait returns satisfied");
    check(unsignaled->reset().has_value(), "reset after the Fence was signaled");

    auto moved = Fence{std::move(*unsignaled)};
    check(!unsignaled->valid() && !unsignaled->nativeHandle() && !unsignaled->configuration() &&
              !unsignaled->correlatedWith(*device) && moved.valid(),
          "a moved-from Fence observes nothing");
    check(hasError(unsignaled->status(), Error::invalid_object) &&
              hasError(unsignaled->wait(0), Error::invalid_object) &&
              hasError(unsignaled->reset(), Error::invalid_object),
          "operations on a moved-from Fence are invalid_object");
  }

  // ---- binary Semaphore ----
  {
    auto first = createSemaphore(*device, {});
    auto fence = createFence(*device, {});
    if (!check(first.has_value() && fence.has_value(), "a binary Semaphore and a Fence"))
      return EXIT_FAILURE;
    check(first->kind() == SubmissionSemaphoreKind::binary && first->nativeHandle() &&
              first->correlatedWith(*device) && first->configuration() != nullptr,
          "a binary Semaphore reports its kind and handle");
    check(hasError(first->counterValue(), Error::wrong_kind) &&
              hasError(first->wait({1, 0}), Error::wrong_kind) &&
              hasError(first->signal(1), Error::wrong_kind),
          "timeline operations on a binary Semaphore are wrong_kind");
    check(hasError(createSemaphore(*device, {SubmissionSemaphoreKind::binary, 3}),
                   Error::invalid_description),
          "a binary Semaphore with an initial value is invalid_description");

    // One submission signals the Semaphore, a second waits for it.
    const auto native = static_cast<VkSemaphore>(first->nativeHandle());
    VkSubmitInfo signal_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    signal_info.signalSemaphoreCount = 1;
    signal_info.pSignalSemaphores = &native;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo wait_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    wait_info.waitSemaphoreCount = 1;
    wait_info.pWaitSemaphores = &native;
    wait_info.pWaitDstStageMask = &wait_stage;
    check(vkQueueSubmit(native_queue, 1, &signal_info, VK_NULL_HANDLE) == VK_SUCCESS &&
              vkQueueSubmit(native_queue, 1, &wait_info,
                            static_cast<VkFence>(fence->nativeHandle())) == VK_SUCCESS &&
              fence->wait(UINT64_MAX).value_or(WaitStatus::timeout) == WaitStatus::satisfied,
          "a binary Semaphore is signaled by one submission and waited by the next");

    const auto wait = submissionWait(*first, vk::PipelineStageFlagBits2::eAllCommands);
    const auto signal = submissionSignal(*first, vk::PipelineStageFlagBits2::eTransfer);
    check(wait.kind == SubmissionSemaphoreKind::binary &&
              wait.native.semaphore == first->nativeHandle() && wait.native.value == 0 &&
              wait.native.stageMask == vk::PipelineStageFlagBits2::eAllCommands &&
              signal.native.stageMask == vk::PipelineStageFlagBits2::eTransfer,
          "the helpers fill the submission semaphore with the given stage and a zero value");
  }

  // ---- timeline Semaphore ----
  {
    auto timeline = createSemaphore(*device, {SubmissionSemaphoreKind::timeline, 5});
    if (!check(timeline.has_value(), "a timeline Semaphore"))
      return EXIT_FAILURE;
    check(timeline->kind() == SubmissionSemaphoreKind::timeline &&
              timeline->configuration()->initial_value == 5,
          "a timeline Semaphore keeps its kind and initial value");
    check(timeline->counterValue().value_or(0) == 5, "the counter starts at the initial value");
    check(timeline->wait({5, 0}).value_or(WaitStatus::timeout) == WaitStatus::satisfied,
          "wait for a reached value is satisfied at once");
    check(timeline->wait({9, 1'000'000}).value_or(WaitStatus::satisfied) == WaitStatus::timeout,
          "wait for an unreached value times out");
    check(timeline->signal(7).has_value() && timeline->counterValue().value_or(0) == 7,
          "a host signal raises the counter");

    // A submission signals value 9; the host waits for it.
    const auto signal = submissionSignal(*timeline, vk::PipelineStageFlagBits2::eAllCommands, 9);
    VkTimelineSemaphoreSubmitInfo timeline_info{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    const std::uint64_t value = signal.native.value;
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = &value;
    const auto native = static_cast<VkSemaphore>(timeline->nativeHandle());
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.pNext = &timeline_info;
    info.signalSemaphoreCount = 1;
    info.pSignalSemaphores = &native;
    check(vkQueueSubmit(native_queue, 1, &info, VK_NULL_HANDLE) == VK_SUCCESS &&
              timeline->wait({9, UINT64_MAX}).value_or(WaitStatus::timeout) ==
                  WaitStatus::satisfied &&
              timeline->counterValue().value_or(0) == 9 &&
              signal.kind == SubmissionSemaphoreKind::timeline && signal.native.value == 9,
          "a submission signals the timeline value and the host wait is satisfied");

    auto moved = Semaphore{std::move(*timeline)};
    check(!timeline->valid() && moved.valid() &&
              hasError(timeline->counterValue(), Error::invalid_object) &&
              hasError(timeline->signal(10), Error::invalid_object),
          "a moved-from Semaphore is invalid_object");
  }

  // ---- the timeline feature rule ----
  check(hasError(createSemaphore(*plain_device, {SubmissionSemaphoreKind::timeline, 0}),
                 Error::timeline_not_enabled) &&
            createSemaphore(*plain_device, {}).has_value(),
        "a timeline Semaphore needs the feature; a binary one does not");
  {
    auto moved_from_source = createFence(*plain_device, {});
    Fence moved_to = std::move(*moved_from_source);
    (void)moved_to;
  }

  vkQueueWaitIdle(native_queue);
  {
    const auto gone = std::move(*device);
  }
  {
    const auto gone = std::move(*plain_device);
  }
  {
    const auto gone = std::move(*instance);
  }
  check(recorder.warnings_or_errors.load() == 0, "no validation warnings or errors");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.synchronization: validation-layer=PASS");
  std::puts("graphics.synchronization: synchronization=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2 || std::string_view{argv[1]} != "gpu")
    return blocked("specify gpu mode");
  return gpuTest();
}

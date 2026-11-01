#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/submission.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace terreate::graphics;
std::size_t submits = 0;
std::size_t polls = 0;
std::size_t waits = 0;
std::optional<VkResult> submit_result;
std::optional<VkResult> query_result;
std::optional<VkResult> wait_result;
std::optional<std::uint64_t> counter_result;
VkQueue seen_queue{};
VkFence seen_fence{};
VkSubmitInfo2 seen_submit{};
std::vector<VkCommandBufferSubmitInfo> seen_commands;
std::vector<VkSemaphoreSubmitInfo> seen_waits;
std::vector<VkSemaphoreSubmitInfo> seen_signals;
std::uint64_t seen_timeout = 0;
VkSemaphore seen_semaphore{};
std::uint64_t seen_value = 0;
bool check(bool condition, const char *message) {
  if (!condition)
    std::fprintf(stderr, "graphics.submission: assertion=FAIL: %s\n", message);
  return condition;
}
int blocked(const char *message) {
  std::fprintf(stderr, "graphics.submission: validation=BLOCKED: %s\n", message);
  return EXIT_FAILURE;
}
template <class T> bool nativeError(const terreate::Result<T> &result, VkResult code) {
  return !result && result.error().code() == vk::make_error_code(static_cast<vk::Result>(code));
}
struct NativeSync {
  VkDevice device{};
  VkFence fence{};
  VkSemaphore semaphore{};
  VkSemaphore binary_wait{};
  ~NativeSync() {
    if (binary_wait)
      vkDestroySemaphore(device, binary_wait, nullptr);
    if (semaphore)
      vkDestroySemaphore(device, semaphore, nullptr);
    if (fence)
      vkDestroyFence(device, fence, nullptr);
  }
};
} // namespace

// Real objects and real successful submissions; only explicit failure/query
// injections bypass the loader. No fabricated successful submission is used.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vkQueueSubmit2(VkQueue, std::uint32_t, const VkSubmitInfo2 *, VkFence);
extern "C" VkResult __real_vkGetFenceStatus(VkDevice, VkFence);
extern "C" VkResult __real_vkWaitForFences(VkDevice, std::uint32_t, const VkFence *, VkBool32,
                                           std::uint64_t);
extern "C" VkResult __real_vkGetSemaphoreCounterValue(VkDevice, VkSemaphore, std::uint64_t *);
extern "C" VkResult __real_vkWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo *, std::uint64_t);
extern "C" VkResult __wrap_vkQueueSubmit2(VkQueue queue, std::uint32_t count,
                                          const VkSubmitInfo2 *info, VkFence fence) {
  ++submits;
  seen_queue = queue;
  seen_fence = fence;
  if (!check(count == 1 && info, "submitCount must equal one"))
    return VK_ERROR_UNKNOWN;
  seen_submit = *info;
  seen_commands.assign(info->pCommandBufferInfos,
                       info->pCommandBufferInfos + info->commandBufferInfoCount);
  seen_waits.clear();
  seen_signals.clear();
  if (info->waitSemaphoreInfoCount)
    seen_waits.assign(info->pWaitSemaphoreInfos,
                      info->pWaitSemaphoreInfos + info->waitSemaphoreInfoCount);
  if (info->signalSemaphoreInfoCount)
    seen_signals.assign(info->pSignalSemaphoreInfos,
                        info->pSignalSemaphoreInfos + info->signalSemaphoreInfoCount);
  if (submit_result)
    return std::exchange(submit_result, std::nullopt).value_or(VK_ERROR_UNKNOWN);
  return __real_vkQueueSubmit2(queue, count, info, fence);
}
extern "C" VkResult __wrap_vkGetFenceStatus(VkDevice device, VkFence fence) {
  ++polls;
  seen_fence = fence;
  if (query_result)
    return std::exchange(query_result, std::nullopt).value_or(VK_ERROR_UNKNOWN);
  return __real_vkGetFenceStatus(device, fence);
}
extern "C" VkResult __wrap_vkWaitForFences(VkDevice device, std::uint32_t count,
                                           const VkFence *fences, VkBool32 all,
                                           std::uint64_t timeout) {
  ++waits;
  seen_timeout = timeout;
  if (!check(count == 1 && all == VK_TRUE, "fence wait shape"))
    return VK_ERROR_UNKNOWN;
  seen_fence = fences[0];
  if (wait_result)
    return std::exchange(wait_result, std::nullopt).value_or(VK_ERROR_UNKNOWN);
  return __real_vkWaitForFences(device, count, fences, all, timeout);
}
extern "C" VkResult __wrap_vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore,
                                                      std::uint64_t *value) {
  ++polls;
  seen_semaphore = semaphore;
  if (query_result)
    return std::exchange(query_result, std::nullopt).value_or(VK_ERROR_UNKNOWN);
  if (counter_result) {
    *value = std::exchange(counter_result, std::nullopt).value_or(std::uint64_t{0});
    return VK_SUCCESS;
  }
  return __real_vkGetSemaphoreCounterValue(device, semaphore, value);
}
extern "C" VkResult __wrap_vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *info,
                                            std::uint64_t timeout) {
  ++waits;
  seen_timeout = timeout;
  if (!check(info->sType == VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO && !info->pNext && !info->flags &&
                 info->semaphoreCount == 1,
             "timeline wait shape"))
    return VK_ERROR_UNKNOWN;
  seen_semaphore = info->pSemaphores[0];
  seen_value = info->pValues[0];
  if (wait_result)
    return std::exchange(wait_result, std::nullopt).value_or(VK_ERROR_UNKNOWN);
  return __real_vkWaitSemaphores(device, info, timeout);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
int exercise(Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             std::uint32_t family, bool timeline_mode) {
  const auto queue = device.queue("submission");
  const auto native_device = static_cast<VkDevice>(device.nativeHandle());
  NativeSync sync{native_device};
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (vkCreateFence(native_device, &fence_info, nullptr, &sync.fence) != VK_SUCCESS)
    return blocked("fence creation");
  VkSemaphoreTypeCreateInfo type{};
  type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo semaphore_info{};
  semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphore_info.pNext = timeline_mode ? &type : nullptr;
  if (vkCreateSemaphore(native_device, &semaphore_info, nullptr, &sync.semaphore) != VK_SUCCESS)
    return blocked("semaphore creation");
  if (!timeline_mode &&
      vkCreateSemaphore(native_device, &semaphore_info, nullptr, &sync.binary_wait) != VK_SUCCESS)
    return blocked("binary wait semaphore creation");
  CommandPoolDescription pool_description;
  pool_description.queue_family_index = family;
  pool_description.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  auto pool = createCommandPool(device, pool_description);
  if (!pool)
    return blocked("pool creation");
  auto first = pool->allocatePrimary();
  auto second = pool->allocatePrimary();
  if (!first || !second || !first->begin() || !first->end() || !second->begin() || !second->end())
    return blocked("record primary buffers");
  const std::array<const CommandBuffer *, 2> buffers{&*first, &*second};
  int identity_domain = 0;
  CompletionDescription completion;
  completion.device = &device;
  completion.queue = queue;
  completion.requested = {&identity_domain, 1};
  completion.effective = {&identity_domain, 2};
  completion.condition =
      timeline_mode
          ? CompletionCondition{TimelineCompletionCondition{vk::Semaphore{sync.semaphore}, 2}}
          : CompletionCondition{FenceCompletionCondition{vk::Fence{sync.fence}}};
  SubmissionSemaphore signal;
  signal.native.semaphore = vk::Semaphore{sync.semaphore};
  signal.native.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
  signal.native.value = timeline_mode ? 2 : 0;
  signal.kind = timeline_mode ? SubmissionSemaphoreKind::timeline : SubmissionSemaphoreKind::binary;
  SubmissionSemaphore wait = signal;
  wait.native.value = 0;
  wait.native.stageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
  if (!timeline_mode)
    wait.native.semaphore = vk::Semaphore{sync.binary_wait};
  SubmissionDescription description;
  description.command_buffers = buffers;
  description.signals = std::span{&signal, 1};
  description.waits = std::span{&wait, 1};
  description.fence = vk::Fence{sync.fence};
  description.completion = completion;
  bool passed = true;
  const auto reject = [&](const Device &selected, Queue selected_queue,
                          const SubmissionDescription &bad, SubmissionError error) {
    const auto before = submits;
    const auto result = submit(selected, selected_queue, bad);
    return check(!result && result.error().code() == make_error_code(error) && before == submits,
                 "pre-native rejection/code/call count");
  };
  passed &= reject(device, Queue{}, description, SubmissionError::invalid_queue);
  auto bad = description;
  bad.command_buffers = {};
  passed &= reject(device, queue, bad, SubmissionError::invalid_batch);
  bad = description;
  bad.completion.effective.serial = 0;
  passed &= reject(device, queue, bad, SubmissionError::invalid_completion);
  bad.completion = completion;
  bad.completion.condition = FenceCompletionCondition{};
  passed &= reject(device, queue, bad, SubmissionError::invalid_completion);
  bad = description;
  bad.completion.condition = FenceCompletionCondition{vk::Fence{sync.fence}};
  bad.fence = vk::Fence{};
  passed &= reject(device, queue, bad, SubmissionError::invalid_completion);
  SubmissionSemaphore invalid = signal;
  invalid.native.semaphore = vk::Semaphore{};
  bad = description;
  bad.signals = std::span{&invalid, 1};
  passed &= reject(device, queue, bad, SubmissionError::invalid_semaphore);
  invalid = signal;
  invalid.native.pNext = &identity_domain;
  passed &= reject(device, queue, bad, SubmissionError::invalid_native_structure);
  invalid = signal;
  invalid.native.deviceIndex = 1;
  passed &= reject(device, queue, bad, SubmissionError::invalid_native_structure);
  invalid = signal;
  invalid.native.sType = vk::StructureType::eSubmitInfo;
  passed &= reject(device, queue, bad, SubmissionError::invalid_native_structure);
  invalid = signal;
  invalid.kind = SubmissionSemaphoreKind::binary;
  invalid.native.value = 1;
  passed &= reject(device, queue, bad, SubmissionError::invalid_semaphore);
  if (timeline_mode) {
    invalid = signal;
    invalid.native.value = 3;
    passed &= reject(device, queue, bad, SubmissionError::invalid_completion);
    invalid = signal;
    invalid.native.stageMask = vk::PipelineStageFlagBits2::eTransfer;
    passed &= reject(device, queue, bad, SubmissionError::invalid_completion);
    bad = description;
    bad.signals = {};
    passed &= reject(device, queue, bad, SubmissionError::invalid_completion);
  } else {
    invalid = signal;
    invalid.kind = SubmissionSemaphoreKind::timeline;
    invalid.native.value = 1;
    passed &= reject(device, queue, bad, SubmissionError::timeline_not_enabled);
  }
  const std::array<const CommandBuffer *, 1> null_buffer{nullptr};
  bad = description;
  bad.command_buffers = null_buffer;
  passed &= reject(device, queue, bad, SubmissionError::invalid_buffer);
  auto moved = std::move(*first);
  passed &= reject(device, queue, description, SubmissionError::invalid_buffer);
  *first = std::move(moved);
  if (!first->reset())
    return blocked("reset before invalid-state test");
  passed &= reject(device, queue, description, SubmissionError::invalid_buffer);
  if (!first->begin() || !first->end())
    return blocked("re-record primary");

  // Genuine second Device tests origin/queue mismatches and absent effective features.
  DeviceDescription disabled;
  DeviceQueueRequest request;
  request.caller_id = "submission";
  request.required_flags = candidate.capabilities().queue_families[family].queueFlags &
                           (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute);
  request.allowed_family_indices = {family};
  disabled.queue_requests.push_back(request);
  auto disabled_plan = resolveDevice(candidate, disabled);
  if (!disabled_plan)
    return blocked("disabled-feature plan");
  auto other = createDevice(instance, *disabled_plan);
  if (!other)
    return blocked("second Device");
  passed &=
      reject(device, other->queue("submission"), description, SubmissionError::parent_mismatch);
  passed &= reject(*other, other->queue("submission"), description,
                   SubmissionError::synchronization2_not_enabled);
  Device moved_device = std::move(*other);
  passed &= reject(*other, queue, description, SubmissionError::invalid_device);
  disabled.required_features_13.synchronization2 = VK_TRUE;
  auto no_timeline_plan = resolveDevice(candidate, disabled);
  if (!no_timeline_plan)
    return blocked("sync2-only plan");
  auto no_timeline = createDevice(instance, *no_timeline_plan);
  if (!no_timeline)
    return blocked("sync2-only Device");
  const auto other_queue = no_timeline->queue("submission");
  bad = description;
  bad.completion.device = &*no_timeline;
  bad.completion.queue = other_queue;
  bad.completion.condition = TimelineCompletionCondition{vk::Semaphore{sync.semaphore}, 2};
  passed &= reject(*no_timeline, other_queue, bad, SubmissionError::timeline_not_enabled);
  bad.completion.condition = FenceCompletionCondition{vk::Fence{sync.fence}};
  passed &= reject(*no_timeline, other_queue, bad, SubmissionError::parent_mismatch);

  const auto alternate = device.queue("alternate");
  if (alternate.valid()) {
    bad = description;
    bad.completion.queue = alternate;
    passed &= reject(device, alternate, bad, SubmissionError::family_mismatch);
  } else {
    std::puts("graphics.submission: alternate-family=SKIP (no second command family)");
  }
  if (device.plan()->enabledFeatures11().protectedMemory &&
      (candidate.capabilities().queue_families[family].queueFlags &
       vk::QueueFlagBits::eProtected)) {
    auto protected_description = pool_description;
    protected_description.flags = vk::CommandPoolCreateFlagBits::eProtected;
    auto protected_pool = createCommandPool(device, protected_description);
    if (!protected_pool)
      return blocked("protected pool for rejection test");
    auto protected_buffer = protected_pool->allocatePrimary();
    if (!protected_buffer || !protected_buffer->begin() || !protected_buffer->end())
      return blocked("protected primary for rejection test");
    const std::array<const CommandBuffer *, 1> protected_buffers{&*protected_buffer};
    bad = description;
    bad.command_buffers = protected_buffers;
    passed &= reject(device, queue, bad, SubmissionError::protected_allocation);
  } else {
    std::puts("graphics.submission: protected-allocation=SKIP (not enabled/supported)");
  }

  for (auto error : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_DEVICE_LOST}) {
    submit_result = error;
    const auto before = submits;
    const auto failed = submit(device, queue, description);
    passed &= check(nativeError(failed, error) && submits == before + 1 && !submit_result,
                    "native submission failure must return Error, not receipt/retry");
  }
  const auto before_submits = submits;
  const auto before_polls = polls;
  const auto before_waits = waits;
  if (!timeline_mode) {
    // Explicit application preparation: signal the binary wait semaphore on the
    // same queue. This is not a library call and is not included in submit's
    // exactly-one-call assertion. The following batch consumes this signal.
    VkSemaphoreSubmitInfo preparation_signal{};
    preparation_signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    preparation_signal.semaphore = sync.binary_wait;
    preparation_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 preparation{};
    preparation.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    preparation.signalSemaphoreInfoCount = 1;
    preparation.pSignalSemaphoreInfos = &preparation_signal;
    if (__real_vkQueueSubmit2(static_cast<VkQueue>(queue.nativeHandle()), 1, &preparation,
                              VK_NULL_HANDLE) != VK_SUCCESS)
      return blocked("explicit binary wait preparation");
  }
  const auto receipt = submit(device, queue, description);
  if (!receipt)
    return blocked("real native submit");
  // Explicit application-owned completion before any native parent cleanup.
  const auto completed = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!completed || completed->state != SubmissionCompletionState::completed)
    return blocked("real native completion wait");
  // Timeline completion proves work, but also wait its supplied submit fence
  // explicitly before destroying that native fence.
  if (timeline_mode && vkWaitForFences(native_device, 1, &sync.fence, VK_TRUE,
                                       std::numeric_limits<std::uint64_t>::max()) != VK_SUCCESS)
    return blocked("explicit fence cleanup wait");
  passed &= check(submits == before_submits + 1 && polls == before_polls &&
                      waits == before_waits + (timeline_mode ? 2 : 1),
                  "submit hid queries/waits/retries");
  passed &= check(
      seen_queue == static_cast<VkQueue>(queue.nativeHandle()) &&
          seen_submit.sType == VK_STRUCTURE_TYPE_SUBMIT_INFO_2 && !seen_submit.pNext &&
          !seen_submit.flags && seen_commands.size() == 2 &&
          seen_commands[0].commandBuffer == static_cast<VkCommandBuffer>(first->nativeHandle()) &&
          seen_commands[1].commandBuffer == static_cast<VkCommandBuffer>(second->nativeHandle()) &&
          !seen_commands[0].pNext && !seen_commands[0].deviceMask && seen_signals.size() == 1 &&
          seen_signals[0].semaphore == sync.semaphore &&
          seen_signals[0].value == signal.native.value &&
          seen_signals[0].stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT &&
          seen_waits.size() == 1,
      "exact native batch");
  if (!seen_waits.empty())
    passed &= check(seen_waits[0].semaphore == static_cast<VkSemaphore>(wait.native.semaphore) &&
                        seen_waits[0].value == 0 &&
                        seen_waits[0].stageMask == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    "caller-selected binary/timeline wait value/stage");
  const auto initial_token = receipt->initialCompletion();
  passed &=
      check(initial_token && !initial_token->empty() &&
                initial_token->outcome() == CompletionOutcome::unavailable &&
                initial_token->description().requested == completion.requested &&
                initial_token->description().effective == completion.effective &&
                receipt->completion().effective == completion.effective && first->isExecutable(),
            "receipt lost #257 Completion token, intent or CPU-only state");
  const auto polled = receipt->poll();
  passed &= check(polled && polled->state == SubmissionCompletionState::completed &&
                      polled->native_result == vk::Result::eSuccess,
                  "real completion poll");
  if (timeline_mode) {
    counter_result = 1;
    const auto pending = receipt->poll();
    passed &=
        check(pending && pending->state == SubmissionCompletionState::pending &&
                  pending->native_result == vk::Result::eSuccess && pending->counter_value == 1,
              "below-target SUCCESS must not become NOT_READY");
  } else {
    query_result = VK_NOT_READY;
    const auto pending = receipt->poll();
    passed &= check(pending && pending->state == SubmissionCompletionState::pending &&
                        pending->native_result == vk::Result::eNotReady,
                    "fence pending result");
  }
  wait_result = VK_TIMEOUT;
  const auto timeout = receipt->wait(1234567);
  passed &= check(timeout && timeout->state == SubmissionCompletionState::timeout &&
                      timeout->native_result == vk::Result::eTimeout && seen_timeout == 1234567,
                  "timeout result/nanosecond forwarding");
  if (timeline_mode)
    passed &= check(seen_semaphore == sync.semaphore && seen_value == 2, "timeline wait binding");
  else
    passed &= check(seen_fence == sync.fence, "fence binding");
  for (auto error : {VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY}) {
    query_result = error;
    passed &= check(nativeError(receipt->poll(), error), "poll native failure code");
    wait_result = error;
    passed &= check(nativeError(receipt->wait(0), error), "wait native failure code");
  }
  if (passed)
    std::puts(timeline_mode ? "graphics.submission: headless-timeline=PASS"
                            : "graphics.submission: headless-fence=PASS");
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run(bool timeline_mode) {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader");
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  const auto instance_plan = resolveInstance(description, *capabilities);
  if (!instance_plan)
    return blocked("Vulkan 1.3 Instance resolution");
  auto instance = createInstance(*instance_plan);
  if (!instance)
    return blocked("headless Instance");
  const auto inventory = queryPhysicalDevices(*instance);
  if (!inventory || inventory->empty())
    return blocked("headless physical device");
  for (const auto &candidate : *inventory) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t family = 0; family < families.size(); ++family) {
      if (!families[family].queueCount ||
          !(families[family].queueFlags &
            (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute)))
        continue;
      DeviceDescription device_description;
      device_description.required_features_13.synchronization2 = VK_TRUE;
      device_description.required_features_12.timelineSemaphore =
          timeline_mode ? VK_TRUE : VK_FALSE;
      device_description.optional_features_11.protectedMemory = VK_TRUE;
      DeviceQueueRequest request;
      request.caller_id = "submission";
      request.required_flags = families[family].queueFlags &
                               (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
                                vk::QueueFlagBits::eTransfer);
      request.allowed_family_indices = {family};
      device_description.queue_requests.push_back(request);
      for (std::size_t other = 0; other < families.size(); ++other) {
        const auto flags = families[other].queueFlags &
                           (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
                            vk::QueueFlagBits::eTransfer);
        if (other == family || !families[other].queueCount || !flags)
          continue;
        request.caller_id = "alternate";
        request.required_flags = flags;
        request.allowed_family_indices = {other};
        device_description.queue_requests.push_back(request);
        break;
      }
      const auto plan = resolveDevice(candidate, device_description);
      if (!plan)
        continue;
      auto device = createDevice(*instance, *plan);
      if (!device)
        return blocked("native Device creation");
      return exercise(*instance, candidate, *device, static_cast<std::uint32_t>(family),
                      timeline_mode);
    }
  }
  if (timeline_mode) {
    std::puts("graphics.submission: timeline-capability=SKIP (no genuinely enabled support)");
    return 77;
  }
  return blocked("no synchronization2-enabled command Device");
}
} // namespace

int main(int argc, char **argv) { return run(argc > 1 && std::string_view{argv[1]} == "timeline"); }

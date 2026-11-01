#include <terreate/graphics/submission.hpp>

#include <limits>
#include <string>
#include <vector>

namespace terreate::graphics {
namespace {
class SubmissionErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.submission"; }
  std::string message(int value) const override {
    switch (static_cast<SubmissionError>(value)) {
    case SubmissionError::invalid_device:
      return "invalid live Device";
    case SubmissionError::invalid_queue:
      return "invalid Queue";
    case SubmissionError::parent_mismatch:
      return "Device origin mismatch";
    case SubmissionError::synchronization2_not_enabled:
      return "synchronization2 not enabled";
    case SubmissionError::timeline_not_enabled:
      return "timelineSemaphore not enabled";
    case SubmissionError::invalid_buffer:
      return "invalid executable primary buffer";
    case SubmissionError::family_mismatch:
      return "command pool and Queue family mismatch";
    case SubmissionError::protected_allocation:
      return "protected submission unsupported";
    case SubmissionError::invalid_semaphore:
      return "invalid semaphore declaration";
    case SubmissionError::invalid_native_structure:
      return "unsupported native structure";
    case SubmissionError::invalid_completion:
      return "inconsistent completion association";
    case SubmissionError::invalid_batch:
      return "empty or oversized batch";
    }
    return "unknown submission error";
  }
};
auto failure(SubmissionError error, const char *operation) {
  return std::unexpected(terreate::Error{make_error_code(error), operation,
                                         "rejected before native submission/completion call"});
}
auto nativeFailure(VkResult result, const char *operation) {
  return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                         operation,
                                         "native submission/completion operation failed"});
}
bool sameQueue(Queue left, Queue right) {
  return left.nativeHandle() == right.nativeHandle() && left.familyIndex() == right.familyIndex() &&
         left.queueIndex() == right.queueIndex();
}
terreate::Result<void> validateCompletion(const CompletionDescription &completion) {
  if (!completion.device || !completion.device->valid() || !completion.device->plan())
    return failure(SubmissionError::invalid_device, "completion");
  if (!completion.queue.valid())
    return failure(SubmissionError::invalid_queue, "completion");
  if (!completion.queue.correlatedWith(*completion.device))
    return failure(SubmissionError::parent_mismatch, "completion");
  if (!completion.requested.valid() || !completion.effective.valid())
    return failure(SubmissionError::invalid_completion, "completion");
  if (const auto *fence = std::get_if<FenceCompletionCondition>(&completion.condition)) {
    if (fence->fence)
      return {};
  } else if (const auto *timeline =
                 std::get_if<TimelineCompletionCondition>(&completion.condition)) {
    if (!completion.device->plan()->enabledFeatures12().timelineSemaphore)
      return failure(SubmissionError::timeline_not_enabled, "completion");
    if (timeline->semaphore)
      return {};
  }
  return failure(SubmissionError::invalid_completion, "completion");
}
terreate::Result<SubmissionObservation> observation(VkResult result, const char *operation) {
  if (result == VK_SUCCESS)
    return SubmissionObservation{SubmissionCompletionState::completed, vk::Result::eSuccess, {}};
  if (result == VK_NOT_READY)
    return SubmissionObservation{SubmissionCompletionState::pending, vk::Result::eNotReady, {}};
  if (result == VK_TIMEOUT)
    return SubmissionObservation{SubmissionCompletionState::timeout, vk::Result::eTimeout, {}};
  return nativeFailure(result, operation);
}
} // namespace

const std::error_category &submission_error_category() noexcept {
  static const SubmissionErrorCategory category;
  return category;
}
std::error_code make_error_code(SubmissionError error) noexcept {
  return {static_cast<int>(error), submission_error_category()};
}

auto submit(const Device &device, Queue queue, const SubmissionDescription &description)
    -> terreate::Result<SubmissionReceipt> {
  if (!device.valid() || !device.plan())
    return failure(SubmissionError::invalid_device, "submit");
  if (!queue.valid())
    return failure(SubmissionError::invalid_queue, "submit");
  if (!queue.correlatedWith(device))
    return failure(SubmissionError::parent_mismatch, "submit");
  if (!device.plan()->enabledFeatures13().synchronization2)
    return failure(SubmissionError::synchronization2_not_enabled, "submit");
  constexpr auto max_count = std::numeric_limits<std::uint32_t>::max();
  if (description.command_buffers.empty() || description.command_buffers.size() > max_count ||
      description.waits.size() > max_count || description.signals.size() > max_count)
    return failure(SubmissionError::invalid_batch, "submit");
  const auto &completion = description.completion;
  if (completion.device != &device || !sameQueue(completion.queue, queue))
    return failure(SubmissionError::invalid_completion, "submit");
  if (auto valid = validateCompletion(completion); !valid)
    return std::unexpected(valid.error());

  std::vector<VkCommandBufferSubmitInfo> commands;
  for (const auto *buffer : description.command_buffers) {
    if (!buffer || !buffer->valid() || !buffer->nativeHandle() || !buffer->isExecutable() ||
        !buffer->isPrimary())
      return failure(SubmissionError::invalid_buffer, "submit");
    if (buffer->nativeDevice() != device.nativeHandle())
      return failure(SubmissionError::parent_mismatch, "submit");
    if (buffer->queueFamilyIndex() != queue.familyIndex())
      return failure(SubmissionError::family_mismatch, "submit");
    if (buffer->isProtected())
      return failure(SubmissionError::protected_allocation, "submit");
    commands.push_back(
        VkCommandBufferSubmitInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr,
                                  static_cast<VkCommandBuffer>(buffer->nativeHandle()), 0});
  }
  std::vector<VkSemaphoreSubmitInfo> waits;
  std::vector<VkSemaphoreSubmitInfo> signals;
  const auto convert = [&](std::span<const SubmissionSemaphore> input,
                           std::vector<VkSemaphoreSubmitInfo> &output) -> terreate::Result<void> {
    for (const auto &entry : input) {
      const auto &info = entry.native;
      if (info.sType != vk::StructureType::eSemaphoreSubmitInfo || info.pNext || info.deviceIndex)
        return failure(SubmissionError::invalid_native_structure, "submit");
      if (!info.semaphore ||
          (entry.kind != SubmissionSemaphoreKind::binary &&
           entry.kind != SubmissionSemaphoreKind::timeline) ||
          (entry.kind == SubmissionSemaphoreKind::binary && info.value != 0))
        return failure(SubmissionError::invalid_semaphore, "submit");
      if (entry.kind == SubmissionSemaphoreKind::timeline &&
          !device.plan()->enabledFeatures12().timelineSemaphore)
        return failure(SubmissionError::timeline_not_enabled, "submit");
      output.push_back(static_cast<const VkSemaphoreSubmitInfo &>(info));
    }
    return {};
  };
  if (auto valid = convert(description.waits, waits); !valid)
    return std::unexpected(valid.error());
  if (auto valid = convert(description.signals, signals); !valid)
    return std::unexpected(valid.error());
  // Duplicate semaphore entries and contradictory type/value declarations are
  // structural errors; object type, ownership and signal history remain caller
  // preconditions because raw Vulkan handles carry no inspectable provenance.
  const auto duplicates = [](std::span<const SubmissionSemaphore> entries) {
    for (std::size_t i = 0; i < entries.size(); ++i)
      for (std::size_t j = i + 1; j < entries.size(); ++j)
        if (entries[i].native.semaphore == entries[j].native.semaphore)
          return true;
    return false;
  };
  if (duplicates(description.waits) || duplicates(description.signals))
    return failure(SubmissionError::invalid_semaphore, "submit");
  for (const auto &wait : description.waits)
    for (const auto &signal : description.signals)
      if (wait.native.semaphore == signal.native.semaphore &&
          (wait.kind != signal.kind || (wait.kind == SubmissionSemaphoreKind::timeline &&
                                        signal.native.value <= wait.native.value)))
        return failure(SubmissionError::invalid_semaphore, "submit");
  if (const auto *fence = std::get_if<FenceCompletionCondition>(&completion.condition)) {
    if (fence->fence != description.fence)
      return failure(SubmissionError::invalid_completion, "submit");
  } else {
    const auto &timeline = std::get<TimelineCompletionCondition>(completion.condition);
    bool bound = false;
    for (const auto &signal : description.signals) {
      if (signal.kind == SubmissionSemaphoreKind::timeline &&
          signal.native.semaphore == timeline.semaphore &&
          signal.native.value == timeline.signal_value &&
          (signal.native.stageMask & vk::PipelineStageFlagBits2::eAllCommands))
        bound = true;
    }
    if (!bound)
      return failure(SubmissionError::invalid_completion, "submit");
  }
  VkSubmitInfo2 info{};
  info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  info.waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size());
  info.pWaitSemaphoreInfos = waits.data();
  info.commandBufferInfoCount = static_cast<std::uint32_t>(commands.size());
  info.pCommandBufferInfos = commands.data();
  info.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size());
  info.pSignalSemaphoreInfos = signals.data();
  const auto result = vkQueueSubmit2(static_cast<VkQueue>(queue.nativeHandle()), 1, &info,
                                     static_cast<VkFence>(description.fence));
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vkQueueSubmit2");
  return SubmissionReceipt{completion};
}

terreate::Result<SubmissionObservation> SubmissionReceipt::poll() const {
  if (auto valid = validateCompletion(completion_); !valid)
    return std::unexpected(valid.error());
  const auto device = static_cast<VkDevice>(completion_.device->nativeHandle());
  if (const auto *fence = std::get_if<FenceCompletionCondition>(&completion_.condition))
    return observation(vkGetFenceStatus(device, static_cast<VkFence>(fence->fence)),
                       "vkGetFenceStatus");
  const auto &timeline = std::get<TimelineCompletionCondition>(completion_.condition);
  std::uint64_t counter = 0;
  const auto result =
      vkGetSemaphoreCounterValue(device, static_cast<VkSemaphore>(timeline.semaphore), &counter);
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vkGetSemaphoreCounterValue");
  return SubmissionObservation{counter >= timeline.signal_value
                                   ? SubmissionCompletionState::completed
                                   : SubmissionCompletionState::pending,
                               vk::Result::eSuccess, counter};
}
terreate::Result<SubmissionObservation> SubmissionReceipt::wait(std::uint64_t timeout_ns) const {
  if (auto valid = validateCompletion(completion_); !valid)
    return std::unexpected(valid.error());
  const auto device = static_cast<VkDevice>(completion_.device->nativeHandle());
  if (const auto *fence = std::get_if<FenceCompletionCondition>(&completion_.condition)) {
    const auto handle = static_cast<VkFence>(fence->fence);
    return observation(vkWaitForFences(device, 1, &handle, VK_TRUE, timeout_ns), "vkWaitForFences");
  }
  const auto &timeline = std::get<TimelineCompletionCondition>(completion_.condition);
  const auto handle = static_cast<VkSemaphore>(timeline.semaphore);
  VkSemaphoreWaitInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
  info.semaphoreCount = 1;
  info.pSemaphores = &handle;
  info.pValues = &timeline.signal_value;
  return observation(vkWaitSemaphores(device, &info, timeout_ns), "vkWaitSemaphores");
}
} // namespace terreate::graphics

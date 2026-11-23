#include <terreate/graphics/swapchain_retirement.hpp>

#include <exception>
#include <new>
#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
class SwapchainRetirementErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.swapchain_retirement"; }
  std::string message(int value) const override {
    switch (static_cast<SwapchainRetirementError>(value)) {
    case SwapchainRetirementError::invalid_device:
      return "invalid Device";
    case SwapchainRetirementError::invalid_swapchain:
      return "invalid or moved-from Swapchain";
    case SwapchainRetirementError::parent_mismatch:
      return "the Swapchain, a submission or the queue belong to another Device";
    case SwapchainRetirementError::missing_evidence:
      return "explicit completion evidence required";
    case SwapchainRetirementError::contradictory_evidence:
      return "no_pending_present contradicts the declared present fences";
    case SwapchainRetirementError::invalid_fence:
      return "a present fence is null";
    case SwapchainRetirementError::invalid_submission:
      return "invalid borrowed submission receipt";
    case SwapchainRetirementError::pending_entries:
      return "the Swapchain retirement queue still owns pending Swapchains";
    case SwapchainRetirementError::device_lost:
      return "the retirement queue's Device is lost";
    case SwapchainRetirementError::device_loss_not_observed:
      return "native device loss must be observed before explicit reclaim";
    case SwapchainRetirementError::allocation_failed:
      return "retirement metadata allocation failed";
    }
    return "unknown swapchain retirement error";
  }
};

auto fail(SwapchainRetirementError error, const char *operation, const char *detail)
    -> terreate::Error {
  try {
    return terreate::Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{make_error_code(error)};
  }
}
} // namespace

const std::error_category &swapchain_retirement_error_category() noexcept {
  static const SwapchainRetirementErrorCategory category;
  return category;
}
std::error_code make_error_code(SwapchainRetirementError error) noexcept {
  return {static_cast<int>(error), swapchain_retirement_error_category()};
}

SwapchainRetirementQueue::~SwapchainRetirementQueue() noexcept {
  if (!entries_.empty())
    std::terminate();
}

auto SwapchainRetirementQueue::requireEmpty() const -> terreate::Result<void> {
  if (!entries_.empty())
    return std::unexpected(fail(SwapchainRetirementError::pending_entries, "requireEmpty",
                                "the queue still owns pending Swapchains"));
  return {};
}

auto SwapchainRetirementQueue::retire(const Device &device, Swapchain &swapchain,
                                      const SwapchainRetirementUses &uses)
    -> terreate::Result<void> {
  constexpr auto operation = "SwapchainRetirementQueue::retire";
  if (!device.valid())
    return std::unexpected(
        fail(SwapchainRetirementError::invalid_device, operation, "moved-from Device"));
  if (!swapchain.valid())
    return std::unexpected(
        fail(SwapchainRetirementError::invalid_swapchain, operation, "moved-from Swapchain"));
  if (lost_)
    return std::unexpected(
        fail(SwapchainRetirementError::device_lost, operation, "the queue observed a lost Device"));
  const auto native_device = static_cast<VkDevice>(device.nativeHandle());
  if (!swapchain.correlatedWith(device) || (device_ != VK_NULL_HANDLE && device_ != native_device))
    return std::unexpected(fail(SwapchainRetirementError::parent_mismatch, operation,
                                "the Swapchain and the queue must share one Device"));
  // Submission completion is never taken as present completion: the present side
  // needs fences, or the explicit statement that no present can be pending.
  if (uses.present_fences.empty() && !uses.no_pending_present)
    return std::unexpected(fail(SwapchainRetirementError::missing_evidence, operation,
                                "declare present fences, or no_pending_present"));
  if (!uses.present_fences.empty() && uses.no_pending_present)
    return std::unexpected(fail(SwapchainRetirementError::contradictory_evidence, operation,
                                "no_pending_present contradicts the present fences"));
  for (const auto fence : uses.present_fences)
    if (!fence)
      return std::unexpected(
          fail(SwapchainRetirementError::invalid_fence, operation, "a present fence is null"));
  for (const auto &receipt : uses.submissions) {
    const auto &completion = receipt.completion();
    if (completion.device == nullptr || !completion.device->valid() ||
        !completion.requested.valid() || !completion.effective.valid())
      return std::unexpected(fail(SwapchainRetirementError::invalid_submission, operation,
                                  "a receipt has no valid completion"));
    if (static_cast<VkDevice>(completion.device->nativeHandle()) != native_device)
      return std::unexpected(fail(SwapchainRetirementError::parent_mismatch, operation,
                                  "a receipt belongs to another Device"));
  }

  try {
    std::vector<SubmissionReceipt> receipts(uses.submissions.begin(), uses.submissions.end());
    std::vector<VkFence> fences;
    fences.reserve(uses.present_fences.size());
    for (const auto fence : uses.present_fences)
      fences.push_back(static_cast<VkFence>(fence));
    entries_.reserve(entries_.size() + 1);
    // Every allocation is done: the noexcept moves below cannot fail.
    entries_.push_back(Entry{std::move(swapchain), std::move(receipts), std::move(fences)});
  } catch (const std::bad_alloc &) {
    return std::unexpected(fail(SwapchainRetirementError::allocation_failed, operation,
                                "the caller still owns the Swapchain"));
  }
  device_ = native_device;
  return {};
}

auto SwapchainRetirementQueue::collect(const Device &device) -> terreate::Result<CollectReport> {
  constexpr auto operation = "SwapchainRetirementQueue::collect";
  if (lost_)
    return std::unexpected(
        fail(SwapchainRetirementError::device_lost, operation, "the queue observed a lost Device"));
  if (entries_.empty())
    return CollectReport{0, 0};
  if (!device.valid() || static_cast<VkDevice>(device.nativeHandle()) != device_)
    return std::unexpected(fail(SwapchainRetirementError::parent_mismatch, operation,
                                "collect with the Device of the entries"));
  std::vector<bool> ready;
  try {
    ready.resize(entries_.size(), false);
  } catch (const std::bad_alloc &) {
    return std::unexpected(
        fail(SwapchainRetirementError::allocation_failed, operation, "ready flags"));
  }
  // Two phases: a failed native observation keeps every owner and every condition.
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    bool all = true;
    for (const auto &receipt : entries_[i].receipts) {
      const auto observation = receipt.poll();
      if (!observation) {
        if (observation.error().code() == vk::make_error_code(vk::Result::eErrorDeviceLost))
          lost_ = true;
        return std::unexpected(observation.error());
      }
      if (observation->state != SubmissionCompletionState::completed)
        all = false;
    }
    for (const auto fence : entries_[i].fences) {
      const VkResult status = vkGetFenceStatus(device_, fence);
      if (status == VK_NOT_READY) {
        all = false;
      } else if (status != VK_SUCCESS) {
        if (status == VK_ERROR_DEVICE_LOST)
          lost_ = true;
        const auto code = vk::make_error_code(static_cast<vk::Result>(status));
        try {
          return std::unexpected(terreate::Error{code, operation, "vkGetFenceStatus failed"});
        } catch (const std::bad_alloc &) {
          return std::unexpected(terreate::Error{code});
        }
      }
    }
    ready[i] = all;
  }
  std::size_t collected = 0;
  for (std::size_t i = entries_.size(); i > 0; --i) {
    if (ready[i - 1]) {
      entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i - 1));
      ++collected;
    }
  }
  if (entries_.empty())
    device_ = VK_NULL_HANDLE;
  return CollectReport{collected, entries_.size()};
}

auto SwapchainRetirementQueue::reclaimAfterDeviceLoss(const Device &device)
    -> terreate::Result<std::size_t> {
  constexpr auto operation = "SwapchainRetirementQueue::reclaimAfterDeviceLoss";
  if (!lost_)
    return std::unexpected(fail(SwapchainRetirementError::device_loss_not_observed, operation,
                                "no native device loss observed"));
  if (!device.valid() || static_cast<VkDevice>(device.nativeHandle()) != device_)
    return std::unexpected(fail(SwapchainRetirementError::parent_mismatch, operation,
                                "must specify the original live lost Device"));
  const auto count = entries_.size();
  // Device loss is not normal completion: this is the explicit lost-Device
  // teardown, under the caller's synchronization, with the parents still alive.
  while (!entries_.empty())
    entries_.pop_back();
  device_ = VK_NULL_HANDLE;
  return count;
}

} // namespace terreate::graphics

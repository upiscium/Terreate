#ifndef TERREATE_GRAPHICS_SWAPCHAIN_RETIREMENT_HPP
#define TERREATE_GRAPHICS_SWAPCHAIN_RETIREMENT_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <vector>

#include <terreate/core/result.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/swapchain.hpp>

namespace terreate::graphics {

enum class SwapchainRetirementError : std::uint8_t {
  invalid_device = 1,
  invalid_swapchain,
  /// The Swapchain, a receipt or the queue's earlier entries belong to another Device.
  parent_mismatch,
  /// No completion evidence was given and `no_pending_present` was not set.
  missing_evidence,
  /// `no_pending_present` was set together with present fences.
  contradictory_evidence,
  invalid_fence,
  invalid_submission,
  pending_entries,
  device_lost,
  device_loss_not_observed,
  allocation_failed,
};

[[nodiscard]] const std::error_category &swapchain_retirement_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SwapchainRetirementError error) noexcept;

/// What proves that nothing can still use the Swapchain, its images or the
/// semaphores of its images. Every term must be complete before it is destroyed.
struct SwapchainRetirementUses {
  /// Successful native submissions that used its images (the existing receipts).
  std::span<const SubmissionReceipt> submissions{};
  /// The present fences (`PresentDescription::fence`, VK_KHR_swapchain_maintenance1)
  /// of every present of its images that may still be pending. Each is owned by
  /// the application and must stay untouched (no reset, reuse or destruction)
  /// until the Swapchain is collected. Do not list the fence of a present that
  /// returned an error or `out_of_date`: it may never be signaled, and the queue
  /// would keep the Swapchain for ever (a non-empty queue terminates at
  /// destruction). The application keeps such a fence and decides, by its own
  /// synchronization, when the Swapchain can go.
  std::span<const vk::Fence> present_fences{};
  /// The application states that no present of this Swapchain can be pending (for
  /// example it never presented). Without it, present fences are required: a
  /// submission receipt alone is never taken as present completion. Setting it
  /// together with present fences is `contradictory_evidence`.
  bool no_pending_present = false;
};

/// A caller-owned, explicit retirement queue for Swapchains: it owns each
/// Swapchain (and with it the image views it made) until every declared
/// submission and every declared present fence is complete, then destroys it. It
/// never waits, schedules, polls in the background or calls a wait-idle: each
/// `collect` makes one non-blocking observation per term. A Swapchain retired by
/// `recreateSwapchain` is the normal input, so the old generation goes away
/// without a `vkDeviceWaitIdle`.
///
/// The present semaphores of the old generation's images (application-owned) may
/// be reused or destroyed once their Swapchain has been collected. This covers
/// presented images only: an image that was acquired and never presented is not
/// covered (its acquire semaphore or fence stays the application's concern, and
/// `vkReleaseSwapchainImagesKHR` is not offered here), so retire a Swapchain only
/// when every image it acquired has been presented. `retire` also accepts a
/// Swapchain that was not retired by a recreation (for example at shutdown). A
/// latched device loss stays latched, also after `reclaimAfterDeviceLoss`.
///
/// Destroying a non-empty queue is a fail-fast contract violation, as for
/// `RetirementQueue`: collect every entry, or use `reclaimAfterDeviceLoss` after
/// `collect` observed VK_ERROR_DEVICE_LOST. The Surface must outlive every entry.
class SwapchainRetirementQueue final {
public:
  SwapchainRetirementQueue() = default;
  SwapchainRetirementQueue(const SwapchainRetirementQueue &) = delete;
  SwapchainRetirementQueue &operator=(const SwapchainRetirementQueue &) = delete;
  SwapchainRetirementQueue(SwapchainRetirementQueue &&) = delete;
  SwapchainRetirementQueue &operator=(SwapchainRetirementQueue &&) = delete;
  ~SwapchainRetirementQueue() noexcept;

  /// Take ownership of `swapchain` on success (it is moved from). On any failure
  /// the caller still owns it. Fails with invalid_device, invalid_swapchain,
  /// parent_mismatch, missing_evidence, invalid_fence (a null fence),
  /// invalid_submission (a receipt of another Device or an invalid one),
  /// device_lost or allocation_failed.
  [[nodiscard]] auto retire(const Device &device, Swapchain &swapchain,
                            const SwapchainRetirementUses &uses) -> terreate::Result<void>;

  /// One non-blocking observation per declared term. Only an entry whose every
  /// term is complete is destroyed; any native error releases nothing, and a
  /// native VK_ERROR_DEVICE_LOST is latched for `reclaimAfterDeviceLoss`.
  [[nodiscard]] auto collect(const Device &device) -> terreate::Result<CollectReport>;

  /// Destructive teardown on a LOST Device only, after `collect` observed the loss.
  [[nodiscard]] auto reclaimAfterDeviceLoss(const Device &device) -> terreate::Result<std::size_t>;

  [[nodiscard]] std::size_t pendingCount() const noexcept { return entries_.size(); }
  [[nodiscard]] bool deviceLossObserved() const noexcept { return lost_; }
  [[nodiscard]] auto requireEmpty() const -> terreate::Result<void>;

private:
  struct Entry {
    Swapchain swapchain;
    std::vector<SubmissionReceipt> receipts;
    std::vector<VkFence> fences;
  };

  std::vector<Entry> entries_{};
  VkDevice device_ = VK_NULL_HANDLE;
  bool lost_ = false;
};

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::SwapchainRetirementError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SWAPCHAIN_RETIREMENT_HPP

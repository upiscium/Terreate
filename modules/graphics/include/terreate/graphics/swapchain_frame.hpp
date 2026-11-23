#ifndef TERREATE_GRAPHICS_SWAPCHAIN_FRAME_HPP
#define TERREATE_GRAPHICS_SWAPCHAIN_FRAME_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <system_error>

#include <terreate/core/result.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/swapchain.hpp>

namespace terreate::graphics {

enum class SwapchainFrameError : std::uint8_t {
  invalid_device = 1,
  invalid_queue,
  invalid_swapchain,
  /// The Device, Queue or Swapchain do not belong together.
  parent_mismatch,
  /// The Swapchain was passed as `old_swapchain` to a successful creation: no
  /// image can be acquired from it any more (images already acquired may still
  /// be presented).
  swapchain_retired,
  /// The image does not belong to this Swapchain (another generation, or empty).
  stale_image,
  invalid_description,
  /// A native function could not be loaded from the Device.
  function_unavailable,
  allocation_failed,
};

[[nodiscard]] const std::error_category &swapchain_frame_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SwapchainFrameError error) noexcept;

/// All inputs of one acquire; nothing is waited for unless `timeout_ns` says so.
/// At least one of `semaphore` and `fence` must be set (Vulkan requires it); the
/// application owns both. The semaphore must be a binary semaphore that is
/// unsignaled and has no pending operation (see the lifetime rules below).
struct AcquireDescription {
  /// 0 polls and never blocks (`not_ready` when no image is available);
  /// UINT64_MAX waits without limit. There is no hidden default wait.
  std::uint64_t timeout_ns = 0;
  vk::Semaphore semaphore{};
  vk::Fence fence{};
};

enum class AcquireStatus : std::uint8_t {
  /// `image` is set and usable.
  acquired,
  /// `image` is set and usable now, but the Swapchain no longer matches the
  /// Surface (VK_SUBOPTIMAL_KHR): recreate when convenient.
  acquired_suboptimal,
  /// No image: the Swapchain can no longer present to the Surface (VK_ERROR_OUT_OF_DATE_KHR)
  /// and must be recreated before any further acquire. The semaphore and fence
  /// were not signaled and stay unsignaled.
  out_of_date,
  /// No image: the timeout elapsed (VK_TIMEOUT); the semaphore and fence are untouched.
  timeout,
  /// No image: `timeout_ns` was 0 and none was ready yet (VK_NOT_READY).
  not_ready,
};

/// A usable image exists only when `image` is set: the status alone never
/// implies a valid image for a failed acquire.
struct AcquireResult {
  AcquireStatus status = AcquireStatus::not_ready;
  /// The image acquired (index and generation inside it); set exactly for
  /// `acquired` and `acquired_suboptimal`.
  std::optional<SwapchainImage> image{};
  /// The native result behind `status`.
  vk::Result native_result = vk::Result::eNotReady;

  [[nodiscard]] bool usable() const noexcept { return image.has_value(); }
  /// True for `acquired_suboptimal` and `out_of_date`.
  [[nodiscard]] bool recreationNeeded() const noexcept {
    return status == AcquireStatus::acquired_suboptimal || status == AcquireStatus::out_of_date;
  }
};

/// Acquire the next image of `swapchain`: exactly one vkAcquireNextImageKHR. The
/// application chooses when to call it and what to do with the result; nothing
/// is scheduled, retried or waited for beyond `timeout_ns`. Fails, before any
/// native call, with invalid_device, invalid_swapchain, parent_mismatch,
/// swapchain_retired, invalid_description (no semaphore or fence) or
/// function_unavailable. A native failure that is not one of the statuses above
/// (VK_ERROR_SURFACE_LOST_KHR, device lost, out of memory, ...) is returned as
/// that native error, never as a status.
///
/// Synchronization lifetime. The application externally synchronizes the
/// Swapchain, the semaphore and the fence. After `acquired*` the semaphore will
/// be signaled and must be waited on (by the submission that renders into the
/// image) before it is used by another acquire; reuse it only after that wait
/// has completed. An acquired image must be presented or the Swapchain must be
/// destroyed; an out-of-date or timed-out acquire acquired nothing.
[[nodiscard]] auto acquireNextImage(const Device &device, const Swapchain &swapchain,
                                    const AcquireDescription &description)
    -> terreate::Result<AcquireResult>;

/// What to present. `wait_semaphores` are binary semaphores that must be
/// signaled (or have a pending signal) by work that finishes the image.
struct PresentDescription {
  /// An image acquired from this same Swapchain; a copy is fine.
  SwapchainImage image{};
  std::span<const vk::Semaphore> wait_semaphores{};
};

enum class PresentStatus : std::uint8_t {
  presented,
  /// Presented, but the Swapchain no longer matches the Surface (VK_SUBOPTIMAL_KHR).
  presented_suboptimal,
  /// Not presented: the Swapchain must be recreated (VK_ERROR_OUT_OF_DATE_KHR).
  out_of_date,
};

struct PresentResult {
  PresentStatus status = PresentStatus::presented;
  vk::Result native_result = vk::Result::eSuccess;

  [[nodiscard]] bool recreationNeeded() const noexcept {
    return status != PresentStatus::presented;
  }
};

/// Present `description.image` on `queue`: exactly one vkQueuePresentKHR for one
/// swapchain. Fails, before any native call, with invalid_device, invalid_queue,
/// invalid_swapchain, parent_mismatch, stale_image (the image does not belong to
/// this Swapchain's generation), invalid_description (a null wait semaphore) or
/// function_unavailable. Native failures other than the statuses above are
/// returned as native errors. That the queue family can present to the Surface
/// is not re-checked here (it was resolved before the Device existed).
///
/// Synchronization lifetime. A semaphore waited on by a present may be reused
/// only after the presentation engine is done with it. Without a present fence
/// (not offered here) that is known only when the same image index is acquired
/// again from this Swapchain, which proves the earlier present of that image
/// has finished waiting. A common safe scheme is one such semaphore per
/// swapchain image index, reused when that index is acquired again. Never reuse
/// it earlier, and never destroy it or the Swapchain while a present may still
/// wait on it (destroying a Swapchain after the GPU is idle for its images is
/// the application's synchronization).
[[nodiscard]] auto presentImage(const Device &device, Queue queue, const Swapchain &swapchain,
                                const PresentDescription &description)
    -> terreate::Result<PresentResult>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::SwapchainFrameError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SWAPCHAIN_FRAME_HPP

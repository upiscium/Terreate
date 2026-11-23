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
  /// A present fence was given but the Device did not enable the
  /// swapchainMaintenance1 feature (never ignored).
  present_fence_not_supported,
};

[[nodiscard]] const std::error_category &swapchain_frame_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SwapchainFrameError error) noexcept;

/// All inputs of one acquire; nothing is waited for unless `timeout_ns` says so.
/// At least one of `semaphore` and `fence` must be set (Vulkan requires it); the
/// application owns both. The semaphore must be a binary semaphore that is
/// unsignaled and has no pending operation (see the lifetime rules below).
struct AcquireDescription {
  /// 0 polls and never blocks (`not_ready` when no image is available);
  /// UINT64_MAX waits without limit. There is no hidden default wait. Do not
  /// wait without limit while the application already holds more acquired,
  /// not yet presented images than `imageCount - minImageCount` allows
  /// (VUID-vkAcquireNextImageKHR-swapchain-01802): no image can become
  /// available and the call would block forever.
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
  /// No image: the Swapchain can no longer present to the Surface
  /// (VK_ERROR_OUT_OF_DATE_KHR) and must be recreated before any further
  /// acquire. Drivers leave the semaphore and fence unsignaled, but Vulkan does
  /// not promise it: do not rely on the semaphore or fence afterwards, and wait
  /// for the device or destroy them before reusing them.
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

/// The pure mapping from a native vkAcquireNextImageKHR result to a status
/// (success, VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR, VK_TIMEOUT,
/// VK_NOT_READY). Any other result is not a status: `acquireNextImage` returns
/// it as a native error. Also useful when the application calls the native
/// function itself.
[[nodiscard]] auto classifyAcquireResult(vk::Result result) noexcept
    -> std::optional<AcquireStatus>;

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
/// Swapchain, the semaphore and the fence. The semaphore must be binary,
/// unsignaled and have no pending operation; the fence must be unsignaled and
/// have no pending operation (VUID-vkAcquireNextImageKHR-fence-01287). After
/// `acquired*` the semaphore will be signaled and must be waited on (by the
/// submission that renders into the image) before another acquire uses it;
/// reuse it only after that wait has completed. With a fence alone (no
/// semaphore) the image must not be used until the fence is signaled. An
/// acquired image must be presented or the Swapchain destroyed; a timed-out or
/// not-ready acquire acquired nothing and leaves the semaphore and fence
/// untouched. If the driver reports an impossible image index the call returns a
/// native error and the semaphore and fence state is then unknown.
[[nodiscard]] auto acquireNextImage(const Device &device, const Swapchain &swapchain,
                                    const AcquireDescription &description)
    -> terreate::Result<AcquireResult>;

/// What to present. `wait_semaphores` are binary semaphores that must be
/// signaled (or have a pending signal) by work that finishes the image.
struct PresentDescription {
  /// An image acquired from this same Swapchain; a copy is fine.
  SwapchainImage image{};
  std::span<const vk::Semaphore> wait_semaphores{};
  /// Optional present fence (VK_KHR_swapchain_maintenance1). It is signaled when
  /// this present no longer uses its wait semaphores and its image, which is the
  /// non-blocking proof the old generation needs before its semaphores can be
  /// reused and its Swapchain destroyed (see `SwapchainRetirementQueue`). It must
  /// be unsignaled with no pending operation, belong to the Device, and stay
  /// untouched (no reset, reuse or destruction) until it is signaled. Setting it
  /// needs the Device to have enabled the swapchainMaintenance1 feature
  /// (`DevicePlan::enabledSwapchainMaintenance1()`), else `present_fence_not_supported`.
  vk::Fence fence{};
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

/// The pure mapping from a native vkQueuePresentKHR result to a status (success,
/// VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR). Any other result is returned by
/// `presentImage` as a native error.
[[nodiscard]] auto classifyPresentResult(vk::Result result) noexcept
    -> std::optional<PresentStatus>;

/// Present `description.image` on `queue`: exactly one vkQueuePresentKHR for one
/// swapchain. Fails, before any native call, with invalid_device, invalid_queue,
/// invalid_swapchain, parent_mismatch, stale_image (the image does not belong to
/// this Swapchain's generation), invalid_description (a null wait semaphore) or
/// function_unavailable. Native failures other than the statuses above are
/// returned as native errors. That the queue family can present to the Surface
/// is not re-checked here (it was resolved before the Device existed).
///
/// Preconditions the library does not check: the application externally
/// synchronizes the Queue and the Swapchain; the image was acquired from this
/// Swapchain, is not already presented, and is in VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
/// when the wait semaphores are signaled (`owns` cannot detect a double
/// present). With no wait semaphores, ordering the rendering before the present
/// is entirely the application's job.
///
/// Synchronization lifetime. A semaphore waited on by a present may be reused
/// only after the presentation engine is done with it. Without a present fence
/// (not offered here) that is known only when the same image index is acquired
/// again from this same Swapchain, which proves the earlier present of that
/// image has finished waiting. A common safe scheme is one such semaphore per
/// swapchain image index, reused when that index is acquired again. The proof
/// is valid only inside one Swapchain: after a recreation the old Swapchain
/// cannot acquire (it is retired), so the semaphores of its images may be
/// reused or destroyed only after the old Swapchain is destroyed or the Queue
/// is idle. If a present returns an error or `out_of_date`, do not assume its
/// wait semaphores were consumed: treat them like the old generation. Never
/// destroy a semaphore or the Swapchain while a present may still wait on it
/// (making the GPU idle for the Swapchain first is the application's
/// synchronization).
[[nodiscard]] auto presentImage(const Device &device, Queue queue, const Swapchain &swapchain,
                                const PresentDescription &description)
    -> terreate::Result<PresentResult>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::SwapchainFrameError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SWAPCHAIN_FRAME_HPP

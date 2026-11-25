#ifndef TERREATE_GRAPHICS_SWAPCHAIN_HPP
#define TERREATE_GRAPHICS_SWAPCHAIN_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include <terreate/core/result.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/presentation.hpp>
#include <terreate/graphics/surface.hpp>

namespace terreate::graphics {

class Swapchain;

enum class SwapchainError : std::uint8_t {
  invalid_instance = 1,
  /// The Device, Surface or old Swapchain is moved-from, or does not belong to
  /// the Instance (or to the same Device and Surface for an old Swapchain).
  parent_mismatch,
  /// The Device plan did not enable VK_KHR_swapchain (never a silent no-op).
  swapchain_extension_not_enabled,
  /// A swapchain function could not be loaded from the Device.
  function_unavailable,
  invalid_description,
  /// The Surface cannot present, or a value is not supported by the observed
  /// capabilities; nothing is clamped or replaced silently.
  unsupported_configuration,
  /// The extent is zero (for example a minimized window): a Swapchain cannot be
  /// created; try again once the window has an extent.
  no_image_extent,
  allocation_failed,
  /// The old Swapchain was already passed to a successful createSwapchain (the
  /// driver retired it; it can be passed only once).
  old_swapchain_retired,
};

[[nodiscard]] const std::error_category &swapchain_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SwapchainError error) noexcept;

/// What the application wants beyond the presentation resolution. Every field
/// has a documented default and the effective value is observable in the
/// resulting `SwapchainDescription` and `SwapchainConfiguration`.
struct SwapchainRequest {
  /// Used only when the Surface leaves its extent to the Swapchain
  /// (`currentExtent` is 0xFFFFFFFF; a defined `currentExtent`, even 0, wins and
  /// this field is ignored); then it is clamped to the supported range
  /// and a zero extent is `no_image_extent`. A Surface with a defined
  /// `currentExtent` always wins (Vulkan requires it).
  vk::Extent2D extent{};
  /// Default: `minImageCount + 1`, limited by `maxImageCount`. An explicit value
  /// outside the supported range is `unsupported_configuration`, not clamped.
  std::optional<std::uint32_t> min_image_count{};
  /// Must be a non-empty subset of `supportedUsageFlags`.
  vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment;
  /// Empty: exclusive sharing. Otherwise concurrent sharing between these (at
  /// least two, distinct) queue families.
  std::vector<std::uint32_t> concurrent_queue_families{};
  /// Default: the Surface's `currentTransform`.
  std::optional<vk::SurfaceTransformFlagBitsKHR> pre_transform{};
  /// Must be supported; no other bit is substituted.
  vk::CompositeAlphaFlagBitsKHR composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
  bool clipped = true;
};

/// The complete, explicit creation input. `resolveSwapchain` fills it from the
/// observation and the presentation resolution; an application may also write
/// it by hand. `old_swapchain` is the recreation hook: a borrowed pointer, used
/// only during `createSwapchain` (it is not retained). It may be passed only once:
/// Vulkan retires it as soon as the native creation is attempted with it, even
/// if that creation fails, so after such an attempt it is `retired()` and passing
/// it again is `old_swapchain_retired`. To recover from a failed creation, make
/// sure the GPU is done with the old Swapchain, destroy it, and create a new
/// Swapchain without `old_swapchain`. `recreateSwapchain` builds on this hook.
struct SwapchainDescription {
  vk::SurfaceFormatKHR format{};
  vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo;
  std::uint32_t min_image_count = 0;
  vk::Extent2D extent{};
  std::uint32_t image_array_layers = 1;
  vk::ImageUsageFlags usage{};
  vk::SharingMode sharing_mode = vk::SharingMode::eExclusive;
  std::vector<std::uint32_t> queue_family_indices{};
  vk::SurfaceTransformFlagBitsKHR pre_transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
  vk::CompositeAlphaFlagBitsKHR composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
  bool clipped = true;
  const Swapchain *old_swapchain = nullptr;
};

/// Pure; no native call. Fails with invalid_description (a malformed
/// concurrent queue-family list), unsupported_configuration (the Surface cannot present, the format
/// or present mode in `resolution` is not in `support`, or a request value is
/// outside the observed capabilities) or no_image_extent.
[[nodiscard]] auto resolveSwapchain(const SurfaceSupport &support,
                                    const PresentationResolution &resolution,
                                    const SwapchainRequest &request)
    -> terreate::Result<SwapchainDescription>;

/// The values of one swapchain image, named so none can be swapped by mistake.
struct SwapchainImageInit {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  std::uint32_t index = 0;
  std::uint64_t generation = 0;
  vk::Format format = vk::Format::eUndefined;
  vk::Extent2D extent{};
  vk::ImageUsageFlags usage{};
};

/// A plain, copyable, non-owning view of one swapchain image and of the
/// Swapchain-owned image view created for it. The native Image is owned by the
/// Swapchain (it is never a VMA allocation and must not be passed to a
/// destroy path); the image view, when there is one, is owned by the Swapchain too. A view exists
/// only when `usage` includes a view-capable bit (sampled, storage, color, depth-stencil or input
/// attachment); a transfer-only Swapchain has none and `nativeImageView()` is null. Both die with
/// the Swapchain generation that produced them: use after that Swapchain is
/// destroyed is invalid by contract, like Queue, but `generation()` lets code
/// detect a view of a retired Swapchain (`Swapchain::owns`).
class SwapchainImage final {
public:
  /// An empty value (`valid()` is false). Only createSwapchain makes a real one,
  /// so a generation cannot be forged to satisfy `Swapchain::owns`.
  SwapchainImage() noexcept = default;

  [[nodiscard]] bool valid() const noexcept { return image_ != VK_NULL_HANDLE; }
  [[nodiscard]] vk::Image nativeImage() const noexcept { return vk::Image{image_}; }
  /// The same image as `nativeImage()`, under the name `Image::nativeHandle()` uses.
  [[nodiscard]] vk::Image nativeHandle() const noexcept { return vk::Image{image_}; }
  [[nodiscard]] vk::ImageView nativeImageView() const noexcept { return vk::ImageView{view_}; }
  /// The index `vkAcquireNextImageKHR` reports for this image.
  [[nodiscard]] std::uint32_t index() const noexcept { return index_; }
  /// The unique identity of the Swapchain that owns this image (never reused
  /// within the process).
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] vk::Format format() const noexcept { return format_; }
  [[nodiscard]] vk::Extent2D extent() const noexcept { return extent_; }
  [[nodiscard]] vk::ImageUsageFlags usage() const noexcept { return usage_; }

private:
  VkImage image_ = VK_NULL_HANDLE;
  VkImageView view_ = VK_NULL_HANDLE;
  std::uint32_t index_ = 0;
  std::uint64_t generation_ = 0;
  vk::Format format_ = vk::Format::eUndefined;
  vk::Extent2D extent_{};
  vk::ImageUsageFlags usage_{};

  explicit SwapchainImage(const SwapchainImageInit &init) noexcept
      : image_(init.image), view_(init.view), index_(init.index), generation_(init.generation),
        format_(init.format), extent_(init.extent), usage_(init.usage) {}

  friend auto createSwapchain(const Instance &, const Device &, const Surface &,
                              const SwapchainDescription &) -> terreate::Result<Swapchain>;
};

/// The effective state, observable after creation.
struct SwapchainConfiguration {
  /// As given; `old_swapchain` is cleared in this copy (it is not retained).
  SwapchainDescription requested{};
  vk::Format format = vk::Format::eUndefined;
  vk::ColorSpaceKHR color_space = vk::ColorSpaceKHR::eSrgbNonlinear;
  vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo;
  vk::Extent2D extent{};
  /// What the driver actually created: it may exceed `min_image_count`.
  std::uint32_t image_count = 0;
  vk::ImageUsageFlags usage{};
  vk::SharingMode sharing_mode = vk::SharingMode::eExclusive;
  /// 1 for a first Swapchain; the old Swapchain's `lineage() + 1` when an old
  /// Swapchain was passed.
  std::uint32_t lineage = 1;
};

/// A move-only owner of one VkSwapchainKHR and of the image views of its
/// images. The Device and Surface are borrowed, not retained. Lifetime
/// contract, not checked: destroy it before its Surface and before its Device,
/// after the GPU has finished with its images (the caller owns that
/// synchronization), and do not destroy the images. Acquire and present are in
/// swapchain_frame.hpp; recreation is `recreateSwapchain` below.
class Swapchain final {
public:
  Swapchain() = delete;
  Swapchain(const Swapchain &) = delete;
  Swapchain &operator=(const Swapchain &) = delete;
  Swapchain(Swapchain &&other) noexcept;
  /// Destroys the swapchain this object owned before taking over `other`'s.
  Swapchain &operator=(Swapchain &&other) noexcept;
  ~Swapchain();

  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] bool valid() const noexcept { return implementation_ != nullptr; }

  /// The borrowed native handle; never destroy it. Null when moved from.
  [[nodiscard]] vk::SwapchainKHR nativeHandle() const noexcept;
  [[nodiscard]] VkSwapchainKHR nativeSwapchain() const noexcept;
  [[nodiscard]] const SwapchainConfiguration *configuration() const noexcept;
  /// Process-unique identity of this Swapchain (0 when moved from).
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] std::uint32_t lineage() const noexcept;
  /// The images in native order; empty when moved from.
  [[nodiscard]] std::span<const SwapchainImage> images() const noexcept;
  /// True when `image` belongs to this Swapchain (same generation). Use it to
  /// reject an image or view that belongs to a retired Swapchain.
  [[nodiscard]] bool owns(const SwapchainImage &image) const noexcept;
  /// True once this Swapchain was passed as `old_swapchain` to a successful
  /// createSwapchain. A retired Swapchain stays valid until the application
  /// destroys it (after the GPU is done with its images) but cannot be passed
  /// as `old_swapchain` again.
  [[nodiscard]] bool retired() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  [[nodiscard]] bool correlatedWith(const Surface &surface) const noexcept;

  struct Impl;

private:
  explicit Swapchain(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> implementation_{};

  friend auto createSwapchain(const Instance &, const Device &, const Surface &,
                              const SwapchainDescription &) -> terreate::Result<Swapchain>;
};

/// Create a Swapchain for `surface` on `device`. Fails, before any native call,
/// with invalid_instance, parent_mismatch, swapchain_extension_not_enabled,
/// function_unavailable, invalid_description, parent_mismatch (also for an old
/// Swapchain of another Device or Surface) or old_swapchain_retired. A native
/// VkResult failure is returned as that native error. The Device must have been
/// created with the present queue and the VK_KHR_swapchain requirement that the
/// presentation resolution produced (see `applyPresentation`); that the
/// queue family can present to this Surface is not re-checked here.
[[nodiscard]] auto createSwapchain(const Instance &instance, const Device &device,
                                   const Surface &surface, const SwapchainDescription &description)
    -> terreate::Result<Swapchain>;

enum class RecreationStatus : std::uint8_t {
  /// `RecreationResult::swapchain` is set; the old Swapchain is now retired.
  recreated,
  /// The Surface has no extent right now (for example a minimized window) and
  /// the request does not give one: nothing was created and the old Swapchain is
  /// untouched. The application decides when to try again; there is no loop,
  /// sleep or retry inside the library, so no busy loop can start here.
  deferred_zero_extent,
};

struct RecreationResult {
  RecreationStatus status = RecreationStatus::deferred_zero_extent;
  /// Set exactly for `recreated`.
  std::optional<Swapchain> swapchain{};
};

/// Recreate `old` for `surface`: observe the Surface again (its capabilities,
/// formats and extent change with the window), re-resolve the presentation and
/// the Swapchain with the application's own requests, and create the new
/// Swapchain with `old` as its old Swapchain. The application chooses when to
/// call it (after a resize, `out_of_date` or `suboptimal`) and when to retry.
/// There is no `vkDeviceWaitIdle` here and nothing is scheduled or retried.
///
/// On `recreated` the old Swapchain is `retired()`: it cannot acquire any more,
/// images already acquired from it may still be presented, and it (with the
/// semaphores of its images) may be destroyed only when the application knows
/// the GPU is done with them (see `presentImage`). A second recreation from the
/// same retired Swapchain is `old_swapchain_retired`. Recreating one
/// Swapchain touches no other Swapchain, Surface or Device, so several windows
/// recreate independently.
///
/// What may change: the Surface is observed again, so the resolved format, color
/// space, present mode, extent, image count and the present queue family can
/// differ from the previous Swapchain (a preference that is not `require_*`
/// falls back as the default policy says). Compare `configuration()` of the new
/// Swapchain with the old one (a changed format needs matching pipeline and
/// attachment formats), or use `require_*` to fail instead. `createSwapchain`
/// does not re-check that the present queue family can present to the Surface.
/// `candidate` must be the physical device that `device` was created from (not
/// checked).
///
/// Failure. Errors of `querySurfaceSupport`, `resolvePresentation` and
/// `resolveSwapchain` are returned before any native creation call, with `old`
/// untouched; `resolveSwapchain` checks the extent first, so with an empty extent a
/// different request error shows up only once the extent is available (the
/// deferred status hides it meanwhile). Once the native creation was attempted
/// (`createSwapchain`), `old` is retired even if it failed: see
/// `SwapchainDescription::old_swapchain` for recovery. A native failure such as
/// VK_ERROR_SURFACE_LOST_KHR is returned as that native error (it needs a new
/// Surface).
[[nodiscard]] auto recreateSwapchain(const Instance &instance, const Device &device,
                                     const PhysicalDeviceCandidate &candidate,
                                     const Surface &surface, const Swapchain &old,
                                     const PresentationRequest &presentation,
                                     const SwapchainRequest &request)
    -> terreate::Result<RecreationResult>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::SwapchainError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SWAPCHAIN_HPP

#ifndef TERREATE_GRAPHICS_PRESENTATION_HPP
#define TERREATE_GRAPHICS_PRESENTATION_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <terreate/core/result.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/surface.hpp>

namespace terreate::graphics {

/// The device extension every presenting Device needs.
inline constexpr const char *presentation_device_extension = "VK_KHR_swapchain";

enum class PresentationError : std::uint8_t {
  invalid_instance = 1,
  /// The PhysicalDeviceCandidate or the Surface does not belong to the Instance.
  parent_mismatch,
  /// A surface-query function could not be loaded from the Instance.
  function_unavailable,
  invalid_request,
  /// No queue family of the device can present to the Surface.
  no_present_queue_family,
  swapchain_extension_unavailable,
  no_supported_format,
  no_supported_present_mode,
  allocation_failed,
  /// No candidate is selectable for the Surface (see `evaluateSurfaceCandidates`).
  no_selectable_candidate,
  /// A library invariant was broken (a defect, not a usage error).
  internal_inconsistency,
};

[[nodiscard]] const std::error_category &presentation_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(PresentationError error) noexcept;

/// A queue family that can present to the Surface, with its native flags (so a
/// request for a flag such as graphics can be checked against it).
struct PresentQueueFamily {
  std::size_t index = 0;
  vk::QueueFlags flags{};
};

/// What one physical device can do for one Surface: a value-owned, point-in-time
/// observation (no native handle is retained). Window-dependent values such as
/// `capabilities.currentExtent` can change later, so a Swapchain (and its
/// recreation) queries again. The public members also make synthetic fixtures
/// for the pure resolution below straightforward.
///
/// A device that cannot present to the Surface is a valid observation, not an
/// error: `present_queue_families` is empty and the remaining surface fields are
/// left empty, because Vulkan allows those queries only for a supported surface.
struct SurfaceSupport {
  /// Queue families of the device that can present to the Surface, by ascending
  /// index.
  std::vector<PresentQueueFamily> present_queue_families{};
  vk::SurfaceCapabilitiesKHR capabilities{};
  std::vector<vk::SurfaceFormatKHR> formats{};
  std::vector<vk::PresentModeKHR> present_modes{};
  /// The device lists VK_KHR_swapchain.
  bool swapchain_extension_available = false;

  [[nodiscard]] bool canPresent() const noexcept { return !present_queue_families.empty(); }
};

/// Observe what `candidate` can do for `surface`. No Device is needed. Fails,
/// before any native call, with invalid_instance or parent_mismatch (the
/// candidate or the Surface belongs to another Instance, or the Surface was
/// moved from); function_unavailable if a surface query cannot be loaded. A
/// native VkResult failure (for example VK_ERROR_SURFACE_LOST_KHR) is returned
/// as that native error.
[[nodiscard]] auto querySurfaceSupport(const Instance &instance,
                                       const PhysicalDeviceCandidate &candidate,
                                       const Surface &surface) -> terreate::Result<SurfaceSupport>;

/// Explicit presentation preferences. There is no hidden default beyond the
/// documented ones: an empty preference list means "use the default policy",
/// and a `require_*` flag turns "none of my preferences is supported" into an
/// error instead of the default.
struct PresentationRequest {
  /// Ordered, best first. Format and color space must both match.
  std::vector<vk::SurfaceFormatKHR> preferred_formats{};
  bool require_preferred_format = false;
  /// Ordered, best first.
  std::vector<vk::PresentModeKHR> preferred_present_modes{};
  bool require_preferred_present_mode = false;
  /// The `caller_id` of the present queue request that is produced.
  std::string queue_caller_id = "present";
  /// The flags the present queue family must also have. Presenting itself needs
  /// none, but a DeviceQueueRequest must name at least one flag; the default is
  /// the family you render with. It is copied into the produced request.
  vk::QueueFlags queue_flags = vk::QueueFlagBits::eGraphics;
};

enum class PresentationChoiceReason : std::uint8_t {
  /// A listed preference is supported; `preference_index` says which.
  preferred,
  /// No preference was given; the default policy chose.
  default_policy,
  /// Preferences were given, none is supported and none was required; the
  /// default policy chose.
  fallback_default,
  /// The surface lists only VK_FORMAT_UNDEFINED, which Vulkan defines as "any
  /// format is acceptable"; the first preference is used as asked.
  any_format_accepted,
};

/// Default policy: the first format the Surface lists (see
/// `PresentationChoiceReason::any_format_accepted` for the undefined-only case).
struct PresentationFormatDecision {
  /// What the Surface offered (the supported side of requested / supported /
  /// effective).
  std::vector<vk::SurfaceFormatKHR> supported{};
  std::vector<vk::SurfaceFormatKHR> requested{};
  bool required = false;
  vk::SurfaceFormatKHR effective{};
  PresentationChoiceReason reason = PresentationChoiceReason::default_policy;
  std::optional<std::size_t> preference_index{};
};

/// Default policy: FIFO, which Vulkan requires of every surface that can present.
struct PresentationModeDecision {
  std::vector<vk::PresentModeKHR> supported{};
  std::vector<vk::PresentModeKHR> requested{};
  bool required = false;
  vk::PresentModeKHR effective = vk::PresentModeKHR::eFifo;
  PresentationChoiceReason reason = PresentationChoiceReason::default_policy;
  std::optional<std::size_t> preference_index{};
};

/// The finalized presentation requirements, before any Device exists.
struct PresentationResolution {
  PresentationFormatDecision format{};
  PresentationModeDecision present_mode{};
  /// A ready queue request: required, `allowed_family_indices` is exactly the
  /// present-capable families, `forbid_alias`. The application keeps full
  /// control and may edit its alias policy, preferred families or priority
  /// before it is resolved.
  DeviceQueueRequest queue_request{};
  /// The present-capable queue families that also have `queue_flags` (the
  /// indices that `queue_request.allowed_family_indices` holds).
  std::vector<std::size_t> present_queue_families{};
  /// Device extensions the Device must enable to present.
  std::vector<std::string> required_device_extensions{};
};

/// Resolve `request` against `support`; pure, no native call. Fails with
/// invalid_request (empty queue_caller_id or queue_flags, a `require_*` flag with
/// an empty preference list, or a preferred format of VK_FORMAT_UNDEFINED), then,
/// in this order: no_present_queue_family (no family can present, or none that
/// also has `queue_flags`), swapchain_extension_unavailable, no_supported_format,
/// no_supported_present_mode. A surface that lists only VK_FORMAT_UNDEFINED needs
/// a stated preferred format, else no_supported_format. Nothing falls back
/// silently: every choice is recorded with its reason.
[[nodiscard]] auto resolvePresentation(const SurfaceSupport &support,
                                       const PresentationRequest &request)
    -> terreate::Result<PresentationResolution>;

/// Add the resolution's required device extensions (deduplicated; one that is
/// listed as optional is promoted to required) and its queue request to
/// `description`. Fails with invalid_request, changing nothing, when
/// `queue_requests` already holds the same `caller_id`.
[[nodiscard]] auto applyPresentation(DeviceDescription &description,
                                     const PresentationResolution &resolution)
    -> terreate::Result<void>;

/// Require VK_KHR_swapchain in the physical-device selection requirements
/// (deduplicated; an optional entry is promoted to required). May throw
/// std::bad_alloc, in which case the extension is still listed as before.
void requirePresentationDevice(PhysicalDeviceRequirements &requirements);

/// Why a candidate is, or is not, usable for presenting to one Surface.
enum class SurfaceCandidateOutcome : std::uint8_t {
  /// It meets the device requirements and `resolvePresentation` succeeded.
  selectable,
  /// It does not meet the device requirements (which include VK_KHR_swapchain);
  /// the Surface was not queried for it.
  requirements_not_met,
  /// No queue family of the device can present to the Surface.
  cannot_present,
  /// It can present but the request cannot be satisfied; `presentation_error`
  /// says which resolution step failed.
  presentation_unresolved,
};

/// One candidate's evaluation for one Surface. A declined candidate is data, not
/// an error: the others stay usable.
struct SurfaceCandidateEvaluation {
  /// Matches `PhysicalDeviceCandidate::enumerationIndex()`.
  std::size_t enumeration_index = 0;
  PhysicalDeviceEvaluation requirements{};
  /// Empty when the Surface was not queried (`requirements_not_met`).
  SurfaceSupport support{};
  SurfaceCandidateOutcome outcome = SurfaceCandidateOutcome::requirements_not_met;
  /// Set only for `presentation_unresolved`.
  std::optional<PresentationError> presentation_error{};
  /// Set only for `selectable`.
  std::optional<PresentationResolution> resolution{};

  [[nodiscard]] bool selectable() const noexcept {
    return outcome == SurfaceCandidateOutcome::selectable;
  }
};

/// Pure classification of one candidate from its device-requirement evaluation
/// and its Surface observation; no native call. `enumeration_index` is copied into
/// the result. A decline is a value; an invalid `request` (invalid_request) or an
/// allocation failure (allocation_failed) is an error, reported even for a
/// candidate the requirements decline.
[[nodiscard]] auto classifySurfaceCandidate(std::size_t enumeration_index,
                                            const PhysicalDeviceEvaluation &requirements,
                                            const SurfaceSupport &support,
                                            const PresentationRequest &request)
    -> terreate::Result<SurfaceCandidateEvaluation>;

/// Evaluate every candidate for `surface`, in the order given. `requirements` is
/// copied and VK_KHR_swapchain is required on the copy (see
/// `requirePresentationDevice`). A candidate that fails the requirements is not
/// queried. A native query failure for a candidate that is queried (for example
/// VK_ERROR_SURFACE_LOST_KHR) fails the whole call; it is not recorded as a
/// decline, since the Surface itself is then in doubt. `invalid_request` from
/// `request`, or from two candidates sharing an enumeration index, is returned as
/// an error (also for an empty list), not recorded per candidate.
[[nodiscard]] auto evaluateSurfaceCandidates(const Instance &instance,
                                             std::span<const PhysicalDeviceCandidate> candidates,
                                             const Surface &surface,
                                             const PhysicalDeviceRequirements &requirements,
                                             const PresentationRequest &request)
    -> terreate::Result<std::vector<SurfaceCandidateEvaluation>>;

/// The selected candidate and why, with every candidate's evaluation.
struct SurfaceSelection {
  PhysicalDeviceSelection selection;
  /// Index into `evaluations` of the selected candidate.
  std::size_t selected_evaluation = 0;
  std::vector<SurfaceCandidateEvaluation> evaluations{};

  [[nodiscard]] const SurfaceCandidateEvaluation &chosen() const {
    return evaluations[selected_evaluation];
  }
};

/// Evaluate the candidates for `surface` and select one through the existing
/// `selectPhysicalDevice` policy over the selectable ones only (zero selectable:
/// `no_selectable_candidate`; several and no policy: `ambiguous_match`, as for
/// `selectPhysicalDevice`; enumeration order is never a fallback). The result's
/// `chosen().resolution` feeds `applyPresentation`. Use
/// `evaluateSurfaceCandidates` to see why candidates were declined when this
/// fails.
[[nodiscard]] auto selectSurfacePhysicalDevice(const Instance &instance,
                                               std::span<const PhysicalDeviceCandidate> candidates,
                                               const Surface &surface,
                                               const PhysicalDeviceRequirements &requirements,
                                               const PresentationRequest &request,
                                               const PhysicalDeviceSelectionPolicy &policy = {})
    -> terreate::Result<SurfaceSelection>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::PresentationError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_PRESENTATION_HPP

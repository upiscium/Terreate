#include <terreate/graphics/presentation.hpp>

#include <algorithm>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace terreate::graphics {
namespace {
class PresentationErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.presentation"; }
  std::string message(int value) const override {
    switch (static_cast<PresentationError>(value)) {
    case PresentationError::invalid_instance:
      return "invalid Instance";
    case PresentationError::parent_mismatch:
      return "the PhysicalDevice or Surface does not belong to the Instance";
    case PresentationError::function_unavailable:
      return "a surface query function could not be loaded";
    case PresentationError::invalid_request:
      return "invalid presentation request";
    case PresentationError::no_present_queue_family:
      return "no queue family can present to the Surface";
    case PresentationError::swapchain_extension_unavailable:
      return "the device does not provide VK_KHR_swapchain";
    case PresentationError::no_supported_format:
      return "the Surface offers no usable format for the request";
    case PresentationError::no_supported_present_mode:
      return "the Surface offers no usable present mode for the request";
    case PresentationError::allocation_failed:
      return "allocation failed";
    }
    return "unknown presentation error";
  }
};

terreate::Error fail(PresentationError error, const char *operation, const char *detail) {
  try {
    return terreate::Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    // Code-only Error construction does not allocate while handling exhaustion.
    return terreate::Error{make_error_code(error)};
  }
}

terreate::Error nativeFailure(VkResult result, const char *operation, const char *detail) {
  const auto code = vk::make_error_code(static_cast<vk::Result>(result));
  try {
    return terreate::Error{code, operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{code};
  }
}

bool sameFormat(const vk::SurfaceFormatKHR &a, const vk::SurfaceFormatKHR &b) noexcept {
  return a.format == b.format && a.colorSpace == b.colorSpace;
}

bool hasName(const std::vector<std::string> &names, std::string_view wanted) noexcept {
  return std::ranges::find(names, wanted) != names.end();
}

struct SurfaceFunctions {
  PFN_vkGetPhysicalDeviceSurfaceSupportKHR support = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats = nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR present_modes = nullptr;

  [[nodiscard]] bool complete() const noexcept {
    return support != nullptr && capabilities != nullptr && formats != nullptr &&
           present_modes != nullptr;
  }
};

template <typename Function> Function load(VkInstance instance, const char *name) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<Function>(vkGetInstanceProcAddr(instance, name));
}

// Vulkan's two-call enumeration, repeated while the count changes underneath
// (VK_INCOMPLETE). `fetch(count, data)` wraps one native call.
template <typename T, typename Fetch> VkResult enumerate(std::vector<T> &out, Fetch &&fetch) {
  for (;;) {
    std::uint32_t count = 0;
    VkResult result = fetch(&count, static_cast<T *>(nullptr));
    if (result != VK_SUCCESS)
      return result;
    out.assign(count, T{});
    result = fetch(&count, out.data());
    if (result == VK_INCOMPLETE)
      continue;
    if (result == VK_SUCCESS)
      out.resize(count);
    return result;
  }
}
} // namespace

const std::error_category &presentation_error_category() noexcept {
  static const PresentationErrorCategory category;
  return category;
}
std::error_code make_error_code(PresentationError error) noexcept {
  return {static_cast<int>(error), presentation_error_category()};
}

auto querySurfaceSupport(const Instance &instance, const PhysicalDeviceCandidate &candidate,
                         const Surface &surface) -> terreate::Result<SurfaceSupport> {
  constexpr auto operation = "querySurfaceSupport";
  if (!instance.valid())
    return std::unexpected(
        fail(PresentationError::invalid_instance, operation, "moved-from Instance"));
  if (!candidate.device().correlatedWith(instance))
    return std::unexpected(fail(PresentationError::parent_mismatch, operation,
                                "the PhysicalDevice belongs to another Instance"));
  if (!surface.correlatedWith(instance))
    return std::unexpected(fail(PresentationError::parent_mismatch, operation,
                                "the Surface belongs to another Instance or was moved from"));

  const auto native_instance = static_cast<VkInstance>(instance.nativeHandle());
  SurfaceFunctions functions;
  functions.support = load<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
      native_instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
  functions.capabilities = load<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
      native_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  functions.formats = load<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
      native_instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
  functions.present_modes = load<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
      native_instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
  if (!functions.complete())
    return std::unexpected(
        fail(PresentationError::function_unavailable, operation, "vkGetInstanceProcAddr"));

  const auto device = static_cast<VkPhysicalDevice>(candidate.device().nativeHandle());
  const auto native_surface = surface.nativeSurface();
  try {
    SurfaceSupport support;
    support.swapchain_extension_available =
        hasName(candidate.capabilities().extensions, presentation_device_extension);

    const auto family_count = candidate.capabilities().queue_families.size();
    for (std::size_t family = 0; family < family_count; ++family) {
      VkBool32 supported = VK_FALSE;
      const auto result =
          functions.support(device, static_cast<std::uint32_t>(family), native_surface, &supported);
      if (result != VK_SUCCESS)
        return std::unexpected(
            nativeFailure(result, operation, "vkGetPhysicalDeviceSurfaceSupportKHR"));
      if (supported == VK_TRUE)
        support.present_queue_families.push_back(
            {family, candidate.capabilities().queue_families[family].queueFlags});
    }
    // Vulkan allows the remaining queries only for a surface the device
    // supports (VUID ...-surface-06521); for an unsupported one the
    // observation is complete and empty.
    if (support.present_queue_families.empty())
      return support;

    VkSurfaceCapabilitiesKHR capabilities{};
    if (const auto result = functions.capabilities(device, native_surface, &capabilities);
        result != VK_SUCCESS)
      return std::unexpected(
          nativeFailure(result, operation, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    support.capabilities = vk::SurfaceCapabilitiesKHR{capabilities};

    std::vector<VkSurfaceFormatKHR> formats;
    if (const auto result = enumerate(formats,
                                      [&](std::uint32_t *count, VkSurfaceFormatKHR *data) {
                                        return functions.formats(device, native_surface, count,
                                                                 data);
                                      });
        result != VK_SUCCESS)
      return std::unexpected(
          nativeFailure(result, operation, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    support.formats.reserve(formats.size());
    for (const auto &format : formats)
      support.formats.emplace_back(vk::SurfaceFormatKHR{format});

    std::vector<VkPresentModeKHR> modes;
    if (const auto result = enumerate(modes,
                                      [&](std::uint32_t *count, VkPresentModeKHR *data) {
                                        return functions.present_modes(device, native_surface,
                                                                       count, data);
                                      });
        result != VK_SUCCESS)
      return std::unexpected(
          nativeFailure(result, operation, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
    support.present_modes.reserve(modes.size());
    for (const auto mode : modes)
      support.present_modes.push_back(static_cast<vk::PresentModeKHR>(mode));
    return support;
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(PresentationError::allocation_failed)});
  }
}

namespace {
terreate::Result<PresentationFormatDecision> resolveFormat(const SurfaceSupport &support,
                                                           const PresentationRequest &request) {
  constexpr auto operation = "resolvePresentation";
  PresentationFormatDecision decision;
  decision.requested = request.preferred_formats;
  decision.required = request.require_preferred_format;
  decision.supported = support.formats;

  if (support.formats.empty())
    return std::unexpected(
        fail(PresentationError::no_supported_format, operation, "the Surface lists no format"));

  // A single VK_FORMAT_UNDEFINED entry means the surface has no preferred
  // format and accepts any (Vulkan specification, vkGetPhysicalDeviceSurfaceFormatsKHR).
  if (support.formats.size() == 1 && support.formats.front().format == vk::Format::eUndefined) {
    if (request.preferred_formats.empty())
      return std::unexpected(fail(PresentationError::no_supported_format, operation,
                                  "the Surface accepts any format but none was requested"));
    decision.effective = request.preferred_formats.front();
    decision.reason = PresentationChoiceReason::any_format_accepted;
    decision.preference_index = 0;
    return decision;
  }

  for (std::size_t i = 0; i < request.preferred_formats.size(); ++i) {
    const auto &wanted = request.preferred_formats[i];
    if (std::ranges::any_of(support.formats,
                            [&](const auto &listed) { return sameFormat(listed, wanted); })) {
      decision.effective = wanted;
      decision.reason = PresentationChoiceReason::preferred;
      decision.preference_index = i;
      return decision;
    }
  }
  if (request.require_preferred_format)
    return std::unexpected(fail(PresentationError::no_supported_format, operation,
                                "no required preferred format is supported"));
  decision.effective = support.formats.front();
  decision.reason = request.preferred_formats.empty() ? PresentationChoiceReason::default_policy
                                                      : PresentationChoiceReason::fallback_default;
  return decision;
}

terreate::Result<PresentationModeDecision> resolveMode(const SurfaceSupport &support,
                                                       const PresentationRequest &request) {
  constexpr auto operation = "resolvePresentation";
  PresentationModeDecision decision;
  decision.requested = request.preferred_present_modes;
  decision.required = request.require_preferred_present_mode;
  decision.supported = support.present_modes;

  for (std::size_t i = 0; i < request.preferred_present_modes.size(); ++i) {
    const auto wanted = request.preferred_present_modes[i];
    if (std::ranges::find(support.present_modes, wanted) != support.present_modes.end()) {
      decision.effective = wanted;
      decision.reason = PresentationChoiceReason::preferred;
      decision.preference_index = i;
      return decision;
    }
  }
  if (request.require_preferred_present_mode)
    return std::unexpected(fail(PresentationError::no_supported_present_mode, operation,
                                "no required preferred present mode is supported"));
  if (std::ranges::find(support.present_modes, vk::PresentModeKHR::eFifo) ==
      support.present_modes.end())
    return std::unexpected(fail(PresentationError::no_supported_present_mode, operation,
                                "the Surface does not list FIFO, the default present mode"));
  decision.effective = vk::PresentModeKHR::eFifo;
  decision.reason = request.preferred_present_modes.empty()
                        ? PresentationChoiceReason::default_policy
                        : PresentationChoiceReason::fallback_default;
  return decision;
}

void addRequired(std::vector<std::string> &required, std::vector<std::string> *optional,
                 const std::string &name) {
  // Add before removing, so an allocation failure never drops the extension.
  if (std::ranges::find(required, name) == required.end())
    required.push_back(name);
  if (optional != nullptr)
    std::erase(*optional, name);
}
} // namespace

auto resolvePresentation(const SurfaceSupport &support, const PresentationRequest &request)
    -> terreate::Result<PresentationResolution> {
  constexpr auto operation = "resolvePresentation";
  if (request.queue_caller_id.empty())
    return std::unexpected(
        fail(PresentationError::invalid_request, operation, "queue_caller_id is empty"));
  if (static_cast<VkQueueFlags>(request.queue_flags) == 0)
    return std::unexpected(
        fail(PresentationError::invalid_request, operation, "queue_flags is empty"));
  if (std::ranges::any_of(request.preferred_formats, [](const auto &format) {
        return format.format == vk::Format::eUndefined;
      }))
    return std::unexpected(fail(PresentationError::invalid_request, operation,
                                "a preferred format must not be VK_FORMAT_UNDEFINED"));
  if (request.require_preferred_format && request.preferred_formats.empty())
    return std::unexpected(fail(PresentationError::invalid_request, operation,
                                "require_preferred_format with no preferred format"));
  if (request.require_preferred_present_mode && request.preferred_present_modes.empty())
    return std::unexpected(fail(PresentationError::invalid_request, operation,
                                "require_preferred_present_mode with no preferred present mode"));
  if (!support.canPresent())
    return std::unexpected(fail(PresentationError::no_present_queue_family, operation,
                                "the device cannot present to the Surface"));
  std::vector<std::size_t> usable;
  try {
    for (const auto &family : support.present_queue_families)
      if ((family.flags & request.queue_flags) == request.queue_flags)
        usable.push_back(family.index);
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(PresentationError::allocation_failed)});
  }
  if (usable.empty())
    return std::unexpected(fail(PresentationError::no_present_queue_family, operation,
                                "no present-capable queue family has the requested queue_flags"));
  if (!support.swapchain_extension_available)
    return std::unexpected(fail(PresentationError::swapchain_extension_unavailable, operation,
                                "VK_KHR_swapchain is not listed by the device"));
  try {
    PresentationResolution resolution;
    auto format = resolveFormat(support, request);
    if (!format)
      return std::unexpected(format.error());
    resolution.format = std::move(*format);
    auto mode = resolveMode(support, request);
    if (!mode)
      return std::unexpected(mode.error());
    resolution.present_mode = std::move(*mode);

    resolution.queue_request.caller_id = request.queue_caller_id;
    resolution.queue_request.strength = RequirementStrength::required;
    resolution.queue_request.required_flags = request.queue_flags;
    resolution.queue_request.allowed_family_indices = usable;
    resolution.present_queue_families = std::move(usable);
    resolution.required_device_extensions.emplace_back(presentation_device_extension);
    return resolution;
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(PresentationError::allocation_failed)});
  }
}

auto applyPresentation(DeviceDescription &description, const PresentationResolution &resolution)
    -> terreate::Result<void> {
  constexpr auto operation = "applyPresentation";
  const auto &caller_id = resolution.queue_request.caller_id;
  if (caller_id.empty())
    return std::unexpected(
        fail(PresentationError::invalid_request, operation, "the queue request has no caller_id"));
  if (std::ranges::any_of(description.queue_requests,
                          [&](const auto &request) { return request.caller_id == caller_id; }))
    return std::unexpected(fail(PresentationError::invalid_request, operation,
                                "queue_requests already holds this caller_id"));
  try {
    // Reserve first so a failure below leaves `description` unchanged.
    auto required = description.required_extensions;
    auto optional = description.optional_extensions;
    for (const auto &name : resolution.required_device_extensions)
      addRequired(required, &optional, name);
    auto queues = description.queue_requests;
    queues.push_back(resolution.queue_request);
    description.required_extensions = std::move(required);
    description.optional_extensions = std::move(optional);
    description.queue_requests = std::move(queues);
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(PresentationError::allocation_failed)});
  }
}

void requirePresentationDevice(PhysicalDeviceRequirements &requirements) {
  addRequired(requirements.required_extensions, &requirements.optional_extensions,
              presentation_device_extension);
}

} // namespace terreate::graphics

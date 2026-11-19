// #364: presentation support, resolution and the hand-off to Device requirements.
//
// `cpu` mode checks the pure resolution over hand-built SurfaceSupport values:
// no GPU, no window system. `gpu` mode observes real devices through a
// VK_EXT_headless_surface (no window system needed) and builds a Device from the
// resolution. A device that cannot present to a headless surface is a valid
// observation; if no device can, or the extension is missing, the GPU test
// reports SKIP, never PASS. No linker or platform mechanism is used.
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/presentation.hpp>
#include <terreate/graphics/surface.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace terreate::graphics;
using Error = PresentationError;

const char *test_name = "graphics.presentation.cpu";
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
  for (const auto &name : names)
    if (name == wanted)
      return true;
  return false;
}

constexpr vk::SurfaceFormatKHR bgra{vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
constexpr vk::SurfaceFormatKHR rgba{vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
constexpr vk::SurfaceFormatKHR rgba_hdr{vk::Format::eR8G8B8A8Unorm,
                                        vk::ColorSpaceKHR::eExtendedSrgbLinearEXT};

SurfaceSupport support() {
  SurfaceSupport s;
  s.present_queue_families = {{1, vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eTransfer},
                              {3, vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute}};
  s.formats = {bgra, rgba};
  s.present_modes = {vk::PresentModeKHR::eMailbox, vk::PresentModeKHR::eFifo};
  s.swapchain_extension_available = true;
  return s;
}

bool same(const vk::SurfaceFormatKHR &a, const vk::SurfaceFormatKHR &b) {
  return a.format == b.format && a.colorSpace == b.colorSpace;
}

int cpuTest() {
  check(std::string{presentation_error_category().name()} == "terreate.graphics.presentation" &&
            make_error_code(Error::no_present_queue_family).message() ==
                "no queue family can present to the Surface",
        "error category name and message");

  // Defaults, each recorded with its reason.
  {
    const auto r = resolvePresentation(support(), {});
    if (check(r.has_value(), "the default request resolves")) {
      check(same(r->format.effective, bgra) &&
                r->format.reason == PresentationChoiceReason::default_policy &&
                !r->format.preference_index,
            "default format is the first the surface lists");
      check(r->present_mode.effective == vk::PresentModeKHR::eFifo &&
                r->present_mode.reason == PresentationChoiceReason::default_policy,
            "default present mode is FIFO");
      check(r->queue_request.caller_id == "present" &&
                r->queue_request.strength == terreate::graphics::RequirementStrength::required &&
                r->queue_request.required_flags == vk::QueueFlagBits::eGraphics &&
                r->queue_request.allowed_family_indices == std::vector<std::size_t>{1, 3} &&
                r->queue_request.alias_policy == DeviceQueueAliasPolicy::forbid_alias,
            "queue request is strict over the present-capable families");
      check(r->required_device_extensions ==
                std::vector<std::string>{presentation_device_extension},
            "VK_KHR_swapchain is required");
      check(r->format.supported == support().formats &&
                r->present_mode.supported == support().present_modes,
            "the decisions carry what the surface offered");
    }
  }

  // Preferences: first supported wins, in order.
  {
    PresentationRequest request;
    request.preferred_formats = {rgba_hdr, rgba, bgra};
    request.preferred_present_modes = {vk::PresentModeKHR::eImmediate,
                                       vk::PresentModeKHR::eMailbox};
    request.queue_caller_id = "screen";
    const auto r = resolvePresentation(support(), request);
    if (check(r.has_value(), "preferences resolve")) {
      check(same(r->format.effective, rgba) &&
                r->format.reason == PresentationChoiceReason::preferred &&
                r->format.preference_index == 1u,
            "first supported format preference wins (color space must match)");
      check(r->present_mode.effective == vk::PresentModeKHR::eMailbox &&
                r->present_mode.preference_index == 1u,
            "first supported present mode preference wins");
      check(r->queue_request.caller_id == "screen", "caller id is taken from the request");
    }
  }

  // Unsupported preference: default and recorded, or an error when required.
  {
    PresentationRequest request;
    request.preferred_formats = {rgba_hdr};
    request.preferred_present_modes = {vk::PresentModeKHR::eImmediate};
    const auto r = resolvePresentation(support(), request);
    check(r && same(r->format.effective, bgra) &&
              r->format.reason == PresentationChoiceReason::fallback_default &&
              r->present_mode.effective == vk::PresentModeKHR::eFifo &&
              r->present_mode.reason == PresentationChoiceReason::fallback_default,
          "unsupported, non-required preferences fall back visibly");
    request.require_preferred_format = true;
    check(hasError(resolvePresentation(support(), request), Error::no_supported_format),
          "a required format that is unsupported is an error");
    request.require_preferred_format = false;
    request.require_preferred_present_mode = true;
    check(hasError(resolvePresentation(support(), request), Error::no_supported_present_mode),
          "a required present mode that is unsupported is an error");
  }

  // Request validation.
  {
    PresentationRequest request;
    request.require_preferred_format = true;
    check(hasError(resolvePresentation(support(), request), Error::invalid_request),
          "require_preferred_format needs preferences");
    request = {};
    request.require_preferred_present_mode = true;
    check(hasError(resolvePresentation(support(), request), Error::invalid_request),
          "require_preferred_present_mode needs preferences");
    request = {};
    request.queue_caller_id.clear();
    check(hasError(resolvePresentation(support(), request), Error::invalid_request),
          "an empty queue caller id is rejected");
    request = {};
    request.queue_flags = {};
    check(hasError(resolvePresentation(support(), request), Error::invalid_request),
          "empty queue flags are rejected");
  }

  // The queue flags filter the present-capable families, before Device creation.
  {
    PresentationRequest request;
    request.queue_flags = vk::QueueFlagBits::eCompute;
    const auto r = resolvePresentation(support(), request);
    check(r && r->queue_request.allowed_family_indices == std::vector<std::size_t>{3} &&
              r->present_queue_families == std::vector<std::size_t>{3} &&
              r->queue_request.required_flags == vk::QueueFlagBits::eCompute,
          "only present-capable families that also have the flags are allowed");
    request.queue_flags = vk::QueueFlagBits::eSparseBinding;
    check(hasError(resolvePresentation(support(), request), Error::no_present_queue_family),
          "no present-capable family with the flags is no_present_queue_family");
    auto s = support();
    s.swapchain_extension_available = false;
    check(hasError(resolvePresentation(s, request), Error::no_present_queue_family),
          "a missing queue family is reported before a missing extension");
  }

  // Required preferences that are supported succeed with the preferred reason.
  {
    PresentationRequest request;
    request.preferred_formats = {rgba};
    request.require_preferred_format = true;
    request.preferred_present_modes = {vk::PresentModeKHR::eMailbox};
    request.require_preferred_present_mode = true;
    const auto r = resolvePresentation(support(), request);
    check(r && r->format.reason == PresentationChoiceReason::preferred && r->format.required &&
              r->present_mode.reason == PresentationChoiceReason::preferred &&
              r->present_mode.preference_index == 0u,
          "supported required preferences resolve as preferred");
    request = {};
    request.preferred_formats = {
        vk::SurfaceFormatKHR{vk::Format::eUndefined, vk::ColorSpaceKHR::eSrgbNonlinear}};
    check(hasError(resolvePresentation(support(), request), Error::invalid_request),
          "a preferred format of VK_FORMAT_UNDEFINED is rejected");
    auto s = support();
    s.present_modes.clear();
    request = {};
    request.preferred_present_modes = {vk::PresentModeKHR::eMailbox};
    check(hasError(resolvePresentation(s, request), Error::no_supported_present_mode),
          "an empty present-mode list with a non-FIFO preference is an error");
  }

  // Missing capabilities are explicit errors.
  {
    auto s = support();
    s.present_queue_families.clear();
    check(hasError(resolvePresentation(s, {}), Error::no_present_queue_family),
          "a device that cannot present is no_present_queue_family");
    s = support();
    s.swapchain_extension_available = false;
    check(hasError(resolvePresentation(s, {}), Error::swapchain_extension_unavailable),
          "a missing VK_KHR_swapchain is swapchain_extension_unavailable");
    s = support();
    s.formats.clear();
    check(hasError(resolvePresentation(s, {}), Error::no_supported_format),
          "no format at all is no_supported_format");
    s = support();
    s.present_modes = {vk::PresentModeKHR::eMailbox};
    check(hasError(resolvePresentation(s, {}), Error::no_supported_present_mode),
          "without FIFO the default present mode is unavailable");
    PresentationRequest mailbox;
    mailbox.preferred_present_modes = {vk::PresentModeKHR::eMailbox};
    check(resolvePresentation(s, mailbox).has_value(), "a supported preference does not need FIFO");
  }

  // A surface that lists only VK_FORMAT_UNDEFINED accepts any format.
  {
    auto s = support();
    s.formats = {vk::SurfaceFormatKHR{vk::Format::eUndefined, vk::ColorSpaceKHR::eSrgbNonlinear}};
    PresentationRequest request;
    request.preferred_formats = {rgba};
    const auto r = resolvePresentation(s, request);
    check(r && same(r->format.effective, rgba) &&
              r->format.reason == PresentationChoiceReason::any_format_accepted,
          "an undefined-format surface accepts the first preference");
    check(hasError(resolvePresentation(s, {}), Error::no_supported_format),
          "an undefined-format surface needs a stated format");
  }

  // Hand-off into the Device and PhysicalDevice requirement structs.
  {
    const auto r = resolvePresentation(support(), {});
    DeviceDescription description;
    description.optional_extensions = {presentation_device_extension, "VK_KHR_maintenance5"};
    DeviceQueueRequest other;
    other.caller_id = "graphics";
    description.queue_requests = {other};
    check(applyPresentation(description, *r).has_value() &&
              description.required_extensions ==
                  std::vector<std::string>{presentation_device_extension} &&
              description.optional_extensions == std::vector<std::string>{"VK_KHR_maintenance5"} &&
              description.queue_requests.size() == 2 &&
              description.queue_requests.back().caller_id == "present",
          "applyPresentation promotes the extension and adds the queue request");
    const auto before = description;
    check(hasError(applyPresentation(description, *r), Error::invalid_request) &&
              description.required_extensions == before.required_extensions &&
              description.optional_extensions == before.optional_extensions &&
              description.queue_requests.size() == before.queue_requests.size(),
          "a duplicate caller id is rejected and changes nothing");

    DeviceDescription twice;
    twice.required_extensions = {presentation_device_extension};
    check(applyPresentation(twice, *r) && twice.required_extensions.size() == 1,
          "an extension that is already required is not added again");
    auto nameless = *r;
    nameless.queue_request.caller_id.clear();
    DeviceDescription untouched;
    untouched.optional_extensions = {presentation_device_extension};
    check(hasError(applyPresentation(untouched, nameless), Error::invalid_request) &&
              untouched.optional_extensions.size() == 1 && untouched.required_extensions.empty() &&
              untouched.queue_requests.empty(),
          "a queue request without a caller id is rejected and changes nothing");

    PhysicalDeviceRequirements requirements;
    requirements.required_extensions = {presentation_device_extension};
    requirements.optional_extensions = {presentation_device_extension};
    requirePresentationDevice(requirements);
    check(requirements.required_extensions ==
                  std::vector<std::string>{presentation_device_extension} &&
              requirements.optional_extensions.empty(),
          "requirePresentationDevice deduplicates and promotes");
  }

  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.presentation.cpu: cpu=PASS");
  return EXIT_SUCCESS;
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

VkResult createHeadless(VkInstance instance, VkSurfaceKHR *surface, void *) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
      vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
  if (create == nullptr)
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  VkHeadlessSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
  return create(instance, &info, nullptr, surface);
}

int gpuTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  const bool has_headless =
      hasName(capabilities->available_extensions, VK_KHR_SURFACE_EXTENSION_NAME) &&
      hasName(capabilities->available_extensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
  if (!has_headless) {
    std::puts(
        "graphics.presentation: presentation=SKIP reason=VK_EXT_headless_surface unavailable");
    return EXIT_SUCCESS;
  }
  const bool has_validation =
      hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation");
  const bool has_debug_utils =
      hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-presentation";
  description.required_extensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                     VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
  description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  if (has_validation && has_debug_utils)
    description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance with surface extensions");
  ValidationRecorder recorder;
  auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  auto other = createInstance(*plan);
  if (!instance || !other)
    return blocked("two Instances with surface extensions");

  auto inventory = queryPhysicalDevices(*instance);
  if (!inventory)
    return blocked("physical device enumeration");
  if (inventory->empty()) {
    std::puts("graphics.presentation: presentation=SKIP reason=no physical device");
    return EXIT_SUCCESS;
  }
  auto other_inventory = queryPhysicalDevices(*other);
  if (!other_inventory || other_inventory->empty())
    return blocked("physical devices of the second Instance");

  const SurfaceCreator creator{&createHeadless, nullptr, {}};
  auto surface = createSurface(*instance, creator);
  if (!surface)
    return blocked("headless surface");
  auto other_surface = createSurface(*other, creator);
  if (!other_surface)
    return blocked("headless surface of the second Instance");

  // Parent checks come before any native call.
  check(hasError(querySurfaceSupport(*instance, other_inventory->front(), *surface),
                 Error::parent_mismatch),
        "a candidate of another Instance is a parent_mismatch");
  check(hasError(querySurfaceSupport(*instance, inventory->front(), *other_surface),
                 Error::parent_mismatch),
        "a Surface of another Instance is a parent_mismatch");
  {
    auto moved_from_source = createSurface(*instance, creator);
    if (!moved_from_source)
      return blocked("headless surface to move from");
    Surface moved_to = std::move(*moved_from_source);
    check(hasError(querySurfaceSupport(*instance, inventory->front(), *moved_from_source),
                   Error::parent_mismatch),
          "a moved-from Surface is a parent_mismatch");
  }

  // Every candidate: a consistent observation or a native error.
  const PhysicalDeviceCandidate *presenting = nullptr;
  SurfaceSupport presenting_support;
  for (const auto &candidate : *inventory) {
    const auto observed = querySurfaceSupport(*instance, candidate, *surface);
    const auto &name = candidate.capabilities().properties.deviceName;
    if (!observed) {
      // A device that cannot present is an observation, so any error is a defect
      // (or a lost surface) and must not pass silently.
      check(false, "querySurfaceSupport returned an error for a candidate");
      std::printf("graphics.presentation: device=%s query=error:%s\n", name.data(),
                  observed.error().code().message().c_str());
      continue;
    }
    const auto family_count = candidate.capabilities().queue_families.size();
    bool consistent = true;
    for (const auto &family : observed->present_queue_families)
      consistent = consistent && family.index < family_count &&
                   family.flags == candidate.capabilities().queue_families[family.index].queueFlags;
    consistent = consistent && std::ranges::is_sorted(observed->present_queue_families, {},
                                                      &PresentQueueFamily::index);
    check(consistent, "present queue families are in range and ascending");
    check(observed->swapchain_extension_available ==
              hasName(candidate.capabilities().extensions, presentation_device_extension),
          "swapchain_extension_available mirrors the extension list");
    if (!observed->canPresent()) {
      check(observed->formats.empty() && observed->present_modes.empty(),
            "a device that cannot present reports an empty observation");
      check(hasError(resolvePresentation(*observed, {}), Error::no_present_queue_family),
            "the resolution reports no_present_queue_family for it");
      std::printf("graphics.presentation: device=%s present=none\n", name.data());
      continue;
    }
    check(!observed->formats.empty() && !observed->present_modes.empty(),
          "a device that can present lists formats and present modes");
    const auto &caps = observed->capabilities;
    check(caps.minImageCount >= 1 &&
              (caps.maxImageCount == 0 || caps.maxImageCount >= caps.minImageCount) &&
              static_cast<VkImageUsageFlags>(caps.supportedUsageFlags) != 0,
          "a device that can present reports consistent surface capabilities");
    check(std::ranges::find(observed->present_modes, vk::PresentModeKHR::eFifo) !=
              observed->present_modes.end(),
          "Vulkan requires FIFO on a surface that can present");
    std::printf("graphics.presentation: device=%s present-families=%zu formats=%zu modes=%zu\n",
                name.data(), observed->present_queue_families.size(), observed->formats.size(),
                observed->present_modes.size());
    // A device that presents but cannot satisfy the default request (for example
    // no graphics family) is not an end-to-end candidate.
    if (presenting == nullptr && resolvePresentation(*observed, {}).has_value()) {
      presenting = &candidate;
      presenting_support = *observed;
    }
  }

  if (presenting == nullptr) {
    if (failures != 0)
      return EXIT_FAILURE;
    std::puts("graphics.presentation: presentation=SKIP reason=no device can present to a "
              "headless surface");
    return EXIT_SUCCESS;
  }

  // End to end, before any Device exists: support -> resolution -> Device.
  const auto resolution = resolvePresentation(presenting_support, {});
  if (!check(resolution.has_value(), "the presenting device resolves"))
    return EXIT_FAILURE;
  PhysicalDeviceRequirements requirements;
  requirePresentationDevice(requirements);
  check(evaluatePhysicalDevice(*presenting, requirements)
            .transform([](const auto &e) { return e.matches; })
            .value_or(false),
        "the presenting device satisfies the physical-device requirements");
  DeviceDescription device_description;
  if (!check(applyPresentation(device_description, *resolution).has_value(),
             "applyPresentation on a DeviceDescription"))
    return EXIT_FAILURE;
  const auto device_plan = resolveDevice(*presenting, device_description);
  if (!check(device_plan.has_value(), "resolveDevice with the presentation requirements"))
    return EXIT_FAILURE;
  auto device = createDevice(*instance, *device_plan);
  if (!check(device.has_value(), "createDevice with VK_KHR_swapchain"))
    return EXIT_FAILURE;
  const auto queue = device->queue("present");
  check(queue.valid() && hasName(device_plan->enabledExtensions(), presentation_device_extension),
        "the present queue exists and the extension is enabled");
  {
    // The planned queue family can really present (native check).
    const auto family =
        static_cast<std::uint32_t>(device_plan->queueAssignments().front().family_index);
    VkBool32 supported = VK_FALSE;
    const auto result = vkGetPhysicalDeviceSurfaceSupportKHR(
        static_cast<VkPhysicalDevice>(presenting->device().nativeHandle()), family,
        surface->nativeSurface(), &supported);
    check(result == VK_SUCCESS && supported == VK_TRUE,
          "the planned present queue's family supports presentation");
  }
  // Destroy in lifetime order: Device, then Surfaces (before the Instances).
  {
    const auto gone = std::move(*device);
  }
  {
    const auto gone = std::move(*surface);
  }
  {
    const auto gone = std::move(*other_surface);
  }

  // Messages are delivered only when the layer and VK_EXT_debug_utils are both on.
  if (has_validation && has_debug_utils) {
    check(recorder.warnings_or_errors.load() == 0, "no validation warnings or errors");
    std::puts("graphics.presentation: validation-layer=PASS");
  } else {
    std::puts("graphics.presentation: validation-layer=SKIP "
              "reason=VK_LAYER_KHRONOS_validation or VK_EXT_debug_utils unavailable");
  }
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.presentation: presentation=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify cpu or gpu mode");
  const std::string_view mode{argv[1]};
  if (mode == "cpu")
    return cpuTest();
  if (mode == "gpu") {
    test_name = "graphics.presentation";
    return gpuTest();
  }
  return blocked("unknown mode");
}

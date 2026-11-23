// #273: explicit Swapchain creation from a resolved Surface/Device configuration.
//
// `cpu` mode checks `resolveSwapchain` over hand-built observations: no GPU, no
// window system. `gpu` mode creates a Swapchain on a VK_EXT_headless_surface
// (no window system needed) with a Device built from the presentation
// resolution. If no device can present to a headless surface, or the extension
// is missing, the GPU test reports SKIP, never PASS. No linker or platform
// mechanism is used.
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/presentation.hpp>
#include <terreate/graphics/surface.hpp>
#include <terreate/graphics/swapchain.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace terreate::graphics;
using Error = SwapchainError;

const char *test_name = "graphics.swapchain.cpu";
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
  return std::ranges::any_of(names, [&](const auto &name) { return name == wanted; });
}

constexpr auto undefined = std::numeric_limits<std::uint32_t>::max();

SurfaceSupport support() {
  SurfaceSupport s;
  s.present_queue_families = {{0, vk::QueueFlagBits::eGraphics}};
  s.formats = {{vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear}};
  s.present_modes = {vk::PresentModeKHR::eFifo};
  s.swapchain_extension_available = true;
  s.capabilities.minImageCount = 2;
  s.capabilities.maxImageCount = 4;
  s.capabilities.currentExtent = vk::Extent2D{undefined, undefined};
  s.capabilities.minImageExtent = vk::Extent2D{1, 1};
  s.capabilities.maxImageExtent = vk::Extent2D{4096, 4096};
  s.capabilities.supportedUsageFlags =
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
  s.capabilities.supportedTransforms = vk::SurfaceTransformFlagBitsKHR::eIdentity;
  s.capabilities.currentTransform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
  s.capabilities.supportedCompositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
  return s;
}

PresentationResolution resolution(const SurfaceSupport &s) {
  return resolvePresentation(s, {}).value();
}

int cpuTest() {
  check(std::string{swapchain_error_category().name()} == "terreate.graphics.swapchain" &&
            make_error_code(Error::no_image_extent).message() ==
                "the Swapchain image extent is zero",
        "error category name and message");

  // Defaults, with the extent taken from the request (the Surface leaves it open).
  {
    const auto s = support();
    SwapchainRequest request;
    request.extent = vk::Extent2D{640, 480};
    const auto d = resolveSwapchain(s, resolution(s), request);
    check(d && d->extent.width == 640 && d->extent.height == 480 && d->min_image_count == 3 &&
              d->usage == vk::ImageUsageFlagBits::eColorAttachment &&
              d->sharing_mode == vk::SharingMode::eExclusive && d->queue_family_indices.empty() &&
              d->pre_transform == vk::SurfaceTransformFlagBitsKHR::eIdentity &&
              d->composite_alpha == vk::CompositeAlphaFlagBitsKHR::eOpaque && d->clipped &&
              d->image_array_layers == 1 && d->old_swapchain == nullptr &&
              d->format.format == vk::Format::eB8G8R8A8Unorm &&
              d->present_mode == vk::PresentModeKHR::eFifo,
          "the documented defaults are resolved");
  }

  // The extent: clamped when open, forced by a defined currentExtent, never zero.
  {
    auto s = support();
    SwapchainRequest request;
    request.extent = vk::Extent2D{100000, 2};
    auto d = resolveSwapchain(s, resolution(s), request);
    check(d && d->extent.width == 4096 && d->extent.height == 2,
          "an open extent is clamped to the supported range");
    check(hasError(resolveSwapchain(s, resolution(s), {}), Error::no_image_extent),
          "an open extent with a zero request is no_image_extent");
    s.capabilities.currentExtent = vk::Extent2D{800, 600};
    request.extent = vk::Extent2D{1, 1};
    d = resolveSwapchain(s, resolution(s), request);
    check(d && d->extent.width == 800 && d->extent.height == 600,
          "a defined currentExtent wins over the request");
    s.capabilities.currentExtent = vk::Extent2D{0, 0};
    check(hasError(resolveSwapchain(s, resolution(s), request), Error::no_image_extent),
          "a zero currentExtent (minimized window) is no_image_extent");
  }

  // Image count: the default is limited by the maximum, an explicit one is never clamped.
  {
    auto s = support();
    SwapchainRequest request;
    request.extent = vk::Extent2D{64, 64};
    s.capabilities.maxImageCount = 2;
    auto d = resolveSwapchain(s, resolution(s), request);
    check(d && d->min_image_count == 2, "the default image count is limited by the maximum");
    s.capabilities.maxImageCount = 0;
    d = resolveSwapchain(s, resolution(s), request);
    check(d && d->min_image_count == 3, "a maximum of 0 means no limit");
    request.min_image_count = 9;
    s.capabilities.maxImageCount = 4;
    check(hasError(resolveSwapchain(s, resolution(s), request), Error::unsupported_configuration),
          "an explicit image count above the maximum is not clamped");
    request.min_image_count = 1;
    check(hasError(resolveSwapchain(s, resolution(s), request), Error::unsupported_configuration),
          "an explicit image count below the minimum is not clamped");
    request.min_image_count = 4;
    d = resolveSwapchain(s, resolution(s), request);
    check(d && d->min_image_count == 4, "an explicit supported image count is kept");
  }

  // Usage, transform and composite alpha are never substituted.
  {
    const auto s = support();
    SwapchainRequest request;
    request.extent = vk::Extent2D{64, 64};
    request.usage = vk::ImageUsageFlagBits::eStorage;
    check(hasError(resolveSwapchain(s, resolution(s), request), Error::unsupported_configuration),
          "an unsupported usage is an error");
    request.usage = {};
    check(hasError(resolveSwapchain(s, resolution(s), request), Error::unsupported_configuration),
          "an empty usage is an error");
    request.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
    check(resolveSwapchain(s, resolution(s), request).has_value(), "a supported usage set is kept");
    request.usage = vk::ImageUsageFlagBits::eColorAttachment;
    request.pre_transform = vk::SurfaceTransformFlagBitsKHR::eRotate90;
    check(hasError(resolveSwapchain(s, resolution(s), request), Error::unsupported_configuration),
          "an unsupported pre-transform is an error");
    request.pre_transform.reset();
    request.composite_alpha = vk::CompositeAlphaFlagBitsKHR::ePreMultiplied;
    check(hasError(resolveSwapchain(s, resolution(s), request), Error::unsupported_configuration),
          "an unsupported composite alpha is an error");
  }

  // Sharing mode.
  {
    const auto s = support();
    SwapchainRequest request;
    request.extent = vk::Extent2D{64, 64};
    request.concurrent_queue_families = {2, 5};
    auto d = resolveSwapchain(s, resolution(s), request);
    check(d && d->sharing_mode == vk::SharingMode::eConcurrent &&
              d->queue_family_indices == std::vector<std::uint32_t>{2, 5},
          "two distinct families select concurrent sharing");
    request.concurrent_queue_families = {2};
    check(hasError(resolveSwapchain(s, resolution(s), request), Error::invalid_description),
          "one concurrent family is invalid");
    request.concurrent_queue_families = {3, 3};
    check(hasError(resolveSwapchain(s, resolution(s), request), Error::invalid_description),
          "a duplicated concurrent family is invalid");
  }

  // The observation and the resolution must agree.
  {
    auto s = support();
    SwapchainRequest request;
    request.extent = vk::Extent2D{64, 64};
    const auto r = resolution(s);
    auto none = s;
    none.present_queue_families.clear();
    check(hasError(resolveSwapchain(none, r, request), Error::unsupported_configuration),
          "a Surface that cannot present is an error");
    auto other_format = s;
    other_format.formats = {{vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear}};
    check(hasError(resolveSwapchain(other_format, r, request), Error::unsupported_configuration),
          "a resolved format the Surface does not offer is an error");
    auto other_mode = s;
    other_mode.present_modes = {vk::PresentModeKHR::eMailbox};
    check(hasError(resolveSwapchain(other_mode, r, request), Error::unsupported_configuration),
          "a resolved present mode the Surface does not offer is an error");
    auto undefined_only = s;
    undefined_only.formats = {{vk::Format::eUndefined, vk::ColorSpaceKHR::eSrgbNonlinear}};
    PresentationRequest stated;
    stated.preferred_formats = {{vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear}};
    const auto accepted = resolvePresentation(undefined_only, stated);
    check(accepted && resolveSwapchain(undefined_only, *accepted, request).has_value(),
          "an undefined-only Surface accepts the stated format");
  }

  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.swapchain.cpu: cpu=PASS");
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

int skip(const char *reason) {
  std::printf("graphics.swapchain: swapchain=SKIP reason=%s\n", reason);
  return EXIT_SUCCESS;
}

int gpuTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  if (!hasName(capabilities->available_extensions, VK_KHR_SURFACE_EXTENSION_NAME) ||
      !hasName(capabilities->available_extensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
    return skip("VK_EXT_headless_surface unavailable");
  const bool has_validation =
      hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation");
  const bool has_debug_utils =
      hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-swapchain";
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
  const SurfaceCreator creator{&createHeadless, nullptr, {}};
  auto surface = createSurface(*instance, creator);
  auto other_surface = createSurface(*other, creator);
  if (!surface || !other_surface)
    return blocked("headless surfaces");

  // The first candidate that can present and satisfies the default request.
  const PhysicalDeviceCandidate *presenting = nullptr;
  SurfaceSupport presenting_support;
  for (const auto &candidate : *inventory) {
    const auto observed = querySurfaceSupport(*instance, candidate, *surface);
    if (!observed || !observed->canPresent() || !resolvePresentation(*observed, {}))
      continue;
    presenting = &candidate;
    presenting_support = *observed;
    break;
  }
  if (presenting == nullptr)
    return skip("no device can present to a headless surface");

  const auto presentation = resolvePresentation(presenting_support, {});
  DeviceDescription device_description;
  if (!check(presentation.has_value() &&
                 applyPresentation(device_description, *presentation).has_value(),
             "presentation requirements apply to a DeviceDescription"))
    return EXIT_FAILURE;
  const auto device_plan = resolveDevice(*presenting, device_description);
  if (!check(device_plan.has_value(), "resolveDevice with the presentation requirements"))
    return EXIT_FAILURE;
  auto device = createDevice(*instance, *device_plan);
  if (!check(device.has_value(), "createDevice with VK_KHR_swapchain"))
    return EXIT_FAILURE;

  SwapchainRequest request;
  request.extent = vk::Extent2D{64, 48};
  const auto swapchain_description = resolveSwapchain(presenting_support, *presentation, request);
  if (!check(swapchain_description.has_value(), "resolveSwapchain on the real observation"))
    return EXIT_FAILURE;

  // Description errors and parents come before any native call.
  {
    auto bad = *swapchain_description;
    bad.extent = vk::Extent2D{0, 0};
    check(hasError(createSwapchain(*instance, *device, *surface, bad), Error::invalid_description),
          "a zero extent is invalid_description");
    bad = *swapchain_description;
    bad.sharing_mode = vk::SharingMode::eConcurrent;
    check(hasError(createSwapchain(*instance, *device, *surface, bad), Error::invalid_description),
          "concurrent sharing without families is invalid_description");
    bad.queue_family_indices = {0, 0};
    check(hasError(createSwapchain(*instance, *device, *surface, bad), Error::invalid_description),
          "duplicated concurrent families are invalid_description");
    check(!SwapchainImage{}.valid(), "a default SwapchainImage is empty");
    check(hasError(createSwapchain(*instance, *device, *other_surface, *swapchain_description),
                   Error::parent_mismatch),
          "a Surface of another Instance is a parent_mismatch");
    check(hasError(createSwapchain(*other, *device, *surface, *swapchain_description),
                   Error::parent_mismatch),
          "a Device of another Instance is a parent_mismatch");
  }

  auto first = createSwapchain(*instance, *device, *surface, *swapchain_description);
  if (!check(first.has_value(), "createSwapchain on a headless surface"))
    return EXIT_FAILURE;
  const auto *effective = first->configuration();
  if (!check(effective != nullptr, "the Swapchain has a configuration"))
    return EXIT_FAILURE;
  check(first->nativeHandle() && first->generation() != 0 && first->lineage() == 1,
        "the Swapchain has a native handle, an identity and lineage 1");
  check(effective->format == swapchain_description->format.format &&
            effective->color_space == swapchain_description->format.colorSpace &&
            effective->present_mode == swapchain_description->present_mode &&
            effective->extent.width == 64 && effective->extent.height == 48 &&
            effective->image_count >= swapchain_description->min_image_count &&
            effective->usage == swapchain_description->usage &&
            effective->requested.old_swapchain == nullptr,
        "the effective format, present mode, extent, image count and usage are observable");
  check(first->images().size() == effective->image_count && first->correlatedWith(*device) &&
            first->correlatedWith(*surface) && !first->correlatedWith(*other_surface),
        "the images and parents are observable");
  bool images_ok = true;
  for (std::size_t i = 0; i < first->images().size(); ++i) {
    const auto &image = first->images()[i];
    images_ok = images_ok && image.valid() && image.nativeImage() && image.nativeImageView() &&
                image.index() == i && image.generation() == first->generation() &&
                first->owns(image) && image.format() == effective->format &&
                image.extent().width == 64 && image.extent().height == 48;
  }
  check(images_ok, "each image is borrowed with its view, index and generation");

  // Recreation hook: a new Swapchain from the old one. The old one is still the
  // application's to destroy; its images are not owned by the new one.
  auto next_description = *swapchain_description;
  next_description.old_swapchain = &*first;
  auto second = createSwapchain(*instance, *device, *surface, next_description);
  if (check(second.has_value(), "createSwapchain with an old Swapchain")) {
    check(second->lineage() == 2 && second->generation() != first->generation() &&
              second->configuration()->requested.old_swapchain == nullptr,
          "the lineage grows and the generation is unique");
    check(!second->owns(first->images().front()) && !first->owns(second->images().front()),
          "an image of another generation is rejected by owns");
    check(first->retired() && !second->retired(),
          "the old Swapchain is retired by a successful creation");
    check(hasError(createSwapchain(*instance, *device, *surface, next_description),
                   Error::old_swapchain_retired),
          "a retired Swapchain cannot be passed as old_swapchain again");
    auto stale = Swapchain{std::move(*second)};
    check(!second->valid() && second->images().empty() && !second->owns(stale.images().front()) &&
              second->generation() == 0,
          "a moved-from Swapchain observes nothing");
  }

  // A transfer-only Swapchain (a blit or readback target) has no image view,
  // since a view needs a view-capable usage.
  if (presenting_support.capabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferDst) {
    SwapchainRequest transfer_request = request;
    transfer_request.usage = vk::ImageUsageFlagBits::eTransferDst;
    const auto transfer_description =
        resolveSwapchain(presenting_support, *presentation, transfer_request);
    if (check(transfer_description.has_value(), "resolveSwapchain for a transfer-only usage")) {
      auto transfer = createSwapchain(*instance, *device, *surface, *transfer_description);
      if (check(transfer.has_value(), "createSwapchain for a transfer-only usage")) {
        bool no_views = !transfer->images().empty();
        for (const auto &image : transfer->images())
          no_views = no_views && image.valid() && !image.nativeImageView();
        check(no_views, "a transfer-only Swapchain has images and no image views");
      }
    }
  }

  // Destroy in lifetime order: Swapchains, Device, then Surfaces (before the Instances).
  {
    const auto gone = std::move(*first);
  }
  {
    const auto gone = std::move(*device);
  }
  {
    const auto gone = std::move(*surface);
  }
  {
    const auto gone = std::move(*other_surface);
  }

  if (has_validation && has_debug_utils) {
    check(recorder.warnings_or_errors.load() == 0, "no validation warnings or errors");
    std::puts("graphics.swapchain: validation-layer=PASS");
  } else {
    std::puts("graphics.swapchain: validation-layer=SKIP "
              "reason=VK_LAYER_KHRONOS_validation or VK_EXT_debug_utils unavailable");
  }
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.swapchain: swapchain=PASS");
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
    test_name = "graphics.swapchain";
    return gpuTest();
  }
  return blocked("unknown mode");
}

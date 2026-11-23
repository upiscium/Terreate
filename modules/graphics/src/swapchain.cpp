#include <terreate/graphics/swapchain.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace terreate::graphics {
namespace {
class SwapchainErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.swapchain"; }
  std::string message(int value) const override {
    switch (static_cast<SwapchainError>(value)) {
    case SwapchainError::invalid_instance:
      return "invalid Instance";
    case SwapchainError::parent_mismatch:
      return "the Device, Surface or old Swapchain does not match";
    case SwapchainError::swapchain_extension_not_enabled:
      return "the Device plan did not enable VK_KHR_swapchain";
    case SwapchainError::function_unavailable:
      return "a swapchain function could not be loaded";
    case SwapchainError::invalid_description:
      return "invalid Swapchain description";
    case SwapchainError::unsupported_configuration:
      return "the Swapchain configuration is not supported by the Surface";
    case SwapchainError::no_image_extent:
      return "the Swapchain image extent is zero";
    case SwapchainError::allocation_failed:
      return "allocation failed";
    case SwapchainError::old_swapchain_retired:
      return "the old Swapchain was already retired by a successful creation";
    }
    return "unknown swapchain error";
  }
};

terreate::Error fail(SwapchainError error, const char *operation, const char *detail) {
  try {
    return terreate::Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
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

template <typename Function> Function loadDevice(VkDevice device, const char *name) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<Function>(vkGetDeviceProcAddr(device, name));
}

template <typename T> bool contains(const std::vector<T> &values, const T &wanted) {
  return std::ranges::find(values, wanted) != values.end();
}

bool sameFormat(const vk::SurfaceFormatKHR &a, const vk::SurfaceFormatKHR &b) noexcept {
  return a.format == b.format && a.colorSpace == b.colorSpace;
}

std::uint64_t nextGeneration() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

constexpr std::uint32_t undefined_extent = std::numeric_limits<std::uint32_t>::max();
} // namespace

const std::error_category &swapchain_error_category() noexcept {
  static const SwapchainErrorCategory category;
  return category;
}
std::error_code make_error_code(SwapchainError error) noexcept {
  return {static_cast<int>(error), swapchain_error_category()};
}

auto resolveSwapchain(const SurfaceSupport &support, const PresentationResolution &resolution,
                      const SwapchainRequest &request) -> terreate::Result<SwapchainDescription> {
  constexpr auto operation = "resolveSwapchain";
  const auto &caps = support.capabilities;
  if (!support.canPresent())
    return std::unexpected(fail(SwapchainError::unsupported_configuration, operation,
                                "no queue family can present to the Surface"));
  if (!std::ranges::any_of(support.formats, [&](const auto &f) {
        return sameFormat(f, resolution.format.effective);
      }))
    // Part of the `any_format_accepted` case: an undefined-only Surface accepts
    // the stated format, so it may be absent from the list.
    if (!(support.formats.size() == 1 && support.formats.front().format == vk::Format::eUndefined))
      return std::unexpected(fail(SwapchainError::unsupported_configuration, operation,
                                  "the resolved format is not offered by the Surface"));
  if (!contains(support.present_modes, resolution.present_mode.effective))
    return std::unexpected(fail(SwapchainError::unsupported_configuration, operation,
                                "the resolved present mode is not offered by the Surface"));

  try {
    SwapchainDescription out;
    out.format = resolution.format.effective;
    out.present_mode = resolution.present_mode.effective;

    // Extent: a defined currentExtent always wins (Vulkan requires it).
    if (caps.currentExtent.width != undefined_extent) {
      out.extent = caps.currentExtent;
    } else {
      out.extent.width =
          std::clamp(request.extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
      out.extent.height =
          std::clamp(request.extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
      if (request.extent.width == 0 || request.extent.height == 0)
        return std::unexpected(
            fail(SwapchainError::no_image_extent, operation, "the requested extent is zero"));
    }
    if (out.extent.width == 0 || out.extent.height == 0)
      return std::unexpected(
          fail(SwapchainError::no_image_extent, operation, "the Surface extent is zero"));

    // Image count: an explicit value is never clamped.
    const std::uint32_t max_count =
        caps.maxImageCount == 0 ? std::numeric_limits<std::uint32_t>::max() : caps.maxImageCount;
    if (request.min_image_count) {
      if (*request.min_image_count < caps.minImageCount || *request.min_image_count > max_count)
        return std::unexpected(fail(SwapchainError::unsupported_configuration, operation,
                                    "the requested image count is outside the supported range"));
      out.min_image_count = *request.min_image_count;
    } else {
      out.min_image_count = std::min(caps.minImageCount + 1, max_count);
    }

    if (!request.usage || (request.usage & ~caps.supportedUsageFlags))
      return std::unexpected(fail(SwapchainError::unsupported_configuration, operation,
                                  "the requested image usage is not supported"));
    out.usage = request.usage;

    if (request.pre_transform && !(*request.pre_transform & caps.supportedTransforms))
      return std::unexpected(fail(SwapchainError::unsupported_configuration, operation,
                                  "the requested pre-transform is not supported"));
    out.pre_transform = request.pre_transform.value_or(caps.currentTransform);

    if (!(request.composite_alpha & caps.supportedCompositeAlpha))
      return std::unexpected(fail(SwapchainError::unsupported_configuration, operation,
                                  "the requested composite alpha is not supported"));
    out.composite_alpha = request.composite_alpha;
    out.clipped = request.clipped;

    if (request.concurrent_queue_families.empty()) {
      out.sharing_mode = vk::SharingMode::eExclusive;
    } else {
      auto families = request.concurrent_queue_families;
      std::ranges::sort(families);
      if (families.size() < 2 || std::ranges::adjacent_find(families) != families.end())
        return std::unexpected(fail(SwapchainError::invalid_description, operation,
                                    "concurrent sharing needs at least two distinct families"));
      out.sharing_mode = vk::SharingMode::eConcurrent;
      out.queue_family_indices = request.concurrent_queue_families;
    }
    return out;
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(SwapchainError::allocation_failed)});
  }
}

struct Swapchain::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
  PFN_vkDestroyImageView destroy_view = nullptr;
  std::uint64_t generation = 0;
  SwapchainConfiguration configuration{};
  std::vector<SwapchainImage> images{};
  bool retired = false;

  ~Impl() {
    for (const auto &image : images)
      if (image.nativeImageView())
        destroy_view(device, static_cast<VkImageView>(image.nativeImageView()), nullptr);
    if (swapchain != VK_NULL_HANDLE && destroy_swapchain != nullptr)
      destroy_swapchain(device, swapchain, nullptr);
  }
};

Swapchain::Swapchain(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}
Swapchain::Swapchain(Swapchain &&other) noexcept = default;
Swapchain &Swapchain::operator=(Swapchain &&other) noexcept = default;
Swapchain::~Swapchain() = default;

vk::SwapchainKHR Swapchain::nativeHandle() const noexcept {
  return vk::SwapchainKHR{nativeSwapchain()};
}
VkSwapchainKHR Swapchain::nativeSwapchain() const noexcept {
  return implementation_ ? implementation_->swapchain : VK_NULL_HANDLE;
}
const SwapchainConfiguration *Swapchain::configuration() const noexcept {
  return implementation_ ? &implementation_->configuration : nullptr;
}
std::uint64_t Swapchain::generation() const noexcept {
  return implementation_ ? implementation_->generation : 0;
}
std::uint32_t Swapchain::lineage() const noexcept {
  return implementation_ ? implementation_->configuration.lineage : 0;
}
std::span<const SwapchainImage> Swapchain::images() const noexcept {
  if (!implementation_)
    return {};
  return implementation_->images;
}
bool Swapchain::retired() const noexcept { return implementation_ && implementation_->retired; }
bool Swapchain::owns(const SwapchainImage &image) const noexcept {
  return implementation_ && image.valid() && image.generation() == implementation_->generation;
}
bool Swapchain::correlatedWith(const Device &device) const noexcept {
  return implementation_ && device.valid() &&
         implementation_->device == static_cast<VkDevice>(device.nativeHandle());
}
bool Swapchain::correlatedWith(const Surface &surface) const noexcept {
  return implementation_ && surface.valid() && implementation_->surface == surface.nativeSurface();
}

auto createSwapchain(const Instance &instance, const Device &device, const Surface &surface,
                     const SwapchainDescription &description) -> terreate::Result<Swapchain> {
  constexpr auto operation = "createSwapchain";
  if (!instance.valid())
    return std::unexpected(
        fail(SwapchainError::invalid_instance, operation, "moved-from Instance"));
  if (!device.valid() || !device.plan() || !device.correlatedWith(instance) ||
      !surface.correlatedWith(instance))
    return std::unexpected(fail(SwapchainError::parent_mismatch, operation,
                                "the Device and Surface must be live and belong to the Instance"));
  const auto &extensions = device.plan()->enabledExtensions();
  if (std::ranges::find(extensions, std::string{presentation_device_extension}) == extensions.end())
    return std::unexpected(fail(SwapchainError::swapchain_extension_not_enabled, operation,
                                "VK_KHR_swapchain is not enabled on the Device"));
  if (description.format.format == vk::Format::eUndefined || description.min_image_count == 0 ||
      description.extent.width == 0 || description.extent.height == 0 ||
      description.image_array_layers == 0 || !description.usage)
    return std::unexpected(fail(SwapchainError::invalid_description, operation,
                                "format, image count, extent, layers and usage must be set"));
  {
    auto families = description.queue_family_indices;
    std::ranges::sort(families);
    if (std::ranges::adjacent_find(families) != families.end())
      return std::unexpected(fail(SwapchainError::invalid_description, operation,
                                  "queue family indices must be distinct"));
  }
  if (description.sharing_mode == vk::SharingMode::eConcurrent
          ? description.queue_family_indices.size() < 2
          : !description.queue_family_indices.empty())
    return std::unexpected(fail(SwapchainError::invalid_description, operation,
                                "queue families must match the sharing mode"));
  const Swapchain::Impl *old = nullptr;
  if (description.old_swapchain != nullptr) {
    if (!description.old_swapchain->valid() || !description.old_swapchain->correlatedWith(device) ||
        !description.old_swapchain->correlatedWith(surface))
      return std::unexpected(
          fail(SwapchainError::parent_mismatch, operation,
               "the old Swapchain must be live and share the Device and Surface"));
    if (description.old_swapchain->retired())
      return std::unexpected(fail(SwapchainError::old_swapchain_retired, operation,
                                  "the old Swapchain was already passed to a creation"));
    old = description.old_swapchain->implementation_.get();
  }

  const auto native_device = static_cast<VkDevice>(device.nativeHandle());
  const auto create = loadDevice<PFN_vkCreateSwapchainKHR>(native_device, "vkCreateSwapchainKHR");
  const auto destroy =
      loadDevice<PFN_vkDestroySwapchainKHR>(native_device, "vkDestroySwapchainKHR");
  const auto get_images =
      loadDevice<PFN_vkGetSwapchainImagesKHR>(native_device, "vkGetSwapchainImagesKHR");
  const auto create_view = loadDevice<PFN_vkCreateImageView>(native_device, "vkCreateImageView");
  const auto destroy_view = loadDevice<PFN_vkDestroyImageView>(native_device, "vkDestroyImageView");
  if (create == nullptr || destroy == nullptr || get_images == nullptr || create_view == nullptr ||
      destroy_view == nullptr)
    return std::unexpected(
        fail(SwapchainError::function_unavailable, operation, "vkGetDeviceProcAddr returned null"));

  try {
    auto impl = std::make_unique<Swapchain::Impl>();
    impl->device = native_device;
    impl->surface = surface.nativeSurface();
    impl->destroy_swapchain = destroy;
    impl->destroy_view = destroy_view;
    impl->generation = nextGeneration();

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = impl->surface;
    info.minImageCount = description.min_image_count;
    info.imageFormat = static_cast<VkFormat>(description.format.format);
    info.imageColorSpace = static_cast<VkColorSpaceKHR>(description.format.colorSpace);
    info.imageExtent = static_cast<VkExtent2D>(description.extent);
    info.imageArrayLayers = description.image_array_layers;
    info.imageUsage = static_cast<VkImageUsageFlags>(description.usage);
    info.imageSharingMode = static_cast<VkSharingMode>(description.sharing_mode);
    info.queueFamilyIndexCount =
        static_cast<std::uint32_t>(description.queue_family_indices.size());
    info.pQueueFamilyIndices = description.queue_family_indices.data();
    info.preTransform = static_cast<VkSurfaceTransformFlagBitsKHR>(description.pre_transform);
    info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(description.composite_alpha);
    info.presentMode = static_cast<VkPresentModeKHR>(description.present_mode);
    info.clipped = description.clipped ? VK_TRUE : VK_FALSE;
    info.oldSwapchain = old != nullptr ? old->swapchain : VK_NULL_HANDLE;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    auto result = create(native_device, &info, nullptr, &created);
    if (result != VK_SUCCESS)
      return std::unexpected(
          nativeFailure(result, "vkCreateSwapchainKHR", "native swapchain creation failed"));
    if (created == VK_NULL_HANDLE)
      return std::unexpected(nativeFailure(VK_ERROR_INITIALIZATION_FAILED, "vkCreateSwapchainKHR",
                                           "native success returned a null swapchain"));
    impl->swapchain = created;
    // The driver retires the old Swapchain as soon as creation succeeds, whatever
    // happens to the image views below.
    if (description.old_swapchain != nullptr)
      description.old_swapchain->implementation_->retired = true;

    std::uint32_t count = 0;
    result = get_images(native_device, created, &count, nullptr);
    if (result != VK_SUCCESS || count == 0)
      return std::unexpected(
          nativeFailure(result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result,
                        "vkGetSwapchainImagesKHR", "image query failed"));
    std::vector<VkImage> native_images(count);
    result = get_images(native_device, created, &count, native_images.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
      return std::unexpected(
          nativeFailure(result, "vkGetSwapchainImagesKHR", "image query failed"));
    native_images.resize(count);

    constexpr auto view_usage =
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eDepthStencilAttachment |
        vk::ImageUsageFlagBits::eInputAttachment;
    const bool create_views = static_cast<bool>(description.usage & view_usage);
    impl->images.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      VkImageViewCreateInfo view_info{};
      view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      view_info.image = native_images[i];
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = info.imageFormat;
      view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view = VK_NULL_HANDLE;
      result = create_views ? create_view(native_device, &view_info, nullptr, &view) : VK_SUCCESS;
      if (result != VK_SUCCESS)
        return std::unexpected(
            nativeFailure(result, "vkCreateImageView", "swapchain image view creation failed"));
      // Recorded at once, so Impl's destructor releases it on a later failure.
      const SwapchainImage entry{SwapchainImageInit{.image = native_images[i],
                                                    .view = view,
                                                    .index = i,
                                                    .generation = impl->generation,
                                                    .format = description.format.format,
                                                    .extent = description.extent,
                                                    .usage = description.usage}};
      impl->images.push_back(entry);
    }

    auto &configuration = impl->configuration;
    configuration.requested = description;
    configuration.requested.old_swapchain = nullptr;
    configuration.format = description.format.format;
    configuration.color_space = description.format.colorSpace;
    configuration.present_mode = description.present_mode;
    configuration.extent = description.extent;
    configuration.image_count = count;
    configuration.usage = description.usage;
    configuration.sharing_mode = description.sharing_mode;
    configuration.lineage = old != nullptr ? old->configuration.lineage + 1 : 1;
    return Swapchain{std::move(impl)};
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(SwapchainError::allocation_failed)});
  }
}

} // namespace terreate::graphics

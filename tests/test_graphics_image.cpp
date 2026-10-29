#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include <terreate/graphics/image_view.hpp>
#include <terreate/graphics/sampler.hpp>

namespace {
struct ImageOwner {
  VmaAllocator allocator = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
};
struct ViewOwner {
  VkDevice device = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};
struct SamplerOwner {
  VkDevice device = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
};
std::array<ImageOwner, 16> live_images{};
std::array<ViewOwner, 16> live_views{};
std::array<SamplerOwner, 16> live_samplers{};
unsigned image_creates = 0;
unsigned image_destroys = 0;
unsigned view_creates = 0;
unsigned view_destroys = 0;
unsigned sampler_creates = 0;
unsigned sampler_destroys = 0;
bool ownership_violation = false;
std::optional<VkResult> image_failure;
std::optional<VkResult> view_failure;
std::optional<VkResult> sampler_failure;
VkImageCreateInfo observed_image{};
VmaAllocationCreateInfo observed_allocation{};
VkImageViewCreateInfo observed_view{};
VkSamplerCreateInfo observed_sampler{};
} // namespace

// GNU ld --wrap symbols are deliberately named with reserved external spellings.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vmaCreateImage(VmaAllocator, const VkImageCreateInfo *,
                                          const VmaAllocationCreateInfo *, VkImage *,
                                          VmaAllocation *, VmaAllocationInfo *);
extern "C" void __real_vmaDestroyImage(VmaAllocator, VkImage, VmaAllocation);
extern "C" VkResult __real_vkCreateImageView(VkDevice, const VkImageViewCreateInfo *,
                                             const VkAllocationCallbacks *, VkImageView *);
extern "C" void __real_vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *);
extern "C" VkResult __real_vkCreateSampler(VkDevice, const VkSamplerCreateInfo *,
                                           const VkAllocationCallbacks *, VkSampler *);
extern "C" void __real_vkDestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks *);

extern "C" VkResult __wrap_vmaCreateImage(VmaAllocator allocator, const VkImageCreateInfo *info,
                                          const VmaAllocationCreateInfo *allocation_info,
                                          VkImage *image, VmaAllocation *allocation,
                                          VmaAllocationInfo *output) {
  ++image_creates;
  observed_image = *info;
  observed_allocation = *allocation_info;
  if (image_failure) {
    const auto failure = *image_failure;
    image_failure.reset();
    *image = VK_NULL_HANDLE;
    *allocation = VK_NULL_HANDLE;
    return failure;
  }
  const auto result =
      __real_vmaCreateImage(allocator, info, allocation_info, image, allocation, output);
  if (result == VK_SUCCESS) {
    bool recorded = false;
    for (auto &owner : live_images) {
      if (owner.image == VK_NULL_HANDLE) {
        owner = {allocator, *image, *allocation};
        recorded = true;
        break;
      }
    }
    ownership_violation |= !recorded;
  }
  return result;
}
extern "C" void __wrap_vmaDestroyImage(VmaAllocator allocator, VkImage image,
                                       VmaAllocation allocation) {
  if (image != VK_NULL_HANDLE || allocation != VK_NULL_HANDLE) {
    ++image_destroys;
    bool found = false;
    for (auto &owner : live_images) {
      if (owner.allocator == allocator && owner.image == image && owner.allocation == allocation &&
          image != VK_NULL_HANDLE) {
        owner = {};
        found = true;
        break;
      }
    }
    ownership_violation |= !found;
  }
  __real_vmaDestroyImage(allocator, image, allocation);
}
extern "C" VkResult __wrap_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *info,
                                             const VkAllocationCallbacks *callbacks,
                                             VkImageView *view) {
  ++view_creates;
  observed_view = *info;
  if (view_failure) {
    const auto failure = *view_failure;
    view_failure.reset();
    *view = VK_NULL_HANDLE;
    return failure;
  }
  const auto result = __real_vkCreateImageView(device, info, callbacks, view);
  if (result == VK_SUCCESS) {
    bool recorded = false;
    for (auto &owner : live_views) {
      if (owner.view == VK_NULL_HANDLE) {
        owner = {device, *view};
        recorded = true;
        break;
      }
    }
    ownership_violation |= !recorded;
  }
  return result;
}
extern "C" void __wrap_vkDestroyImageView(VkDevice device, VkImageView view,
                                          const VkAllocationCallbacks *callbacks) {
  if (view != VK_NULL_HANDLE) {
    ++view_destroys;
    bool found = false;
    for (auto &owner : live_views) {
      if (owner.device == device && owner.view == view) {
        owner = {};
        found = true;
        break;
      }
    }
    ownership_violation |= !found;
  }
  __real_vkDestroyImageView(device, view, callbacks);
}
extern "C" VkResult __wrap_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *info,
                                           const VkAllocationCallbacks *callbacks,
                                           VkSampler *sampler) {
  ++sampler_creates;
  observed_sampler = *info;
  if (sampler_failure) {
    const auto failure = *sampler_failure;
    sampler_failure.reset();
    *sampler = VK_NULL_HANDLE;
    return failure;
  }
  const auto result = __real_vkCreateSampler(device, info, callbacks, sampler);
  if (result == VK_SUCCESS) {
    bool recorded = false;
    for (auto &owner : live_samplers) {
      if (owner.sampler == VK_NULL_HANDLE) {
        owner = {device, *sampler};
        recorded = true;
        break;
      }
    }
    ownership_violation |= !recorded;
  }
  return result;
}
extern "C" void __wrap_vkDestroySampler(VkDevice device, VkSampler sampler,
                                        const VkAllocationCallbacks *callbacks) {
  if (sampler != VK_NULL_HANDLE) {
    ++sampler_destroys;
    bool found = false;
    for (auto &owner : live_samplers) {
      if (owner.device == device && owner.sampler == sampler) {
        owner = {};
        found = true;
        break;
      }
    }
    ownership_violation |= !found;
  }
  __real_vkDestroySampler(device, sampler, callbacks);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
using namespace terreate::graphics;

static_assert(!std::is_default_constructible_v<Image>);
static_assert(!std::copy_constructible<Image>);
static_assert(std::movable<Image>);
static_assert(std::is_nothrow_move_constructible_v<Image>);
static_assert(std::is_nothrow_move_assignable_v<Image>);
static_assert(!std::is_default_constructible_v<ImageView>);
static_assert(!std::copy_constructible<ImageView>);
static_assert(std::movable<ImageView>);
static_assert(std::is_nothrow_move_constructible_v<ImageView>);
static_assert(!std::is_default_constructible_v<Sampler>);
static_assert(!std::copy_constructible<Sampler>);
static_assert(std::movable<Sampler>);
static_assert(std::is_nothrow_move_constructible_v<Sampler>);

bool check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "graphics.image: assertion=FAIL: %s\n", message);
  }
  return condition;
}
bool blocked(const char *message) {
  std::fprintf(stderr, "graphics.image: validation=BLOCKED: %s\n", message);
  return false;
}
bool native_code(const terreate::Error &error, VkResult result) {
  const auto expected = vk::make_error_code(static_cast<vk::Result>(result));
  return error.code() == expected && error.code().value() == static_cast<int>(result) &&
         error.code().category() == expected.category();
}

bool test_resources(Instance &instance, Device &device, Allocator &allocator) {
  bool passed = true;
  std::size_t view_case_count = 0;
  std::size_t sampler_case_count = 0;
  ImageDescription image_desc;
  image_desc.format = vk::Format::eR8G8B8A8Unorm;
  image_desc.extent = vk::Extent3D{4, 4, 1};
  image_desc.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;

  // These are semantic rejections before VMA's native creation boundary.
  // Every expectation is fixed by the public contract, not generated by the implementation.
  struct InvalidImageCase {
    const char *name;
    void (*mutate)(ImageDescription &);
    ImageError expected;
  };
  const std::array image_cases{
      InvalidImageCase{"undefined image format",
                       [](ImageDescription &d) { d.format = vk::Format::eUndefined; },
                       ImageError::invalid_description},
      InvalidImageCase{"zero image width", [](ImageDescription &d) { d.extent.width = 0; },
                       ImageError::invalid_description},
      InvalidImageCase{"zero image height", [](ImageDescription &d) { d.extent.height = 0; },
                       ImageError::invalid_description},
      InvalidImageCase{"zero image depth", [](ImageDescription &d) { d.extent.depth = 0; },
                       ImageError::invalid_description},
      InvalidImageCase{"zero mip levels", [](ImageDescription &d) { d.mip_levels = 0; },
                       ImageError::invalid_description},
      InvalidImageCase{"zero array layers", [](ImageDescription &d) { d.array_layers = 0; },
                       ImageError::invalid_description},
      InvalidImageCase{"empty image usage", [](ImageDescription &d) { d.usage = {}; },
                       ImageError::invalid_description},
      InvalidImageCase{"1D image with height > 1",
                       [](ImageDescription &d) { d.dimensions = vk::ImageType::e1D; },
                       ImageError::invalid_description},
      InvalidImageCase{"2D image with depth > 1", [](ImageDescription &d) { d.extent.depth = 2; },
                       ImageError::invalid_description},
      InvalidImageCase{"3D image with multiple layers",
                       [](ImageDescription &d) {
                         d.dimensions = vk::ImageType::e3D;
                         d.array_layers = 2;
                       },
                       ImageError::invalid_description},
      InvalidImageCase{"mip count exceeds 4x4 image", [](ImageDescription &d) { d.mip_levels = 4; },
                       ImageError::invalid_description},
      InvalidImageCase{"multisample image with multiple mip levels",
                       [](ImageDescription &d) {
                         d.samples = vk::SampleCountFlagBits::e2;
                         d.mip_levels = 2;
                       },
                       ImageError::invalid_description},
      InvalidImageCase{"multisample image with linear tiling",
                       [](ImageDescription &d) {
                         d.samples = vk::SampleCountFlagBits::e2;
                         d.tiling = vk::ImageTiling::eLinear;
                       },
                       ImageError::invalid_description},
      InvalidImageCase{
          "unsupported image tiling",
          [](ImageDescription &d) { d.tiling = vk::ImageTiling::eDrmFormatModifierEXT; },
          ImageError::unsupported_configuration},
      InvalidImageCase{"creation layout GENERAL",
                       [](ImageDescription &d) { d.initial_layout = vk::ImageLayout::eGeneral; },
                       ImageError::invalid_description},
      InvalidImageCase{
          "PREINITIALIZED with optimal tiling",
          [](ImageDescription &d) { d.initial_layout = vk::ImageLayout::ePreinitialized; },
          ImageError::invalid_description},
      InvalidImageCase{
          "unsupported transient image usage",
          [](ImageDescription &d) { d.usage = vk::ImageUsageFlagBits::eTransientAttachment; },
          ImageError::unsupported_configuration},
      InvalidImageCase{"unsupported multisample storage usage",
                       [](ImageDescription &d) {
                         d.samples = vk::SampleCountFlagBits::e2;
                         d.usage = vk::ImageUsageFlagBits::eStorage;
                       },
                       ImageError::unsupported_configuration},
      InvalidImageCase{"dedicated and never-allocate conflict",
                       [](ImageDescription &d) {
                         d.allocation.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT |
                                              VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
                       },
                       ImageError::unsupported_configuration},
      InvalidImageCase{"mutually exclusive host access flags",
                       [](ImageDescription &d) {
                         d.allocation.flags =
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
                       },
                       ImageError::unsupported_configuration},
      InvalidImageCase{
          "unsupported mapped allocation flag",
          [](ImageDescription &d) { d.allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT; },
          ImageError::unsupported_configuration},
      InvalidImageCase{"multiple allocation strategies",
                       [](ImageDescription &d) {
                         d.allocation.flags = VMA_ALLOCATION_CREATE_STRATEGY_MIN_TIME_BIT |
                                              VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT;
                       },
                       ImageError::unsupported_configuration},
      InvalidImageCase{"unsupported allocation usage",
                       [](ImageDescription &d) { d.allocation.usage = VMA_MEMORY_USAGE_MAX_ENUM; },
                       ImageError::unsupported_configuration},
      InvalidImageCase{"unsupported required lazy memory property",
                       [](ImageDescription &d) {
                         d.allocation.required_flags = vk::MemoryPropertyFlagBits::eLazilyAllocated;
                       },
                       ImageError::unsupported_configuration},
      InvalidImageCase{"unsupported preferred protected memory",
                       [](ImageDescription &d) {
                         d.allocation.preferred_flags = vk::MemoryPropertyFlagBits::eProtected;
                       },
                       ImageError::unsupported_configuration},
  };
  for (const auto &item : image_cases) {
    auto invalid = image_desc;
    item.mutate(invalid);
    const auto before = image_creates;
    const auto result = createImage(allocator, invalid);
    passed &= check(!result && result.error().code() == make_error_code(item.expected), item.name);
    passed &= check(image_creates == before, "invalid Image reached vmaCreateImage");
  }

  // Use an actual physical-device limit, never a fabricated capability.
  VmaAllocatorInfo parent{};
  vmaGetAllocatorInfo(allocator.nativeHandle(), &parent);
  VkImageFormatProperties image_limits{};
  const auto format_result = vkGetPhysicalDeviceImageFormatProperties(
      parent.physicalDevice, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &image_limits);
  std::size_t capability_checked = 0;
  std::size_t capability_skipped = 0;
  if (format_result == VK_SUCCESS &&
      image_limits.maxExtent.width < std::numeric_limits<std::uint32_t>::max()) {
    auto oversized = image_desc;
    oversized.extent.width = image_limits.maxExtent.width + 1;
    const auto before = image_creates;
    const auto result = createImage(allocator, oversized);
    passed &= check(!result && result.error().code() ==
                                   make_error_code(ImageError::unsupported_configuration),
                    "image extent larger than actual format limit accepted");
    passed &= check(image_creates == before, "oversized Image reached vmaCreateImage");
    ++capability_checked;
  } else {
    std::puts("graphics.image: capability=image-extent-excess=SKIP");
    ++capability_skipped;
  }

  for (const auto code : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
    image_failure = code;
    auto result = createImage(allocator, image_desc);
    passed &= check(!result && native_code(result.error(), code),
                    "native vmaCreateImage error was not preserved");
    passed &= check(!image_failure, "native Image failure was not exercised");
    image_failure.reset();
  }

  VmaTotalStatistics baseline{};
  vmaCalculateStatistics(allocator.nativeHandle(), &baseline);
  const unsigned image_destroy_before = image_destroys;
  const unsigned view_destroy_before = view_destroys;
  const unsigned sampler_destroy_before = sampler_destroys;
  {
    auto first = createImage(allocator, image_desc);
    auto second = createImage(allocator, image_desc);
    if (!first || !second)
      return blocked("genuine VMA image allocation unavailable");
    passed &=
        check(first->valid() && second->valid() && first->nativeImage() != second->nativeImage() &&
                  first->nativeImage() != VK_NULL_HANDLE,
              "real Image owners are missing or aliased");
    const auto *image_config = first->configuration();
    if (!check(image_config != nullptr, "Image has no configuration"))
      return false;
    passed &=
        check(image_config->requested.format == image_desc.format &&
                  image_config->requested.extent.width == 4 && image_config->extent.width == 4 &&
                  image_config->initial_layout == vk::ImageLayout::eUndefined &&
                  observed_image.extent.width == 4 &&
                  observed_image.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                  observed_allocation.usage == image_desc.allocation.usage,
              "Image requested/effective/native creation differs");
    const auto observation = first->allocationInfo();
    passed &=
        check(observation && observation->native.deviceMemory != VK_NULL_HANDLE &&
                  observation->native.size != 0 && first->nativeAllocation() != VK_NULL_HANDLE &&
                  first->nativeDevice() == device.nativeHandle(),
              "Image allocation/native device observation is invalid");
    image_desc.extent.width = 8;
    passed &= check(image_config->requested.extent.width == 4,
                    "Image retained mutable caller description");
    image_desc.extent.width = 4;
    VmaTotalStatistics live{};
    vmaCalculateStatistics(allocator.nativeHandle(), &live);
    passed &= check(live.total.statistics.allocationCount ==
                        baseline.total.statistics.allocationCount + 2,
                    "two Image owners did not acquire two allocations");

    ImageViewDescription view_desc;
    view_desc.format = image_desc.format;
    view_desc.subresource_range = vk::ImageSubresourceRange{
        vk::ImageAspectFlagBits::eColor, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    // Validate format/aspect, subresource bounds and view-type restrictions
    // before any vkCreateImageView invocation.
    struct InvalidViewCase {
      const char *name;
      void (*mutate)(ImageViewDescription &);
      ImageViewError expected;
    };
    const std::array view_cases{
        InvalidViewCase{"view format undefined",
                        [](ImageViewDescription &d) { d.format = vk::Format::eUndefined; },
                        ImageViewError::invalid_description},
        InvalidViewCase{"view aspect empty",
                        [](ImageViewDescription &d) { d.subresource_range.aspectMask = {}; },
                        ImageViewError::invalid_description},
        InvalidViewCase{"view depth aspect on color image",
                        [](ImageViewDescription &d) {
                          d.subresource_range.aspectMask = vk::ImageAspectFlagBits::eDepth;
                        },
                        ImageViewError::invalid_description},
        InvalidViewCase{"view mixed color/depth aspect",
                        [](ImageViewDescription &d) {
                          d.subresource_range.aspectMask =
                              vk::ImageAspectFlagBits::eColor | vk::ImageAspectFlagBits::eDepth;
                        },
                        ImageViewError::invalid_description},
        InvalidViewCase{"view level count zero",
                        [](ImageViewDescription &d) { d.subresource_range.levelCount = 0; },
                        ImageViewError::invalid_description},
        InvalidViewCase{"view layer count zero",
                        [](ImageViewDescription &d) { d.subresource_range.layerCount = 0; },
                        ImageViewError::invalid_description},
        InvalidViewCase{"view mip base out of bounds",
                        [](ImageViewDescription &d) { d.subresource_range.baseMipLevel = 1; },
                        ImageViewError::invalid_description},
        InvalidViewCase{"view layer base out of bounds",
                        [](ImageViewDescription &d) { d.subresource_range.baseArrayLayer = 1; },
                        ImageViewError::invalid_description},
        InvalidViewCase{"view excessive mip count",
                        [](ImageViewDescription &d) { d.subresource_range.levelCount = 2; },
                        ImageViewError::invalid_description},
        InvalidViewCase{"view excessive layer count",
                        [](ImageViewDescription &d) { d.subresource_range.layerCount = 2; },
                        ImageViewError::invalid_description},
        InvalidViewCase{"view format not mutable",
                        [](ImageViewDescription &d) { d.format = vk::Format::eR8Unorm; },
                        ImageViewError::unsupported_configuration},
        InvalidViewCase{"1D view of 2D image",
                        [](ImageViewDescription &d) { d.view_type = vk::ImageViewType::e1D; },
                        ImageViewError::unsupported_configuration},
        InvalidViewCase{"3D view of 2D image",
                        [](ImageViewDescription &d) { d.view_type = vk::ImageViewType::e3D; },
                        ImageViewError::unsupported_configuration},
        InvalidViewCase{"cube view without cube compatibility",
                        [](ImageViewDescription &d) { d.view_type = vk::ImageViewType::eCube; },
                        ImageViewError::unsupported_configuration},
    };
    for (const auto &item : view_cases) {
      ++view_case_count;
      auto invalid_view = view_desc;
      item.mutate(invalid_view);
      const auto before = view_creates;
      const auto result = createImageView(device, *first, invalid_view);
      passed &=
          check(!result && result.error().code() == make_error_code(item.expected), item.name);
      passed &= check(view_creates == before, "invalid ImageView reached vkCreateImageView");
    }

    {
      auto other_device = createDevice(instance, *device.plan());
      if (!other_device)
        return blocked("second Device unavailable for parent mismatch test");
      const auto mismatch = createImageView(*other_device, *first, view_desc);
      passed &= check(!mismatch && mismatch.error().code() ==
                                       make_error_code(ImageViewError::parent_mismatch),
                      "ImageView accepted a different Device parent");
    }

    const auto image_native = first->nativeImage();
    {
      for (const auto code : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        view_failure = code;
        auto failed_view = createImageView(device, *first, view_desc);
        passed &= check(!failed_view && native_code(failed_view.error(), code),
                        "native vkCreateImageView error was not preserved");
        passed &= check(!view_failure, "native ImageView failure was not exercised");
      }
      auto view_a = createImageView(device, *first, view_desc);
      auto view_b = createImageView(device, *first, view_desc);
      if (!view_a || !view_b)
        return blocked("genuine Vulkan image views unavailable");
      passed &= check(view_a->valid() && view_b->valid() &&
                          view_a->nativeImageView() != view_b->nativeImageView() &&
                          view_a->nativeImage() == first->nativeHandle() &&
                          view_a->correlatedWith(device) && view_a->correlatedWith(*first) &&
                          observed_view.image == image_native &&
                          observed_view.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT,
                      "ImageView handles or parent correlation invalid");
      const auto *view_config = view_a->configuration();
      passed &= check(view_config &&
                          view_config->requested.subresource_range.levelCount ==
                              VK_REMAINING_MIP_LEVELS &&
                          view_config->effective.subresource_range.levelCount == 1 &&
                          view_config->effective.subresource_range.layerCount == 1,
                      "ImageView remaining ranges not resolved in effective configuration");
      const auto view_handle = view_a->nativeImageView();
      ImageView moved_view = std::move(*view_a);
      passed &= check(!view_a->valid() && !view_a->configuration() &&
                          moved_view.nativeImageView() == view_handle,
                      "ImageView move construction did not transfer ownership");
      *view_b = std::move(moved_view);
      // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
      passed &= check(!moved_view.valid() && view_b->nativeImageView() == view_handle &&
                          view_destroys == view_destroy_before + 1,
                      "ImageView move assignment did not release displaced owner");
    }
    passed &= check(view_destroys == view_destroy_before + 2,
                    "ImageViews not destroyed exactly once before Image");

    SamplerDescription sampler_desc;
    // The device can legitimately disable optional features. Rejections must
    // occur before vkCreateSampler, irrespective of host GPU configuration.
    struct InvalidSamplerCase {
      const char *name;
      void (*mutate)(SamplerDescription &);
      SamplerError expected;
    };
    const std::array sampler_cases{
        InvalidSamplerCase{"sampler reversed LOD bounds",
                           [](SamplerDescription &d) {
                             d.min_lod = 2.0F;
                             d.max_lod = 1.0F;
                           },
                           SamplerError::invalid_description},
        InvalidSamplerCase{"sampler unsupported mag filter",
                           [](SamplerDescription &d) { d.mag_filter = vk::Filter::eCubicEXT; },
                           SamplerError::unsupported_configuration},
        InvalidSamplerCase{"sampler unsupported min filter",
                           [](SamplerDescription &d) { d.min_filter = vk::Filter::eCubicEXT; },
                           SamplerError::unsupported_configuration},
        InvalidSamplerCase{"sampler unsupported U address mode",
                           [](SamplerDescription &d) {
                             d.address_mode_u = vk::SamplerAddressMode::eMirrorClampToEdge;
                           },
                           SamplerError::unsupported_configuration},
        InvalidSamplerCase{"sampler unsupported W address mode",
                           [](SamplerDescription &d) {
                             d.address_mode_w = vk::SamplerAddressMode::eMirrorClampToEdge;
                           },
                           SamplerError::unsupported_configuration},
        InvalidSamplerCase{
            "sampler unsupported border color",
            [](SamplerDescription &d) { d.border_color = vk::BorderColor::eFloatCustomEXT; },
            SamplerError::unsupported_configuration},
        InvalidSamplerCase{
            "sampler infinite LOD bias",
            [](SamplerDescription &d) { d.mip_lod_bias = std::numeric_limits<float>::infinity(); },
            SamplerError::invalid_description},
        InvalidSamplerCase{
            "sampler infinite min LOD",
            [](SamplerDescription &d) { d.min_lod = std::numeric_limits<float>::infinity(); },
            SamplerError::invalid_description},
        InvalidSamplerCase{
            "sampler nonfinite max LOD",
            [](SamplerDescription &d) { d.max_lod = std::numeric_limits<float>::quiet_NaN(); },
            SamplerError::invalid_description},
        InvalidSamplerCase{"sampler infinite anisotropy",
                           [](SamplerDescription &d) {
                             d.max_anisotropy = std::numeric_limits<float>::infinity();
                           },
                           SamplerError::invalid_description},
        InvalidSamplerCase{"sampler anisotropy less than one",
                           [](SamplerDescription &d) {
                             d.anisotropy_enable = true;
                             d.max_anisotropy = 0.5F;
                           },
                           SamplerError::unsupported_configuration},
        InvalidSamplerCase{"unnormalized sampler differing filters",
                           [](SamplerDescription &d) {
                             d.unnormalized_coordinates = true;
                             d.min_filter = vk::Filter::eLinear;
                           },
                           SamplerError::invalid_description},
        InvalidSamplerCase{"unnormalized sampler linear mipmapping",
                           [](SamplerDescription &d) {
                             d.unnormalized_coordinates = true;
                             d.mipmap_mode = vk::SamplerMipmapMode::eLinear;
                           },
                           SamplerError::invalid_description},
        InvalidSamplerCase{"unnormalized sampler nonzero max LOD",
                           [](SamplerDescription &d) {
                             d.unnormalized_coordinates = true;
                             d.max_lod = 1.0F;
                           },
                           SamplerError::invalid_description},
        InvalidSamplerCase{"unnormalized sampler repeat addressing",
                           [](SamplerDescription &d) {
                             d.unnormalized_coordinates = true;
                             d.address_mode_u = vk::SamplerAddressMode::eRepeat;
                           },
                           SamplerError::invalid_description},
        InvalidSamplerCase{"unnormalized sampler compare enabled",
                           [](SamplerDescription &d) {
                             d.unnormalized_coordinates = true;
                             d.compare_enable = true;
                           },
                           SamplerError::invalid_description},
    };
    for (const auto &item : sampler_cases) {
      ++sampler_case_count;
      auto invalid_sampler = sampler_desc;
      item.mutate(invalid_sampler);
      const auto before = sampler_creates;
      const auto result = createSampler(device, invalid_sampler);
      passed &=
          check(!result && result.error().code() == make_error_code(item.expected), item.name);
      passed &= check(sampler_creates == before, "invalid Sampler reached vkCreateSampler");
    }

    const auto &sampler_limits = device.plan()->capabilities().properties.limits;
    if (std::isfinite(sampler_limits.maxSamplerLodBias) &&
        sampler_limits.maxSamplerLodBias < std::numeric_limits<float>::max() / 2.0F) {
      auto beyond_lod_bias = sampler_desc;
      beyond_lod_bias.mip_lod_bias = sampler_limits.maxSamplerLodBias + 1.0F;
      const auto before = sampler_creates;
      const auto result = createSampler(device, beyond_lod_bias);
      passed &= check(!result && result.error().code() ==
                                     make_error_code(SamplerError::unsupported_configuration),
                      "sampler LOD bias above actual Device limit accepted");
      passed &= check(sampler_creates == before, "over-limit Sampler reached vkCreateSampler");
      ++capability_checked;
    } else {
      std::puts("graphics.image: capability=sampler-lod-bias-excess=SKIP");
      ++capability_skipped;
    }
    if (!device.plan()->enabledFeatures().samplerAnisotropy) {
      auto unavailable_anisotropy = sampler_desc;
      unavailable_anisotropy.anisotropy_enable = true;
      const auto before = sampler_creates;
      const auto result = createSampler(device, unavailable_anisotropy);
      passed &= check(!result && result.error().code() ==
                                     make_error_code(SamplerError::unsupported_configuration),
                      "disabled sampler anisotropy feature was implicitly enabled");
      passed &=
          check(sampler_creates == before, "feature-disabled Sampler reached vkCreateSampler");
      ++capability_checked;
    } else {
      std::puts("graphics.image: capability=sampler-anisotropy-disabled=SKIP");
      ++capability_skipped;
    }

    {
      for (const auto code : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        sampler_failure = code;
        auto failed_sampler = createSampler(device, sampler_desc);
        passed &= check(!failed_sampler && native_code(failed_sampler.error(), code),
                        "native vkCreateSampler error was not preserved");
        passed &= check(!sampler_failure, "native Sampler failure was not exercised");
      }
      auto sampler_a = createSampler(device, sampler_desc);
      auto sampler_b = createSampler(device, sampler_desc);
      if (!sampler_a || !sampler_b)
        return blocked("genuine Vulkan samplers unavailable");
      const auto handle = sampler_a->nativeSampler();
      passed &=
          check(sampler_a->valid() && sampler_b->valid() && handle != sampler_b->nativeSampler() &&
                    sampler_a->correlatedWith(device) && sampler_a->configuration() &&
                    observed_sampler.magFilter == VK_FILTER_NEAREST,
                "Sampler creation, native options or parent relation incorrect");
      Sampler moved_sampler = std::move(*sampler_a);
      passed &= check(!sampler_a->valid() && !sampler_a->configuration() &&
                          moved_sampler.nativeSampler() == handle,
                      "Sampler move construction retained old owner");
      *sampler_b = std::move(moved_sampler);
      // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
      passed &= check(!moved_sampler.valid() && sampler_b->nativeSampler() == handle &&
                          sampler_destroys == sampler_destroy_before + 1,
                      "Sampler move assignment did not release displaced owner");
    }
    passed &= check(sampler_destroys == sampler_destroy_before + 2,
                    "Samplers not destroyed exactly once");

    const VkImage original = first->nativeImage();
    const VmaAllocation original_allocation = first->nativeAllocation();
    Image moved_image = std::move(*first);
    passed &= check(!first->valid() && first->nativeImage() == VK_NULL_HANDLE &&
                        first->nativeAllocation() == VK_NULL_HANDLE &&
                        first->configuration() == nullptr && !first->allocationInfo() &&
                        moved_image.nativeImage() == original &&
                        moved_image.nativeAllocation() == original_allocation,
                    "Image moved-from or move construction invalid");
    *second = std::move(moved_image);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    passed &= check(!moved_image.valid() && second->nativeImage() == original &&
                        image_destroys == image_destroy_before + 1,
                    "Image move assignment did not free displaced native owner");
  }
  VmaTotalStatistics after{};
  vmaCalculateStatistics(allocator.nativeHandle(), &after);
  passed &= check(
      image_destroys == image_destroy_before + 2 && !ownership_violation &&
          after.total.statistics.allocationCount == baseline.total.statistics.allocationCount &&
          after.total.statistics.allocationBytes == baseline.total.statistics.allocationBytes,
      "Image ownership leaked, double-freed or altered VMA statistics");
  for (const auto &x : live_images)
    passed &= check(x.image == VK_NULL_HANDLE, "Image native pair remains live");
  for (const auto &x : live_views)
    passed &= check(x.view == VK_NULL_HANDLE, "ImageView remains live");
  for (const auto &x : live_samplers)
    passed &= check(x.sampler == VK_NULL_HANDLE, "Sampler remains live");
  std::printf(
      "graphics.image: negative-cases=image:%zu view:%zu sampler:%zu capability:%zu skipped:%zu\n",
      image_cases.size(), view_case_count, sampler_case_count, capability_checked,
      capability_skipped);
  return passed;
}

bool test_headless() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capabilities unavailable");
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_0;
  const auto plan = resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("headless Instance resolution unavailable");
  auto instance = createInstance(*plan);
  if (!instance)
    return blocked("headless Instance creation unavailable");
  const auto inventory = queryPhysicalDevices(*instance);
  if (!inventory || inventory->empty())
    return blocked("no native physical device available");

  for (const auto &candidate : *inventory) {
    DeviceDescription device_desc;
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t i = 0; i < families.size(); ++i) {
      const auto flags =
          families[i].queueFlags & (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
                                    vk::QueueFlagBits::eTransfer);
      if (families[i].queueCount != 0 && static_cast<bool>(flags)) {
        DeviceQueueRequest request;
        request.caller_id = "image-test";
        request.required_flags = flags;
        request.allowed_family_indices = {i};
        device_desc.queue_requests.push_back(std::move(request));
        break;
      }
    }
    if (device_desc.queue_requests.empty())
      continue;
    const auto device_plan = resolveDevice(candidate, device_desc);
    if (!device_plan)
      continue;
    auto device = createDevice(*instance, *device_plan);
    if (!device)
      continue;
    auto allocator = createAllocator(*instance, candidate, *device);
    if (!allocator)
      continue;
    // No GPU submissions: view and sampler die before Image, then Allocator,
    // Device, PhysicalDevice inventory, and Instance in the caller.
    return test_resources(*instance, *device, *allocator);
  }
  return blocked("no usable native Image test Device/Allocator");
}
} // namespace

int main() {
  const bool passed = test_headless();
  if (passed)
    std::puts("graphics.image: headless-image=PASS");
  else
    std::fputs("graphics.image: headless-image=BLOCKED; native coverage not established\n", stderr);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

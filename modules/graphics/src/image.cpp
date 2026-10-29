#include <terreate/graphics/image.hpp>

#include <algorithm>
#include <bit>
#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
class ImageErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.image"; }
  std::string message(int value) const override {
    switch (static_cast<ImageError>(value)) {
    case ImageError::invalid_parent:
      return "invalid image allocator";
    case ImageError::invalid_description:
      return "invalid image description";
    case ImageError::unsupported_configuration:
      return "unsupported image configuration";
    case ImageError::invalid_image:
      return "invalid image";
    }
    return "unknown image error";
  }
};
auto failure(ImageError error, const char *detail) {
  return std::unexpected(terreate::Error{make_error_code(error), "createImage", detail});
}
auto nativeFailure(VkResult result, const char *operation) {
  return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                         operation, "native image operation failed"});
}
} // namespace

const std::error_category &image_error_category() noexcept {
  static const ImageErrorCategory category;
  return category;
}
std::error_code make_error_code(ImageError error) noexcept {
  return {static_cast<int>(error), image_error_category()};
}

struct Image::Impl {
  VmaAllocator allocator = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  vk::Device device{};
  ImageConfiguration configuration{};
  ~Impl() {
    if (allocation != VK_NULL_HANDLE)
      vmaDestroyImage(allocator, image, allocation);
  }
};

Image::Image(std::unique_ptr<Impl> impl) noexcept : implementation_(std::move(impl)) {}
Image::Image(Image &&) noexcept = default;
Image &Image::operator=(Image &&) noexcept = default;
Image::~Image() = default;
bool Image::valid() const noexcept {
  return nativeImage() != VK_NULL_HANDLE && nativeAllocation() != VK_NULL_HANDLE;
}
vk::Image Image::nativeHandle() const noexcept { return vk::Image{nativeImage()}; }
VkImage Image::nativeImage() const noexcept {
  return implementation_ ? implementation_->image : VK_NULL_HANDLE;
}
VmaAllocator Image::nativeAllocator() const noexcept {
  return implementation_ ? implementation_->allocator : VK_NULL_HANDLE;
}
VmaAllocation Image::nativeAllocation() const noexcept {
  return implementation_ ? implementation_->allocation : VK_NULL_HANDLE;
}
vk::Device Image::nativeDevice() const noexcept {
  return implementation_ ? implementation_->device : vk::Device{};
}
const ImageConfiguration *Image::configuration() const noexcept {
  return implementation_ ? &implementation_->configuration : nullptr;
}
terreate::Result<ImageAllocationObservation> Image::allocationInfo() const {
  if (!valid())
    return std::unexpected(terreate::Error{make_error_code(ImageError::invalid_image),
                                           "allocationInfo", "moved-from image"});
  ImageAllocationObservation observation{};
  vmaGetAllocationInfo(nativeAllocator(), nativeAllocation(), &observation.native);
  VkMemoryPropertyFlags properties = 0;
  vmaGetAllocationMemoryProperties(nativeAllocator(), nativeAllocation(), &properties);
  observation.memory_properties = vk::MemoryPropertyFlags{properties};
  return observation;
}

auto createImage(const Allocator &allocator, const ImageDescription &d) -> terreate::Result<Image> {
  if (!allocator.valid())
    return failure(ImageError::invalid_parent, "live borrowed Allocator required");
  if (static_cast<std::uint32_t>(d.format) >
      static_cast<std::uint32_t>(vk::Format::eAstc12x12SrgbBlock))
    return failure(ImageError::unsupported_configuration, "only core Vulkan 1.0 formats supported");
  if (d.format == vk::Format::eUndefined || !d.extent.width || !d.extent.height ||
      !d.extent.depth || !d.mip_levels || !d.array_layers || !d.usage)
    return failure(ImageError::invalid_description,
                   "nonzero format, extent, mips, layers and usage required");
  switch (d.dimensions) {
  case vk::ImageType::e1D:
    if (d.extent.height != 1 || d.extent.depth != 1)
      return failure(ImageError::invalid_description, "1D requires height and depth one");
    break;
  case vk::ImageType::e2D:
    if (d.extent.depth != 1)
      return failure(ImageError::invalid_description, "2D requires depth one");
    break;
  case vk::ImageType::e3D:
    if (d.array_layers != 1)
      return failure(ImageError::invalid_description, "3D requires one array layer");
    break;
  default:
    return failure(ImageError::invalid_description, "unknown image dimensions");
  }
  const auto samples = static_cast<VkSampleCountFlags>(d.samples);
  if (!std::has_single_bit(samples) || samples > VK_SAMPLE_COUNT_64_BIT ||
      d.mip_levels > std::bit_width(std::max({d.extent.width, d.extent.height, d.extent.depth})))
    return failure(ImageError::invalid_description, "invalid sample count or mip count");
  if (d.tiling != vk::ImageTiling::eOptimal && d.tiling != vk::ImageTiling::eLinear)
    return failure(ImageError::unsupported_configuration,
                   "only optimal and linear tiling supported");
  if (d.initial_layout != vk::ImageLayout::eUndefined &&
      d.initial_layout != vk::ImageLayout::ePreinitialized)
    return failure(ImageError::invalid_description,
                   "initial layout must be UNDEFINED or PREINITIALIZED");
  if (d.initial_layout == vk::ImageLayout::ePreinitialized && d.tiling != vk::ImageTiling::eLinear)
    return failure(ImageError::invalid_description, "PREINITIALIZED requires linear tiling");
  if (samples != VK_SAMPLE_COUNT_1_BIT &&
      (d.dimensions != vk::ImageType::e2D || d.mip_levels != 1 ||
       d.tiling != vk::ImageTiling::eOptimal))
    return failure(ImageError::invalid_description,
                   "multisampling requires optimal 2D with one mip");
  constexpr VkImageUsageFlags supported_usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  const auto usage = static_cast<VkImageUsageFlags>(d.usage);
  if ((usage & ~supported_usage) ||
      (samples != VK_SAMPLE_COUNT_1_BIT && (usage & VK_IMAGE_USAGE_STORAGE_BIT)))
    return failure(ImageError::unsupported_configuration,
                   "unsupported usage or feature-dependent multisample storage");
  const auto &a = d.allocation;
  constexpr VmaAllocationCreateFlags supported_flags =
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT | VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT |
      VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT |
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT |
      VMA_ALLOCATION_CREATE_STRATEGY_MIN_TIME_BIT | VMA_ALLOCATION_CREATE_STRATEGY_MIN_OFFSET_BIT;
  const auto strategies = a.flags & VMA_ALLOCATION_CREATE_STRATEGY_MASK;
  if ((a.flags & ~supported_flags) ||
      ((a.flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT) &&
       (a.flags & VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT)) ||
      ((a.flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) &&
       (a.flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT)) ||
      (strategies && (strategies & (strategies - 1))))
    return failure(ImageError::unsupported_configuration,
                   "unsupported or conflicting allocation flags");
  switch (a.usage) {
  case VMA_MEMORY_USAGE_UNKNOWN:
  case VMA_MEMORY_USAGE_GPU_ONLY:
  case VMA_MEMORY_USAGE_CPU_ONLY:
  case VMA_MEMORY_USAGE_CPU_TO_GPU:
  case VMA_MEMORY_USAGE_GPU_TO_CPU:
  case VMA_MEMORY_USAGE_CPU_COPY:
  case VMA_MEMORY_USAGE_AUTO:
  case VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE:
  case VMA_MEMORY_USAGE_AUTO_PREFER_HOST:
    break;
  default:
    return failure(ImageError::unsupported_configuration, "unsupported allocation usage");
  }
  constexpr VkMemoryPropertyFlags supported_properties =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  if ((static_cast<VkMemoryPropertyFlags>(a.required_flags) |
       static_cast<VkMemoryPropertyFlags>(a.preferred_flags)) &
      ~supported_properties)
    return failure(ImageError::unsupported_configuration, "unsupported memory properties");

  VmaAllocatorInfo parent{};
  vmaGetAllocatorInfo(allocator.nativeHandle(), &parent);
  // Capability queries only: never record commands or change image layouts.
  try {
    vk::raii::Context context;
    const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
        context.getDispatcher()->vkGetInstanceProcAddr(parent.instance,
                                                       "vkGetPhysicalDeviceImageFormatProperties"));
    if (!query)
      return failure(ImageError::unsupported_configuration, "image format query unavailable");
    VkImageFormatProperties limits{};
    const auto result = query(parent.physicalDevice, static_cast<VkFormat>(d.format),
                              static_cast<VkImageType>(d.dimensions),
                              static_cast<VkImageTiling>(d.tiling), usage, 0, &limits);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
      return failure(ImageError::unsupported_configuration, "format/type/tiling/usage unsupported");
    if (result != VK_SUCCESS)
      return nativeFailure(result, "vkGetPhysicalDeviceImageFormatProperties");
    if (d.extent.width > limits.maxExtent.width || d.extent.height > limits.maxExtent.height ||
        d.extent.depth > limits.maxExtent.depth || d.mip_levels > limits.maxMipLevels ||
        d.array_layers > limits.maxArrayLayers || !(limits.sampleCounts & samples))
      return failure(ImageError::unsupported_configuration, "image exceeds format limits");
  } catch (const vk::SystemError &error) {
    return std::unexpected(terreate::Error{error.code(), "createImage", error.what()});
  }

  auto impl = std::make_unique<Image::Impl>();
  impl->allocator = allocator.nativeHandle();
  impl->device = vk::Device{parent.device};
  impl->configuration = {d,
                         d.format,
                         d.extent,
                         d.dimensions,
                         d.mip_levels,
                         d.array_layers,
                         d.samples,
                         d.tiling,
                         d.usage,
                         d.initial_layout,
                         a};
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = static_cast<VkImageType>(d.dimensions);
  image_info.format = static_cast<VkFormat>(d.format);
  image_info.extent = static_cast<VkExtent3D>(d.extent);
  image_info.mipLevels = d.mip_levels;
  image_info.arrayLayers = d.array_layers;
  image_info.samples = static_cast<VkSampleCountFlagBits>(d.samples);
  image_info.tiling = static_cast<VkImageTiling>(d.tiling);
  image_info.usage = usage;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = static_cast<VkImageLayout>(d.initial_layout);
  VmaAllocationCreateInfo allocation_info{};
  allocation_info.usage = a.usage;
  allocation_info.flags = a.flags;
  allocation_info.requiredFlags = static_cast<VkMemoryPropertyFlags>(a.required_flags);
  allocation_info.preferredFlags = static_cast<VkMemoryPropertyFlags>(a.preferred_flags);
  allocation_info.memoryTypeBits = a.memory_type_bits;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  const auto result =
      vmaCreateImage(impl->allocator, &image_info, &allocation_info, &image, &allocation, nullptr);
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vmaCreateImage");
  // VMA owns failed-call cleanup; publish only a successfully coupled pair.
  impl->image = image;
  impl->allocation = allocation;
  return Image{std::move(impl)};
}

} // namespace terreate::graphics

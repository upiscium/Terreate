#include <terreate/graphics/image_view.hpp>

#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
class ImageViewErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.image_view"; }
  std::string message(int value) const override {
    switch (static_cast<ImageViewError>(value)) {
    case ImageViewError::invalid_parent:
      return "invalid image view parent";
    case ImageViewError::parent_mismatch:
      return "image view parents do not match";
    case ImageViewError::invalid_description:
      return "invalid image view description";
    case ImageViewError::unsupported_configuration:
      return "unsupported image view configuration";
    }
    return "unknown image view error";
  }
};
auto failure(ImageViewError error, const char *detail) {
  return std::unexpected(terreate::Error{make_error_code(error), "createImageView", detail});
}
VkImageAspectFlags aspects(vk::Format format) {
  switch (format) {
  case vk::Format::eD16Unorm:
  case vk::Format::eX8D24UnormPack32:
  case vk::Format::eD32Sfloat:
    return VK_IMAGE_ASPECT_DEPTH_BIT;
  case vk::Format::eS8Uint:
    return VK_IMAGE_ASPECT_STENCIL_BIT;
  case vk::Format::eD16UnormS8Uint:
  case vk::Format::eD24UnormS8Uint:
  case vk::Format::eD32SfloatS8Uint:
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  default:
    return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}
bool component(vk::ComponentSwizzle value) {
  return static_cast<unsigned>(value) <= static_cast<unsigned>(vk::ComponentSwizzle::eA);
}
bool identity(const vk::ComponentMapping &c) {
  return (c.r == vk::ComponentSwizzle::eIdentity || c.r == vk::ComponentSwizzle::eR) &&
         (c.g == vk::ComponentSwizzle::eIdentity || c.g == vk::ComponentSwizzle::eG) &&
         (c.b == vk::ComponentSwizzle::eIdentity || c.b == vk::ComponentSwizzle::eB) &&
         (c.a == vk::ComponentSwizzle::eIdentity || c.a == vk::ComponentSwizzle::eA);
}
} // namespace
const std::error_category &image_view_error_category() noexcept {
  static const ImageViewErrorCategory category;
  return category;
}
std::error_code make_error_code(ImageViewError error) noexcept {
  return {static_cast<int>(error), image_view_error_category()};
}
struct ImageView::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  ImageViewConfiguration configuration{};
  ~Impl() {
    if (view != VK_NULL_HANDLE)
      vkDestroyImageView(device, view, nullptr);
  }
};
ImageView::ImageView(std::unique_ptr<Impl> impl) noexcept : implementation_(std::move(impl)) {}
ImageView::ImageView(ImageView &&) noexcept = default;
ImageView &ImageView::operator=(ImageView &&) noexcept = default;
ImageView::~ImageView() = default;
bool ImageView::valid() const noexcept { return nativeImageView() != VK_NULL_HANDLE; }
vk::ImageView ImageView::nativeHandle() const noexcept { return vk::ImageView{nativeImageView()}; }
VkImageView ImageView::nativeImageView() const noexcept {
  return implementation_ ? implementation_->view : VK_NULL_HANDLE;
}
vk::Device ImageView::nativeDevice() const noexcept {
  return vk::Device{implementation_ ? implementation_->device : VK_NULL_HANDLE};
}
vk::Image ImageView::nativeImage() const noexcept {
  return vk::Image{implementation_ ? implementation_->image : VK_NULL_HANDLE};
}
bool ImageView::correlatedWith(const Device &device) const noexcept {
  return valid() && device.valid() && nativeDevice() == device.nativeHandle();
}
bool ImageView::correlatedWith(const Image &image) const noexcept {
  return valid() && image.valid() && nativeImage() == image.nativeHandle() &&
         nativeDevice() == image.nativeDevice();
}
const ImageViewConfiguration *ImageView::configuration() const noexcept {
  return implementation_ ? &implementation_->configuration : nullptr;
}
auto createImageView(const Device &device, const Image &image,
                     const ImageViewDescription &description) -> terreate::Result<ImageView> {
  if (!device.valid() || !device.plan() || !image.valid() || !image.configuration())
    return failure(ImageViewError::invalid_parent, "live borrowed Device and Image required");
  if (image.nativeDevice() != device.nativeHandle())
    return failure(ImageViewError::parent_mismatch, "Image must belong to supplied Device");
  const auto &parent = *image.configuration();
  auto effective = description;
  auto &range = effective.subresource_range;
  if (description.format == vk::Format::eUndefined || !range.aspectMask || !range.levelCount ||
      !range.layerCount || range.baseMipLevel >= parent.mip_levels ||
      range.baseArrayLayer >= parent.array_layers || !component(description.components.r) ||
      !component(description.components.g) || !component(description.components.b) ||
      !component(description.components.a))
    return failure(ImageViewError::invalid_description,
                   "invalid format, components or subresource range");
  // Image has no MUTABLE_FORMAT, CUBE_COMPATIBLE or 2D_ARRAY_COMPATIBLE flags.
  if (description.format != parent.format)
    return failure(ImageViewError::unsupported_configuration, "Image format is not mutable");
  if (static_cast<unsigned>(parent.format) > static_cast<unsigned>(vk::Format::eAstc12x12SrgbBlock))
    return failure(ImageViewError::unsupported_configuration,
                   "extension and multiplanar formats require a separate explicit view contract");
  if (range.levelCount == VK_REMAINING_MIP_LEVELS)
    range.levelCount = parent.mip_levels - range.baseMipLevel;
  if (range.layerCount == VK_REMAINING_ARRAY_LAYERS)
    range.layerCount = parent.array_layers - range.baseArrayLayer;
  if (range.levelCount > parent.mip_levels - range.baseMipLevel ||
      range.layerCount > parent.array_layers - range.baseArrayLayer)
    return failure(ImageViewError::invalid_description, "subresource range exceeds Image");
  const auto valid_aspects = aspects(parent.format);
  if (static_cast<VkImageAspectFlags>(range.aspectMask) & ~valid_aspects)
    return failure(ImageViewError::invalid_description, "aspect incompatible with format");
  constexpr VkImageUsageFlags view_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  if (!(static_cast<VkImageUsageFlags>(parent.usage) & view_usage))
    return failure(ImageViewError::unsupported_configuration, "Image usage does not permit views");
  bool compatible_type = false;
  switch (description.view_type) {
  case vk::ImageViewType::e1D:
    compatible_type = parent.dimensions == vk::ImageType::e1D && range.layerCount == 1;
    break;
  case vk::ImageViewType::e1DArray:
    compatible_type = parent.dimensions == vk::ImageType::e1D;
    break;
  case vk::ImageViewType::e2D:
    compatible_type = parent.dimensions == vk::ImageType::e2D && range.layerCount == 1;
    break;
  case vk::ImageViewType::e2DArray:
    compatible_type = parent.dimensions == vk::ImageType::e2D;
    break;
  case vk::ImageViewType::e3D:
    compatible_type = parent.dimensions == vk::ImageType::e3D;
    break;
  default:
    break;
  }
  if (!compatible_type)
    return failure(ImageViewError::unsupported_configuration,
                   "view type incompatible with Image creation");
  if ((valid_aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
       (parent.usage &
        (vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eInputAttachment))) &&
      !identity(description.components))
    return failure(ImageViewError::invalid_description,
                   "usage/format requires identity components");
  auto impl = std::make_unique<ImageView::Impl>();
  impl->device = static_cast<VkDevice>(device.nativeHandle());
  impl->image = image.nativeImage();
  impl->configuration = {description, effective};
  VkImageViewCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = impl->image;
  info.viewType = static_cast<VkImageViewType>(effective.view_type);
  info.format = static_cast<VkFormat>(effective.format);
  info.components = static_cast<VkComponentMapping>(effective.components);
  info.subresourceRange = static_cast<VkImageSubresourceRange>(range);
  VkImageView view = VK_NULL_HANDLE;
  const auto result = vkCreateImageView(impl->device, &info, nullptr, &view);
  if (result != VK_SUCCESS)
    return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                           "vkCreateImageView",
                                           "native image view creation failed"});
  if (view == VK_NULL_HANDLE)
    return failure(ImageViewError::invalid_parent, "native success returned a null view");
  impl->view = view;
  return ImageView{std::move(impl)};
}
} // namespace terreate::graphics

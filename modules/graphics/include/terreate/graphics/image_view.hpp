#ifndef TERREATE_GRAPHICS_IMAGE_VIEW_HPP
#define TERREATE_GRAPHICS_IMAGE_VIEW_HPP

#include <terreate/graphics/image.hpp>

namespace terreate::graphics {

/// Explicit interpretation; undefined format and empty aspect are invalid.
/// Remaining mip/layer counts are resolved in the effective configuration.
struct ImageViewDescription {
  vk::ImageViewType view_type = vk::ImageViewType::e2D;
  vk::Format format = vk::Format::eUndefined;
  vk::ComponentMapping components{};
  vk::ImageSubresourceRange subresource_range{};
};
struct ImageViewConfiguration {
  ImageViewDescription requested{};
  ImageViewDescription effective{};
};
enum class ImageViewError : std::uint8_t {
  invalid_parent = 1,
  parent_mismatch,
  invalid_description,
  unsupported_configuration,
};
[[nodiscard]] const std::error_category &image_view_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(ImageViewError error) noexcept;

/// Owns one VkImageView; native Device/Image are borrowed, not wrapper addresses.
/// Destroy View before Image before Allocator before Device before Instance.
/// Complete GPU use before destruction or move-assignment displacement. Native
/// handles are never caller-destroyed; host synchronization is caller-owned.
class ImageView final {
public:
  ImageView() = delete;
  ImageView(const ImageView &) = delete;
  ImageView &operator=(const ImageView &) = delete;
  ImageView(ImageView &&) noexcept;
  ImageView &operator=(ImageView &&) noexcept;
  ~ImageView();
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::ImageView nativeHandle() const noexcept;
  [[nodiscard]] VkImageView nativeImageView() const noexcept;
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  [[nodiscard]] vk::Image nativeImage() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &) const noexcept;
  [[nodiscard]] bool correlatedWith(const Image &) const noexcept;
  /// Null for moved-from values.
  [[nodiscard]] const ImageViewConfiguration *configuration() const noexcept;

private:
  struct Impl;
  explicit ImageView(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createImageView(const Device &, const Image &, const ImageViewDescription &)
      -> terreate::Result<ImageView>;
};
[[nodiscard]] auto createImageView(const Device &, const Image &, const ImageViewDescription &)
    -> terreate::Result<ImageView>;

} // namespace terreate::graphics
namespace std {
template <> struct is_error_code_enum<terreate::graphics::ImageViewError> : true_type {};
} // namespace std
#endif // TERREATE_GRAPHICS_IMAGE_VIEW_HPP

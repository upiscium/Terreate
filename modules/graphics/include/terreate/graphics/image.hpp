#ifndef TERREATE_GRAPHICS_IMAGE_HPP
#define TERREATE_GRAPHICS_IMAGE_HPP

#include <terreate/graphics/allocator.hpp>

namespace terreate::graphics {

struct ImageAllocationDescription {
  VmaMemoryUsage usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationCreateFlags flags = 0;
  vk::MemoryPropertyFlags required_flags{};
  vk::MemoryPropertyFlags preferred_flags{};
  std::uint32_t memory_type_bits = 0;
};

/// Exclusive sharing, no creation flags/pNext, no implicit usage or transitions.
struct ImageDescription {
  vk::Format format = vk::Format::eUndefined;
  vk::Extent3D extent{0, 0, 0};
  vk::ImageType dimensions = vk::ImageType::e2D;
  std::uint32_t mip_levels = 1;
  std::uint32_t array_layers = 1;
  vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;
  vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
  vk::ImageUsageFlags usage{};
  vk::ImageLayout initial_layout = vk::ImageLayout::eUndefined;
  ImageAllocationDescription allocation{};
};

struct ImageConfiguration {
  ImageDescription requested{};
  vk::Format format = vk::Format::eUndefined;
  vk::Extent3D extent{0, 0, 0};
  vk::ImageType dimensions = vk::ImageType::e2D;
  std::uint32_t mip_levels = 1;
  std::uint32_t array_layers = 1;
  vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;
  vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
  vk::ImageUsageFlags usage{};
  vk::ImageLayout initial_layout = vk::ImageLayout::eUndefined;
  ImageAllocationDescription allocation{};
};

struct ImageAllocationObservation {
  VmaAllocationInfo native{};
  vk::MemoryPropertyFlags memory_properties{};
};

enum class ImageError : std::uint8_t {
  invalid_parent = 1,
  invalid_description,
  unsupported_configuration,
  invalid_image,
};

[[nodiscard]] const std::error_category &image_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(ImageError error) noexcept;

/// Owns one coupled VkImage/VmaAllocation; the native allocator is borrowed.
/// Destroy views and complete GPU use before destruction or displacement.
/// All access requires caller-owned synchronization and live parents.
class Image final {
public:
  Image() = delete;
  Image(const Image &) = delete;
  Image &operator=(const Image &) = delete;
  Image(Image &&) noexcept;
  Image &operator=(Image &&) noexcept;
  ~Image();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::Image nativeHandle() const noexcept;
  [[nodiscard]] VkImage nativeImage() const noexcept;
  [[nodiscard]] VmaAllocator nativeAllocator() const noexcept;
  [[nodiscard]] VmaAllocation nativeAllocation() const noexcept;
  /// Borrowed parent identity for ImageView parent compatibility checks.
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  [[nodiscard]] const ImageConfiguration *configuration() const noexcept;
  [[nodiscard]] terreate::Result<ImageAllocationObservation> allocationInfo() const;

private:
  struct Impl;
  explicit Image(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createImage(const Allocator &, const ImageDescription &) -> terreate::Result<Image>;
};

[[nodiscard]] auto createImage(const Allocator &allocator, const ImageDescription &description)
    -> terreate::Result<Image>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::ImageError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_IMAGE_HPP

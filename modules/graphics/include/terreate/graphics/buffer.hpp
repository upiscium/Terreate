#ifndef TERREATE_GRAPHICS_BUFFER_HPP
#define TERREATE_GRAPHICS_BUFFER_HPP

#include <terreate/graphics/allocator.hpp>

namespace terreate::graphics {

/// Value configuration, not a borrowed VmaAllocationCreateInfo. No pool or user data.
struct BufferAllocationDescription {
  VmaMemoryUsage usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationCreateFlags flags = 0;
  vk::MemoryPropertyFlags required_flags{};
  vk::MemoryPropertyFlags preferred_flags{};
  std::uint32_t memory_type_bits = 0;
};

/// Exclusive queue-family sharing; no implicit usage bits or host access.
struct BufferDescription {
  vk::DeviceSize size = 0;
  vk::BufferUsageFlags usage{};
  BufferAllocationDescription allocation{};
};

struct BufferConfiguration {
  BufferDescription requested{};
  vk::DeviceSize size = 0;
  vk::BufferUsageFlags usage{};
  BufferAllocationDescription allocation{};
};

/// Allocation size is not the logical buffer size. Memory and offset are borrowed.
struct BufferAllocationObservation {
  VmaAllocationInfo native{};
  vk::MemoryPropertyFlags memory_properties{};
};

enum class BufferError : std::uint8_t {
  invalid_parent = 1,
  invalid_description,
  unsupported_configuration,
  invalid_buffer,
  not_host_visible,
  already_mapped,
  not_mapped,
  invalid_range,
};

[[nodiscard]] const std::error_category &buffer_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(BufferError error) noexcept;

/// Owns a coupled VkBuffer/VmaAllocation, borrowing the native Allocator.
/// Complete GPU use before destruction or displacement by move assignment.
/// All operations and native access require caller-owned host synchronization.
class Buffer final {
public:
  Buffer() = delete;
  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;
  Buffer(Buffer &&) noexcept;
  Buffer &operator=(Buffer &&) noexcept;
  ~Buffer();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::Buffer nativeHandle() const noexcept;
  [[nodiscard]] VkBuffer nativeBuffer() const noexcept;
  [[nodiscard]] VmaAllocator nativeAllocator() const noexcept;
  [[nodiscard]] VmaAllocation nativeAllocation() const noexcept;
  [[nodiscard]] const BufferConfiguration *configuration() const noexcept;
  [[nodiscard]] terreate::Result<BufferAllocationObservation> allocationInfo() const;
  [[nodiscard]] bool mapped() const noexcept;
  [[nodiscard]] void *mappedData() const noexcept;

  /// Maps the whole allocation; caller may access only the logical buffer range.
  /// One outstanding map per wrapper; no automatic flush/invalidate or copying.
  [[nodiscard]] terreate::Result<void *> map();
  [[nodiscard]] terreate::Result<void> unmap();
  /// Buffer-relative ranges. VK_WHOLE_SIZE means [offset, logical size).
  /// Requires a live wrapper mapping even for coherent memory.
  [[nodiscard]] terreate::Result<void> flush(vk::DeviceSize offset = 0,
                                             vk::DeviceSize size = VK_WHOLE_SIZE);
  [[nodiscard]] terreate::Result<void> invalidate(vk::DeviceSize offset = 0,
                                                  vk::DeviceSize size = VK_WHOLE_SIZE);

private:
  struct Impl;
  explicit Buffer(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> implementation_;
  [[nodiscard]] terreate::Result<void> synchronize(bool flush, vk::DeviceSize offset,
                                                   vk::DeviceSize size);
  friend auto createBuffer(const Allocator &, const BufferDescription &)
      -> terreate::Result<Buffer>;
};

[[nodiscard]] auto createBuffer(const Allocator &allocator, const BufferDescription &description)
    -> terreate::Result<Buffer>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::BufferError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_BUFFER_HPP

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

/// What the application means to do with a Buffer's memory, so that it does not have to know the
/// VMA flags that say it. `bufferAllocationFor` turns an intent into an ordinary
/// `BufferAllocationDescription`.
enum class BufferMemoryIntent : std::uint8_t {
  /// The GPU reads and writes it and the host does not map it (vertex, index and storage buffers,
  /// filled by an upload batch or the GPU). It does not promise that the host cannot see the
  /// memory:
  /// an integrated GPU may make it host visible.
  device_local,
  /// The host writes it in order and the GPU reads it (a staging buffer, a per-frame uniform
  /// buffer).
  host_sequential_write,
  /// The host reads it back, or writes it at random (a readback buffer).
  host_random_access,
};

/// The allocation description for an intent. A plain value: every raw VMA field stays available, so
/// take the preset and edit it when more is needed (a dedicated allocation, a preferred property, a
/// memory type mask). The intent is a request, not a promise: the effective memory is observable
/// through `Buffer::allocationInfo()`, and a device without a suitable memory type refuses the
/// creation.
///   - `device_local`: VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, no host access flag.
///   - `host_sequential_write`: AUTO, HOST_ACCESS_SEQUENTIAL_WRITE_BIT, required HostVisible.
///   - `host_random_access`: AUTO, HOST_ACCESS_RANDOM_BIT, required HostVisible.
[[nodiscard]] inline BufferAllocationDescription
bufferAllocationFor(BufferMemoryIntent intent) noexcept {
  BufferAllocationDescription value{};
  switch (intent) {
  case BufferMemoryIntent::device_local:
    value.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    break;
  case BufferMemoryIntent::host_sequential_write:
    value.usage = VMA_MEMORY_USAGE_AUTO;
    value.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    value.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
    break;
  case BufferMemoryIntent::host_random_access:
    value.usage = VMA_MEMORY_USAGE_AUTO;
    value.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    value.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
    break;
  }
  return value;
}

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
  /// Borrowed native logical Device identity captured from the allocating VMA.
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  /// Compares native Device identities, never parent lifetime or wrapper addresses.
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
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

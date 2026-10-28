#include <terreate/graphics/buffer.hpp>

#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
class BufferErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.buffer"; }
  std::string message(int value) const override {
    switch (static_cast<BufferError>(value)) {
    case BufferError::invalid_parent:
      return "invalid buffer allocator";
    case BufferError::invalid_description:
      return "invalid buffer description";
    case BufferError::unsupported_configuration:
      return "unsupported buffer configuration";
    case BufferError::invalid_buffer:
      return "invalid buffer";
    case BufferError::not_host_visible:
      return "buffer memory is not host visible";
    case BufferError::already_mapped:
      return "buffer is already mapped";
    case BufferError::not_mapped:
      return "buffer is not mapped";
    case BufferError::invalid_range:
      return "invalid buffer range";
    }
    return "unknown buffer error";
  }
};

auto failure(BufferError error, const char *operation, const char *detail) {
  return std::unexpected(terreate::Error{make_error_code(error), operation, detail});
}
auto nativeFailure(VkResult result, const char *operation) {
  return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                         operation, "native buffer operation failed"});
}
} // namespace

const std::error_category &buffer_error_category() noexcept {
  static const BufferErrorCategory category;
  return category;
}
std::error_code make_error_code(BufferError error) noexcept {
  return {static_cast<int>(error), buffer_error_category()};
}

struct Buffer::Impl {
  VmaAllocator allocator = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  BufferConfiguration configuration{};
  void *mapping = nullptr;
  ~Impl() {
    if (allocation != VK_NULL_HANDLE) {
      if (mapping)
        vmaUnmapMemory(allocator, allocation);
      vmaDestroyBuffer(allocator, buffer, allocation);
    }
  }
};

Buffer::Buffer(std::unique_ptr<Impl> impl) noexcept : implementation_(std::move(impl)) {}
Buffer::Buffer(Buffer &&) noexcept = default;
Buffer &Buffer::operator=(Buffer &&) noexcept = default;
Buffer::~Buffer() = default;
bool Buffer::valid() const noexcept {
  return nativeBuffer() != VK_NULL_HANDLE && nativeAllocation() != VK_NULL_HANDLE;
}
vk::Buffer Buffer::nativeHandle() const noexcept { return vk::Buffer{nativeBuffer()}; }
VkBuffer Buffer::nativeBuffer() const noexcept {
  return implementation_ ? implementation_->buffer : VK_NULL_HANDLE;
}
VmaAllocator Buffer::nativeAllocator() const noexcept {
  return implementation_ ? implementation_->allocator : VK_NULL_HANDLE;
}
VmaAllocation Buffer::nativeAllocation() const noexcept {
  return implementation_ ? implementation_->allocation : VK_NULL_HANDLE;
}
const BufferConfiguration *Buffer::configuration() const noexcept {
  return implementation_ ? &implementation_->configuration : nullptr;
}
terreate::Result<BufferAllocationObservation> Buffer::allocationInfo() const {
  if (!valid())
    return failure(BufferError::invalid_buffer, "allocationInfo", "moved-from buffer");
  BufferAllocationObservation observation{};
  vmaGetAllocationInfo(nativeAllocator(), nativeAllocation(), &observation.native);
  VkMemoryPropertyFlags properties = 0;
  vmaGetAllocationMemoryProperties(nativeAllocator(), nativeAllocation(), &properties);
  observation.memory_properties = vk::MemoryPropertyFlags{properties};
  return observation;
}
bool Buffer::mapped() const noexcept { return mappedData() != nullptr; }
void *Buffer::mappedData() const noexcept {
  return implementation_ ? implementation_->mapping : nullptr;
}
terreate::Result<void *> Buffer::map() {
  if (!valid())
    return failure(BufferError::invalid_buffer, "map", "moved-from buffer");
  if (mapped())
    return failure(BufferError::already_mapped, "map", "unmap before mapping again");
  const auto observation = allocationInfo();
  if (!observation)
    return std::unexpected(observation.error());
  if (!(observation->memory_properties & vk::MemoryPropertyFlagBits::eHostVisible))
    return failure(BufferError::not_host_visible, "map", "allocation requires HOST_VISIBLE");
  const auto &allocation = configuration()->allocation;
  if ((allocation.usage == VMA_MEMORY_USAGE_AUTO ||
       allocation.usage == VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE ||
       allocation.usage == VMA_MEMORY_USAGE_AUTO_PREFER_HOST) &&
      !(allocation.flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT)))
    return failure(BufferError::unsupported_configuration, "map",
                   "AUTO requires host access flags");
  void *data = nullptr;
  const auto result = vmaMapMemory(nativeAllocator(), nativeAllocation(), &data);
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vmaMapMemory");
  implementation_->mapping = data;
  return data;
}
terreate::Result<void> Buffer::unmap() {
  if (!valid())
    return failure(BufferError::invalid_buffer, "unmap", "moved-from buffer");
  if (!mapped())
    return failure(BufferError::not_mapped, "unmap", "no outstanding map");
  vmaUnmapMemory(nativeAllocator(), nativeAllocation());
  implementation_->mapping = nullptr;
  return {};
}
terreate::Result<void> Buffer::synchronize(bool flush, vk::DeviceSize offset, vk::DeviceSize size) {
  const char *operation = flush ? "vmaFlushAllocation" : "vmaInvalidateAllocation";
  if (!valid())
    return failure(BufferError::invalid_buffer, operation, "moved-from buffer");
  if (!mapped())
    return failure(BufferError::not_mapped, operation, "map before cache maintenance");
  const auto logical_size = configuration()->size;
  if (offset >= logical_size)
    return failure(BufferError::invalid_range, operation, "offset outside logical buffer");
  if (size == VK_WHOLE_SIZE)
    size = logical_size - offset;
  if (size == 0 || size > logical_size - offset)
    return failure(BufferError::invalid_range, operation, "empty or overflowing buffer range");
  const VkResult result =
      flush ? vmaFlushAllocation(nativeAllocator(), nativeAllocation(), offset, size)
            : vmaInvalidateAllocation(nativeAllocator(), nativeAllocation(), offset, size);
  if (result != VK_SUCCESS)
    return nativeFailure(result, operation);
  return {};
}
terreate::Result<void> Buffer::flush(vk::DeviceSize offset, vk::DeviceSize size) {
  return synchronize(true, offset, size);
}
terreate::Result<void> Buffer::invalidate(vk::DeviceSize offset, vk::DeviceSize size) {
  return synchronize(false, offset, size);
}

auto createBuffer(const Allocator &allocator, const BufferDescription &description)
    -> terreate::Result<Buffer> {
  if (!allocator.valid())
    return failure(BufferError::invalid_parent, "createBuffer", "live borrowed Allocator required");
  if (description.size == 0 || !description.usage)
    return failure(BufferError::invalid_description, "createBuffer",
                   "nonzero size and usage required");
  constexpr VkBufferUsageFlags supported_usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  const auto usage = static_cast<VkBufferUsageFlags>(description.usage);
  if ((usage & ~supported_usage) ||
      ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) &&
       !(allocator.effectiveFlags() & VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT)))
    return failure(BufferError::unsupported_configuration, "createBuffer",
                   "usage prerequisites unavailable");
  const auto &allocation = description.allocation;
  constexpr VmaAllocationCreateFlags supported_flags =
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT | VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT |
      VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT |
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT |
      VMA_ALLOCATION_CREATE_STRATEGY_MIN_TIME_BIT | VMA_ALLOCATION_CREATE_STRATEGY_MIN_OFFSET_BIT;
  const auto flags = allocation.flags;
  const auto strategies = flags & VMA_ALLOCATION_CREATE_STRATEGY_MASK;
  if ((flags & ~supported_flags) ||
      ((flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT) &&
       (flags & VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT)) ||
      ((flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) &&
       (flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT)) ||
      (strategies && (strategies & (strategies - 1))))
    return failure(BufferError::unsupported_configuration, "createBuffer",
                   "unsupported or conflicting allocation flags");
  switch (allocation.usage) {
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
    return failure(BufferError::unsupported_configuration, "createBuffer",
                   "unsupported allocation usage");
  }
  constexpr VkMemoryPropertyFlags supported_properties =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  if ((static_cast<VkMemoryPropertyFlags>(allocation.required_flags) |
       static_cast<VkMemoryPropertyFlags>(allocation.preferred_flags)) &
      ~supported_properties)
    return failure(BufferError::unsupported_configuration, "createBuffer",
                   "unsupported memory properties");
  auto impl = std::make_unique<Buffer::Impl>();
  impl->allocator = allocator.nativeHandle();
  impl->configuration = {description, description.size, description.usage, allocation};
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = description.size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo allocation_info{};
  allocation_info.usage = allocation.usage;
  allocation_info.flags = flags;
  allocation_info.requiredFlags = static_cast<VkMemoryPropertyFlags>(allocation.required_flags);
  allocation_info.preferredFlags = static_cast<VkMemoryPropertyFlags>(allocation.preferred_flags);
  allocation_info.memoryTypeBits = allocation.memory_type_bits;
  // Publish the coupled owner only after success; VMA owns failed-call cleanup.
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation native_allocation = VK_NULL_HANDLE;
  const auto result = vmaCreateBuffer(impl->allocator, &buffer_info, &allocation_info, &buffer,
                                      &native_allocation, nullptr);
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vmaCreateBuffer");
  impl->buffer = buffer;
  impl->allocation = native_allocation;
  return Buffer{std::move(impl)};
}

} // namespace terreate::graphics

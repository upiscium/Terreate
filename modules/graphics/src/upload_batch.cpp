#include <terreate/graphics/upload_batch.hpp>

#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <terreate/graphics/transfer_commands.hpp>
#include <utility>

namespace terreate::graphics {
namespace {
class UploadErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.upload"; }
  std::string message(int value) const override {
    switch (static_cast<UploadError>(value)) {
    case UploadError::invalid_allocator:
      return "invalid VMA Allocator";
    case UploadError::empty_batch:
      return "upload batch contains no Buffer writes";
    case UploadError::invalid_destination:
      return "invalid or displaced destination Buffer";
    case UploadError::invalid_payload:
      return "upload data is empty or invalid";
    case UploadError::missing_usage:
      return "destination Buffer lacks TRANSFER_DST usage";
    case UploadError::invalid_range:
      return "Buffer upload offset, size or total is out of bounds";
    case UploadError::unaligned_copy:
      return "Buffer copy offsets and sizes must be 4-byte aligned";
    case UploadError::allocator_mismatch:
      return "Buffer and staging Allocator mismatch";
    case UploadError::overlapping_writes:
      return "overlapping writes to the same Buffer";
    case UploadError::allocation_failed:
      return "host allocation failed";
    case UploadError::invalid_batch:
      return "prepared upload batch is invalid";
    case UploadError::invalid_device:
      return "invalid logical Device";
    case UploadError::invalid_command_buffer:
      return "invalid CommandBuffer";
    case UploadError::not_recording:
      return "CommandBuffer is not recording";
    case UploadError::parent_mismatch:
      return "CommandBuffer's parent Device differs";
    case UploadError::protected_recording:
      return "protected command recording is unsupported";
    }
    return "unknown upload error";
  }
};

auto fail(UploadError reason, const char *operation) {
  return std::unexpected(terreate::Error{make_error_code(reason), operation,
                                         "upload rejected before recording GPU work"});
}

bool within(vk::DeviceSize total, vk::DeviceSize offset, vk::DeviceSize size) {
  return size != 0 && offset <= total && size <= total - offset;
}

bool overlaps(vk::DeviceSize a, vk::DeviceSize a_size, vk::DeviceSize b, vk::DeviceSize b_size) {
  // All inputs have been proven non-overflowing by within() beforehand.
  return a < b + b_size && b < a + a_size;
}

bool validDestination(const Buffer *buffer, VmaAllocator allocator) {
  return buffer && buffer->valid() && buffer->configuration() &&
         buffer->nativeBuffer() != VK_NULL_HANDLE && buffer->nativeAllocator() == allocator &&
         static_cast<bool>(buffer->configuration()->usage & vk::BufferUsageFlagBits::eTransferDst);
}
} // namespace

const std::error_category &upload_error_category() noexcept {
  static const UploadErrorCategory category;
  return category;
}
std::error_code make_error_code(UploadError error) noexcept {
  return {static_cast<int>(error), upload_error_category()};
}

terreate::Result<PreparedUploadBatch>
prepareUploadBatch(const Allocator &allocator, const UploadBatchDescription &description) {
  if (!allocator.valid() || !allocator.nativeHandle())
    return fail(UploadError::invalid_allocator, "prepareUploadBatch");
  if (description.buffers.empty())
    return fail(UploadError::empty_batch, "prepareUploadBatch");

  std::vector<PreparedBufferUpload> copies;
  vk::DeviceSize staging_bytes = 0;
  try {
    if (description.buffers.size() > copies.max_size())
      return fail(UploadError::allocation_failed, "prepareUploadBatch");
    copies.reserve(description.buffers.size());

    for (const auto &write : description.buffers) {
      if (!write.destination || !write.destination->valid() ||
          !write.destination->configuration() ||
          write.destination->nativeBuffer() == VK_NULL_HANDLE)
        return fail(UploadError::invalid_destination, "prepareUploadBatch");
      if (!(write.destination->configuration()->usage & vk::BufferUsageFlagBits::eTransferDst))
        return fail(UploadError::missing_usage, "prepareUploadBatch");
      if (write.destination->nativeAllocator() != allocator.nativeHandle())
        return fail(UploadError::allocator_mismatch, "prepareUploadBatch");

      if (write.bytes.empty() || !write.bytes.data())
        return fail(UploadError::invalid_payload, "prepareUploadBatch");
      if (write.bytes.size() > std::numeric_limits<vk::DeviceSize>::max())
        return fail(UploadError::invalid_range, "prepareUploadBatch");
      const auto size = static_cast<vk::DeviceSize>(write.bytes.size());
      if ((size % 4) != 0 || (write.destination_offset % 4) != 0)
        return fail(UploadError::unaligned_copy, "prepareUploadBatch");
      if (!within(write.destination->configuration()->size, write.destination_offset, size))
        return fail(UploadError::invalid_range, "prepareUploadBatch");
      if (size > std::numeric_limits<vk::DeviceSize>::max() - staging_bytes ||
          size > std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(staging_bytes))
        return fail(UploadError::invalid_range, "prepareUploadBatch");

      for (const auto &previous : copies) {
        if (previous.destination->nativeBuffer() == write.destination->nativeBuffer() &&
            overlaps(previous.destination_offset, previous.size, write.destination_offset, size))
          return fail(UploadError::overlapping_writes, "prepareUploadBatch");
      }

      copies.push_back(PreparedBufferUpload{.destination = write.destination,
                                            .staging_offset = staging_bytes,
                                            .destination_offset = write.destination_offset,
                                            .size = size});
      staging_bytes += size;
    }
  } catch (const std::bad_alloc &) {
    return fail(UploadError::allocation_failed, "prepareUploadBatch");
  }

  BufferDescription staging_desc;
  staging_desc.size = staging_bytes;
  staging_desc.usage = vk::BufferUsageFlagBits::eTransferSrc;
  staging_desc.allocation = description.staging_allocation;
  auto buffer = createBuffer(allocator, staging_desc);
  if (!buffer)
    return std::unexpected(buffer.error());

  auto mapped = buffer->map();
  if (!mapped)
    return std::unexpected(mapped.error());
  auto *destination = static_cast<std::byte *>(*mapped);
  for (std::size_t i = 0; i < description.buffers.size(); ++i) {
    const auto &write = description.buffers[i];
    // Total byte span was checked against size_t and VkDeviceSize bounds.
    std::memcpy(destination + static_cast<std::size_t>(copies[i].staging_offset),
                write.bytes.data(), write.bytes.size());
  }
  if (auto flushed = buffer->flush(); !flushed)
    return std::unexpected(flushed.error());
  if (auto unmapped = buffer->unmap(); !unmapped)
    return std::unexpected(unmapped.error());
  return PreparedUploadBatch{std::move(*buffer), std::move(copies), description.staging_allocation};
}

terreate::Result<void> recordUploadBatch(const Device &device, const CommandBuffer &command,
                                         const PreparedUploadBatch &batch) {
  if (!batch.valid())
    return fail(UploadError::invalid_batch, "recordUploadBatch");
  if (!device.valid() || !device.plan())
    return fail(UploadError::invalid_device, "recordUploadBatch");
  if (!command.valid())
    return fail(UploadError::invalid_command_buffer, "recordUploadBatch");
  if (command.nativeDevice() != device.nativeHandle())
    return fail(UploadError::parent_mismatch, "recordUploadBatch");
  if (command.state() != CommandBufferState::recording)
    return fail(UploadError::not_recording, "recordUploadBatch");
  if (command.isProtected())
    return fail(UploadError::protected_recording, "recordUploadBatch");

  const auto *staging = batch.stagingBuffer();
  if (!staging || !staging->valid() || !staging->configuration() ||
      staging->nativeBuffer() == VK_NULL_HANDLE ||
      !(staging->configuration()->usage & vk::BufferUsageFlagBits::eTransferSrc))
    return fail(UploadError::invalid_batch, "recordUploadBatch");

  // Prevalidate all entries before recording any vkCmd*, so no predictable
  // input error causes a partially recorded command sequence.
  const auto copies = batch.copies();
  for (const auto &copy : copies) {
    if (!copy.destination || !copy.destination->valid() || !copy.destination->configuration() ||
        copy.destination->nativeBuffer() == VK_NULL_HANDLE)
      return fail(UploadError::invalid_destination, "recordUploadBatch");
    if (copy.destination->nativeAllocator() != staging->nativeAllocator())
      return fail(UploadError::allocator_mismatch, "recordUploadBatch");
    if (!(copy.destination->configuration()->usage & vk::BufferUsageFlagBits::eTransferDst))
      return fail(UploadError::missing_usage, "recordUploadBatch");
    if ((copy.staging_offset % 4) != 0 || (copy.destination_offset % 4) != 0 ||
        (copy.size % 4) != 0)
      return fail(UploadError::unaligned_copy, "recordUploadBatch");
    if (!within(staging->configuration()->size, copy.staging_offset, copy.size) ||
        !within(copy.destination->configuration()->size, copy.destination_offset, copy.size))
      return fail(UploadError::invalid_range, "recordUploadBatch");
  }
  for (std::size_t i = 0; i < copies.size(); ++i) {
    for (std::size_t j = i + 1; j < copies.size(); ++j) {
      if (copies[i].destination->nativeBuffer() == copies[j].destination->nativeBuffer() &&
          overlaps(copies[i].destination_offset, copies[i].size, copies[j].destination_offset,
                   copies[j].size))
        return fail(UploadError::overlapping_writes, "recordUploadBatch");
    }
  }
  for (const auto &copy : copies) {
    const VkBufferCopy2 native{VK_STRUCTURE_TYPE_BUFFER_COPY_2, nullptr, copy.staging_offset,
                               copy.destination_offset, copy.size};
    if (auto recorded = recordCopyBuffer(device, command, *staging, *copy.destination,
                                         std::span{&native, std::size_t{1}});
        !recorded)
      return recorded;
  }
  return {};
}
} // namespace terreate::graphics

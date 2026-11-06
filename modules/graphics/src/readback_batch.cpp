#include <terreate/graphics/readback_batch.hpp>

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <terreate/graphics/transfer_commands.hpp>
#include <utility>

namespace terreate::graphics {
namespace {
class ReadbackErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.readback"; }
  std::string message(int value) const override {
    switch (static_cast<ReadbackError>(value)) {
    case ReadbackError::invalid_allocator:
      return "invalid VMA Allocator";
    case ReadbackError::empty_batch:
      return "readback batch contains no Buffer ranges";
    case ReadbackError::invalid_source:
      return "invalid or displaced Buffer readback source";
    case ReadbackError::missing_usage:
      return "readback source lacks TRANSFER_SRC usage";
    case ReadbackError::invalid_range:
      return "readback source range or total staging size is invalid";
    case ReadbackError::allocator_mismatch:
      return "readback source and staging Allocator differ";
    case ReadbackError::allocation_failed:
      return "CPU memory allocation failed";
    case ReadbackError::invalid_batch:
      return "readback batch is invalid or moved-from";
    case ReadbackError::invalid_device:
      return "invalid logical Device";
    case ReadbackError::invalid_command_buffer:
      return "invalid CommandBuffer";
    case ReadbackError::not_recording:
      return "CommandBuffer is not recording";
    case ReadbackError::parent_mismatch:
      return "CommandBuffer parent Device differs";
    case ReadbackError::protected_recording:
      return "protected recording is unsupported";
    case ReadbackError::missing_completion:
      return "explicit real SubmissionReceipt(s) required";
    case ReadbackError::invalid_completion:
      return "invalid or cross-Device submission completion";
    case ReadbackError::completion_pending:
      return "GPU completion is pending; no host data exposed";
    case ReadbackError::unsupported_host_access:
      return "staging allocation must permit HOST_VISIBLE CPU read access";
    }
    return "unknown readback error";
  }
};

auto fail(ReadbackError reason, const char *operation) {
  return std::unexpected(
      terreate::Error{make_error_code(reason), operation, "readback failed closed"});
}
bool within(vk::DeviceSize total, vk::DeviceSize offset, vk::DeviceSize size) {
  return size != 0 && offset <= total && size <= total - offset;
}
bool hostAccessible(const BufferAllocationDescription &description) {
  switch (description.usage) {
  case VMA_MEMORY_USAGE_AUTO:
  case VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE:
  case VMA_MEMORY_USAGE_AUTO_PREFER_HOST:
    return (description.flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)) != 0;
  default:
    return true;
  }
}
} // namespace

const std::error_category &readback_error_category() noexcept {
  static const ReadbackErrorCategory category;
  return category;
}
std::error_code make_error_code(ReadbackError reason) noexcept {
  return {static_cast<int>(reason), readback_error_category()};
}

terreate::Result<BufferAllocationObservation> PreparedReadbackBatch::stagingAllocationInfo() const {
  if (!staging_ || !valid())
    return fail(ReadbackError::invalid_batch, "stagingAllocationInfo");
  return staging_->allocationInfo();
}

terreate::Result<PreparedReadbackBatch>
prepareReadbackBatch(const Allocator &allocator, const ReadbackBatchDescription &description) {
  if (!allocator.valid() || !allocator.nativeHandle())
    return fail(ReadbackError::invalid_allocator, "prepareReadbackBatch");
  if (description.buffers.empty())
    return fail(ReadbackError::empty_batch, "prepareReadbackBatch");
  if (!hostAccessible(description.staging_allocation))
    return fail(ReadbackError::unsupported_host_access, "prepareReadbackBatch");

  std::vector<PreparedBufferReadback> regions;
  vk::DeviceSize total = 0;
  try {
    if (description.buffers.size() > regions.max_size())
      return fail(ReadbackError::allocation_failed, "prepareReadbackBatch");
    regions.reserve(description.buffers.size());
    for (const auto &read : description.buffers) {
      if (!read.source || !read.source->valid() || !read.source->configuration() ||
          read.source->nativeBuffer() == VK_NULL_HANDLE)
        return fail(ReadbackError::invalid_source, "prepareReadbackBatch");
      if (read.source->nativeAllocator() != allocator.nativeHandle())
        return fail(ReadbackError::allocator_mismatch, "prepareReadbackBatch");
      if (!(read.source->configuration()->usage & vk::BufferUsageFlagBits::eTransferSrc))
        return fail(ReadbackError::missing_usage, "prepareReadbackBatch");
      if (!within(read.source->configuration()->size, read.source_offset, read.size))
        return fail(ReadbackError::invalid_range, "prepareReadbackBatch");
      if (read.size > std::numeric_limits<vk::DeviceSize>::max() - total ||
          read.size > std::numeric_limits<std::size_t>::max() ||
          total > std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(read.size))
        return fail(ReadbackError::invalid_range, "prepareReadbackBatch");
      regions.push_back(PreparedBufferReadback{read.source, read.source->nativeBuffer(),
                                               read.source_offset, total, read.size});
      total += read.size;
    }
  } catch (const std::bad_alloc &) {
    return fail(ReadbackError::allocation_failed, "prepareReadbackBatch");
  } catch (const std::length_error &) {
    return fail(ReadbackError::allocation_failed, "prepareReadbackBatch");
  }

  BufferDescription staging_description;
  staging_description.size = total;
  staging_description.usage = vk::BufferUsageFlagBits::eTransferDst;
  staging_description.allocation = description.staging_allocation;
  auto staging = createBuffer(allocator, staging_description);
  if (!staging)
    return std::unexpected(staging.error());
  auto info = staging->allocationInfo();
  if (!info)
    return std::unexpected(info.error());
  if (!(info->memory_properties & vk::MemoryPropertyFlagBits::eHostVisible))
    return fail(ReadbackError::unsupported_host_access, "prepareReadbackBatch");

  return PreparedReadbackBatch{std::move(*staging), total, std::move(regions),
                               description.staging_allocation};
}

terreate::Result<void> recordReadbackBatch(const Device &device, const CommandBuffer &command,
                                           const PreparedReadbackBatch &batch) {
  if (!batch.valid())
    return fail(ReadbackError::invalid_batch, "recordReadbackBatch");
  if (!device.valid() || !device.plan())
    return fail(ReadbackError::invalid_device, "recordReadbackBatch");
  if (!command.valid())
    return fail(ReadbackError::invalid_command_buffer, "recordReadbackBatch");
  if (command.nativeDevice() != device.nativeHandle())
    return fail(ReadbackError::parent_mismatch, "recordReadbackBatch");
  if (command.state() != CommandBufferState::recording)
    return fail(ReadbackError::not_recording, "recordReadbackBatch");
  if (command.isProtected())
    return fail(ReadbackError::protected_recording, "recordReadbackBatch");

  if (!batch.staging_)
    return fail(ReadbackError::invalid_batch, "recordReadbackBatch");
  const auto &staging = *batch.staging_;
  if (!staging.valid() || !staging.configuration() || staging.nativeBuffer() == VK_NULL_HANDLE ||
      !(staging.configuration()->usage & vk::BufferUsageFlagBits::eTransferDst) ||
      staging.configuration()->size != batch.size_)
    return fail(ReadbackError::invalid_batch, "recordReadbackBatch");

  // Prevalidate the ENTIRE batch before any native vkCmdCopyBuffer2.
  for (const auto &region : batch.copies_) {
    if (!region.source || !region.source->valid() || !region.source->configuration() ||
        region.source->nativeBuffer() == VK_NULL_HANDLE ||
        region.source->nativeBuffer() != region.expected_source)
      return fail(ReadbackError::invalid_source, "recordReadbackBatch");
    if (region.source->nativeAllocator() != staging.nativeAllocator())
      return fail(ReadbackError::allocator_mismatch, "recordReadbackBatch");
    if (!(region.source->configuration()->usage & vk::BufferUsageFlagBits::eTransferSrc))
      return fail(ReadbackError::missing_usage, "recordReadbackBatch");
    if (!within(region.source->configuration()->size, region.source_offset, region.size) ||
        !within(staging.configuration()->size, region.staging_offset, region.size))
      return fail(ReadbackError::invalid_range, "recordReadbackBatch");
  }

  // All source buffers must in fact belong to device; Buffer has no nativeDevice()
  // observer, so this remains a documented caller obligation.
  for (const auto &region : batch.copies_) {
    const VkBufferCopy2 copy{VK_STRUCTURE_TYPE_BUFFER_COPY_2, nullptr, region.source_offset,
                             region.staging_offset, region.size};
    auto result = recordCopyBuffer(device, command, *region.source, staging, std::span{&copy, 1});
    if (!result)
      return result; // Native recording is not transactional; caller resets if needed.
  }
  return {};
}

terreate::Result<CompletedReadbackBatch>
tryCopyCompletedReadbackBatch(const Device &device, PreparedReadbackBatch &batch,
                              std::span<const SubmissionReceipt> all_uses) {
  if (!batch.valid())
    return fail(ReadbackError::invalid_batch, "tryCopyCompletedReadbackBatch");
  if (!device.valid())
    return fail(ReadbackError::invalid_device, "tryCopyCompletedReadbackBatch");
  if (all_uses.empty())
    return fail(ReadbackError::missing_completion, "tryCopyCompletedReadbackBatch");

  bool pending = false;
  for (const auto &receipt : all_uses) {
    const auto &completion = receipt.completion();
    if (completion.device != &device || !completion.queue.valid() ||
        !completion.queue.correlatedWith(device) || !completion.requested.valid() ||
        !completion.effective.valid())
      return fail(ReadbackError::invalid_completion, "tryCopyCompletedReadbackBatch");
    const auto result = receipt.poll();
    if (!result)
      return std::unexpected(result.error()); // Native error incl. DEVICE_LOST: fail closed.
    if (result->state != SubmissionCompletionState::completed)
      pending = true;
  }
  if (pending)
    return fail(ReadbackError::completion_pending, "tryCopyCompletedReadbackBatch");

  std::vector<std::byte> bytes;
  std::vector<CompletedBufferReadback> regions;
  try {
    bytes.resize(static_cast<std::size_t>(batch.size_));
    regions.reserve(batch.copies_.size());
    for (const auto &copy : batch.copies_)
      regions.push_back(CompletedBufferReadback{copy.source_offset,
                                                static_cast<std::size_t>(copy.staging_offset),
                                                static_cast<std::size_t>(copy.size)});
  } catch (const std::bad_alloc &) {
    return fail(ReadbackError::allocation_failed, "tryCopyCompletedReadbackBatch");
  } catch (const std::length_error &) {
    return fail(ReadbackError::allocation_failed, "tryCopyCompletedReadbackBatch");
  }

  if (!batch.staging_)
    return fail(ReadbackError::invalid_batch, "tryCopyCompletedReadbackBatch");
  auto &staging = *batch.staging_;
  auto mapped = staging.map();
  if (!mapped)
    return std::unexpected(mapped.error());
  const auto invalidated = staging.invalidate();
  if (!invalidated) {
    if (auto unmapped = staging.unmap(); !unmapped)
      return std::unexpected(unmapped.error());
    return std::unexpected(invalidated.error());
  }
  std::memcpy(bytes.data(), *mapped, bytes.size());
  if (auto unmapped = staging.unmap(); !unmapped)
    return std::unexpected(unmapped.error());
  return CompletedReadbackBatch{std::move(bytes), std::move(regions)};
}
} // namespace terreate::graphics

#ifndef TERREATE_GRAPHICS_UPLOAD_BATCH_HPP
#define TERREATE_GRAPHICS_UPLOAD_BATCH_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <utility>
#include <vector>

namespace terreate::graphics {

enum class UploadError : std::uint8_t {
  invalid_allocator = 1,
  empty_batch,
  invalid_destination,
  invalid_payload,
  missing_usage,
  invalid_range,
  unaligned_copy,
  allocator_mismatch,
  overlapping_writes,
  allocation_failed,
  invalid_batch,
  invalid_device,
  invalid_command_buffer,
  not_recording,
  parent_mismatch,
  protected_recording,
};

[[nodiscard]] const std::error_category &upload_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(UploadError error) noexcept;

struct BufferUploadItem {
  // Borrowed until recordUploadBatch returns. The caller owns all targets and
  // must keep them alive through every GPU use, including after submission.
  const Buffer *destination = nullptr;
  vk::DeviceSize destination_offset = 0;
  // Copied into staging during prepareUploadBatch; never retained as a span.
  std::span<const std::byte> bytes{};
};

[[nodiscard]] inline BufferAllocationDescription defaultUploadStagingAllocation() noexcept {
  BufferAllocationDescription result{};
  result.usage = VMA_MEMORY_USAGE_AUTO;
  result.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  result.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
  return result;
}

struct UploadBatchDescription {
  std::span<const BufferUploadItem> buffers{};
  // Caller-overridable; the chosen description and actual memory properties
  // are observable on PreparedUploadBatch and its staging Buffer.
  BufferAllocationDescription staging_allocation = defaultUploadStagingAllocation();
};

struct PreparedBufferUpload {
  const Buffer *destination = nullptr;
  vk::DeviceSize staging_offset = 0;
  vk::DeviceSize destination_offset = 0;
  vk::DeviceSize size = 0;
};

/// Move-only owner of the Staging Buffer plus snapshots of the copy commands.
/// Target resources and Allocator are only borrowed. A recorded command may
/// reference staging until ALL submitted GPU uses complete: destroying or
/// displacing this owner early is a caller contract violation, not tracked.
///
/// After a successful explicit submit(), move this owner into the caller's
/// RetirementQueue using the REAL SubmissionReceipt(s) for every GPU use.
/// If submission fails, keep staging alive and perform explicit recovery.
/// No implicit submit, wait, completion token, global resource tracker, or
/// destructor wait is implemented. Native device/parent lifetime is borrowed.
/// The move/destructor contract satisfies RetirementQueue::retire() requirements.
class PreparedUploadBatch final {
public:
  PreparedUploadBatch() = delete;
  PreparedUploadBatch(const PreparedUploadBatch &) = delete;
  PreparedUploadBatch &operator=(const PreparedUploadBatch &) = delete;
  PreparedUploadBatch(PreparedUploadBatch &&) noexcept = default;
  PreparedUploadBatch &operator=(PreparedUploadBatch &&) noexcept = default;
  ~PreparedUploadBatch() = default;

  [[nodiscard]] bool valid() const noexcept {
    return staging_ && staging_->valid() && !copies_.empty();
  }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] const Buffer *stagingBuffer() const noexcept {
    if (!staging_.has_value())
      return nullptr;
    const Buffer *buffer = &*staging_;
    return buffer->valid() && !copies_.empty() ? buffer : nullptr;
  }
  [[nodiscard]] std::span<const PreparedBufferUpload> copies() const noexcept {
    return valid() ? std::span<const PreparedBufferUpload>{copies_}
                   : std::span<const PreparedBufferUpload>{};
  }
  [[nodiscard]] const BufferAllocationDescription *requestedStagingAllocation() const noexcept {
    return valid() ? &requested_allocation_ : nullptr;
  }

private:
  PreparedUploadBatch(Buffer &&staging, std::vector<PreparedBufferUpload> &&copies,
                      BufferAllocationDescription requested) noexcept
      : staging_(std::in_place, std::move(staging)), copies_(std::move(copies)),
        requested_allocation_(requested) {}

  std::optional<Buffer> staging_{};
  std::vector<PreparedBufferUpload> copies_{};
  BufferAllocationDescription requested_allocation_{};

  friend auto prepareUploadBatch(const Allocator &, const UploadBatchDescription &)
      -> terreate::Result<PreparedUploadBatch>;
};

/// Validates all Buffer ranges/4-byte Vulkan copy alignment before allocating.
/// Copies every byte span into one host-visible VMA staging Buffer and performs
/// explicit map -> copy -> flush -> unmap; no input span is retained.
/// Native Buffer creation, mapping and flush failures propagate unchanged;
/// caller-owned targets are never modified and no GPU work is recorded.
[[nodiscard]] auto prepareUploadBatch(const Allocator &allocator,
                                      const UploadBatchDescription &description)
    -> terreate::Result<PreparedUploadBatch>;

/// Prevalidates the complete batch, then records one vkCmdCopyBuffer2 per item
/// through the #264 helper. No barriers, queue selection, submit or waiting.
/// All destination Buffer wrappers must remain stable between prepare/record.
/// Device ownership of Buffers is not observable through Buffer::nativeDevice:
/// the application must supply the correct Device and compatible Queue.
/// Native vkCmd* calls return void and command recording is NOT transactional.
[[nodiscard]] auto recordUploadBatch(const Device &device, const CommandBuffer &command,
                                     const PreparedUploadBatch &batch) -> terreate::Result<void>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::UploadError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_UPLOAD_BATCH_HPP

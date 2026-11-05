#ifndef TERREATE_GRAPHICS_UPLOAD_BATCH_HPP
#define TERREATE_GRAPHICS_UPLOAD_BATCH_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/image.hpp>
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
  unsupported_image,
  unsupported_layout,
  invalid_image_region,
  invalid_image_footprint,
  overlapping_image_regions,
  conflicting_image_layouts,
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

/// Bounded Phase B: R8G8B8A8_UNORM, 2D, sample-count 1, one layer per
/// operation, with explicit VkBufferImageCopy2 texel pitch and mip/extent.
/// The input region's bufferOffset must be zero: prepare assigns its own
/// staging offset. No image layout transition is invented or recorded.
/// Destination is borrowed and must survive recording + all GPU use.
struct ImageUploadItem {
  const Image *destination = nullptr;
  vk::ImageLayout copy_layout = vk::ImageLayout::eTransferDstOptimal;
  VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
  // Exact accessible footprint, including any inter-row padding; copied
  // during prepare and never retained.
  std::span<const std::byte> bytes{};
};

struct UploadBatchDescription {
  std::span<const BufferUploadItem> buffers{};
  // Caller-overridable; the chosen description and actual memory properties
  // are observable on PreparedUploadBatch and its staging Buffer.
  BufferAllocationDescription staging_allocation = defaultUploadStagingAllocation();
  // Appended to preserve existing Phase A positional aggregate initialization.
  std::span<const ImageUploadItem> images{};
};

struct PreparedBufferUpload {
  const Buffer *destination = nullptr;
  vk::DeviceSize staging_offset = 0;
  vk::DeviceSize destination_offset = 0;
  vk::DeviceSize size = 0;
};

struct PreparedImageUpload {
  const Image *destination = nullptr;
  // Detect accidental replacement of a borrowed Image wrapper after prepare.
  VkImage expected_image = VK_NULL_HANDLE;
  vk::ImageLayout copy_layout = vk::ImageLayout::eTransferDstOptimal;
  // Snapshotted native region; bufferOffset is the EFFECTIVE staging offset.
  VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
  vk::DeviceSize payload_bytes = 0;
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
    return staging_ && staging_->valid() && (!copies_.empty() || !images_.empty());
  }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] const Buffer *stagingBuffer() const noexcept {
    if (!staging_.has_value())
      return nullptr;
    const Buffer *buffer = &*staging_;
    return buffer->valid() && (!copies_.empty() || !images_.empty()) ? buffer : nullptr;
  }
  [[nodiscard]] std::span<const PreparedBufferUpload> copies() const noexcept {
    return valid() ? std::span<const PreparedBufferUpload>{copies_}
                   : std::span<const PreparedBufferUpload>{};
  }
  [[nodiscard]] std::span<const PreparedImageUpload> images() const noexcept {
    return valid() ? std::span<const PreparedImageUpload>{images_}
                   : std::span<const PreparedImageUpload>{};
  }
  [[nodiscard]] const BufferAllocationDescription *requestedStagingAllocation() const noexcept {
    return valid() ? &requested_allocation_ : nullptr;
  }

private:
  PreparedUploadBatch(Buffer &&staging, std::vector<PreparedBufferUpload> &&copies,
                      std::vector<PreparedImageUpload> &&images,
                      BufferAllocationDescription requested) noexcept
      : staging_(std::in_place, std::move(staging)), copies_(std::move(copies)),
        images_(std::move(images)), requested_allocation_(requested) {}

  std::optional<Buffer> staging_{};
  std::vector<PreparedBufferUpload> copies_{};
  std::vector<PreparedImageUpload> images_{};
  BufferAllocationDescription requested_allocation_{};

  friend auto prepareUploadBatch(const Allocator &, const UploadBatchDescription &)
      -> terreate::Result<PreparedUploadBatch>;
};

/// Validates Buffer and bounded RGBA8 Image regions and exact Image byte
/// footprints before allocating. Staging offsets are generated explicitly.
/// Copies every byte span into one host-visible VMA staging Buffer and performs
/// explicit map -> copy -> flush -> unmap; no input span is retained.
/// Native Buffer creation, mapping and flush failures propagate unchanged;
/// caller-owned targets are never modified and no GPU work is recorded.
[[nodiscard]] auto prepareUploadBatch(const Allocator &allocator,
                                      const UploadBatchDescription &description)
    -> terreate::Result<PreparedUploadBatch>;

/// Prevalidates the complete batch, then records one vkCmdCopyBuffer2 per
/// Buffer item and one vkCmdCopyBufferToImage2 per Image item through #264.
/// No layout transitions, barriers, queue selection, submit or waiting.
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

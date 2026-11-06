#ifndef TERREATE_GRAPHICS_READBACK_BATCH_HPP
#define TERREATE_GRAPHICS_READBACK_BATCH_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/submission.hpp>
#include <utility>
#include <vector>

namespace terreate::graphics {

enum class ReadbackError : std::uint8_t {
  invalid_allocator = 1,
  empty_batch,
  invalid_source,
  missing_usage,
  invalid_range,
  allocator_mismatch,
  allocation_failed,
  invalid_batch,
  invalid_device,
  invalid_command_buffer,
  not_recording,
  parent_mismatch,
  protected_recording,
  missing_completion,
  invalid_completion,
  completion_pending,
  unsupported_host_access,
};

[[nodiscard]] const std::error_category &readback_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(ReadbackError error) noexcept;

/// Buffer copy requests borrow the sources through recording and GPU execution.
/// Multiple/overlapping reads from one source are legal; each result is separate.
struct BufferReadbackItem {
  const Buffer *source = nullptr;
  vk::DeviceSize source_offset = 0;
  vk::DeviceSize size = 0;
};

[[nodiscard]] inline BufferAllocationDescription defaultReadbackStagingAllocation() noexcept {
  BufferAllocationDescription value{};
  value.usage = VMA_MEMORY_USAGE_AUTO;
  value.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  value.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
  return value;
}

struct ReadbackBatchDescription {
  std::span<const BufferReadbackItem> buffers{};
  BufferAllocationDescription staging_allocation = defaultReadbackStagingAllocation();
};

struct PreparedBufferReadback {
  const Buffer *source = nullptr; // borrowed; wrapper must remain stable until recording
  VkBuffer expected_source = VK_NULL_HANDLE;
  vk::DeviceSize source_offset = 0;
  vk::DeviceSize staging_offset = 0;
  vk::DeviceSize size = 0;
};

struct CompletedBufferReadback {
  vk::DeviceSize source_offset = 0;
  std::size_t data_offset = 0;
  std::size_t size = 0;
};

/// CPU-owned bytes, never a borrow of mapped staging memory.
class CompletedReadbackBatch final {
public:
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return data_; }
  [[nodiscard]] std::span<const CompletedBufferReadback> buffers() const noexcept {
    return buffers_;
  }
  [[nodiscard]] std::span<const std::byte> dataFor(std::size_t index) const noexcept {
    if (index >= buffers_.size())
      return {};
    const auto &region = buffers_[index];
    return {data_.data() + region.data_offset, region.size};
  }

private:
  CompletedReadbackBatch(std::vector<std::byte> &&data,
                         std::vector<CompletedBufferReadback> &&buffers) noexcept
      : data_(std::move(data)), buffers_(std::move(buffers)) {}
  std::vector<std::byte> data_;
  std::vector<CompletedBufferReadback> buffers_;
  friend auto tryCopyCompletedReadbackBatch(const Device &, class PreparedReadbackBatch &,
                                            std::span<const SubmissionReceipt>)
      -> terreate::Result<CompletedReadbackBatch>;
};

/// Exclusive owner of one TRANSFER_DST staging Buffer and snapshotted regions.
/// No staging map/pointer is exposed. Destruction before *all* GPU uses finish
/// violates the caller's lifetime contract. CPU result copying must happen
/// BEFORE releasing/retiring this owner. No destructor wait or implicit Submit.
class PreparedReadbackBatch final {
public:
  PreparedReadbackBatch() = delete;
  PreparedReadbackBatch(const PreparedReadbackBatch &) = delete;
  PreparedReadbackBatch &operator=(const PreparedReadbackBatch &) = delete;
  PreparedReadbackBatch(PreparedReadbackBatch &&) noexcept = default;
  PreparedReadbackBatch &operator=(PreparedReadbackBatch &&) noexcept = default;
  ~PreparedReadbackBatch() = default;

  [[nodiscard]] bool valid() const noexcept {
    return staging_ && staging_->valid() && !copies_.empty();
  }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::DeviceSize stagingSize() const noexcept { return valid() ? size_ : 0; }
  [[nodiscard]] std::span<const PreparedBufferReadback> buffers() const noexcept {
    return valid() ? std::span<const PreparedBufferReadback>{copies_}
                   : std::span<const PreparedBufferReadback>{};
  }
  [[nodiscard]] const BufferAllocationDescription *requestedStagingAllocation() const noexcept {
    return valid() ? &requested_allocation_ : nullptr;
  }
  [[nodiscard]] const BufferConfiguration *stagingConfiguration() const noexcept {
    if (!staging_)
      return nullptr;
    return staging_->valid() && !copies_.empty() ? staging_->configuration() : nullptr;
  }
  [[nodiscard]] terreate::Result<BufferAllocationObservation> stagingAllocationInfo() const;

private:
  PreparedReadbackBatch(Buffer &&staging, vk::DeviceSize size,
                        std::vector<PreparedBufferReadback> &&copies,
                        BufferAllocationDescription requested) noexcept
      : staging_(std::in_place, std::move(staging)), size_(size), copies_(std::move(copies)),
        requested_allocation_(requested) {}

  std::optional<Buffer> staging_{};
  vk::DeviceSize size_ = 0;
  std::vector<PreparedBufferReadback> copies_{};
  BufferAllocationDescription requested_allocation_{};

  friend auto prepareReadbackBatch(const Allocator &, const ReadbackBatchDescription &)
      -> terreate::Result<PreparedReadbackBatch>;
  friend auto recordReadbackBatch(const Device &, const CommandBuffer &,
                                  const PreparedReadbackBatch &) -> terreate::Result<void>;
  friend auto tryCopyCompletedReadbackBatch(const Device &, PreparedReadbackBatch &,
                                            std::span<const SubmissionReceipt>)
      -> terreate::Result<CompletedReadbackBatch>;
};

/// Prepare one packed host-visible VMA TRANSFER_DST buffer. No native GPU work,
/// memory map or CPU results. Request spans are not retained.
[[nodiscard]] auto prepareReadbackBatch(const Allocator &allocator,
                                        const ReadbackBatchDescription &description)
    -> terreate::Result<PreparedReadbackBatch>;

/// Record one vkCmdCopyBuffer2 per source range, via #264. Fully prevalidates
/// the batch before the first native command; no barriers/Submit/Queue/wait.
[[nodiscard]] auto recordReadbackBatch(const Device &device, const CommandBuffer &command,
                                       const PreparedReadbackBatch &batch)
    -> terreate::Result<void>;

/// Nonblocking: polls EVERY caller-declared genuine #258 receipt before any
/// map/invalidate/read. Requires caller-selected Device/receipts all agree.
/// The caller MUST provide receipts covering all GPU uses and record the
/// required GPU transfer-write -> HOST_READ dependency; provenance is not
/// inferred from receipts. No hidden wait/submit. Completion pending is an
/// explicit error, not a partial result. Copies into independent CPU storage;
/// caller still owns staging and decides when/how to release or retire it.
[[nodiscard]] auto tryCopyCompletedReadbackBatch(const Device &device, PreparedReadbackBatch &batch,
                                                 std::span<const SubmissionReceipt> all_uses)
    -> terreate::Result<CompletedReadbackBatch>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::ReadbackError> : true_type {};
} // namespace std
#endif // TERREATE_GRAPHICS_READBACK_BATCH_HPP

#include <terreate/graphics/readback_batch.hpp>

#include <algorithm>
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
      return "readback batch contains no Buffer or Image ranges";
    case ReadbackError::invalid_source:
      return "invalid or displaced readback source";
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
    case ReadbackError::unsupported_image:
      return "Image readback requires RGBA8_UNORM 2D single-sample color";
    case ReadbackError::unsupported_layout:
      return "explicit source copy layout must be TRANSFER_SRC_OPTIMAL or GENERAL";
    case ReadbackError::invalid_image_region:
      return "invalid Image region, aspect, mip/layer, pitch, or native structure";
    case ReadbackError::invalid_image_footprint:
      return "Image copy footprint exceeds supported buffer or CPU allocation";
    case ReadbackError::conflicting_image_layouts:
      return "same Image mip/layer cannot have conflicting declared copy layouts";
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

/// Bounded native path: RGBA8 color only, 2D single-sample.
bool supportedImage(const ImageConfiguration &cfg) noexcept {
  return cfg.format == vk::Format::eR8G8B8A8Unorm && cfg.dimensions == vk::ImageType::e2D &&
         cfg.samples == vk::SampleCountFlagBits::e1 && cfg.extent.depth == 1;
}
bool supportedLayout(vk::ImageLayout layout) noexcept {
  return layout == vk::ImageLayout::eTransferSrcOptimal || layout == vk::ImageLayout::eGeneral;
}
std::uint32_t mipDimension(std::uint32_t dimension, std::uint32_t level) noexcept {
  const auto value = dimension >> (level < 32 ? level : 31);
  return value ? value : 1U;
}
bool validImageRegion(const ImageConfiguration &cfg, const VkBufferImageCopy2 &r) noexcept {
  if (r.sType != VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 || r.pNext || r.bufferOffset % 4 != 0 ||
      r.imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
      r.imageSubresource.layerCount != 1 || r.imageSubresource.mipLevel >= cfg.mip_levels ||
      r.imageSubresource.baseArrayLayer >= cfg.array_layers || r.imageOffset.x < 0 ||
      r.imageOffset.y < 0 || r.imageOffset.z != 0 || r.imageExtent.width == 0 ||
      r.imageExtent.height == 0 || r.imageExtent.depth != 1)
    return false;

  if ((r.bufferRowLength != 0 && r.bufferRowLength < r.imageExtent.width) ||
      (r.bufferImageHeight != 0 && r.bufferImageHeight < r.imageExtent.height))
    return false;
  const std::uint64_t row_texels = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
  // VUID 09108: even a pitched row must fit the signed 31-bit byte limit.
  if (row_texels * 4 > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
    return false;
  return static_cast<std::uint64_t>(r.imageOffset.x) + r.imageExtent.width <=
             mipDimension(cfg.extent.width, r.imageSubresource.mipLevel) &&
         static_cast<std::uint64_t>(r.imageOffset.y) + r.imageExtent.height <=
             mipDimension(cfg.extent.height, r.imageSubresource.mipLevel);
}
std::optional<vk::DeviceSize> imageFootprint(const VkBufferImageCopy2 &r) noexcept {
  const std::uint64_t pitch_texels = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
  const std::uint64_t row_bytes = static_cast<std::uint64_t>(r.imageExtent.width) * 4;
  const std::uint64_t pitch_bytes = pitch_texels * 4;
  const std::uint64_t preceding_rows = static_cast<std::uint64_t>(r.imageExtent.height) - 1;
  const auto limit = std::min<std::uint64_t>(std::numeric_limits<vk::DeviceSize>::max(),
                                             std::numeric_limits<std::size_t>::max());
  if (row_bytes > limit ||
      (preceding_rows != 0 && pitch_bytes > (limit - row_bytes) / preceding_rows))
    return std::nullopt;
  return static_cast<vk::DeviceSize>(preceding_rows * pitch_bytes + row_bytes);
}
bool sameSubresource(const PreparedImageReadback &a, const PreparedImageReadback &b) noexcept {
  return a.expected_image == b.expected_image &&
         a.region.imageSubresource.mipLevel == b.region.imageSubresource.mipLevel &&
         a.region.imageSubresource.baseArrayLayer == b.region.imageSubresource.baseArrayLayer;
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
  if (description.buffers.empty() && description.images.empty())
    return fail(ReadbackError::empty_batch, "prepareReadbackBatch");
  if (!hostAccessible(description.staging_allocation))
    return fail(ReadbackError::unsupported_host_access, "prepareReadbackBatch");

  std::vector<PreparedBufferReadback> regions;
  std::vector<PreparedImageReadback> images;
  vk::DeviceSize total = 0;
  try {
    if (description.buffers.size() > regions.max_size() ||
        description.images.size() > images.max_size())
      return fail(ReadbackError::allocation_failed, "prepareReadbackBatch");
    regions.reserve(description.buffers.size());
    images.reserve(description.images.size());
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

    // Append Image readbacks after Buffer entries; VkBufferImageCopy2 requires
    // an RGBA8 4-byte-aligned bufferOffset, even when a Buffer read was odd-sized.
    for (const auto &read : description.images) {
      if (!read.source || !read.source->valid() || !read.source->configuration() ||
          read.source->nativeImage() == VK_NULL_HANDLE)
        return fail(ReadbackError::invalid_source, "prepareReadbackBatch");
      if (read.source->nativeAllocator() != allocator.nativeHandle())
        return fail(ReadbackError::allocator_mismatch, "prepareReadbackBatch");
      const auto &cfg = *read.source->configuration();
      if (!supportedImage(cfg))
        return fail(ReadbackError::unsupported_image, "prepareReadbackBatch");
      if (!(cfg.usage & vk::ImageUsageFlagBits::eTransferSrc))
        return fail(ReadbackError::missing_usage, "prepareReadbackBatch");
      if (!supportedLayout(read.copy_layout))
        return fail(ReadbackError::unsupported_layout, "prepareReadbackBatch");
      if (read.region.bufferOffset != 0 || !validImageRegion(cfg, read.region))
        return fail(ReadbackError::invalid_image_region, "prepareReadbackBatch");
      const auto payload = imageFootprint(read.region);
      if (!payload)
        return fail(ReadbackError::invalid_image_footprint, "prepareReadbackBatch");

      const auto padding = (4 - (total % 4)) % 4;
      if (padding > std::numeric_limits<vk::DeviceSize>::max() - total)
        return fail(ReadbackError::invalid_image_footprint, "prepareReadbackBatch");
      const auto offset = total + padding;
      const auto limit = std::min<std::uint64_t>(std::numeric_limits<vk::DeviceSize>::max(),
                                                 std::numeric_limits<std::size_t>::max());
      if (offset > limit || *payload > limit - offset)
        return fail(ReadbackError::invalid_image_footprint, "prepareReadbackBatch");

      VkBufferImageCopy2 effective = read.region;
      effective.bufferOffset = offset;
      PreparedImageReadback snapshot{read.source, read.source->nativeImage(), read.copy_layout,
                                     effective, *payload};
      for (const auto &previous : images) {
        // A source Image can be read more than once, but a single mip/layer
        // cannot have conflicting declared layouts without caller transitions.
        if (sameSubresource(previous, snapshot) && previous.copy_layout != snapshot.copy_layout)
          return fail(ReadbackError::conflicting_image_layouts, "prepareReadbackBatch");
      }
      images.push_back(snapshot);
      total = offset + *payload;
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

  return PreparedReadbackBatch{std::move(*staging), total, std::move(regions), std::move(images),
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

  // Full Image prevalidation must happen before ANY Buffer or Image vkCmd call.
  for (const auto &image : batch.images_) {
    if (!image.source || !image.source->valid() || !image.source->configuration() ||
        image.source->nativeImage() == VK_NULL_HANDLE ||
        image.source->nativeImage() != image.expected_image)
      return fail(ReadbackError::invalid_source, "recordReadbackBatch");
    if (image.source->nativeAllocator() != staging.nativeAllocator())
      return fail(ReadbackError::allocator_mismatch, "recordReadbackBatch");
    if (image.source->nativeDevice() != device.nativeHandle())
      return fail(ReadbackError::parent_mismatch, "recordReadbackBatch");
    const auto &cfg = *image.source->configuration();
    if (!supportedImage(cfg))
      return fail(ReadbackError::unsupported_image, "recordReadbackBatch");
    if (!(cfg.usage & vk::ImageUsageFlagBits::eTransferSrc))
      return fail(ReadbackError::missing_usage, "recordReadbackBatch");
    if (!supportedLayout(image.copy_layout))
      return fail(ReadbackError::unsupported_layout, "recordReadbackBatch");
    if (!validImageRegion(cfg, image.region))
      return fail(ReadbackError::invalid_image_region, "recordReadbackBatch");
    const auto footprint = imageFootprint(image.region);
    if (!footprint || *footprint != image.payload_bytes ||
        !within(staging.configuration()->size, image.region.bufferOffset, *footprint))
      return fail(ReadbackError::invalid_image_footprint, "recordReadbackBatch");
  }
  for (std::size_t i = 0; i < batch.images_.size(); ++i) {
    for (std::size_t j = i + 1; j < batch.images_.size(); ++j) {
      if (sameSubresource(batch.images_[i], batch.images_[j]) &&
          batch.images_[i].copy_layout != batch.images_[j].copy_layout)
        return fail(ReadbackError::conflicting_image_layouts, "recordReadbackBatch");
    }
  }

  // All Buffer source owners must in fact belong to device; Buffer has no
  // nativeDevice() observer, so this remains a caller obligation.
  for (const auto &region : batch.copies_) {
    const VkBufferCopy2 copy{VK_STRUCTURE_TYPE_BUFFER_COPY_2, nullptr, region.source_offset,
                             region.staging_offset, region.size};
    auto result = recordCopyBuffer(device, command, *region.source, staging, std::span{&copy, 1});
    if (!result)
      return result; // Native recording is not transactional; caller resets if needed.
  }
  for (const auto &image : batch.images_) {
    auto result = recordCopyImageToBuffer(device, command, *image.source, image.copy_layout,
                                          staging, std::span{&image.region, 1});
    if (!result)
      return result;
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
  std::vector<CompletedImageReadback> image_regions;
  try {
    // Staging row padding and odd-sized Buffer/Image inter-entry gaps are
    // undefined GPU output. Only defined copied bytes are published; padding
    // in this CPU snapshot remains deterministic zero.
    bytes.resize(static_cast<std::size_t>(batch.size_), std::byte{0});
    regions.reserve(batch.copies_.size());
    image_regions.reserve(batch.images_.size());
    for (const auto &copy : batch.copies_)
      regions.push_back(CompletedBufferReadback{copy.source_offset,
                                                static_cast<std::size_t>(copy.staging_offset),
                                                static_cast<std::size_t>(copy.size)});
    for (const auto &image : batch.images_) {
      const auto effective_row_texels = image.region.bufferRowLength
                                            ? image.region.bufferRowLength
                                            : image.region.imageExtent.width;
      image_regions.push_back(CompletedImageReadback{
          .format = vk::Format::eR8G8B8A8Unorm,
          .copy_layout = image.copy_layout,
          .region = image.region,
          .data_offset = static_cast<std::size_t>(image.region.bufferOffset),
          .payload_bytes = static_cast<std::size_t>(image.payload_bytes),
          .row_pitch_bytes = static_cast<std::size_t>(effective_row_texels) * 4,
      });
    }
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
  const auto *mapped_bytes = static_cast<const std::byte *>(*mapped);
  for (const auto &copy : batch.copies_) {
    const auto offset = static_cast<std::size_t>(copy.staging_offset);
    std::memcpy(bytes.data() + offset, mapped_bytes + offset, static_cast<std::size_t>(copy.size));
  }
  for (const auto &image : batch.images_) {
    const auto effective_row_texels = image.region.bufferRowLength ? image.region.bufferRowLength
                                                                   : image.region.imageExtent.width;
    const auto row_pitch_bytes = static_cast<std::size_t>(effective_row_texels) * 4;
    const auto active_row_bytes = static_cast<std::size_t>(image.region.imageExtent.width) * 4;
    const auto base = static_cast<std::size_t>(image.region.bufferOffset);
    for (std::size_t row = 0; row < image.region.imageExtent.height; ++row) {
      const auto offset = base + row * row_pitch_bytes;
      std::memcpy(bytes.data() + offset, mapped_bytes + offset, active_row_bytes);
    }
  }
  if (auto unmapped = staging.unmap(); !unmapped)
    return std::unexpected(unmapped.error());
  return CompletedReadbackBatch{std::move(bytes), std::move(regions), std::move(image_regions)};
}
} // namespace terreate::graphics

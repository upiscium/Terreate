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
      return "upload batch contains no Buffer or Image writes";
    case UploadError::invalid_destination:
      return "invalid or displaced upload destination";
    case UploadError::invalid_payload:
      return "upload data is empty or invalid";
    case UploadError::missing_usage:
      return "upload destination lacks TRANSFER_DST usage";
    case UploadError::invalid_range:
      return "Buffer upload offset, size or total is out of bounds";
    case UploadError::unaligned_copy:
      return "Buffer copy offsets and sizes must be 4-byte aligned";
    case UploadError::allocator_mismatch:
      return "upload resource and staging Allocator mismatch";
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
      return "CommandBuffer or Image parent Device differs";
    case UploadError::protected_recording:
      return "protected command recording is unsupported";
    case UploadError::unsupported_image:
      return "Image upload requires R8G8B8A8_UNORM 2D sample-count 1";
    case UploadError::unsupported_layout:
      return "copy requires explicit TRANSFER_DST_OPTIMAL or GENERAL layout";
    case UploadError::invalid_image_region:
      return "Image copy region, subresource or pitch is invalid";
    case UploadError::invalid_image_footprint:
      return "CPU byte count must equal the exact Image copy footprint";
    case UploadError::overlapping_image_regions:
      return "overlapping writes to the same Image mip/layer";
    case UploadError::conflicting_image_layouts:
      return "same Image mip/layer cannot use two different copy layouts";
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
[[nodiscard]] bool supportedImage(const ImageConfiguration &configuration) {
  return configuration.format == vk::Format::eR8G8B8A8Unorm &&
         configuration.dimensions == vk::ImageType::e2D &&
         configuration.samples == vk::SampleCountFlagBits::e1;
}
[[nodiscard]] bool supportedLayout(vk::ImageLayout layout) {
  return layout == vk::ImageLayout::eTransferDstOptimal || layout == vk::ImageLayout::eGeneral;
}
[[nodiscard]] std::uint32_t mipDimension(std::uint32_t dimension, std::uint32_t level) {
  const auto shifted = dimension >> (level < 32 ? level : 31);
  return shifted != 0 ? shifted : 1U;
}
[[nodiscard]] bool validImageRegion(const ImageConfiguration &configuration,
                                    const VkBufferImageCopy2 &region) {
  if (region.sType != VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 || region.pNext ||
      (region.bufferOffset % 4) != 0 ||
      region.imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
      region.imageSubresource.layerCount != 1 ||
      region.imageSubresource.mipLevel >= configuration.mip_levels ||
      region.imageSubresource.baseArrayLayer >= configuration.array_layers ||
      region.imageOffset.x < 0 || region.imageOffset.y < 0 || region.imageOffset.z != 0 ||
      region.imageExtent.width == 0 || region.imageExtent.height == 0 ||
      region.imageExtent.depth != 1)
    return false;
  if ((region.bufferRowLength && region.bufferRowLength < region.imageExtent.width) ||
      (region.bufferImageHeight && region.bufferImageHeight < region.imageExtent.height))
    return false;
  const auto row_texels =
      region.bufferRowLength ? region.bufferRowLength : region.imageExtent.width;
  // VUID 09108: row texels times RGBA8 block size must fit in signed 31 bits.
  if (static_cast<std::uint64_t>(row_texels) * 4 >
      static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
    return false;
  return static_cast<std::uint64_t>(region.imageOffset.x) + region.imageExtent.width <=
             mipDimension(configuration.extent.width, region.imageSubresource.mipLevel) &&
         static_cast<std::uint64_t>(region.imageOffset.y) + region.imageExtent.height <=
             mipDimension(configuration.extent.height, region.imageSubresource.mipLevel);
}
[[nodiscard]] std::optional<vk::DeviceSize> imageFootprint(const VkBufferImageCopy2 &region) {
  // Exactly one 2D RGBA8 layer: last row ends after imageExtent.width texels.
  // bufferImageHeight controls slice/layer strides, neither of which occurs
  // in this bounded MVI; any nonzero height has already been validated.
  const std::uint64_t row_texels =
      region.bufferRowLength ? region.bufferRowLength : region.imageExtent.width;
  const std::uint64_t rows = static_cast<std::uint64_t>(region.imageExtent.height) - 1;
  const std::uint64_t last_width = region.imageExtent.width;
  const auto limit = std::numeric_limits<vk::DeviceSize>::max();
  if (rows != 0 && row_texels > (limit - last_width) / rows)
    return std::nullopt;
  const auto texels = rows * row_texels + last_width;
  if (texels > limit / 4 || texels > std::numeric_limits<std::size_t>::max() / 4)
    return std::nullopt;
  return static_cast<vk::DeviceSize>(texels * 4);
}
[[nodiscard]] bool sameImageSubresource(const PreparedImageUpload &a,
                                        const PreparedImageUpload &b) {
  return a.expected_image == b.expected_image &&
         a.region.imageSubresource.mipLevel == b.region.imageSubresource.mipLevel &&
         a.region.imageSubresource.baseArrayLayer == b.region.imageSubresource.baseArrayLayer;
}
[[nodiscard]] bool imageRegionsOverlap(const PreparedImageUpload &a, const PreparedImageUpload &b) {
  const auto &left = a.region;
  const auto &right = b.region;
  if (!sameImageSubresource(a, b))
    return false;
  const auto ax = static_cast<std::uint64_t>(left.imageOffset.x);
  const auto ay = static_cast<std::uint64_t>(left.imageOffset.y);
  const auto bx = static_cast<std::uint64_t>(right.imageOffset.x);
  const auto by = static_cast<std::uint64_t>(right.imageOffset.y);
  return ax < bx + right.imageExtent.width && bx < ax + left.imageExtent.width &&
         ay < by + right.imageExtent.height && by < ay + left.imageExtent.height;
}
[[nodiscard]] bool withinStaging(vk::DeviceSize used, std::size_t n, vk::DeviceSize &offset) {
  if (used > std::numeric_limits<std::size_t>::max())
    return false;
  if (n > std::numeric_limits<vk::DeviceSize>::max() - used ||
      n > std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(used))
    return false;
  offset = used;
  return true;
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
  if (description.buffers.empty() && description.images.empty())
    return fail(UploadError::empty_batch, "prepareUploadBatch");

  std::vector<PreparedBufferUpload> copies;
  std::vector<PreparedImageUpload> images;
  vk::DeviceSize staging_bytes = 0;
  try {
    if (description.buffers.size() > copies.max_size() ||
        description.images.size() > images.max_size())
      return fail(UploadError::allocation_failed, "prepareUploadBatch");
    copies.reserve(description.buffers.size());
    images.reserve(description.images.size());

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
      vk::DeviceSize offset = 0;
      if (!withinStaging(staging_bytes, write.bytes.size(), offset))
        return fail(UploadError::invalid_range, "prepareUploadBatch");
      for (const auto &previous : copies) {
        if (previous.destination->nativeBuffer() == write.destination->nativeBuffer() &&
            overlaps(previous.destination_offset, previous.size, write.destination_offset, size))
          return fail(UploadError::overlapping_writes, "prepareUploadBatch");
      }
      copies.push_back(PreparedBufferUpload{.destination = write.destination,
                                            .staging_offset = offset,
                                            .destination_offset = write.destination_offset,
                                            .size = size});
      staging_bytes += size;
    }

    for (const auto &write : description.images) {
      if (!write.destination || !write.destination->valid() ||
          !write.destination->configuration() || write.destination->nativeImage() == VK_NULL_HANDLE)
        return fail(UploadError::invalid_destination, "prepareUploadBatch");
      if (write.destination->nativeAllocator() != allocator.nativeHandle())
        return fail(UploadError::allocator_mismatch, "prepareUploadBatch");
      const auto &configuration = *write.destination->configuration();
      if (!supportedImage(configuration))
        return fail(UploadError::unsupported_image, "prepareUploadBatch");
      if (!(configuration.usage & vk::ImageUsageFlagBits::eTransferDst))
        return fail(UploadError::missing_usage, "prepareUploadBatch");
      if (!supportedLayout(write.copy_layout))
        return fail(UploadError::unsupported_layout, "prepareUploadBatch");
      // The request must not assume the offset of the library-owned staging.
      if (write.region.bufferOffset != 0 || !validImageRegion(configuration, write.region))
        return fail(UploadError::invalid_image_region, "prepareUploadBatch");
      const auto footprint = imageFootprint(write.region);
      if (!footprint || write.bytes.empty() || !write.bytes.data() ||
          write.bytes.size() != *footprint)
        return fail(UploadError::invalid_image_footprint, "prepareUploadBatch");
      vk::DeviceSize offset = 0;
      if (!withinStaging(staging_bytes, write.bytes.size(), offset))
        return fail(UploadError::invalid_range, "prepareUploadBatch");
      auto effective = write.region;
      effective.bufferOffset = offset;
      PreparedImageUpload snapshot{.destination = write.destination,
                                   .expected_image = write.destination->nativeImage(),
                                   .copy_layout = write.copy_layout,
                                   .region = effective,
                                   .payload_bytes = *footprint};
      for (const auto &previous : images) {
        if (sameImageSubresource(previous, snapshot) &&
            previous.copy_layout != snapshot.copy_layout)
          return fail(UploadError::conflicting_image_layouts, "prepareUploadBatch");
        if (imageRegionsOverlap(previous, snapshot))
          return fail(UploadError::overlapping_image_regions, "prepareUploadBatch");
      }
      images.push_back(snapshot);
      staging_bytes += *footprint;
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
    std::memcpy(destination + static_cast<std::size_t>(copies[i].staging_offset),
                write.bytes.data(), write.bytes.size());
  }
  for (std::size_t i = 0; i < description.images.size(); ++i) {
    const auto &write = description.images[i];
    std::memcpy(destination + static_cast<std::size_t>(images[i].region.bufferOffset),
                write.bytes.data(), write.bytes.size());
  }
  if (auto flushed = buffer->flush(); !flushed)
    return std::unexpected(flushed.error());
  if (auto unmapped = buffer->unmap(); !unmapped)
    return std::unexpected(unmapped.error());
  return PreparedUploadBatch{std::move(*buffer), std::move(copies), std::move(images),
                             description.staging_allocation};
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

  // Validate every Buffer and Image entry before the first native vkCmd call.
  // This prevents deterministic input failures from partial recording.
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
  for (std::size_t i = 0; i < copies.size(); ++i)
    for (std::size_t j = i + 1; j < copies.size(); ++j)
      if (copies[i].destination->nativeBuffer() == copies[j].destination->nativeBuffer() &&
          overlaps(copies[i].destination_offset, copies[i].size, copies[j].destination_offset,
                   copies[j].size))
        return fail(UploadError::overlapping_writes, "recordUploadBatch");

  const auto images = batch.images();
  for (const auto &copy : images) {
    if (!copy.destination || !copy.destination->valid() || !copy.destination->configuration() ||
        copy.destination->nativeImage() != copy.expected_image)
      return fail(UploadError::invalid_destination, "recordUploadBatch");
    if (copy.destination->nativeAllocator() != staging->nativeAllocator())
      return fail(UploadError::allocator_mismatch, "recordUploadBatch");
    if (copy.destination->nativeDevice() != device.nativeHandle())
      return fail(UploadError::parent_mismatch, "recordUploadBatch");
    const auto &configuration = *copy.destination->configuration();
    if (!supportedImage(configuration))
      return fail(UploadError::unsupported_image, "recordUploadBatch");
    if (!(configuration.usage & vk::ImageUsageFlagBits::eTransferDst))
      return fail(UploadError::missing_usage, "recordUploadBatch");
    if (!supportedLayout(copy.copy_layout))
      return fail(UploadError::unsupported_layout, "recordUploadBatch");
    if (!validImageRegion(configuration, copy.region))
      return fail(UploadError::invalid_image_region, "recordUploadBatch");
    const auto footprint = imageFootprint(copy.region);
    if (!footprint || *footprint != copy.payload_bytes ||
        !within(staging->configuration()->size, copy.region.bufferOffset, *footprint))
      return fail(UploadError::invalid_image_footprint, "recordUploadBatch");
  }
  for (std::size_t i = 0; i < images.size(); ++i) {
    for (std::size_t j = i + 1; j < images.size(); ++j) {
      if (sameImageSubresource(images[i], images[j]) &&
          images[i].copy_layout != images[j].copy_layout)
        return fail(UploadError::conflicting_image_layouts, "recordUploadBatch");
      if (imageRegionsOverlap(images[i], images[j]))
        return fail(UploadError::overlapping_image_regions, "recordUploadBatch");
    }
  }

  for (const auto &copy : copies) {
    const VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2, nullptr, copy.staging_offset,
                               copy.destination_offset, copy.size};
    if (auto recorded = recordCopyBuffer(device, command, *staging, *copy.destination,
                                         std::span{&region, std::size_t{1}});
        !recorded)
      return recorded;
  }
  for (const auto &copy : images) {
    if (auto recorded =
            recordCopyBufferToImage(device, command, *staging, *copy.destination, copy.copy_layout,
                                    std::span{&copy.region, std::size_t{1}});
        !recorded)
      return recorded;
  }
  return {};
}
} // namespace terreate::graphics

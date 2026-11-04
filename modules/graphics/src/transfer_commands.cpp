#include <terreate/graphics/transfer_commands.hpp>

#include <limits>
#include <string>
#include <vulkan/vulkan.h>

namespace terreate::graphics {
namespace {
class TransferCommandErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.transfer_commands"; }
  std::string message(int value) const override {
    switch (static_cast<TransferCommandError>(value)) {
    case TransferCommandError::invalid_device:
      return "invalid logical Device";
    case TransferCommandError::invalid_command_buffer:
      return "invalid CommandBuffer";
    case TransferCommandError::not_recording:
      return "CommandBuffer is not recording";
    case TransferCommandError::parent_mismatch:
      return "CommandBuffer or Image Device mismatch";
    case TransferCommandError::synchronization2_not_enabled:
      return "synchronization2 not enabled";
    case TransferCommandError::invalid_resource:
      return "invalid source or destination";
    case TransferCommandError::missing_regions:
      return "at least one region or barrier required";
    case TransferCommandError::invalid_native_structure:
      return "invalid native structure, pNext or native handle";
    case TransferCommandError::invalid_region:
      return "invalid native transfer region";
    case TransferCommandError::missing_usage:
      return "resource missing transfer usage";
    case TransferCommandError::out_of_bounds:
      return "buffer transfer range out of bounds";
    }
    return "unknown transfer command error";
  }
};
terreate::Result<void> fail(TransferCommandError error, const char *name) {
  return std::unexpected(terreate::Error{make_error_code(error), name,
                                         "recording rejected before native Vulkan call"});
}
terreate::Result<void> validate(const Device &device, const CommandBuffer &command,
                                bool needs_sync2, const char *name) {
  if (!device.valid() || !device.plan())
    return fail(TransferCommandError::invalid_device, name);
  if (!command.valid())
    return fail(TransferCommandError::invalid_command_buffer, name);
  if (command.nativeDevice() != device.nativeHandle())
    return fail(TransferCommandError::parent_mismatch, name);
  if (command.state() != CommandBufferState::recording)
    return fail(TransferCommandError::not_recording, name);
  if (needs_sync2 && !device.plan()->enabledFeatures13().synchronization2)
    return fail(TransferCommandError::synchronization2_not_enabled, name);
  return {};
}
bool containsRange(VkDeviceSize total, VkDeviceSize offset, VkDeviceSize size) {
  return size != 0 && offset <= total && size <= total - offset;
}
bool validImageSubresource(const ImageConfiguration &cfg, const VkImageSubresourceLayers &sub) {
  return sub.aspectMask != 0 && sub.layerCount != 0 && sub.mipLevel < cfg.mip_levels &&
         sub.baseArrayLayer < cfg.array_layers &&
         sub.layerCount <= cfg.array_layers - sub.baseArrayLayer;
}
bool validExtent(const VkExtent3D &e) { return e.width != 0 && e.height != 0 && e.depth != 0; }
bool validOffset(const VkOffset3D &e) { return e.x >= 0 && e.y >= 0 && e.z >= 0; }
bool validImageBox(const ImageConfiguration &cfg, const VkImageSubresourceLayers &sub,
                   const VkOffset3D &offset, const VkExtent3D &extent) {
  if (!validImageSubresource(cfg, sub) || !validOffset(offset) || !validExtent(extent))
    return false;
  const auto mip_dimension = [level = sub.mipLevel](std::uint32_t dimension) {
    const auto shifted = dimension >> (level < 32 ? level : 31);
    return shifted ? shifted : 1U;
  };
  return static_cast<std::uint64_t>(offset.x) + extent.width <= mip_dimension(cfg.extent.width) &&
         static_cast<std::uint64_t>(offset.y) + extent.height <= mip_dimension(cfg.extent.height) &&
         static_cast<std::uint64_t>(offset.z) + extent.depth <= mip_dimension(cfg.extent.depth);
}
bool validBufferImageRegion(const VkBufferImageCopy2 &region, const ImageConfiguration &image_cfg) {
  if (region.sType != VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 || region.pNext ||
      !validImageBox(image_cfg, region.imageSubresource, region.imageOffset, region.imageExtent))
    return false;
  return (!region.bufferRowLength || region.bufferRowLength >= region.imageExtent.width) &&
         (!region.bufferImageHeight || region.bufferImageHeight >= region.imageExtent.height);
}
bool validBufferImageOffset(const BufferConfiguration &buffer_cfg,
                            const VkBufferImageCopy2 &region) {
  // The exact footprint depends on image format, compression and texel blocks;
  // the caller must supply Vulkan-valid row pitch/size and bounds. This checks
  // only the definitely invalid offset, without pretending to infer format.
  return region.bufferOffset < buffer_cfg.size;
}
bool validImages(const Device &device, const Image &src, const Image &dst) {
  return src.valid() && dst.valid() && src.configuration() && dst.configuration() &&
         src.nativeDevice() == device.nativeHandle() && dst.nativeDevice() == device.nativeHandle();
}
} // namespace

const std::error_category &transfer_command_error_category() noexcept {
  static const TransferCommandErrorCategory category;
  return category;
}
std::error_code make_error_code(TransferCommandError error) noexcept {
  return {static_cast<int>(error), transfer_command_error_category()};
}

terreate::Result<void> recordBarriers(const Device &device, const CommandBuffer &command,
                                      const BarrierDescription &description) {
  if (auto check = validate(device, command, true, "recordBarriers"); !check)
    return check;
  if (description.memory.empty() && description.buffers.empty() && description.images.empty())
    return fail(TransferCommandError::missing_regions, "recordBarriers");
  if (description.memory.size() > std::numeric_limits<std::uint32_t>::max() ||
      description.buffers.size() > std::numeric_limits<std::uint32_t>::max() ||
      description.images.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(TransferCommandError::invalid_region, "recordBarriers");
  for (const auto &barrier : description.memory)
    if (barrier.sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 || barrier.pNext)
      return fail(TransferCommandError::invalid_native_structure, "recordBarriers");
  for (const auto &barrier : description.buffers)
    if (barrier.sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 || barrier.pNext ||
        barrier.buffer == VK_NULL_HANDLE || !barrier.size)
      return fail(TransferCommandError::invalid_native_structure, "recordBarriers");
  for (const auto &barrier : description.images)
    if (barrier.sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 || barrier.pNext ||
        barrier.image == VK_NULL_HANDLE || !barrier.subresourceRange.aspectMask ||
        !barrier.subresourceRange.levelCount || !barrier.subresourceRange.layerCount)
      return fail(TransferCommandError::invalid_native_structure, "recordBarriers");
  VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  info.dependencyFlags = description.flags;
  info.memoryBarrierCount = static_cast<std::uint32_t>(description.memory.size());
  info.pMemoryBarriers = description.memory.data();
  info.bufferMemoryBarrierCount = static_cast<std::uint32_t>(description.buffers.size());
  info.pBufferMemoryBarriers = description.buffers.data();
  info.imageMemoryBarrierCount = static_cast<std::uint32_t>(description.images.size());
  info.pImageMemoryBarriers = description.images.data();
  vkCmdPipelineBarrier2(static_cast<VkCommandBuffer>(command.nativeHandle()), &info);
  return {};
}

terreate::Result<void> recordCopyBuffer(const Device &device, const CommandBuffer &command,
                                        const Buffer &src, const Buffer &dst,
                                        std::span<const VkBufferCopy2> regions) {
  if (auto check = validate(device, command, false, "recordCopyBuffer"); !check)
    return check;
  if (!src.valid() || !dst.valid() || !src.configuration() || !dst.configuration())
    return fail(TransferCommandError::invalid_resource, "recordCopyBuffer");
  if (!(src.configuration()->usage & vk::BufferUsageFlagBits::eTransferSrc) ||
      !(dst.configuration()->usage & vk::BufferUsageFlagBits::eTransferDst))
    return fail(TransferCommandError::missing_usage, "recordCopyBuffer");
  if (regions.empty() || regions.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(TransferCommandError::missing_regions, "recordCopyBuffer");
  for (const auto &region : regions) {
    if (region.sType != VK_STRUCTURE_TYPE_BUFFER_COPY_2 || region.pNext || !region.size)
      return fail(TransferCommandError::invalid_region, "recordCopyBuffer");
    if (!containsRange(src.configuration()->size, region.srcOffset, region.size) ||
        !containsRange(dst.configuration()->size, region.dstOffset, region.size))
      return fail(TransferCommandError::out_of_bounds, "recordCopyBuffer");
  }
  VkCopyBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
  info.srcBuffer = src.nativeBuffer();
  info.dstBuffer = dst.nativeBuffer();
  info.regionCount = static_cast<std::uint32_t>(regions.size());
  info.pRegions = regions.data();
  vkCmdCopyBuffer2(static_cast<VkCommandBuffer>(command.nativeHandle()), &info);
  return {};
}

terreate::Result<void> recordCopyImage(const Device &device, const CommandBuffer &command,
                                       const Image &src, vk::ImageLayout src_layout,
                                       const Image &dst, vk::ImageLayout dst_layout,
                                       std::span<const VkImageCopy2> regions) {
  if (auto check = validate(device, command, false, "recordCopyImage"); !check)
    return check;
  if (!validImages(device, src, dst))
    return fail(TransferCommandError::invalid_resource, "recordCopyImage");
  if (!(src.configuration()->usage & vk::ImageUsageFlagBits::eTransferSrc) ||
      !(dst.configuration()->usage & vk::ImageUsageFlagBits::eTransferDst))
    return fail(TransferCommandError::missing_usage, "recordCopyImage");
  if (regions.empty() || regions.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(TransferCommandError::missing_regions, "recordCopyImage");
  for (const auto &region : regions)
    if (region.sType != VK_STRUCTURE_TYPE_IMAGE_COPY_2 || region.pNext ||
        !validImageBox(*src.configuration(), region.srcSubresource, region.srcOffset,
                       region.extent) ||
        !validImageBox(*dst.configuration(), region.dstSubresource, region.dstOffset,
                       region.extent))
      return fail(TransferCommandError::invalid_region, "recordCopyImage");
  VkCopyImageInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
  info.srcImage = src.nativeImage();
  info.srcImageLayout = static_cast<VkImageLayout>(src_layout);
  info.dstImage = dst.nativeImage();
  info.dstImageLayout = static_cast<VkImageLayout>(dst_layout);
  info.regionCount = static_cast<std::uint32_t>(regions.size());
  info.pRegions = regions.data();
  vkCmdCopyImage2(static_cast<VkCommandBuffer>(command.nativeHandle()), &info);
  return {};
}

terreate::Result<void> recordCopyBufferToImage(const Device &device, const CommandBuffer &command,
                                               const Buffer &src, const Image &dst,
                                               vk::ImageLayout dst_layout,
                                               std::span<const VkBufferImageCopy2> regions) {
  if (auto check = validate(device, command, false, "recordCopyBufferToImage"); !check)
    return check;
  if (!src.valid() || !src.configuration() || !dst.valid() || !dst.configuration())
    return fail(TransferCommandError::invalid_resource, "recordCopyBufferToImage");
  if (dst.nativeDevice() != device.nativeHandle())
    return fail(TransferCommandError::parent_mismatch, "recordCopyBufferToImage");
  if (!(src.configuration()->usage & vk::BufferUsageFlagBits::eTransferSrc) ||
      !(dst.configuration()->usage & vk::ImageUsageFlagBits::eTransferDst))
    return fail(TransferCommandError::missing_usage, "recordCopyBufferToImage");
  if (regions.empty() || regions.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(TransferCommandError::missing_regions, "recordCopyBufferToImage");
  for (const auto &region : regions) {
    if (!validBufferImageRegion(region, *dst.configuration()))
      return fail(TransferCommandError::invalid_region, "recordCopyBufferToImage");
    if (!validBufferImageOffset(*src.configuration(), region))
      return fail(TransferCommandError::out_of_bounds, "recordCopyBufferToImage");
  }
  VkCopyBufferToImageInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
  info.srcBuffer = src.nativeBuffer();
  info.dstImage = dst.nativeImage();
  info.dstImageLayout = static_cast<VkImageLayout>(dst_layout);
  info.regionCount = static_cast<std::uint32_t>(regions.size());
  info.pRegions = regions.data();
  vkCmdCopyBufferToImage2(static_cast<VkCommandBuffer>(command.nativeHandle()), &info);
  return {};
}

terreate::Result<void> recordCopyImageToBuffer(const Device &device, const CommandBuffer &command,
                                               const Image &src, vk::ImageLayout src_layout,
                                               const Buffer &dst,
                                               std::span<const VkBufferImageCopy2> regions) {
  if (auto check = validate(device, command, false, "recordCopyImageToBuffer"); !check)
    return check;
  if (!src.valid() || !src.configuration() || !dst.valid() || !dst.configuration())
    return fail(TransferCommandError::invalid_resource, "recordCopyImageToBuffer");
  if (src.nativeDevice() != device.nativeHandle())
    return fail(TransferCommandError::parent_mismatch, "recordCopyImageToBuffer");
  if (!(src.configuration()->usage & vk::ImageUsageFlagBits::eTransferSrc) ||
      !(dst.configuration()->usage & vk::BufferUsageFlagBits::eTransferDst))
    return fail(TransferCommandError::missing_usage, "recordCopyImageToBuffer");
  if (regions.empty() || regions.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(TransferCommandError::missing_regions, "recordCopyImageToBuffer");
  for (const auto &region : regions) {
    if (!validBufferImageRegion(region, *src.configuration()))
      return fail(TransferCommandError::invalid_region, "recordCopyImageToBuffer");
    if (!validBufferImageOffset(*dst.configuration(), region))
      return fail(TransferCommandError::out_of_bounds, "recordCopyImageToBuffer");
  }
  VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
  info.srcImage = src.nativeImage();
  info.srcImageLayout = static_cast<VkImageLayout>(src_layout);
  info.dstBuffer = dst.nativeBuffer();
  info.regionCount = static_cast<std::uint32_t>(regions.size());
  info.pRegions = regions.data();
  vkCmdCopyImageToBuffer2(static_cast<VkCommandBuffer>(command.nativeHandle()), &info);
  return {};
}
} // namespace terreate::graphics

#include <terreate/graphics/barriers.hpp>

#include <span>

namespace terreate::graphics {
namespace {

VkImageMemoryBarrier2 build(VkImage image, const ImageBarrierDescription &description) noexcept {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = static_cast<VkPipelineStageFlags2>(description.src_stage);
  barrier.srcAccessMask = static_cast<VkAccessFlags2>(description.src_access);
  barrier.dstStageMask = static_cast<VkPipelineStageFlags2>(description.dst_stage);
  barrier.dstAccessMask = static_cast<VkAccessFlags2>(description.dst_access);
  barrier.oldLayout = static_cast<VkImageLayout>(description.old_layout);
  barrier.newLayout = static_cast<VkImageLayout>(description.new_layout);
  barrier.srcQueueFamilyIndex = description.src_queue_family;
  barrier.dstQueueFamilyIndex = description.dst_queue_family;
  barrier.image = image;
  barrier.subresourceRange = static_cast<VkImageSubresourceRange>(description.subresource_range);
  return barrier;
}

} // namespace

VkImageMemoryBarrier2 imageBarrier(const Image &image,
                                   const ImageBarrierDescription &description) noexcept {
  return build(image.nativeImage(), description);
}

VkImageMemoryBarrier2 imageBarrier(const SwapchainImage &image,
                                   const ImageBarrierDescription &description) noexcept {
  return build(static_cast<VkImage>(image.nativeImage()), description);
}

terreate::Result<void> recordImageBarrier(const Device &device, const CommandBuffer &command,
                                          const Image &image,
                                          const ImageBarrierDescription &description) {
  const VkImageMemoryBarrier2 barrier = imageBarrier(image, description);
  return recordBarriers(device, command, {.images = std::span{&barrier, 1}});
}

terreate::Result<void> recordImageBarrier(const Device &device, const CommandBuffer &command,
                                          const SwapchainImage &image,
                                          const ImageBarrierDescription &description) {
  const VkImageMemoryBarrier2 barrier = imageBarrier(image, description);
  return recordBarriers(device, command, {.images = std::span{&barrier, 1}});
}

} // namespace terreate::graphics

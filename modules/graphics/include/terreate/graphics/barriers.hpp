#ifndef TERREATE_GRAPHICS_BARRIERS_HPP
#define TERREATE_GRAPHICS_BARRIERS_HPP

#include <cstdint>

#include <terreate/core/result.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/swapchain.hpp>
#include <terreate/graphics/transfer_commands.hpp>

namespace terreate::graphics {

/// One image layout or access transition, every part of it stated by the caller.
/// Nothing is inferred, remembered or tracked: the stage and access masks, both
/// layouts and the subresource range are exactly what ends up in the native
/// barrier. The defaults are Vulkan's neutral values (empty masks, `eUndefined`,
/// no queue family transfer), not a policy; in particular `subresource_range` has
/// no default aspect, so an unset range is refused by `recordBarriers`.
struct ImageBarrierDescription {
  vk::PipelineStageFlags2 src_stage{};
  vk::AccessFlags2 src_access{};
  vk::PipelineStageFlags2 dst_stage{};
  vk::AccessFlags2 dst_access{};
  vk::ImageLayout old_layout = vk::ImageLayout::eUndefined;
  vk::ImageLayout new_layout = vk::ImageLayout::eUndefined;
  vk::ImageSubresourceRange subresource_range{};
  /// Queue family ownership transfer: both stay `VK_QUEUE_FAMILY_IGNORED` unless the
  /// application releases and acquires the image across queue families.
  std::uint32_t src_queue_family = VK_QUEUE_FAMILY_IGNORED;
  std::uint32_t dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
};

/// Build the native barrier for `image` from `description`. Only builds a value:
/// nothing is recorded and no native function is called. A moved-from Image, or an
/// empty SwapchainImage, gives a barrier whose `image` is null, which
/// `recordBarriers` then refuses (`invalid_native_structure`). The result belongs
/// in `BarrierDescription::images`, alone or together with other barriers.
[[nodiscard]] VkImageMemoryBarrier2
imageBarrier(const Image &image, const ImageBarrierDescription &description) noexcept;
[[nodiscard]] VkImageMemoryBarrier2
imageBarrier(const SwapchainImage &image, const ImageBarrierDescription &description) noexcept;

/// Record exactly one image barrier: one `vkCmdPipelineBarrier2`, through
/// `recordBarriers` (the same preconditions, the same refusals, nothing else). Use
/// `recordBarriers` with several `imageBarrier` values to put more than one
/// transition into a single call.
[[nodiscard]] terreate::Result<void> recordImageBarrier(const Device &device,
                                                        const CommandBuffer &command,
                                                        const Image &image,
                                                        const ImageBarrierDescription &description);
[[nodiscard]] terreate::Result<void> recordImageBarrier(const Device &device,
                                                        const CommandBuffer &command,
                                                        const SwapchainImage &image,
                                                        const ImageBarrierDescription &description);

} // namespace terreate::graphics

#endif // TERREATE_GRAPHICS_BARRIERS_HPP

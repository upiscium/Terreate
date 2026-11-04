#ifndef TERREATE_GRAPHICS_TRANSFER_COMMANDS_HPP
#define TERREATE_GRAPHICS_TRANSFER_COMMANDS_HPP

#include <cstdint>
#include <span>
#include <system_error>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/image.hpp>

namespace terreate::graphics {

enum class TransferCommandError : std::uint8_t {
  invalid_device = 1,
  invalid_command_buffer,
  not_recording,
  parent_mismatch,
  synchronization2_not_enabled,
  invalid_resource,
  missing_regions,
  invalid_native_structure,
  invalid_region,
  missing_usage,
  out_of_bounds,
};
[[nodiscard]] const std::error_category &transfer_command_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(TransferCommandError error) noexcept;

/// Native synchronization2 structures are caller-owned, borrowed only during
/// this call. All stage/access masks, layouts and queue family indices are
/// explicit. No history, dependencies or resource ownership are inferred.
struct BarrierDescription {
  std::span<const VkMemoryBarrier2> memory{};
  std::span<const VkBufferMemoryBarrier2> buffers{};
  std::span<const VkImageMemoryBarrier2> images{};
  VkDependencyFlags flags = 0;
};

/// Record exactly one vkCmdPipelineBarrier2. Requires an already recording
/// CommandBuffer and actually enabled synchronization2 on Device. Does NOT
/// submit or validate the caller's historical image layout or queue transfers.
[[nodiscard]] terreate::Result<void> recordBarriers(const Device &device,
                                                    const CommandBuffer &command,
                                                    const BarrierDescription &description);

/// Copy operations use Vulkan 1.3 *2 commands. Regions are explicit borrowed
/// Vk*Copy2 structs, with null pNext; this is a native low-level escape hatch.
/// No implicit barriers, layout tracking, submission or waits are performed.
/// Buffer owner Device provenance cannot be recovered from the public Buffer:
/// the caller guarantees Buffer, CommandBuffer and Device refer to one Device.
[[nodiscard]] terreate::Result<void> recordCopyBuffer(const Device &device,
                                                      const CommandBuffer &command,
                                                      const Buffer &src, const Buffer &dst,
                                                      std::span<const VkBufferCopy2> regions);

[[nodiscard]] terreate::Result<void> recordCopyImage(const Device &device,
                                                     const CommandBuffer &command, const Image &src,
                                                     vk::ImageLayout src_layout, const Image &dst,
                                                     vk::ImageLayout dst_layout,
                                                     std::span<const VkImageCopy2> regions);

[[nodiscard]] terreate::Result<void>
recordCopyBufferToImage(const Device &device, const CommandBuffer &command, const Buffer &src,
                        const Image &dst, vk::ImageLayout dst_layout,
                        std::span<const VkBufferImageCopy2> regions);

[[nodiscard]] terreate::Result<void>
recordCopyImageToBuffer(const Device &device, const CommandBuffer &command, const Image &src,
                        vk::ImageLayout src_layout, const Buffer &dst,
                        std::span<const VkBufferImageCopy2> regions);

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::TransferCommandError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_TRANSFER_COMMANDS_HPP

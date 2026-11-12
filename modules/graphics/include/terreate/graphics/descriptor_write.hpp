#ifndef TERREATE_GRAPHICS_DESCRIPTOR_WRITE_HPP
#define TERREATE_GRAPHICS_DESCRIPTOR_WRITE_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <variant>
#include <vector>

#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/descriptor_set.hpp>
#include <terreate/graphics/descriptor_set_layout.hpp>
#include <terreate/graphics/image_view.hpp>
#include <terreate/graphics/sampler.hpp>

namespace terreate::graphics {

struct DescriptorBufferWriteElement {
  const Buffer *buffer = nullptr;
  vk::DeviceSize offset = 0;
  vk::DeviceSize range = 0;
};
struct DescriptorImageWriteElement {
  const Image *image = nullptr;
  const ImageView *view = nullptr;
  const Sampler *sampler = nullptr;
  vk::ImageLayout image_layout = vk::ImageLayout::eUndefined;
};
struct DescriptorSamplerWriteElement {
  const Sampler *sampler = nullptr;
};
using DescriptorWritePayload = std::variant<std::vector<DescriptorBufferWriteElement>,
                                            std::vector<DescriptorImageWriteElement>,
                                            std::vector<DescriptorSamplerWriteElement>>;
struct DescriptorWriteDescription {
  std::uint32_t binding = 0;
  std::uint32_t first_array_element = 0;
  vk::DescriptorType descriptor_type = static_cast<vk::DescriptorType>(VK_DESCRIPTOR_TYPE_MAX_ENUM);
  DescriptorWritePayload elements{};
};
enum class DescriptorUpdateError : std::uint8_t {
  invalid_device = 1,
  invalid_set,
  invalid_layout,
  layout_mismatch,
  device_mismatch,
  invalid_write_count,
  invalid_binding,
  descriptor_type_mismatch,
  invalid_payload,
  invalid_array_range,
  invalid_resource,
  invalid_buffer_range,
  invalid_image_layout,
  unsupported_configuration,
  allocation_failed
};
[[nodiscard]] const std::error_category &descriptor_update_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(DescriptorUpdateError error) noexcept;

/// Pure CPU, allocation-free count check. Failure has code only (empty text).
/// A representable count is not a guarantee that storage can be allocated.
[[nodiscard]] auto validateDescriptorWriteCount(std::size_t count) noexcept
    -> terreate::Result<void>;

/// Fully preflights eight core descriptor types, then synchronously invokes
/// vkUpdateDescriptorSets exactly once, in caller order, without copies.
/// Supply the ORIGINAL live layout and a live, unfreed/unreset set. Cached
/// handles cannot prove historical identity or native liveness. All wrappers
/// and descriptions must survive this call; native resources/parents must
/// survive descriptor use under Vulkan's application-owned lifetime rules.
/// Caller serializes conflicting host access, completes GPU use before ordinary
/// updates, and handles invalidation of recording/executable command buffers.
/// No ownership retention, synchronization, transitions, scheduling or waits.
/// Color-only views; storage requires eIdentity components; combined samplers
/// must use normalized coordinates. Sampled/combined allow General or
/// ShaderReadOnlyOptimal, storage only General, declared for access time.
/// Dynamic buffers require finite range; non-dynamic VK_WHOLE_SIZE is preserved.
[[nodiscard]] auto updateDescriptorSet(const Device &device,
                                       const DescriptorSetLayout &original_live_layout,
                                       const DescriptorSet &live_set,
                                       std::span<const DescriptorWriteDescription> writes)
    -> terreate::Result<void>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::DescriptorUpdateError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_DESCRIPTOR_WRITE_HPP

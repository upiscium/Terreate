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
// ---- Builders for the common one-element write ----
//
// Each builds a `DescriptorWriteDescription` with `first_array_element` 0 and exactly one element,
// for the descriptor type its name states (the type is never inferred from the layout:
// `updateDescriptorSet` still checks it against the layout and reports `descriptor_type_mismatch`).
// They only build a value: nothing is recorded or called; an allocation failure is a
// `std::bad_alloc` thrown by the vector the payload holds. The resources are borrowed, as in the
// struct, and must outlive `updateDescriptorSet`. Arrays of descriptors and a nonzero first array
// element are written with the struct.

/// A buffer write of any buffer descriptor type (the dynamic ones too). `range` defaults to the
/// whole buffer from `offset` (VK_WHOLE_SIZE; a dynamic buffer needs a finite range).
[[nodiscard]] inline DescriptorWriteDescription
bufferWrite(std::uint32_t binding, vk::DescriptorType type, const Buffer &buffer,
            vk::DeviceSize offset = 0, vk::DeviceSize range = vk::WholeSize) {
  return {binding, 0, type,
          std::vector<DescriptorBufferWriteElement>{
              DescriptorBufferWriteElement{&buffer, offset, range}}};
}
[[nodiscard]] inline DescriptorWriteDescription
uniformBufferWrite(std::uint32_t binding, const Buffer &buffer, vk::DeviceSize offset = 0,
                   vk::DeviceSize range = vk::WholeSize) {
  return bufferWrite(binding, vk::DescriptorType::eUniformBuffer, buffer, offset, range);
}
[[nodiscard]] inline DescriptorWriteDescription
storageBufferWrite(std::uint32_t binding, const Buffer &buffer, vk::DeviceSize offset = 0,
                   vk::DeviceSize range = vk::WholeSize) {
  return bufferWrite(binding, vk::DescriptorType::eStorageBuffer, buffer, offset, range);
}

/// A combined image sampler: the image, its view, the sampler and the layout the image is in when
/// the descriptor is accessed.
[[nodiscard]] inline DescriptorWriteDescription
combinedImageSamplerWrite(std::uint32_t binding, const Image &image, const ImageView &view,
                          const Sampler &sampler, vk::ImageLayout image_layout) {
  return {binding, 0, vk::DescriptorType::eCombinedImageSampler,
          std::vector<DescriptorImageWriteElement>{
              DescriptorImageWriteElement{&image, &view, &sampler, image_layout}}};
}
/// A sampled image (no sampler).
[[nodiscard]] inline DescriptorWriteDescription sampledImageWrite(std::uint32_t binding,
                                                                  const Image &image,
                                                                  const ImageView &view,
                                                                  vk::ImageLayout image_layout) {
  return {binding, 0, vk::DescriptorType::eSampledImage,
          std::vector<DescriptorImageWriteElement>{
              DescriptorImageWriteElement{&image, &view, nullptr, image_layout}}};
}
/// A storage image, accessed in the General layout.
[[nodiscard]] inline DescriptorWriteDescription
storageImageWrite(std::uint32_t binding, const Image &image, const ImageView &view) {
  return {binding, 0, vk::DescriptorType::eStorageImage,
          std::vector<DescriptorImageWriteElement>{
              DescriptorImageWriteElement{&image, &view, nullptr, vk::ImageLayout::eGeneral}}};
}
/// A standalone sampler.
[[nodiscard]] inline DescriptorWriteDescription samplerWrite(std::uint32_t binding,
                                                             const Sampler &sampler) {
  return {binding, 0, vk::DescriptorType::eSampler,
          std::vector<DescriptorSamplerWriteElement>{DescriptorSamplerWriteElement{&sampler}}};
}

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

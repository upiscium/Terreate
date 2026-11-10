#include <terreate/graphics/descriptor_write.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

namespace terreate::graphics {
namespace {
class DescriptorUpdateErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.descriptor_update"; }
  std::string message(int value) const override {
    switch (static_cast<DescriptorUpdateError>(value)) {
    case DescriptorUpdateError::invalid_device:
      return "invalid descriptor update Device";
    case DescriptorUpdateError::invalid_set:
      return "invalid descriptor set";
    case DescriptorUpdateError::invalid_layout:
      return "invalid original live layout";
    case DescriptorUpdateError::layout_mismatch:
      return "descriptor layout identity mismatch";
    case DescriptorUpdateError::device_mismatch:
      return "descriptor Device identity mismatch";
    case DescriptorUpdateError::invalid_write_count:
      return "empty or unrepresentable write count";
    case DescriptorUpdateError::invalid_binding:
      return "descriptor binding not present";
    case DescriptorUpdateError::descriptor_type_mismatch:
      return "descriptor type mismatch";
    case DescriptorUpdateError::invalid_payload:
      return "invalid typed descriptor payload";
    case DescriptorUpdateError::invalid_array_range:
      return "descriptor array range exceeds binding";
    case DescriptorUpdateError::invalid_resource:
      return "invalid descriptor resource";
    case DescriptorUpdateError::invalid_buffer_range:
      return "invalid descriptor buffer range";
    case DescriptorUpdateError::invalid_image_layout:
      return "unsupported declared image layout";
    case DescriptorUpdateError::unsupported_configuration:
      return "unsupported descriptor configuration";
    case DescriptorUpdateError::allocation_failed:
      return "descriptor temporary storage allocation failed";
    }
    return "unknown descriptor update error";
  }
};
auto failure(std::error_code code, const char *detail) {
  try {
    return std::unexpected(terreate::Error{code, "updateDescriptorSet", detail});
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{code});
  }
}
auto fail(DescriptorUpdateError error, const char *detail) {
  return failure(make_error_code(error), detail);
}
enum class PayloadKind : std::uint8_t { buffer, image, sampler, unsupported };
PayloadKind kind(vk::DescriptorType type) noexcept {
  switch (type) {
  case vk::DescriptorType::eUniformBuffer:
  case vk::DescriptorType::eStorageBuffer:
  case vk::DescriptorType::eUniformBufferDynamic:
  case vk::DescriptorType::eStorageBufferDynamic:
    return PayloadKind::buffer;
  case vk::DescriptorType::eSampledImage:
  case vk::DescriptorType::eStorageImage:
  case vk::DescriptorType::eCombinedImageSampler:
    return PayloadKind::image;
  case vk::DescriptorType::eSampler:
    return PayloadKind::sampler;
  default:
    return PayloadKind::unsupported;
  }
}
auto validateSampler(const Device &device, const Sampler *sampler) -> terreate::Result<void> {
  if (!sampler || !sampler->valid() || !sampler->configuration() || !sampler->nativeHandle())
    return fail(DescriptorUpdateError::invalid_resource, "live Sampler required");
  if (sampler->nativeDevice() != device.nativeHandle())
    return fail(DescriptorUpdateError::device_mismatch, "Sampler belongs to another Device");
  return {};
}
auto validateBuffer(const Device &device, vk::DescriptorType type,
                    const DescriptorBufferWriteElement &element) -> terreate::Result<void> {
  const auto *buffer = element.buffer;
  if (!buffer || !buffer->valid() || !buffer->configuration() || !buffer->nativeHandle())
    return fail(DescriptorUpdateError::invalid_resource, "live allocated Buffer required");
  if (buffer->nativeDevice() != device.nativeHandle())
    return fail(DescriptorUpdateError::device_mismatch, "Buffer belongs to another Device");
  const bool uniform = type == vk::DescriptorType::eUniformBuffer ||
                       type == vk::DescriptorType::eUniformBufferDynamic;
  const bool dynamic = type == vk::DescriptorType::eUniformBufferDynamic ||
                       type == vk::DescriptorType::eStorageBufferDynamic;
  const auto &config = *buffer->configuration();
  const auto usage =
      uniform ? vk::BufferUsageFlagBits::eUniformBuffer : vk::BufferUsageFlagBits::eStorageBuffer;
  if (!(config.usage & usage))
    return fail(DescriptorUpdateError::unsupported_configuration, "Buffer lacks descriptor usage");
  const auto &limits = device.plan()->capabilities().properties.limits;
  const auto alignment =
      uniform ? limits.minUniformBufferOffsetAlignment : limits.minStorageBufferOffsetAlignment;
  const auto maximum = uniform ? limits.maxUniformBufferRange : limits.maxStorageBufferRange;
  if (element.offset >= config.size || element.range == 0 ||
      (alignment != 0 && element.offset % alignment != 0) ||
      (dynamic && element.range == VK_WHOLE_SIZE))
    return fail(DescriptorUpdateError::invalid_buffer_range, "invalid offset, alignment or range");
  const auto remaining = config.size - element.offset;
  const auto effective = element.range == VK_WHOLE_SIZE ? remaining : element.range;
  if (effective > remaining || effective > maximum)
    return fail(DescriptorUpdateError::invalid_buffer_range, "Buffer range exceeds size or limit");
  return {};
}
auto validateImage(const Device &device, vk::DescriptorType type,
                   const DescriptorImageWriteElement &element) -> terreate::Result<void> {
  const bool combined = type == vk::DescriptorType::eCombinedImageSampler;
  const bool storage = type == vk::DescriptorType::eStorageImage;
  if (!combined && element.sampler)
    return fail(DescriptorUpdateError::invalid_payload, "non-combined image requires null sampler");
  const auto *image = element.image;
  const auto *view = element.view;
  if (!image || !view || !image->valid() || !view->valid() || !image->configuration() ||
      !view->configuration() || !image->nativeHandle() || !view->nativeHandle())
    return fail(DescriptorUpdateError::invalid_resource, "live allocated Image and View required");
  if (image->nativeDevice() != device.nativeHandle() ||
      view->nativeDevice() != device.nativeHandle())
    return fail(DescriptorUpdateError::device_mismatch, "Image or View belongs to another Device");
  if (!view->correlatedWith(*image))
    return fail(DescriptorUpdateError::invalid_resource, "View does not belong to supplied Image");
  const auto &effective = view->configuration()->effective;
  if (effective.subresource_range.aspectMask != vk::ImageAspectFlagBits::eColor)
    return fail(DescriptorUpdateError::unsupported_configuration, "only color views supported");
  const auto usage = storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
  if (!(image->configuration()->usage & usage))
    return fail(DescriptorUpdateError::unsupported_configuration, "Image lacks descriptor usage");
  const auto &components = effective.components;
  if (storage && (components.r != vk::ComponentSwizzle::eIdentity ||
                  components.g != vk::ComponentSwizzle::eIdentity ||
                  components.b != vk::ComponentSwizzle::eIdentity ||
                  components.a != vk::ComponentSwizzle::eIdentity))
    return fail(DescriptorUpdateError::unsupported_configuration,
                "storage requires eIdentity swizzle");
  if (combined) {
    if (auto result = validateSampler(device, element.sampler); !result)
      return result;
    if (element.sampler->configuration()->effective.unnormalized_coordinates)
      return fail(DescriptorUpdateError::unsupported_configuration,
                  "combined requires normalized sampler");
  }
  if (element.image_layout != vk::ImageLayout::eGeneral &&
      (storage || element.image_layout != vk::ImageLayout::eShaderReadOnlyOptimal))
    return fail(DescriptorUpdateError::invalid_image_layout,
                "declared access layout outside allowlist");
  return {};
}
template <typename T> bool addCount(std::size_t &total, std::size_t count) noexcept {
  const auto maximum = std::vector<T>{}.max_size();
  if (count > maximum || total > maximum - count)
    return false;
  total += count;
  return true;
}
} // namespace

const std::error_category &descriptor_update_error_category() noexcept {
  static const DescriptorUpdateErrorCategory category;
  return category;
}
std::error_code make_error_code(DescriptorUpdateError error) noexcept {
  return {static_cast<int>(error), descriptor_update_error_category()};
}
auto validateDescriptorWriteCount(std::size_t count) noexcept -> terreate::Result<void> {
  if (count == 0 || count > std::numeric_limits<std::uint32_t>::max())
    return std::unexpected(
        terreate::Error{make_error_code(DescriptorUpdateError::invalid_write_count)});
  return {};
}
auto updateDescriptorSet(const Device &device, const DescriptorSetLayout &original_live_layout,
                         const DescriptorSet &live_set,
                         std::span<const DescriptorWriteDescription> writes)
    -> terreate::Result<void> {
  if (!device.valid() || !device.plan() || !device.nativeHandle())
    return fail(DescriptorUpdateError::invalid_device, "live Device with retained plan required");
  if (!original_live_layout.valid() || !original_live_layout.configuration() ||
      !original_live_layout.nativeHandle() || !original_live_layout.nativeDevice())
    return fail(DescriptorUpdateError::invalid_layout, "original live layout required");
  if (!live_set.valid() || !live_set.nativeHandle() || !live_set.nativeDevice() ||
      !live_set.nativeLayout() || !live_set.nativePool())
    return fail(DescriptorUpdateError::invalid_set, "live allocated set required");
  if (original_live_layout.nativeDevice() != device.nativeHandle() ||
      live_set.nativeDevice() != device.nativeHandle())
    return fail(DescriptorUpdateError::device_mismatch, "set and layout must belong to Device");
  if (original_live_layout.nativeHandle() != live_set.nativeLayout())
    return fail(DescriptorUpdateError::layout_mismatch, "original allocation-time layout required");
  if (auto count = validateDescriptorWriteCount(writes.size()); !count)
    return failure(count.error().code(), "nonempty representable write batch required");

  std::size_t buffer_count = 0, image_count = 0, write_count = 0;
  if (!addCount<VkWriteDescriptorSet>(write_count, writes.size()))
    return fail(DescriptorUpdateError::allocation_failed, "native write capacity exceeded");
  const auto &bindings = original_live_layout.configuration()->effective.bindings;
  // No temporary allocations until ALL typed payloads and resources pass.
  for (const auto &write : writes) {
    const auto payload_kind = kind(write.descriptor_type);
    if (payload_kind == PayloadKind::unsupported)
      return fail(DescriptorUpdateError::unsupported_configuration,
                  "descriptor type not supported");
    const auto binding =
        std::ranges::find(bindings, write.binding, &DescriptorSetLayoutBindingDescription::binding);
    if (binding == bindings.end())
      return fail(DescriptorUpdateError::invalid_binding, "binding absent from live layout");
    if (binding->descriptor_type != write.descriptor_type)
      return fail(DescriptorUpdateError::descriptor_type_mismatch,
                  "type differs from layout binding");
    const auto *buffers = std::get_if<std::vector<DescriptorBufferWriteElement>>(&write.elements);
    const auto *images = std::get_if<std::vector<DescriptorImageWriteElement>>(&write.elements);
    const auto *samplers = std::get_if<std::vector<DescriptorSamplerWriteElement>>(&write.elements);
    if ((payload_kind == PayloadKind::buffer && !buffers) ||
        (payload_kind == PayloadKind::image && !images) ||
        (payload_kind == PayloadKind::sampler && !samplers))
      return fail(DescriptorUpdateError::invalid_payload,
                  "type requires matching payload alternative");
    const auto count = buffers ? buffers->size() : (images ? images->size() : samplers->size());
    if (auto validation = validateDescriptorWriteCount(count); !validation)
      return failure(validation.error().code(), "nonempty representable element count required");
    if (write.first_array_element >= binding->descriptor_count ||
        count > binding->descriptor_count - write.first_array_element)
      return fail(DescriptorUpdateError::invalid_array_range, "write must stay inside one binding");
    if (buffers) {
      for (const auto &element : *buffers)
        if (auto validation = validateBuffer(device, write.descriptor_type, element); !validation)
          return validation;
      if (!addCount<VkDescriptorBufferInfo>(buffer_count, count))
        return fail(DescriptorUpdateError::allocation_failed, "native buffer capacity exceeded");
    } else {
      if (images) {
        for (const auto &element : *images)
          if (auto validation = validateImage(device, write.descriptor_type, element); !validation)
            return validation;
      } else {
        for (const auto &element : *samplers)
          if (auto validation = validateSampler(device, element.sampler); !validation)
            return validation;
      }
      if (!addCount<VkDescriptorImageInfo>(image_count, count))
        return fail(DescriptorUpdateError::allocation_failed, "native image capacity exceeded");
    }
  }
  try {
    std::vector<VkDescriptorBufferInfo> buffers(buffer_count);
    std::vector<VkDescriptorImageInfo> images(image_count);
    std::vector<VkWriteDescriptorSet> native_writes(write_count);
    std::size_t buffer_index = 0, image_index = 0, write_index = 0;
    // Storage is final: no resizing after pointers are published below.
    for (const auto &write : writes) {
      auto &native = native_writes[write_index++];
      native.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      native.dstSet = live_set.nativeDescriptorSet();
      native.dstBinding = write.binding;
      native.dstArrayElement = write.first_array_element;
      native.descriptorType = static_cast<VkDescriptorType>(write.descriptor_type);
      if (const auto *elements =
              std::get_if<std::vector<DescriptorBufferWriteElement>>(&write.elements)) {
        native.descriptorCount = static_cast<std::uint32_t>(elements->size());
        native.pBufferInfo = buffers.data() + buffer_index;
        for (const auto &element : *elements)
          buffers[buffer_index++] = {element.buffer->nativeBuffer(), element.offset, element.range};
      } else {
        native.pImageInfo = images.data() + image_index;
        if (const auto *image_elements =
                std::get_if<std::vector<DescriptorImageWriteElement>>(&write.elements)) {
          native.descriptorCount = static_cast<std::uint32_t>(image_elements->size());
          for (const auto &element : *image_elements)
            images[image_index++] = {
                element.sampler ? element.sampler->nativeSampler() : VK_NULL_HANDLE,
                element.view->nativeImageView(), static_cast<VkImageLayout>(element.image_layout)};
        } else {
          const auto &sampler_elements =
              std::get<std::vector<DescriptorSamplerWriteElement>>(write.elements);
          native.descriptorCount = static_cast<std::uint32_t>(sampler_elements.size());
          for (const auto &element : sampler_elements)
            images[image_index++] = {element.sampler->nativeSampler(), VK_NULL_HANDLE,
                                     VK_IMAGE_LAYOUT_UNDEFINED};
        }
      }
    }
    vkUpdateDescriptorSets(static_cast<VkDevice>(device.nativeHandle()),
                           static_cast<std::uint32_t>(native_writes.size()), native_writes.data(),
                           0, nullptr);
    return {};
  } catch (const std::bad_alloc &) {
    return fail(DescriptorUpdateError::allocation_failed,
                "native descriptor storage allocation failed");
  } catch (const std::length_error &) {
    return fail(DescriptorUpdateError::allocation_failed,
                "native descriptor storage capacity exceeded");
  }
}
} // namespace terreate::graphics

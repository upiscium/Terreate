#include <terreate/graphics/pipeline_commands.hpp>

#include <limits>
#include <new>
#include <string>
#include <vector>

namespace terreate::graphics {
namespace {
class PipelineCommandErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.pipeline_commands"; }
  std::string message(int value) const override {
    switch (static_cast<PipelineCommandError>(value)) {
    case PipelineCommandError::invalid_device:
      return "invalid logical Device";
    case PipelineCommandError::invalid_command_buffer:
      return "invalid CommandBuffer";
    case PipelineCommandError::not_recording:
      return "CommandBuffer is not recording";
    case PipelineCommandError::parent_mismatch:
      return "object belongs to another logical Device";
    case PipelineCommandError::unsupported_queue:
      return "command queue family does not support the bind point";
    case PipelineCommandError::invalid_pipeline:
      return "invalid pipeline";
    case PipelineCommandError::invalid_layout:
      return "invalid PipelineLayout";
    case PipelineCommandError::invalid_bind_point:
      return "bind point must be compute or graphics";
    case PipelineCommandError::invalid_descriptor_set:
      return "invalid DescriptorSet";
    case PipelineCommandError::invalid_set_range:
      return "descriptor set range outside the PipelineLayout";
    case PipelineCommandError::set_layout_mismatch:
      return "DescriptorSet layout differs from the PipelineLayout set layout";
    case PipelineCommandError::dynamic_offset_count_mismatch:
      return "dynamic offset count does not match dynamic descriptors";
    case PipelineCommandError::dynamic_offset_misaligned:
      return "dynamic offset violates minimum buffer offset alignment";
    case PipelineCommandError::invalid_push_constants:
      return "push constant update not covered by PipelineLayout ranges";
    case PipelineCommandError::invalid_dispatch:
      return "dispatch group count exceeds maxComputeWorkGroupCount";
    case PipelineCommandError::allocation_failed:
      return "temporary native storage allocation failed";
    }
    return "unknown pipeline command error";
  }
};

terreate::Result<void> fail(PipelineCommandError error, const char *name) {
  try {
    return std::unexpected(terreate::Error{make_error_code(error), name,
                                           "recording rejected before native Vulkan call"});
  } catch (const std::bad_alloc &) {
    // Code-only Error construction does not allocate while handling exhaustion.
    return std::unexpected(terreate::Error{make_error_code(error)});
  }
}

terreate::Result<void> withContext(terreate::Result<void> result, const char *name) {
  if (result)
    return result;
  return fail(static_cast<PipelineCommandError>(result.error().code().value()), name);
}

bool dynamicType(vk::DescriptorType type) noexcept {
  return type == vk::DescriptorType::eUniformBufferDynamic ||
         type == vk::DescriptorType::eStorageBufferDynamic;
}

bool aligned(std::uint32_t offset, vk::DeviceSize alignment) noexcept {
  return alignment == 0 || offset % alignment == 0;
}

constexpr VkShaderStageFlags core_stages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

vk::QueueFlags requiredQueueFlags(vk::PipelineBindPoint bind_point) noexcept {
  return bind_point == vk::PipelineBindPoint::eCompute ? vk::QueueFlagBits::eCompute
                                                       : vk::QueueFlagBits::eGraphics;
}

// Shared preconditions; on success the queue family supports `required`.
terreate::Result<void> validateRecording(const Device &device, const CommandBuffer &command,
                                         vk::QueueFlags required, const char *name) {
  if (!device.valid() || !device.plan())
    return fail(PipelineCommandError::invalid_device, name);
  if (!command.valid())
    return fail(PipelineCommandError::invalid_command_buffer, name);
  if (command.nativeDevice() != device.nativeHandle())
    return fail(PipelineCommandError::parent_mismatch, name);
  if (command.state() != CommandBufferState::recording)
    return fail(PipelineCommandError::not_recording, name);
  const auto &families = device.plan()->capabilities().queue_families;
  const auto family = command.queueFamilyIndex();
  if (family >= families.size() || (families[family].queueFlags & required) != required)
    return fail(PipelineCommandError::unsupported_queue, name);
  return {};
}

const vk::PhysicalDeviceLimits &limitsOf(const Device &device) noexcept {
  return device.plan()->capabilities().properties.limits;
}

VkCommandBuffer nativeCommand(const CommandBuffer &command) noexcept {
  return static_cast<VkCommandBuffer>(command.nativeHandle());
}
} // namespace

const std::error_category &pipeline_command_error_category() noexcept {
  static const PipelineCommandErrorCategory category;
  return category;
}
std::error_code make_error_code(PipelineCommandError error) noexcept {
  return {static_cast<int>(error), pipeline_command_error_category()};
}

auto validateDynamicOffsets(std::span<const DescriptorSetLayoutDescription> set_layouts,
                            std::span<const std::uint32_t> dynamic_offsets,
                            const vk::PhysicalDeviceLimits &limits) -> terreate::Result<void> {
  constexpr auto name = "validateDynamicOffsets";
  std::uint64_t expected = 0;
  for (const auto &layout : set_layouts)
    for (const auto &binding : layout.bindings)
      if (dynamicType(binding.descriptor_type))
        expected += binding.descriptor_count;
  if (expected != dynamic_offsets.size())
    return fail(PipelineCommandError::dynamic_offset_count_mismatch, name);

  // Vulkan orders offsets by binding number, not by description order.
  // Repeated minimum selection avoids temporary storage; binding numbers are
  // unique per DescriptorSetLayout.
  std::size_t index = 0;
  for (const auto &layout : set_layouts) {
    bool has_previous = false;
    std::uint32_t previous = 0;
    for (;;) {
      const DescriptorSetLayoutBindingDescription *next = nullptr;
      for (const auto &binding : layout.bindings) {
        if (!dynamicType(binding.descriptor_type) || (has_previous && binding.binding <= previous))
          continue;
        if (!next || binding.binding < next->binding)
          next = &binding;
      }
      if (!next)
        break;
      const auto alignment = next->descriptor_type == vk::DescriptorType::eUniformBufferDynamic
                                 ? limits.minUniformBufferOffsetAlignment
                                 : limits.minStorageBufferOffsetAlignment;
      for (std::uint32_t element = 0; element < next->descriptor_count; ++element)
        if (!aligned(dynamic_offsets[index++], alignment))
          return fail(PipelineCommandError::dynamic_offset_misaligned, name);
      has_previous = true;
      previous = next->binding;
    }
  }
  return {};
}

auto validatePushConstants(const PipelineLayoutDescription &layout, vk::ShaderStageFlags stages,
                           std::uint32_t offset, std::size_t size,
                           std::uint32_t max_push_constants_size) -> terreate::Result<void> {
  constexpr auto name = "validatePushConstants";
  const auto requested = static_cast<VkShaderStageFlags>(stages);
  if (requested == 0 || (requested & ~core_stages) != 0 || size == 0 || offset % 4 != 0 ||
      size % 4 != 0 || offset >= max_push_constants_size || size > max_push_constants_size - offset)
    return fail(PipelineCommandError::invalid_push_constants, name);
  const auto end = offset + static_cast<std::uint32_t>(size);
  for (std::uint32_t byte = offset; byte < end; byte += 4) {
    VkShaderStageFlags covered = 0;
    for (const auto &range : layout.push_constant_ranges) {
      if (byte < range.offset || byte - range.offset >= range.size)
        continue;
      const auto range_stages = static_cast<VkShaderStageFlags>(range.stageFlags);
      // Every range overlapping a byte must be updated for all of its stages.
      if ((range_stages & ~requested) != 0)
        return fail(PipelineCommandError::invalid_push_constants, name);
      covered |= range_stages;
    }
    // Every byte must be covered for every requested stage.
    if ((requested & ~covered) != 0)
      return fail(PipelineCommandError::invalid_push_constants, name);
  }
  return {};
}

auto validateDispatch(std::uint32_t group_count_x, std::uint32_t group_count_y,
                      std::uint32_t group_count_z, const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void> {
  if (group_count_x > limits.maxComputeWorkGroupCount[0] ||
      group_count_y > limits.maxComputeWorkGroupCount[1] ||
      group_count_z > limits.maxComputeWorkGroupCount[2])
    return fail(PipelineCommandError::invalid_dispatch, "validateDispatch");
  return {};
}

auto recordBindPipeline(const Device &device, const CommandBuffer &command,
                        const ComputePipeline &pipeline) -> terreate::Result<void> {
  constexpr auto name = "recordBindPipeline";
  if (auto valid = validateRecording(device, command, vk::QueueFlagBits::eCompute, name); !valid)
    return valid;
  if (!pipeline.valid())
    return fail(PipelineCommandError::invalid_pipeline, name);
  if (!pipeline.correlatedWith(device))
    return fail(PipelineCommandError::parent_mismatch, name);
  vkCmdBindPipeline(nativeCommand(command), VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline.nativePipeline());
  return {};
}

auto recordBindPipeline(const Device &device, const CommandBuffer &command,
                        const GraphicsPipeline &pipeline) -> terreate::Result<void> {
  constexpr auto name = "recordBindPipeline";
  if (auto valid = validateRecording(device, command, vk::QueueFlagBits::eGraphics, name); !valid)
    return valid;
  if (!pipeline.valid())
    return fail(PipelineCommandError::invalid_pipeline, name);
  if (!pipeline.correlatedWith(device))
    return fail(PipelineCommandError::parent_mismatch, name);
  vkCmdBindPipeline(nativeCommand(command), VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline.nativePipeline());
  return {};
}

auto recordBindDescriptorSets(const Device &device, const CommandBuffer &command,
                              vk::PipelineBindPoint bind_point, const PipelineLayout &layout,
                              std::uint32_t first_set, std::span<const DescriptorSet *const> sets,
                              std::span<const std::uint32_t> dynamic_offsets)
    -> terreate::Result<void> {
  constexpr auto name = "recordBindDescriptorSets";
  if (bind_point != vk::PipelineBindPoint::eCompute &&
      bind_point != vk::PipelineBindPoint::eGraphics)
    return fail(PipelineCommandError::invalid_bind_point, name);
  if (auto valid = validateRecording(device, command, requiredQueueFlags(bind_point), name); !valid)
    return valid;
  if (!layout.valid() || !layout.configuration())
    return fail(PipelineCommandError::invalid_layout, name);
  if (!layout.correlatedWith(device))
    return fail(PipelineCommandError::parent_mismatch, name);
  const auto &configuration = *layout.configuration();
  const auto &handles = configuration.set_layout_handles;
  if (sets.empty() || first_set > handles.size() || sets.size() > handles.size() - first_set)
    return fail(PipelineCommandError::invalid_set_range, name);
  const auto native_device = static_cast<VkDevice>(device.nativeHandle());
  for (std::size_t index = 0; index < sets.size(); ++index) {
    const auto *set = sets[index];
    if (!set || !set->valid())
      return fail(PipelineCommandError::invalid_descriptor_set, name);
    if (set->nativeVkDevice() != native_device)
      return fail(PipelineCommandError::parent_mismatch, name);
    if (set->nativeDescriptorSetLayout() != handles[first_set + index])
      return fail(PipelineCommandError::set_layout_mismatch, name);
  }
  const std::span<const DescriptorSetLayoutDescription> bound{
      configuration.set_layout_descriptions.data() + first_set, sets.size()};
  if (auto offsets = validateDynamicOffsets(bound, dynamic_offsets, configuration.limits); !offsets)
    return withContext(std::move(offsets), name);

  std::vector<VkDescriptorSet> native_sets;
  try {
    native_sets.reserve(sets.size());
  } catch (const std::bad_alloc &) {
    return std::unexpected(
        terreate::Error{make_error_code(PipelineCommandError::allocation_failed)});
  }
  for (const auto *set : sets)
    native_sets.push_back(set->nativeDescriptorSet());
  vkCmdBindDescriptorSets(nativeCommand(command), static_cast<VkPipelineBindPoint>(bind_point),
                          layout.nativePipelineLayout(), first_set,
                          static_cast<std::uint32_t>(native_sets.size()), native_sets.data(),
                          static_cast<std::uint32_t>(dynamic_offsets.size()),
                          dynamic_offsets.empty() ? nullptr : dynamic_offsets.data());
  return {};
}

auto recordPushConstants(const Device &device, const CommandBuffer &command,
                         const PipelineLayout &layout, vk::ShaderStageFlags stages,
                         std::uint32_t offset, std::span<const std::byte> data)
    -> terreate::Result<void> {
  constexpr auto name = "recordPushConstants";
  // Push constants are valid on any queue family; only Device/recording checks.
  if (auto valid = validateRecording(device, command, vk::QueueFlags{}, name); !valid)
    return valid;
  if (!layout.valid() || !layout.configuration())
    return fail(PipelineCommandError::invalid_layout, name);
  if (!layout.correlatedWith(device))
    return fail(PipelineCommandError::parent_mismatch, name);
  const auto &configuration = *layout.configuration();
  if (auto pushed = validatePushConstants(configuration.effective, stages, offset, data.size(),
                                          configuration.max_push_constants_size);
      !pushed)
    return withContext(std::move(pushed), name);
  vkCmdPushConstants(nativeCommand(command), layout.nativePipelineLayout(),
                     static_cast<VkShaderStageFlags>(stages), offset,
                     static_cast<std::uint32_t>(data.size()), data.data());
  return {};
}

auto recordDispatch(const Device &device, const CommandBuffer &command, std::uint32_t group_count_x,
                    std::uint32_t group_count_y, std::uint32_t group_count_z)
    -> terreate::Result<void> {
  constexpr auto name = "recordDispatch";
  if (auto valid = validateRecording(device, command, vk::QueueFlagBits::eCompute, name); !valid)
    return valid;
  if (auto counts = validateDispatch(group_count_x, group_count_y, group_count_z, limitsOf(device));
      !counts)
    return withContext(std::move(counts), name);
  vkCmdDispatch(nativeCommand(command), group_count_x, group_count_y, group_count_z);
  return {};
}
} // namespace terreate::graphics

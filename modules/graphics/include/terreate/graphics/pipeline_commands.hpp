#ifndef TERREATE_GRAPHICS_PIPELINE_COMMANDS_HPP
#define TERREATE_GRAPHICS_PIPELINE_COMMANDS_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>

#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/compute_pipeline.hpp>
#include <terreate/graphics/descriptor_set.hpp>
#include <terreate/graphics/descriptor_set_layout.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/graphics_pipeline.hpp>
#include <terreate/graphics/pipeline_layout.hpp>

namespace terreate::graphics {

enum class PipelineCommandError : std::uint8_t {
  invalid_device = 1,
  invalid_command_buffer,
  not_recording,
  parent_mismatch,
  unsupported_queue,
  invalid_pipeline,
  invalid_layout,
  invalid_bind_point,
  invalid_descriptor_set,
  invalid_set_range,
  set_layout_mismatch,
  dynamic_offset_count_mismatch,
  dynamic_offset_misaligned,
  invalid_push_constants,
  invalid_dispatch,
  allocation_failed,
};

[[nodiscard]] const std::error_category &pipeline_command_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(PipelineCommandError error) noexcept;

/// Pure CPU validation of dynamic offsets for consecutive sets, in Vulkan order:
/// set by set, then by ascending binding number, then by array element. Checks
/// the count and the min{Uniform,Storage}BufferOffsetAlignment of each offset.
/// Whether offset + bound range stays inside the written Buffer is NOT known
/// here (descriptor contents are not tracked) and remains caller responsibility.
[[nodiscard]] auto
validateDynamicOffsets(std::span<const DescriptorSetLayoutDescription> set_layouts,
                       std::span<const std::uint32_t> dynamic_offsets,
                       const vk::PhysicalDeviceLimits &limits) -> terreate::Result<void>;

/// Pure CPU validation of a vkCmdPushConstants update against explicit layout
/// ranges: nonempty stages, 4-byte aligned nonempty data within
/// maxPushConstantsSize, every byte covered for every requested stage, and the
/// requested stages include all stages of every range overlapping the update.
[[nodiscard]] auto validatePushConstants(const PipelineLayoutDescription &layout,
                                         vk::ShaderStageFlags stages, std::uint32_t offset,
                                         std::size_t size, std::uint32_t max_push_constants_size)
    -> terreate::Result<void>;

/// Pure CPU validation of dispatch group counts against maxComputeWorkGroupCount.
/// Zero counts are valid and record a no-op dispatch.
[[nodiscard]] auto validateDispatch(std::uint32_t group_count_x, std::uint32_t group_count_y,
                                    std::uint32_t group_count_z,
                                    const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void>;

/// Each recorder issues exactly one native vkCmd* call after validation, or
/// none on error. Requirements shared by all of them: a live Device with plan,
/// a live CommandBuffer of that Device in the recording state, and a command
/// queue family whose flags support the bind point. Nothing tracks the bound
/// pipeline, bound sets or pushed values; compatibility between bound state
/// and later dispatches/draws, resource lifetime, barriers, submission and
/// completion all remain application-owned. No implicit submit or wait.
[[nodiscard]] auto recordBindPipeline(const Device &device, const CommandBuffer &command,
                                      const ComputePipeline &pipeline) -> terreate::Result<void>;
[[nodiscard]] auto recordBindPipeline(const Device &device, const CommandBuffer &command,
                                      const GraphicsPipeline &pipeline) -> terreate::Result<void>;

/// Binds sets to [first_set, first_set + sets.size()) of `layout`, which must
/// have been created with those set layouts. A set is accepted only when its
/// allocation-time native layout handle equals the layout's handle at that set
/// number: identity, stricter than Vulkan's "identically defined" rule. Sets
/// and Pipeline objects are borrowed only during the call.
[[nodiscard]] auto recordBindDescriptorSets(const Device &device, const CommandBuffer &command,
                                            vk::PipelineBindPoint bind_point,
                                            const PipelineLayout &layout, std::uint32_t first_set,
                                            std::span<const DescriptorSet *const> sets,
                                            std::span<const std::uint32_t> dynamic_offsets)
    -> terreate::Result<void>;

/// Copies `data` into the command buffer through vkCmdPushConstants; the span
/// need only survive the call.
[[nodiscard]] auto recordPushConstants(const Device &device, const CommandBuffer &command,
                                       const PipelineLayout &layout, vk::ShaderStageFlags stages,
                                       std::uint32_t offset, std::span<const std::byte> data)
    -> terreate::Result<void>;

/// Requires a compute-capable queue family. A bound compute pipeline and
/// compatible descriptor/push-constant state are caller responsibilities.
[[nodiscard]] auto recordDispatch(const Device &device, const CommandBuffer &command,
                                  std::uint32_t group_count_x, std::uint32_t group_count_y,
                                  std::uint32_t group_count_z) -> terreate::Result<void>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::PipelineCommandError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_PIPELINE_COMMANDS_HPP

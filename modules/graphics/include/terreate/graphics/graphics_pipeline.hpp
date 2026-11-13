#ifndef TERREATE_GRAPHICS_GRAPHICS_PIPELINE_HPP
#define TERREATE_GRAPHICS_GRAPHICS_PIPELINE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <terreate/graphics/pipeline_layout.hpp>
#include <terreate/graphics/shader_module.hpp>

namespace terreate::graphics {

/// Library bound excluding the terminator, not a Vulkan limit.
inline constexpr std::size_t max_graphics_entry_point_bytes = 4096;

/// Pointer-free caller intent. Entry points are required, never inferred.
struct GraphicsPipelineDescription {
  std::string vertex_entry_point{};
  std::string fragment_entry_point{};
  std::vector<vk::VertexInputBindingDescription> vertex_bindings{};
  std::vector<vk::VertexInputAttributeDescription> vertex_attributes{};
  vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
  bool primitive_restart = false;
  vk::CullModeFlags cull_mode{};
  vk::FrontFace front_face = vk::FrontFace::eCounterClockwise;
  bool depth_test_enable = false;
  bool depth_write_enable = false;
  vk::CompareOp depth_compare_op = vk::CompareOp::eLess;
  std::vector<vk::Format> color_attachment_formats{};
  std::vector<vk::PipelineColorBlendAttachmentState> color_blend_attachments{};
  vk::Format depth_attachment_format = vk::Format::eUndefined;
  vk::Format stencil_attachment_format = vk::Format::eUndefined;
};

struct GraphicsPipelineConfiguration {
  GraphicsPipelineDescription requested{};
  GraphicsPipelineDescription effective{};
  vk::ShaderStageFlags stages =
      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
  vk::PhysicalDeviceLimits limits{};
};

enum class GraphicsPipelineError : std::uint8_t {
  invalid_device = 1,
  invalid_shader_module,
  invalid_layout,
  parent_mismatch,
  invalid_entry_point,
  invalid_description,
  unsupported_configuration,
  feature_not_enabled,
  native_null_handle,
  allocation_failed,
};

[[nodiscard]] const std::error_category &graphics_pipeline_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(GraphicsPipelineError error) noexcept;

/// Pure CPU structural/limit validation. Format features, shader semantics and
/// shader/layout compatibility remain the caller's responsibility.
[[nodiscard]] auto
validateGraphicsPipelineDescription(const GraphicsPipelineDescription &description,
                                    const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void>;

/// Exclusive pipeline owner. Device must outlive it; retain the layout through
/// pipeline use. Wrappers and shader modules are borrowed only during creation.
/// Native observers do not confer ownership or track liveness. Finish GPU use
/// and externally synchronize destruction/replacement; no hidden waits occur.
class GraphicsPipeline final {
public:
  GraphicsPipeline() = delete;
  GraphicsPipeline(const GraphicsPipeline &) = delete;
  GraphicsPipeline &operator=(const GraphicsPipeline &) = delete;
  GraphicsPipeline(GraphicsPipeline &&) noexcept;
  GraphicsPipeline &operator=(GraphicsPipeline &&) noexcept;
  ~GraphicsPipeline();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::Pipeline nativeHandle() const noexcept;
  [[nodiscard]] VkPipeline nativePipeline() const noexcept;
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  [[nodiscard]] VkDevice nativeVkDevice() const noexcept;
  [[nodiscard]] vk::PipelineLayout nativeLayout() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  [[nodiscard]] bool correlatedWith(const PipelineLayout &layout) const noexcept;
  /// Borrowed snapshot; null after moving. Reacquire after a move.
  [[nodiscard]] const GraphicsPipelineConfiguration *configuration() const noexcept;

private:
  struct Impl;
  explicit GraphicsPipeline(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createGraphicsPipeline(const Device &, const PipelineLayout &, const ShaderModule &,
                                     const ShaderModule &, const GraphicsPipelineDescription &)
      -> terreate::Result<GraphicsPipeline>;
};

/// One synchronous dynamic-rendering pipeline creation, without cache or
/// specialization. B0 fixes fill, line width 1, one sample, and dynamic viewport
/// and scissor (one each). No other dynamic states or extension chains are used.
/// dynamicRendering must already be explicitly enabled in the DevicePlan.
[[nodiscard]] auto createGraphicsPipeline(const Device &device, const PipelineLayout &layout,
                                          const ShaderModule &vertex_module,
                                          const ShaderModule &fragment_module,
                                          const GraphicsPipelineDescription &description)
    -> terreate::Result<GraphicsPipeline>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::GraphicsPipelineError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_GRAPHICS_PIPELINE_HPP

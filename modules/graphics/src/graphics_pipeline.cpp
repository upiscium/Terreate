#include <terreate/graphics/graphics_pipeline.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace terreate::graphics {
namespace {
class GraphicsPipelineErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.graphics_pipeline"; }
  std::string message(int value) const override {
    switch (static_cast<GraphicsPipelineError>(value)) {
    case GraphicsPipelineError::invalid_device:
      return "invalid GraphicsPipeline Device";
    case GraphicsPipelineError::invalid_shader_module:
      return "invalid GraphicsPipeline ShaderModule";
    case GraphicsPipelineError::invalid_layout:
      return "invalid GraphicsPipeline PipelineLayout";
    case GraphicsPipelineError::parent_mismatch:
      return "GraphicsPipeline parents belong to different Devices";
    case GraphicsPipelineError::invalid_entry_point:
      return "invalid graphics entry-point string";
    case GraphicsPipelineError::invalid_description:
      return "invalid GraphicsPipeline description";
    case GraphicsPipelineError::unsupported_configuration:
      return "unsupported GraphicsPipeline configuration";
    case GraphicsPipelineError::feature_not_enabled:
      return "required GraphicsPipeline feature is not enabled";
    case GraphicsPipelineError::native_null_handle:
      return "Vulkan returned null GraphicsPipeline on success";
    case GraphicsPipelineError::allocation_failed:
      return "GraphicsPipeline allocation failed";
    }
    return "unknown GraphicsPipeline error";
  }
};

auto fail(GraphicsPipelineError error) {
  return std::unexpected(terreate::Error{make_error_code(error)});
}

bool validEntryPoint(const std::string &name) {
  return !name.empty() && name.size() <= max_graphics_entry_point_bytes &&
         name.find('\0') == std::string::npos;
}

bool hasDepth(vk::Format format) {
  switch (format) {
  case vk::Format::eD16Unorm:
  case vk::Format::eX8D24UnormPack32:
  case vk::Format::eD32Sfloat:
  case vk::Format::eD16UnormS8Uint:
  case vk::Format::eD24UnormS8Uint:
  case vk::Format::eD32SfloatS8Uint:
    return true;
  default:
    return false;
  }
}

bool hasStencil(vk::Format format) {
  return format == vk::Format::eS8Uint || format == vk::Format::eD16UnormS8Uint ||
         format == vk::Format::eD24UnormS8Uint || format == vk::Format::eD32SfloatS8Uint;
}

bool colorFormat(vk::Format format) {
  return format != vk::Format::eUndefined && !hasDepth(format) && !hasStencil(format);
}

// Check both the native count conversion and byte multiplication before copying.
template <typename Native, typename Value>
bool boundedCount(const std::vector<Value> &values, std::uint32_t limit) {
  return values.size() <= limit &&
         values.size() <= std::numeric_limits<std::size_t>::max() / sizeof(Native);
}
} // namespace

const std::error_category &graphics_pipeline_error_category() noexcept {
  static const GraphicsPipelineErrorCategory category;
  return category;
}

std::error_code make_error_code(GraphicsPipelineError error) noexcept {
  return {static_cast<int>(error), graphics_pipeline_error_category()};
}

auto validateGraphicsPipelineDescription(const GraphicsPipelineDescription &description,
                                         const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void> {
  if (!validEntryPoint(description.vertex_entry_point) ||
      !validEntryPoint(description.fragment_entry_point))
    return fail(GraphicsPipelineError::invalid_entry_point);
  if (!boundedCount<VkVertexInputBindingDescription>(description.vertex_bindings,
                                                     limits.maxVertexInputBindings) ||
      !boundedCount<VkVertexInputAttributeDescription>(description.vertex_attributes,
                                                       limits.maxVertexInputAttributes) ||
      !boundedCount<VkFormat>(description.color_attachment_formats, limits.maxColorAttachments) ||
      !boundedCount<VkPipelineColorBlendAttachmentState>(description.color_blend_attachments,
                                                         limits.maxColorAttachments))
    return fail(GraphicsPipelineError::unsupported_configuration);

  for (std::size_t index = 0; index < description.vertex_bindings.size(); ++index) {
    const auto &binding = description.vertex_bindings[index];
    if (binding.binding >= limits.maxVertexInputBindings ||
        binding.stride > limits.maxVertexInputBindingStride)
      return fail(GraphicsPipelineError::unsupported_configuration);
    if (binding.inputRate != vk::VertexInputRate::eVertex &&
        binding.inputRate != vk::VertexInputRate::eInstance)
      return fail(GraphicsPipelineError::invalid_description);
    for (std::size_t previous = 0; previous < index; ++previous)
      if (description.vertex_bindings[previous].binding == binding.binding)
        return fail(GraphicsPipelineError::invalid_description);
  }
  for (std::size_t index = 0; index < description.vertex_attributes.size(); ++index) {
    const auto &attribute = description.vertex_attributes[index];
    if (attribute.location >= limits.maxVertexInputAttributes ||
        attribute.offset > limits.maxVertexInputAttributeOffset)
      return fail(GraphicsPipelineError::unsupported_configuration);
    if (!colorFormat(attribute.format) ||
        std::ranges::none_of(description.vertex_bindings, [&](const auto &binding) {
          return binding.binding == attribute.binding;
        }))
      return fail(GraphicsPipelineError::invalid_description);
    for (std::size_t previous = 0; previous < index; ++previous)
      if (description.vertex_attributes[previous].location == attribute.location)
        return fail(GraphicsPipelineError::invalid_description);
  }

  switch (description.topology) {
  case vk::PrimitiveTopology::ePointList:
  case vk::PrimitiveTopology::eLineList:
  case vk::PrimitiveTopology::eTriangleList:
    if (description.primitive_restart)
      return fail(GraphicsPipelineError::unsupported_configuration);
    break;
  case vk::PrimitiveTopology::eLineStrip:
  case vk::PrimitiveTopology::eTriangleStrip:
  case vk::PrimitiveTopology::eTriangleFan:
    break;
  default:
    return fail(GraphicsPipelineError::unsupported_configuration);
  }
  if ((static_cast<VkCullModeFlags>(description.cull_mode) &
       ~static_cast<VkCullModeFlags>(VK_CULL_MODE_FRONT_AND_BACK)) != 0 ||
      (description.front_face != vk::FrontFace::eClockwise &&
       description.front_face != vk::FrontFace::eCounterClockwise) ||
      description.depth_compare_op < vk::CompareOp::eNever ||
      description.depth_compare_op > vk::CompareOp::eAlways)
    return fail(GraphicsPipelineError::invalid_description);
  if (description.color_attachment_formats.size() != description.color_blend_attachments.size() ||
      std::ranges::any_of(description.color_attachment_formats,
                          [](vk::Format format) { return !colorFormat(format); }))
    return fail(GraphicsPipelineError::invalid_description);
  for (const auto &blend : description.color_blend_attachments) {
    const auto core_factor = [](vk::BlendFactor factor) {
      return factor >= vk::BlendFactor::eZero && factor <= vk::BlendFactor::eOneMinusSrc1Alpha;
    };
    const auto core_op = [](vk::BlendOp operation) {
      return operation >= vk::BlendOp::eAdd && operation <= vk::BlendOp::eMax;
    };
    if (blend.blendEnable > VK_TRUE || !core_factor(blend.srcColorBlendFactor) ||
        !core_factor(blend.dstColorBlendFactor) || !core_factor(blend.srcAlphaBlendFactor) ||
        !core_factor(blend.dstAlphaBlendFactor) || !core_op(blend.colorBlendOp) ||
        !core_op(blend.alphaBlendOp) ||
        (static_cast<VkColorComponentFlags>(blend.colorWriteMask) &
         ~static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT |
                                             VK_COLOR_COMPONENT_A_BIT)) != 0)
      return fail(GraphicsPipelineError::invalid_description);
  }
  const auto depth = description.depth_attachment_format;
  const auto stencil = description.stencil_attachment_format;
  if ((depth != vk::Format::eUndefined && !hasDepth(depth)) ||
      (stencil != vk::Format::eUndefined && !hasStencil(stencil)) ||
      (depth != vk::Format::eUndefined && stencil != vk::Format::eUndefined && depth != stencil) ||
      ((description.depth_test_enable || description.depth_write_enable) &&
       depth == vk::Format::eUndefined))
    return fail(GraphicsPipelineError::invalid_description);
  return {};
}

struct GraphicsPipeline::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkPipeline handle = VK_NULL_HANDLE;
  vk::PipelineLayout layout{};
  GraphicsPipelineConfiguration config{};

  ~Impl() {
    if (handle != VK_NULL_HANDLE)
      vkDestroyPipeline(device, handle, nullptr);
  }
};

GraphicsPipeline::GraphicsPipeline(std::unique_ptr<Impl> impl) noexcept
    : implementation_(std::move(impl)) {}
GraphicsPipeline::GraphicsPipeline(GraphicsPipeline &&) noexcept = default;
GraphicsPipeline &GraphicsPipeline::operator=(GraphicsPipeline &&) noexcept = default;
GraphicsPipeline::~GraphicsPipeline() = default;

bool GraphicsPipeline::valid() const noexcept { return nativePipeline() != VK_NULL_HANDLE; }
vk::Pipeline GraphicsPipeline::nativeHandle() const noexcept {
  return vk::Pipeline{nativePipeline()};
}
VkPipeline GraphicsPipeline::nativePipeline() const noexcept {
  return implementation_ ? implementation_->handle : VK_NULL_HANDLE;
}
vk::Device GraphicsPipeline::nativeDevice() const noexcept { return vk::Device{nativeVkDevice()}; }
VkDevice GraphicsPipeline::nativeVkDevice() const noexcept {
  return implementation_ ? implementation_->device : VK_NULL_HANDLE;
}
vk::PipelineLayout GraphicsPipeline::nativeLayout() const noexcept {
  return implementation_ ? implementation_->layout : vk::PipelineLayout{};
}
bool GraphicsPipeline::correlatedWith(const Device &device) const noexcept {
  return valid() && device.valid() && nativeDevice() == device.nativeHandle();
}
bool GraphicsPipeline::correlatedWith(const PipelineLayout &layout) const noexcept {
  return valid() && layout.valid() && nativeDevice() == layout.nativeDevice() &&
         nativeLayout() == layout.nativeHandle();
}
const GraphicsPipelineConfiguration *GraphicsPipeline::configuration() const noexcept {
  return implementation_ ? &implementation_->config : nullptr;
}

auto createGraphicsPipeline(const Device &device, const PipelineLayout &layout,
                            const ShaderModule &vertex_module, const ShaderModule &fragment_module,
                            const GraphicsPipelineDescription &description)
    -> terreate::Result<GraphicsPipeline> {
  if (!device.valid() || !device.plan())
    return fail(GraphicsPipelineError::invalid_device);
  if (!device.plan()->enabledFeatures13().dynamicRendering)
    return fail(GraphicsPipelineError::feature_not_enabled);
  if (!layout.valid())
    return fail(GraphicsPipelineError::invalid_layout);
  if (!vertex_module.valid() || !fragment_module.valid())
    return fail(GraphicsPipelineError::invalid_shader_module);
  if (!layout.correlatedWith(device) || !vertex_module.correlatedWith(device) ||
      !fragment_module.correlatedWith(device))
    return fail(GraphicsPipelineError::parent_mismatch);
  const auto &limits = device.plan()->capabilities().properties.limits;
  if (auto validation = validateGraphicsPipelineDescription(description, limits); !validation)
    return std::unexpected(std::move(validation.error()));

  std::unique_ptr<GraphicsPipeline::Impl> impl;
  std::vector<VkVertexInputBindingDescription> bindings;
  std::vector<VkVertexInputAttributeDescription> attributes;
  std::vector<VkFormat> formats;
  std::vector<VkPipelineColorBlendAttachmentState> attachments;
  try {
    impl = std::make_unique<GraphicsPipeline::Impl>();
    impl->config.requested = description;
    impl->config.effective = description;
    impl->config.limits = limits;
    bindings.reserve(description.vertex_bindings.size());
    for (const auto &binding : description.vertex_bindings)
      bindings.push_back(static_cast<VkVertexInputBindingDescription>(binding));
    attributes.reserve(description.vertex_attributes.size());
    for (const auto &attribute : description.vertex_attributes)
      attributes.push_back(static_cast<VkVertexInputAttributeDescription>(attribute));
    formats.reserve(description.color_attachment_formats.size());
    for (const auto format : description.color_attachment_formats)
      formats.push_back(static_cast<VkFormat>(format));
    attachments.reserve(description.color_blend_attachments.size());
    for (const auto &attachment : description.color_blend_attachments)
      attachments.push_back(static_cast<VkPipelineColorBlendAttachmentState>(attachment));
  } catch (const std::bad_alloc &) {
    return fail(GraphicsPipelineError::allocation_failed);
  }
  impl->device = static_cast<VkDevice>(device.nativeHandle());
  impl->layout = layout.nativeHandle();
  const auto &effective = impl->config.effective;
  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex_module.nativeShaderModule();
  stages[0].pName = effective.vertex_entry_point.c_str();
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragment_module.nativeShaderModule();
  stages[1].pName = effective.fragment_entry_point.c_str();

  VkPipelineVertexInputStateCreateInfo vertex{};
  vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
  vertex.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
  vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
  vertex.pVertexAttributeDescriptions = attributes.empty() ? nullptr : attributes.data();
  VkPipelineInputAssemblyStateCreateInfo assembly{};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = static_cast<VkPrimitiveTopology>(effective.topology);
  assembly.primitiveRestartEnable = effective.primitive_restart ? VK_TRUE : VK_FALSE;
  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = static_cast<VkCullModeFlags>(effective.cull_mode);
  raster.frontFace = static_cast<VkFrontFace>(effective.front_face);
  raster.lineWidth = 1.0F;
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth.depthTestEnable = effective.depth_test_enable ? VK_TRUE : VK_FALSE;
  depth.depthWriteEnable = effective.depth_write_enable ? VK_TRUE : VK_FALSE;
  depth.depthCompareOp = static_cast<VkCompareOp>(effective.depth_compare_op);
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = static_cast<std::uint32_t>(attachments.size());
  blend.pAttachments = attachments.empty() ? nullptr : attachments.data();
  constexpr std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
  dynamic.pDynamicStates = dynamic_states.data();
  VkPipelineRenderingCreateInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = static_cast<std::uint32_t>(formats.size());
  rendering.pColorAttachmentFormats = formats.empty() ? nullptr : formats.data();
  rendering.depthAttachmentFormat = static_cast<VkFormat>(effective.depth_attachment_format);
  rendering.stencilAttachmentFormat = static_cast<VkFormat>(effective.stencil_attachment_format);
  VkGraphicsPipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.pNext = &rendering;
  info.stageCount = static_cast<std::uint32_t>(stages.size());
  info.pStages = stages.data();
  info.pVertexInputState = &vertex;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout.nativePipelineLayout();
  info.basePipelineIndex = -1;
  // Adopt output immediately, including a partial result on native failure.
  // All owned storage is stable and all allocations precede this single call.
  const auto result =
      vkCreateGraphicsPipelines(impl->device, VK_NULL_HANDLE, 1, &info, nullptr, &impl->handle);
  if (result != VK_SUCCESS)
    return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                           "vkCreateGraphicsPipelines",
                                           "native graphics pipeline creation failed"});
  if (impl->handle == VK_NULL_HANDLE)
    return fail(GraphicsPipelineError::native_null_handle);
  return GraphicsPipeline{std::move(impl)};
}
} // namespace terreate::graphics

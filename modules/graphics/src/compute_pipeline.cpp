#include <terreate/graphics/compute_pipeline.hpp>

#include <new>
#include <utility>

namespace terreate::graphics {
namespace {
class ComputePipelineErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.compute_pipeline"; }
  std::string message(int value) const override {
    switch (static_cast<ComputePipelineError>(value)) {
    case ComputePipelineError::invalid_device:
      return "invalid ComputePipeline Device";
    case ComputePipelineError::invalid_shader_module:
      return "invalid ComputePipeline ShaderModule";
    case ComputePipelineError::invalid_layout:
      return "invalid ComputePipeline PipelineLayout";
    case ComputePipelineError::parent_mismatch:
      return "ComputePipeline parents belong to different Devices";
    case ComputePipelineError::invalid_entry_point:
      return "invalid compute entry-point string";
    case ComputePipelineError::native_null_handle:
      return "Vulkan returned null ComputePipeline on success";
    case ComputePipelineError::allocation_failed:
      return "ComputePipeline wrapper allocation failed";
    }
    return "unknown ComputePipeline error";
  }
};
auto fail(ComputePipelineError error, const char *detail) {
  return std::unexpected(terreate::Error{make_error_code(error), "ComputePipeline", detail});
}
} // namespace

const std::error_category &compute_pipeline_error_category() noexcept {
  static const ComputePipelineErrorCategory category;
  return category;
}
std::error_code make_error_code(ComputePipelineError error) noexcept {
  return {static_cast<int>(error), compute_pipeline_error_category()};
}

auto validateComputePipelineDescription(const ComputePipelineDescription &description)
    -> terreate::Result<void> {
  const auto &name = description.entry_point;
  if (name.empty() || name.size() > max_compute_entry_point_bytes ||
      name.find('\0') != std::string::npos)
    return fail(ComputePipelineError::invalid_entry_point,
                "explicit nonempty entry point of at most 4096 bytes without NUL required");
  return {};
}

struct ComputePipeline::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkPipeline handle = VK_NULL_HANDLE;
  vk::PipelineLayout layout{};
  ComputePipelineConfiguration config{};

  ~Impl() {
    if (handle != VK_NULL_HANDLE)
      vkDestroyPipeline(device, handle, nullptr);
  }
};

ComputePipeline::ComputePipeline(std::unique_ptr<Impl> impl) noexcept
    : implementation_(std::move(impl)) {}
ComputePipeline::ComputePipeline(ComputePipeline &&) noexcept = default;
ComputePipeline &ComputePipeline::operator=(ComputePipeline &&) noexcept = default;
ComputePipeline::~ComputePipeline() = default;

bool ComputePipeline::valid() const noexcept { return nativePipeline() != VK_NULL_HANDLE; }
vk::Pipeline ComputePipeline::nativeHandle() const noexcept {
  return vk::Pipeline{nativePipeline()};
}
VkPipeline ComputePipeline::nativePipeline() const noexcept {
  return implementation_ ? implementation_->handle : VK_NULL_HANDLE;
}
vk::Device ComputePipeline::nativeDevice() const noexcept { return vk::Device{nativeVkDevice()}; }
VkDevice ComputePipeline::nativeVkDevice() const noexcept {
  return implementation_ ? implementation_->device : VK_NULL_HANDLE;
}
vk::PipelineLayout ComputePipeline::nativeLayout() const noexcept {
  return implementation_ ? implementation_->layout : vk::PipelineLayout{};
}
bool ComputePipeline::correlatedWith(const Device &device) const noexcept {
  return valid() && device.valid() && nativeDevice() == device.nativeHandle();
}
bool ComputePipeline::correlatedWith(const PipelineLayout &layout) const noexcept {
  return valid() && layout.valid() && nativeDevice() == layout.nativeDevice() &&
         nativeLayout() == layout.nativeHandle();
}
const ComputePipelineConfiguration *ComputePipeline::configuration() const noexcept {
  return implementation_ ? &implementation_->config : nullptr;
}

auto createComputePipeline(const Device &device, const ShaderModule &shader_module,
                           const PipelineLayout &layout,
                           const ComputePipelineDescription &description)
    -> terreate::Result<ComputePipeline> {
  if (!device.valid() || !device.plan())
    return fail(ComputePipelineError::invalid_device, "live owning Device required");
  if (!shader_module.valid())
    return fail(ComputePipelineError::invalid_shader_module, "live ShaderModule required");
  if (!layout.valid())
    return fail(ComputePipelineError::invalid_layout, "live PipelineLayout required");
  if (!shader_module.correlatedWith(device) || !layout.correlatedWith(device))
    return fail(ComputePipelineError::parent_mismatch, "ShaderModule and layout must match Device");
  if (auto validation = validateComputePipelineDescription(description); !validation)
    return std::unexpected(std::move(validation.error()));

  std::unique_ptr<ComputePipeline::Impl> impl;
  try {
    impl = std::make_unique<ComputePipeline::Impl>();
    impl->config = {description, description, vk::ShaderStageFlagBits::eCompute};
  } catch (const std::bad_alloc &) {
    return std::unexpected(
        terreate::Error{make_error_code(ComputePipelineError::allocation_failed)});
  }
  impl->device = static_cast<VkDevice>(device.nativeHandle());
  impl->layout = layout.nativeHandle();
  const VkPipelineShaderStageCreateInfo stage{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shader_module.nativeShaderModule(),
      .pName = impl->config.effective.entry_point.c_str(),
      .pSpecializationInfo = nullptr,
  };
  const VkComputePipelineCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stage = stage,
      .layout = layout.nativePipelineLayout(),
      .basePipelineHandle = VK_NULL_HANDLE,
      .basePipelineIndex = -1,
  };
  // Unlike single-object creation, pipeline multi-create may return owned handles
  // on failure. Adopt the initialized output immediately so every nonnull result
  // is cleaned up, even if constructing the diagnostic subsequently throws.
  const auto result =
      vkCreateComputePipelines(impl->device, VK_NULL_HANDLE, 1, &info, nullptr, &impl->handle);
  if (result != VK_SUCCESS)
    return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                           "vkCreateComputePipelines",
                                           "native compute pipeline creation failed"});
  if (impl->handle == VK_NULL_HANDLE)
    return fail(ComputePipelineError::native_null_handle, "successful Vulkan call returned null");
  return ComputePipeline{std::move(impl)};
}
} // namespace terreate::graphics

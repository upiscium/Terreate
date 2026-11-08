#include <terreate/graphics/pipeline_layout.hpp>

#include <array>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
class PipelineLayoutErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.pipeline_layout"; }
  std::string message(int value) const override {
    switch (static_cast<PipelineLayoutError>(value)) {
    case PipelineLayoutError::invalid_device:
      return "invalid PipelineLayout Device";
    case PipelineLayoutError::invalid_description:
      return "invalid PipelineLayout description";
    case PipelineLayoutError::unsupported_configuration:
      return "push constant range exceeds Device limit";
    case PipelineLayoutError::native_null_handle:
      return "Vulkan returned null PipelineLayout on success";
    case PipelineLayoutError::allocation_failed:
      return "PipelineLayout wrapper allocation failed";
    }
    return "unknown PipelineLayout error";
  }
};
auto fail(PipelineLayoutError error, const char *detail) {
  return std::unexpected(terreate::Error{make_error_code(error), "PipelineLayout", detail});
}
} // namespace

const std::error_category &pipeline_layout_error_category() noexcept {
  static const PipelineLayoutErrorCategory category;
  return category;
}
std::error_code make_error_code(PipelineLayoutError error) noexcept {
  return {static_cast<int>(error), pipeline_layout_error_category()};
}

auto validatePipelineLayoutDescription(const PipelineLayoutDescription &description,
                                       std::uint32_t max_push_constants_size)
    -> terreate::Result<void> {
  const auto &ranges = description.push_constant_ranges;
  if (ranges.size() > max_pipeline_layout_push_constant_ranges)
    return fail(PipelineLayoutError::invalid_description,
                "at most six core-stage ranges supported");
  constexpr VkShaderStageFlags core_stages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
      VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  VkShaderStageFlags used_stages = 0;
  for (const auto &range : ranges) {
    const auto stages = static_cast<VkShaderStageFlags>(range.stageFlags);
    if (stages == 0 || (stages & ~core_stages) != 0)
      return fail(PipelineLayoutError::invalid_description,
                  "nonempty explicit core shader-stage mask required");
    // Vulkan forbids repeating a stage even when the byte intervals are disjoint.
    if ((used_stages & stages) != 0)
      return fail(PipelineLayoutError::invalid_description,
                  "a shader stage may occur in only one push constant range");
    used_stages |= stages;
    if (range.size == 0 || range.offset % 4 != 0 || range.size % 4 != 0 ||
        range.offset > std::numeric_limits<std::uint32_t>::max() - range.size)
      return fail(PipelineLayoutError::invalid_description,
                  "nonzero size, four-byte alignment and nonoverflowing range required");
    if (range.offset > max_push_constants_size ||
        range.size > max_push_constants_size - range.offset)
      return fail(PipelineLayoutError::unsupported_configuration,
                  "range exceeds maxPushConstantsSize");
  }
  return {};
}

struct PipelineLayout::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkPipelineLayout handle = VK_NULL_HANDLE;
  PipelineLayoutConfiguration config{};

  ~Impl() {
    if (handle != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device, handle, nullptr);
  }
};

PipelineLayout::PipelineLayout(std::unique_ptr<Impl> impl) noexcept
    : implementation_(std::move(impl)) {}
PipelineLayout::PipelineLayout(PipelineLayout &&) noexcept = default;
PipelineLayout &PipelineLayout::operator=(PipelineLayout &&) noexcept = default;
PipelineLayout::~PipelineLayout() = default;

bool PipelineLayout::valid() const noexcept { return nativePipelineLayout() != VK_NULL_HANDLE; }
vk::PipelineLayout PipelineLayout::nativeHandle() const noexcept {
  return vk::PipelineLayout{nativePipelineLayout()};
}
VkPipelineLayout PipelineLayout::nativePipelineLayout() const noexcept {
  return implementation_ ? implementation_->handle : VK_NULL_HANDLE;
}
vk::Device PipelineLayout::nativeDevice() const noexcept { return vk::Device{nativeVkDevice()}; }
VkDevice PipelineLayout::nativeVkDevice() const noexcept {
  return implementation_ ? implementation_->device : VK_NULL_HANDLE;
}
bool PipelineLayout::correlatedWith(const Device &device) const noexcept {
  return valid() && device.valid() && nativeDevice() == device.nativeHandle();
}
const PipelineLayoutConfiguration *PipelineLayout::configuration() const noexcept {
  return implementation_ ? &implementation_->config : nullptr;
}

auto createPipelineLayout(const Device &device, const PipelineLayoutDescription &description)
    -> terreate::Result<PipelineLayout> {
  if (!device.valid() || !device.plan())
    return fail(PipelineLayoutError::invalid_device, "live owning Device required");
  const auto limit = device.plan()->capabilities().properties.limits.maxPushConstantsSize;
  if (auto validation = validatePipelineLayoutDescription(description, limit); !validation)
    return std::unexpected(std::move(validation.error()));

  std::unique_ptr<PipelineLayout::Impl> impl;
  try {
    impl = std::make_unique<PipelineLayout::Impl>();
    impl->config = {description, description, limit};
  } catch (const std::bad_alloc &) {
    // Code-only Error construction does not allocate while handling exhaustion.
    return std::unexpected(
        terreate::Error{make_error_code(PipelineLayoutError::allocation_failed)});
  }
  impl->device = static_cast<VkDevice>(device.nativeHandle());
  std::array<VkPushConstantRange, max_pipeline_layout_push_constant_ranges> native_ranges{};
  const auto &ranges = impl->config.effective.push_constant_ranges;
  for (std::size_t index = 0; index < ranges.size(); ++index) {
    const auto &range = ranges[index];
    native_ranges[index] = {static_cast<VkShaderStageFlags>(range.stageFlags), range.offset,
                            range.size};
  }
  const VkPipelineLayoutCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .setLayoutCount = 0,
      .pSetLayouts = nullptr,
      .pushConstantRangeCount = static_cast<std::uint32_t>(ranges.size()),
      .pPushConstantRanges = ranges.empty() ? nullptr : native_ranges.data(),
  };
  VkPipelineLayout handle = VK_NULL_HANDLE;
  const auto result = vkCreatePipelineLayout(impl->device, &info, nullptr, &handle);
  if (result != VK_SUCCESS)
    return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                           "vkCreatePipelineLayout",
                                           "native layout creation failed"});
  if (handle == VK_NULL_HANDLE)
    return fail(PipelineLayoutError::native_null_handle, "successful Vulkan call returned null");
  impl->handle = handle;
  return PipelineLayout{std::move(impl)};
}
} // namespace terreate::graphics

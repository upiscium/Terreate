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
      return "PipelineLayout configuration exceeds supported Device limits";
    case PipelineLayoutError::native_null_handle:
      return "Vulkan returned null PipelineLayout on success";
    case PipelineLayoutError::allocation_failed:
      return "PipelineLayout wrapper allocation failed";
    case PipelineLayoutError::invalid_set_layout:
      return "invalid PipelineLayout descriptor set layout";
    case PipelineLayoutError::device_mismatch:
      return "PipelineLayout descriptor set layout Device mismatch";
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

auto validatePipelineLayoutSetLayouts(std::span<const DescriptorSetLayoutDescription> set_layouts,
                                      const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void> {
  // Category order: sampler, uniform, storage, sampled image, storage image,
  // dynamic uniform, dynamic storage, resources. Subtraction precedes addition.
  const std::array<std::uint32_t, 8> set_limits{limits.maxDescriptorSetSamplers,
                                                limits.maxDescriptorSetUniformBuffers,
                                                limits.maxDescriptorSetStorageBuffers,
                                                limits.maxDescriptorSetSampledImages,
                                                limits.maxDescriptorSetStorageImages,
                                                limits.maxDescriptorSetUniformBuffersDynamic,
                                                limits.maxDescriptorSetStorageBuffersDynamic,
                                                0};
  const std::array<std::uint32_t, 8> stage_limits{limits.maxPerStageDescriptorSamplers,
                                                  limits.maxPerStageDescriptorUniformBuffers,
                                                  limits.maxPerStageDescriptorStorageBuffers,
                                                  limits.maxPerStageDescriptorSampledImages,
                                                  limits.maxPerStageDescriptorStorageImages,
                                                  0,
                                                  0,
                                                  limits.maxPerStageResources};
  std::array<std::uint32_t, 8> totals{};
  std::array<std::array<std::uint32_t, 8>, 6> stage_totals{};
  try {
    if (set_layouts.size() > limits.maxBoundDescriptorSets ||
        set_layouts.size() > std::numeric_limits<std::uint32_t>::max())
      return fail(PipelineLayoutError::unsupported_configuration,
                  "set count exceeds maxBoundDescriptorSets or Vulkan ABI");
    for (const auto &layout : set_layouts) {
      if (auto valid = validateDescriptorSetLayoutDescription(layout, limits); !valid) {
        const auto code = valid.error().code();
        const auto mapped = code == make_error_code(DescriptorSetLayoutError::allocation_failed)
                                ? PipelineLayoutError::allocation_failed
                            : code == make_error_code(DescriptorSetLayoutError::invalid_description)
                                ? PipelineLayoutError::invalid_set_layout
                                : PipelineLayoutError::unsupported_configuration;
        return std::unexpected(terreate::Error{make_error_code(mapped)});
      }
      for (const auto &binding : layout.bindings) {
        std::array<bool, 8> categories{};
        switch (binding.descriptor_type) {
        case vk::DescriptorType::eSampler:
          categories[0] = true;
          break;
        case vk::DescriptorType::eCombinedImageSampler:
          categories[0] = categories[3] = true;
          break;
        case vk::DescriptorType::eSampledImage:
          categories[3] = true;
          break;
        case vk::DescriptorType::eStorageImage:
          categories[4] = true;
          break;
        case vk::DescriptorType::eUniformBuffer:
          categories[1] = true;
          break;
        case vk::DescriptorType::eStorageBuffer:
          categories[2] = true;
          break;
        case vk::DescriptorType::eUniformBufferDynamic:
          categories[1] = categories[5] = true;
          break;
        case vk::DescriptorType::eStorageBufferDynamic:
          categories[2] = categories[6] = true;
          break;
        default:
          return fail(PipelineLayoutError::unsupported_configuration,
                      "unsupported descriptor type");
        }
        categories[7] = binding.descriptor_type != vk::DescriptorType::eSampler;
        const auto add = [&](std::uint32_t &total, std::uint32_t limit) {
          if (total > limit || binding.descriptor_count > limit - total)
            return false;
          total += binding.descriptor_count;
          return true;
        };
        for (std::size_t category = 0; category < categories.size(); ++category) {
          if (!categories[category])
            continue;
          if (category != 7 && !add(totals[category], set_limits[category]))
            return fail(PipelineLayoutError::unsupported_configuration,
                        "aggregate descriptors exceed maxDescriptorSet limit");
          if (category == 5 || category == 6)
            continue;
          // The six core Vulkan stage bits are contiguous, vertex through compute.
          for (std::size_t stage = 0; stage < stage_totals.size(); ++stage) {
            if ((static_cast<VkShaderStageFlags>(binding.stage_flags) & (1u << stage)) != 0 &&
                !add(stage_totals[stage][category], stage_limits[category]))
              return fail(PipelineLayoutError::unsupported_configuration,
                          "aggregate descriptors exceed per-stage limit");
          }
        }
      }
    }
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(
        terreate::Error{make_error_code(PipelineLayoutError::allocation_failed)});
  }
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
  return createPipelineLayout(device, {}, description);
}

auto createPipelineLayout(const Device &device,
                          std::span<const DescriptorSetLayout *const> set_layouts,
                          const PipelineLayoutDescription &description)
    -> terreate::Result<PipelineLayout> {
  std::unique_ptr<PipelineLayout::Impl> impl;
  try {
    if (!device.valid() || !device.plan())
      return fail(PipelineLayoutError::invalid_device, "live owning Device required");
    const auto &limits = device.plan()->capabilities().properties.limits;
    const auto limit = limits.maxPushConstantsSize;
    if (auto validation = validatePipelineLayoutDescription(description, limit); !validation)
      return std::unexpected(std::move(validation.error()));
    if (set_layouts.size() > limits.maxBoundDescriptorSets ||
        set_layouts.size() > std::numeric_limits<std::uint32_t>::max())
      return fail(PipelineLayoutError::unsupported_configuration, "excessive descriptor set count");
    for (const auto *layout : set_layouts) {
      if (!layout || !layout->valid() || !layout->configuration() ||
          layout->nativeDescriptorSetLayout() == VK_NULL_HANDLE)
        return fail(PipelineLayoutError::invalid_set_layout, "live nonnull set layout required");
      if (!layout->correlatedWith(device))
        return fail(PipelineLayoutError::device_mismatch, "set layout belongs to another Device");
    }
    impl = std::make_unique<PipelineLayout::Impl>();
    impl->config = {description, description, limit};
    impl->config.limits = limits;
    impl->config.set_layout_handles.reserve(set_layouts.size());
    impl->config.set_layout_descriptions.reserve(set_layouts.size());
    for (const auto *layout : set_layouts) {
      impl->config.set_layout_handles.push_back(layout->nativeDescriptorSetLayout());
      impl->config.set_layout_descriptions.push_back(layout->configuration()->effective);
    }
    if (auto validation =
            validatePipelineLayoutSetLayouts(impl->config.set_layout_descriptions, limits);
        !validation)
      return std::unexpected(std::move(validation.error()));
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
      .setLayoutCount = static_cast<std::uint32_t>(impl->config.set_layout_handles.size()),
      .pSetLayouts = impl->config.set_layout_handles.empty()
                         ? nullptr
                         : impl->config.set_layout_handles.data(),
      .pushConstantRangeCount = static_cast<std::uint32_t>(ranges.size()),
      .pPushConstantRanges = ranges.empty() ? nullptr : native_ranges.data(),
  };
  VkPipelineLayout handle = VK_NULL_HANDLE;
  const auto result = vkCreatePipelineLayout(impl->device, &info, nullptr, &handle);
  if (result != VK_SUCCESS) {
    const auto code = vk::make_error_code(static_cast<vk::Result>(result));
    try {
      return std::unexpected(
          terreate::Error{code, "vkCreatePipelineLayout", "native layout creation failed"});
    } catch (const std::bad_alloc &) {
      return std::unexpected(terreate::Error{code});
    }
  }
  if (handle == VK_NULL_HANDLE) {
    try {
      return fail(PipelineLayoutError::native_null_handle, "successful Vulkan call returned null");
    } catch (const std::bad_alloc &) {
      return std::unexpected(
          terreate::Error{make_error_code(PipelineLayoutError::native_null_handle)});
    }
  }
  impl->handle = handle;
  return PipelineLayout{std::move(impl)};
}
} // namespace terreate::graphics

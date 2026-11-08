#include <terreate/graphics/descriptor_set_layout.hpp>

#include <array>
#include <limits>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>

namespace terreate::graphics {
namespace {
class DescriptorSetLayoutErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.descriptor_set_layout"; }
  std::string message(int value) const override {
    switch (static_cast<DescriptorSetLayoutError>(value)) {
    case DescriptorSetLayoutError::invalid_device:
      return "invalid DescriptorSetLayout Device";
    case DescriptorSetLayoutError::invalid_description:
      return "invalid DescriptorSetLayout description";
    case DescriptorSetLayoutError::unsupported_configuration:
      return "unsupported DescriptorSetLayout type or descriptor limits exceeded";
    case DescriptorSetLayoutError::native_null_handle:
      return "Vulkan returned null DescriptorSetLayout on success";
    case DescriptorSetLayoutError::allocation_failed:
      return "DescriptorSetLayout CPU allocation failed";
    }
    return "unknown DescriptorSetLayout error";
  }
};

auto failure(std::error_code code, const char *context, const char *detail) {
  try {
    return std::unexpected(terreate::Error{code, context, detail});
  } catch (const std::bad_alloc &) {
    // In particular, diagnostic allocation must not replace a native VkResult.
    return std::unexpected(terreate::Error{code});
  }
}
auto fail(DescriptorSetLayoutError error, const char *detail) {
  return failure(make_error_code(error), "DescriptorSetLayout", detail);
}
auto allocationFailure() {
  return std::unexpected(
      terreate::Error{make_error_code(DescriptorSetLayoutError::allocation_failed)});
}

constexpr std::array<VkShaderStageFlags, 6> core_stages{
    VK_SHADER_STAGE_VERTEX_BIT,
    VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
    VK_SHADER_STAGE_GEOMETRY_BIT,
    VK_SHADER_STAGE_FRAGMENT_BIT,
    VK_SHADER_STAGE_COMPUTE_BIT,
};
constexpr VkShaderStageFlags core_stage_mask =
    VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;

bool supportedType(vk::DescriptorType type) noexcept {
  switch (type) {
  case vk::DescriptorType::eUniformBuffer:
  case vk::DescriptorType::eStorageBuffer:
  case vk::DescriptorType::eSampler:
  case vk::DescriptorType::eSampledImage:
  case vk::DescriptorType::eStorageImage:
  case vk::DescriptorType::eCombinedImageSampler:
  case vk::DescriptorType::eUniformBufferDynamic:
  case vk::DescriptorType::eStorageBufferDynamic:
    return true;
  default:
    // Includes input attachments, texel buffers and all extension types.
    return false;
  }
}

struct DescriptorCounts {
  std::uint32_t samplers = 0;
  std::uint32_t uniform_buffers = 0;
  std::uint32_t storage_buffers = 0;
  std::uint32_t sampled_images = 0;
  std::uint32_t storage_images = 0;
  std::uint32_t dynamic_uniform_buffers = 0;
  std::uint32_t dynamic_storage_buffers = 0;
  std::uint32_t resources = 0;
};

bool addWithinLimit(std::uint32_t &total, std::uint32_t count, std::uint32_t limit) noexcept {
  // Check before addition, including synthetic UINT32_MAX limits. No wrapping
  // sums or multiplication by the number of stages can hide excess descriptors.
  if (total > limit || count > limit - total)
    return false;
  total += count;
  return true;
}

bool countSetBinding(DescriptorCounts &totals, const DescriptorSetLayoutBindingDescription &binding,
                     const vk::PhysicalDeviceLimits &limits) noexcept {
  const auto count = binding.descriptor_count;
  switch (binding.descriptor_type) {
  case vk::DescriptorType::eSampler:
    return addWithinLimit(totals.samplers, count, limits.maxDescriptorSetSamplers);
  case vk::DescriptorType::eCombinedImageSampler:
    return addWithinLimit(totals.samplers, count, limits.maxDescriptorSetSamplers) &&
           addWithinLimit(totals.sampled_images, count, limits.maxDescriptorSetSampledImages);
  case vk::DescriptorType::eSampledImage:
    return addWithinLimit(totals.sampled_images, count, limits.maxDescriptorSetSampledImages);
  case vk::DescriptorType::eStorageImage:
    return addWithinLimit(totals.storage_images, count, limits.maxDescriptorSetStorageImages);
  case vk::DescriptorType::eUniformBuffer:
    return addWithinLimit(totals.uniform_buffers, count, limits.maxDescriptorSetUniformBuffers);
  case vk::DescriptorType::eStorageBuffer:
    return addWithinLimit(totals.storage_buffers, count, limits.maxDescriptorSetStorageBuffers);
  case vk::DescriptorType::eUniformBufferDynamic:
    return addWithinLimit(totals.uniform_buffers, count, limits.maxDescriptorSetUniformBuffers) &&
           addWithinLimit(totals.dynamic_uniform_buffers, count,
                          limits.maxDescriptorSetUniformBuffersDynamic);
  case vk::DescriptorType::eStorageBufferDynamic:
    return addWithinLimit(totals.storage_buffers, count, limits.maxDescriptorSetStorageBuffers) &&
           addWithinLimit(totals.dynamic_storage_buffers, count,
                          limits.maxDescriptorSetStorageBuffersDynamic);
  default:
    return false;
  }
}

bool countStageBinding(DescriptorCounts &totals,
                       const DescriptorSetLayoutBindingDescription &binding,
                       const vk::PhysicalDeviceLimits &limits) noexcept {
  const auto count = binding.descriptor_count;
  // Standalone samplers are excluded from maxPerStageResources. A combined
  // image sampler counts once there, but against BOTH sampler and image limits.
  if (binding.descriptor_type != vk::DescriptorType::eSampler &&
      !addWithinLimit(totals.resources, count, limits.maxPerStageResources))
    return false;
  switch (binding.descriptor_type) {
  case vk::DescriptorType::eSampler:
    return addWithinLimit(totals.samplers, count, limits.maxPerStageDescriptorSamplers);
  case vk::DescriptorType::eCombinedImageSampler:
    return addWithinLimit(totals.samplers, count, limits.maxPerStageDescriptorSamplers) &&
           addWithinLimit(totals.sampled_images, count, limits.maxPerStageDescriptorSampledImages);
  case vk::DescriptorType::eSampledImage:
    return addWithinLimit(totals.sampled_images, count, limits.maxPerStageDescriptorSampledImages);
  case vk::DescriptorType::eStorageImage:
    return addWithinLimit(totals.storage_images, count, limits.maxPerStageDescriptorStorageImages);
  case vk::DescriptorType::eUniformBuffer:
  case vk::DescriptorType::eUniformBufferDynamic:
    return addWithinLimit(totals.uniform_buffers, count,
                          limits.maxPerStageDescriptorUniformBuffers);
  case vk::DescriptorType::eStorageBuffer:
  case vk::DescriptorType::eStorageBufferDynamic:
    return addWithinLimit(totals.storage_buffers, count,
                          limits.maxPerStageDescriptorStorageBuffers);
  default:
    return false;
  }
}
} // namespace

const std::error_category &descriptor_set_layout_error_category() noexcept {
  static const DescriptorSetLayoutErrorCategory category;
  return category;
}
std::error_code make_error_code(DescriptorSetLayoutError error) noexcept {
  return {static_cast<int>(error), descriptor_set_layout_error_category()};
}

auto validateDescriptorSetLayoutDescription(const DescriptorSetLayoutDescription &description,
                                            const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void> {
  if (description.bindings.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(DescriptorSetLayoutError::invalid_description, "binding count exceeds Vulkan ABI");
  try {
    std::unordered_set<std::uint32_t> binding_numbers;
    DescriptorCounts set_totals;
    std::array<DescriptorCounts, core_stages.size()> stage_totals{};
    for (const auto &binding : description.bindings) {
      const auto stages = static_cast<VkShaderStageFlags>(binding.stage_flags);
      if (binding.descriptor_count == 0 || stages == 0 || (stages & ~core_stage_mask) != 0)
        return fail(DescriptorSetLayoutError::invalid_description,
                    "positive descriptor count and nonempty explicit core stage mask required");
      if (!binding_numbers.insert(binding.binding).second)
        return fail(DescriptorSetLayoutError::invalid_description,
                    "binding numbers must be unique");
      if (!supportedType(binding.descriptor_type))
        return fail(DescriptorSetLayoutError::unsupported_configuration,
                    "descriptor type is outside the Phase A0 core subset");
      if (!countSetBinding(set_totals, binding, limits))
        return fail(DescriptorSetLayoutError::unsupported_configuration,
                    "binding totals exceed a maxDescriptorSet limit");
      for (std::size_t index = 0; index < core_stages.size(); ++index) {
        if ((stages & core_stages[index]) != 0 &&
            !countStageBinding(stage_totals[index], binding, limits))
          return fail(DescriptorSetLayoutError::unsupported_configuration,
                      "stage totals exceed maxPerStageDescriptor limits or maxPerStageResources");
      }
    }
    return {};
  } catch (const std::bad_alloc &) {
    return allocationFailure();
  }
}

struct DescriptorSetLayout::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkDescriptorSetLayout handle = VK_NULL_HANDLE;
  DescriptorSetLayoutConfiguration config{};

  ~Impl() {
    if (handle != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(device, handle, nullptr);
  }
};

DescriptorSetLayout::DescriptorSetLayout(std::unique_ptr<Impl> impl) noexcept
    : implementation_(std::move(impl)) {}
DescriptorSetLayout::DescriptorSetLayout(DescriptorSetLayout &&) noexcept = default;
DescriptorSetLayout &DescriptorSetLayout::operator=(DescriptorSetLayout &&) noexcept = default;
DescriptorSetLayout::~DescriptorSetLayout() = default;

bool DescriptorSetLayout::valid() const noexcept {
  return nativeDescriptorSetLayout() != VK_NULL_HANDLE;
}
vk::DescriptorSetLayout DescriptorSetLayout::nativeHandle() const noexcept {
  return vk::DescriptorSetLayout{nativeDescriptorSetLayout()};
}
VkDescriptorSetLayout DescriptorSetLayout::nativeDescriptorSetLayout() const noexcept {
  return implementation_ ? implementation_->handle : VK_NULL_HANDLE;
}
vk::Device DescriptorSetLayout::nativeDevice() const noexcept {
  return vk::Device{nativeVkDevice()};
}
VkDevice DescriptorSetLayout::nativeVkDevice() const noexcept {
  return implementation_ ? implementation_->device : VK_NULL_HANDLE;
}
bool DescriptorSetLayout::correlatedWith(const Device &device) const noexcept {
  return valid() && device.valid() && nativeDevice() == device.nativeHandle();
}
const DescriptorSetLayoutConfiguration *DescriptorSetLayout::configuration() const noexcept {
  return implementation_ ? &implementation_->config : nullptr;
}

auto createDescriptorSetLayout(Device &device, const DescriptorSetLayoutDescription &description)
    -> terreate::Result<DescriptorSetLayout> {
  if (!device.valid() || !device.plan())
    return fail(DescriptorSetLayoutError::invalid_device, "live owning Device required");
  const auto &limits = device.plan()->capabilities().properties.limits;
  if (auto validation = validateDescriptorSetLayoutDescription(description, limits); !validation)
    return std::unexpected(std::move(validation.error()));

  try {
    auto impl = std::make_unique<DescriptorSetLayout::Impl>();
    impl->device = static_cast<VkDevice>(device.nativeHandle());
    impl->config = {description, description, limits};
    // Temporary native storage is owned through the synchronous call, independent
    // of the caller's vector and both retained snapshots. Preserve exact order.
    std::vector<VkDescriptorSetLayoutBinding> native_bindings;
    native_bindings.reserve(impl->config.effective.bindings.size());
    for (const auto &binding : impl->config.effective.bindings)
      native_bindings.push_back({binding.binding,
                                 static_cast<VkDescriptorType>(binding.descriptor_type),
                                 binding.descriptor_count,
                                 static_cast<VkShaderStageFlags>(binding.stage_flags), nullptr});
    const VkDescriptorSetLayoutCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = static_cast<std::uint32_t>(native_bindings.size()),
        .pBindings = native_bindings.empty() ? nullptr : native_bindings.data(),
    };
    VkDescriptorSetLayout handle = VK_NULL_HANDLE;
    const auto result = vkCreateDescriptorSetLayout(impl->device, &info, nullptr, &handle);
    if (result != VK_SUCCESS)
      return failure(vk::make_error_code(static_cast<vk::Result>(result)),
                     "vkCreateDescriptorSetLayout", "native descriptor set layout creation failed");
    // Single-object creation transfers ownership only on success; failed output
    // is undefined, unlike pipeline multi-create. Never adopt or destroy it.
    impl->handle = handle;
    if (handle == VK_NULL_HANDLE)
      return fail(DescriptorSetLayoutError::native_null_handle,
                  "successful Vulkan call returned null");
    return DescriptorSetLayout{std::move(impl)};
  } catch (const std::bad_alloc &) {
    return allocationFailure();
  }
}
} // namespace terreate::graphics

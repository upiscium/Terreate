#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

#include "graphics_diagnostics.hpp"
#include "instance_impl.hpp"
#include "instance_query.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace terreate::graphics {

// Keep the borrowed PhysicalDevice authority in the same archive object as the
// real Instance implementation.  A static consumer that uses a genuine
// Instance therefore extracts this object before any consumer replacement can
// satisfy the PhysicalDevice member references.
struct PhysicalDevice::QueryAuthority {};

namespace {

class InstanceErrorCategory final : public std::error_category {
public:
  [[nodiscard]] const char *name() const noexcept override { return "terreate.graphics.instance"; }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<InstanceError>(value)) {
    case InstanceError::invalid_description:
      return "invalid instance description";
    case InstanceError::contradictory_requirements:
      return "contradictory instance requirements";
    case InstanceError::unsupported_api_version:
      return "requested Vulkan API version is not supported by the loader";
    case InstanceError::missing_required_extension:
      return "required Vulkan instance extension is unavailable";
    case InstanceError::missing_required_layer:
      return "required Vulkan instance layer is unavailable";
    case InstanceError::loader_unavailable:
      return "the Vulkan loader could not be opened or initialized";
    }
    return "unknown Terreate Graphics instance error";
  }
};

[[nodiscard]] const InstanceErrorCategory &error_category() noexcept {
  static const InstanceErrorCategory category;
  return category;
}

[[nodiscard]] terreate::Error semantic_error(InstanceError error, std::string context,
                                             std::string detail) {
  return terreate::Error{make_error_code(error), std::move(context), std::move(detail)};
}

template <typename T>
[[nodiscard]] terreate::Result<T> semantic_failure(InstanceError error, std::string context,
                                                   std::string detail) {
  return std::unexpected(semantic_error(error, std::move(context), std::move(detail)));
}

[[nodiscard]] terreate::Result<Instance> invalid_plan_failure(std::string detail) {
  return semantic_failure<Instance>(InstanceError::invalid_description, "create Vulkan instance",
                                    std::move(detail));
}

using CheckedAbiCount = std::optional<std::uint32_t>;

[[nodiscard]] constexpr CheckedAbiCount checked_abi_count(std::size_t count) noexcept {
  if (count > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(count);
}

// A vector with UINT32_MAX + 1 entries is not a feasible runtime fixture.  The
// compile-time boundary proof keeps the first unrepresentable ABI count
// covered, while native apply uses this same checked conversion for both
// Vulkan name arrays.
[[nodiscard]] constexpr bool abi_count_overflow_boundary_is_rejected() noexcept {
  if constexpr (std::numeric_limits<std::size_t>::digits >
                std::numeric_limits<std::uint32_t>::digits) {
    constexpr auto first_unrepresentable =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + std::size_t{1};
    return !checked_abi_count(first_unrepresentable).has_value();
  }
  return true;
}

static_assert(checked_abi_count(std::numeric_limits<std::uint32_t>::max()).has_value());
static_assert(abi_count_overflow_boundary_is_rejected());

struct InstanceAbiCounts {
  std::uint32_t extension_count = 0;
  std::uint32_t layer_count = 0;
};

[[nodiscard]] std::optional<InstanceAbiCounts>
checked_instance_abi_counts(const std::vector<std::string> &enabled_extensions,
                            const std::vector<std::string> &enabled_layers) noexcept {
  const auto extension_count = checked_abi_count(enabled_extensions.size());
  const auto layer_count = checked_abi_count(enabled_layers.size());
  if (!extension_count || !layer_count) {
    return std::nullopt;
  }
  return InstanceAbiCounts{.extension_count = *extension_count, .layer_count = *layer_count};
}

template <typename CharacterRange>
[[nodiscard]] std::string copy_native_name(const CharacterRange &characters) {
  const auto first = characters.begin();
  const auto last = std::find(first, characters.end(), '\0');
  return std::string{first, last};
}

[[nodiscard]] std::vector<std::string> canonical_names(std::vector<std::string> names) {
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

struct RequirementEntry {
  RequirementStrength strength = RequirementStrength::optional;
  bool derived = false;
};

using RequirementMap = std::map<std::string, RequirementEntry>;

struct ExplicitRequirementLists {
  const std::vector<std::string> &required;
  const std::vector<std::string> &optional;
};

struct RequirementBuildError {
  InstanceError error = InstanceError::invalid_description;
  std::string name{};
  std::string detail{};
};

[[nodiscard]] std::optional<RequirementBuildError>
embedded_nul_capability_error(const std::vector<std::string> &names, std::string_view kind) {
  for (const auto &name : names) {
    if (name.find('\0') != std::string::npos) {
      std::string detail = "an instance ";
      detail.append(kind);
      detail.append(" capability name must not contain an embedded NUL byte");
      return RequirementBuildError{.error = InstanceError::invalid_description,
                                   .detail = std::move(detail)};
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<RequirementBuildError> add_requirement(RequirementMap &requirements,
                                                                   std::string name,
                                                                   RequirementStrength strength,
                                                                   bool derived) {
  if (name.find('\0') != std::string::npos) {
    return RequirementBuildError{
        .error = InstanceError::invalid_description,
        .detail = "an instance extension or layer requirement name must not contain an embedded "
                  "NUL byte",
    };
  }
  if (name.empty()) {
    return RequirementBuildError{
        .error = InstanceError::invalid_description,
        .detail = "an instance extension or layer name must not be empty",
    };
  }

  const auto [iterator, inserted] =
      requirements.emplace(name, RequirementEntry{.strength = strength, .derived = derived});
  if (inserted) {
    return std::nullopt;
  }

  auto &existing = iterator->second;
  if (existing.strength == strength) {
    // A name explicitly requested and also derived by a documented
    // prerequisite remains explicitly requested.  `derived` describes the
    // effective requirement only when no explicit request supplied it.
    existing.derived = existing.derived && derived;
    return std::nullopt;
  }

  // An explicit required requirement dominates a derived optional
  // prerequisite.  The reverse is a real contradiction: a required Debug
  // Utils policy cannot be hidden behind an explicitly optional extension.
  if (!existing.derived && derived && existing.strength == RequirementStrength::required) {
    return std::nullopt;
  }
  if (existing.derived && !derived && strength == RequirementStrength::required) {
    existing.strength = strength;
    existing.derived = false;
    return std::nullopt;
  }

  return RequirementBuildError{
      .error = InstanceError::contradictory_requirements,
      .name = std::move(name),
      .detail = "the same instance name was declared with required and optional strength",
  };
}

[[nodiscard]] std::optional<RequirementBuildError>
add_explicit_requirements(RequirementMap &requirements, ExplicitRequirementLists lists) {
  for (const auto &name : lists.required) {
    if (const auto error =
            add_requirement(requirements, name, RequirementStrength::required, false)) {
      return error;
    }
  }
  for (const auto &name : lists.optional) {
    if (const auto error =
            add_requirement(requirements, name, RequirementStrength::optional, false)) {
      return error;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::string requirement_detail(std::string_view kind, std::string_view name) {
  std::string detail;
  detail.reserve(kind.size() + name.size() + 64);
  detail.append(kind);
  detail.append(" '");
  detail.append(name);
  detail.append("' is not reported by the Vulkan loader");
  return detail;
}

[[nodiscard]] InstanceDecision make_decision(const std::string &name,
                                             const RequirementEntry &requirement,
                                             InstanceDecisionOutcome outcome,
                                             InstanceDecisionReason reason) {
  return InstanceDecision{
      .name = name,
      .strength = requirement.strength,
      .outcome = outcome,
      .reason = reason,
      .derived = requirement.derived,
  };
}

[[nodiscard]] std::string version_detail(std::uint32_t requested, std::uint32_t available) {
  return "requested Vulkan API version " + std::to_string(requested) + " exceeds loader version " +
         std::to_string(available);
}

[[nodiscard]] std::string_view object_type_name(VkObjectType object_type) noexcept {
  switch (object_type) {
  case VK_OBJECT_TYPE_INSTANCE:
    return "VkInstance";
  case VK_OBJECT_TYPE_PHYSICAL_DEVICE:
    return "VkPhysicalDevice";
  case VK_OBJECT_TYPE_DEVICE:
    return "VkDevice";
  case VK_OBJECT_TYPE_QUEUE:
    return "VkQueue";
  case VK_OBJECT_TYPE_SEMAPHORE:
    return "VkSemaphore";
  case VK_OBJECT_TYPE_COMMAND_BUFFER:
    return "VkCommandBuffer";
  case VK_OBJECT_TYPE_FENCE:
    return "VkFence";
  case VK_OBJECT_TYPE_DEVICE_MEMORY:
    return "VkDeviceMemory";
  case VK_OBJECT_TYPE_BUFFER:
    return "VkBuffer";
  case VK_OBJECT_TYPE_IMAGE:
    return "VkImage";
  case VK_OBJECT_TYPE_EVENT:
    return "VkEvent";
  case VK_OBJECT_TYPE_QUERY_POOL:
    return "VkQueryPool";
  case VK_OBJECT_TYPE_BUFFER_VIEW:
    return "VkBufferView";
  case VK_OBJECT_TYPE_IMAGE_VIEW:
    return "VkImageView";
  case VK_OBJECT_TYPE_SHADER_MODULE:
    return "VkShaderModule";
  case VK_OBJECT_TYPE_PIPELINE_CACHE:
    return "VkPipelineCache";
  case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
    return "VkPipelineLayout";
  case VK_OBJECT_TYPE_RENDER_PASS:
    return "VkRenderPass";
  case VK_OBJECT_TYPE_PIPELINE:
    return "VkPipeline";
  case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:
    return "VkDescriptorSetLayout";
  case VK_OBJECT_TYPE_SAMPLER:
    return "VkSampler";
  case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
    return "VkDescriptorPool";
  case VK_OBJECT_TYPE_DESCRIPTOR_SET:
    return "VkDescriptorSet";
  case VK_OBJECT_TYPE_FRAMEBUFFER:
    return "VkFramebuffer";
  case VK_OBJECT_TYPE_COMMAND_POOL:
    return "VkCommandPool";
  default:
    return "VkObject";
  }
}

[[nodiscard]] detail::NativeDiagnosticSeverity
callback_severity(VkDebugUtilsMessageSeverityFlagBitsEXT severity) noexcept {
  switch (severity) {
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
    return detail::NativeDiagnosticSeverity::verbose;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
    return detail::NativeDiagnosticSeverity::info;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
    return detail::NativeDiagnosticSeverity::warning;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
    return detail::NativeDiagnosticSeverity::error;
  default:
    return detail::NativeDiagnosticSeverity::unknown;
  }
}

VKAPI_ATTR VkBool32 VKAPI_CALL instance_debug_callback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT message_severity,
    vk::DebugUtilsMessageTypeFlagsEXT message_types,
    const vk::DebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data) noexcept {
  // Vulkan invokes this function from native code.  Neither a null user-data
  // pointer nor malformed optional callback data may escape into the bridge,
  // and no exception may cross the Vulkan ABI boundary.
  if (user_data == nullptr || callback_data == nullptr) {
    return VK_FALSE;
  }

  try {
    auto *state = static_cast<CallbackState *>(user_data);

    // The Vulkan-Hpp callback typedef intentionally uses its strongly typed
    // wrappers.  Convert those values explicitly at this private raw-mask
    // boundary before handing them to the Vulkan-neutral diagnostics bridge.
    const auto raw_message_severity =
        static_cast<VkDebugUtilsMessageSeverityFlagBitsEXT>(message_severity);
    const auto raw_message_types = static_cast<VkDebugUtilsMessageTypeFlagsEXT>(message_types);
    const auto mapped_categories = detail::map_vulkan_message_types(raw_message_types);
    if (!mapped_categories) {
      return VK_FALSE;
    }

    std::vector<detail::NativeDiagnosticObject> objects;
    if (callback_data->pObjects != nullptr) {
      objects.reserve(callback_data->objectCount);
      for (std::uint32_t index = 0; index < callback_data->objectCount; ++index) {
        const auto &object = callback_data->pObjects[index];
        objects.push_back(detail::NativeDiagnosticObject{
            .type = object_type_name(static_cast<VkObjectType>(object.objectType)),
            .object_handle = object.objectHandle,
            .name = object.pObjectName == nullptr ? std::string_view{}
                                                  : std::string_view{object.pObjectName},
        });
      }
    }

    const detail::NativeDiagnosticCallbackData native{
        .severity = callback_severity(raw_message_severity),
        .categories = mapped_categories->view(),
        .source = "vulkan",
        .operation = {},
        .message_id_name = callback_data->pMessageIdName == nullptr
                               ? std::string_view{}
                               : std::string_view{callback_data->pMessageIdName},
        .message_id_number = callback_data->messageIdNumber,
        .message = callback_data->pMessage == nullptr ? std::string_view{}
                                                      : std::string_view{callback_data->pMessage},
        .context = {},
        .objects = std::span<const detail::NativeDiagnosticObject>{objects.data(), objects.size()},
    };

    const auto translated = detail::translate_and_emit(native, state->sink);
    if (!translated) {
      return VK_FALSE;
    }
  } catch (...) {
    // Allocation and translation failures are observations only.  A callback
    // must never make a Vulkan implementation call through an exception.
    return VK_FALSE;
  }
  return VK_FALSE;
}

[[nodiscard]] bool
same_extension_properties(const std::vector<PhysicalDeviceExtensionProperty> &left,
                          const std::vector<PhysicalDeviceExtensionProperty> &right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].name != right[index].name ||
        left[index].spec_version != right[index].spec_version) {
      return false;
    }
  }
  return true;
}

/// Vulkan-Hpp's aggregate comparisons are not part of the capability binding
/// contract.  Compare the named semantic members explicitly so that padding in
/// a native Vulkan structure cannot make two equal observations differ.
// clang-format off
template <typename T, typename... Member>
[[nodiscard]] bool same_named_members(const T &left, const T &right, Member... members) noexcept {
  return ((left.*members == right.*members) && ...);
}

[[nodiscard]] bool same_physical_device_limits(const vk::PhysicalDeviceLimits &left,
                                               const vk::PhysicalDeviceLimits &right) noexcept {
  return std::tie(
             left.maxImageDimension1D, left.maxImageDimension2D, left.maxImageDimension3D,
             left.maxImageDimensionCube, left.maxImageArrayLayers, left.maxTexelBufferElements,
             left.maxUniformBufferRange, left.maxStorageBufferRange, left.maxPushConstantsSize,
             left.maxMemoryAllocationCount, left.maxSamplerAllocationCount,
             left.bufferImageGranularity, left.sparseAddressSpaceSize, left.maxBoundDescriptorSets,
             left.maxPerStageDescriptorSamplers, left.maxPerStageDescriptorUniformBuffers,
             left.maxPerStageDescriptorStorageBuffers, left.maxPerStageDescriptorSampledImages,
             left.maxPerStageDescriptorStorageImages, left.maxPerStageDescriptorInputAttachments,
             left.maxPerStageResources, left.maxDescriptorSetSamplers,
             left.maxDescriptorSetUniformBuffers, left.maxDescriptorSetUniformBuffersDynamic,
             left.maxDescriptorSetStorageBuffers, left.maxDescriptorSetStorageBuffersDynamic,
             left.maxDescriptorSetSampledImages, left.maxDescriptorSetStorageImages,
             left.maxDescriptorSetInputAttachments, left.maxVertexInputAttributes,
             left.maxVertexInputBindings, left.maxVertexInputAttributeOffset,
             left.maxVertexInputBindingStride, left.maxVertexOutputComponents,
             left.maxTessellationGenerationLevel, left.maxTessellationPatchSize,
             left.maxTessellationControlPerVertexInputComponents,
             left.maxTessellationControlPerVertexOutputComponents,
             left.maxTessellationControlPerPatchOutputComponents,
             left.maxTessellationControlTotalOutputComponents,
             left.maxTessellationEvaluationInputComponents,
             left.maxTessellationEvaluationOutputComponents, left.maxGeometryShaderInvocations,
             left.maxGeometryInputComponents, left.maxGeometryOutputComponents,
             left.maxGeometryOutputVertices, left.maxGeometryTotalOutputComponents,
             left.maxFragmentInputComponents, left.maxFragmentOutputAttachments,
             left.maxFragmentDualSrcAttachments, left.maxFragmentCombinedOutputResources,
             left.maxComputeSharedMemorySize, left.maxComputeWorkGroupCount,
             left.maxComputeWorkGroupInvocations, left.maxComputeWorkGroupSize,
             left.subPixelPrecisionBits, left.subTexelPrecisionBits, left.mipmapPrecisionBits,
             left.maxDrawIndexedIndexValue, left.maxDrawIndirectCount, left.maxSamplerLodBias,
             left.maxSamplerAnisotropy, left.maxViewports, left.maxViewportDimensions,
             left.viewportBoundsRange, left.viewportSubPixelBits, left.minMemoryMapAlignment,
             left.minTexelBufferOffsetAlignment, left.minUniformBufferOffsetAlignment,
             left.minStorageBufferOffsetAlignment, left.minTexelOffset, left.maxTexelOffset,
             left.minTexelGatherOffset, left.maxTexelGatherOffset, left.minInterpolationOffset,
             left.maxInterpolationOffset, left.subPixelInterpolationOffsetBits,
             left.maxFramebufferWidth, left.maxFramebufferHeight, left.maxFramebufferLayers,
             left.framebufferColorSampleCounts, left.framebufferDepthSampleCounts,
             left.framebufferStencilSampleCounts, left.framebufferNoAttachmentsSampleCounts,
             left.maxColorAttachments, left.sampledImageColorSampleCounts,
             left.sampledImageIntegerSampleCounts, left.sampledImageDepthSampleCounts,
             left.sampledImageStencilSampleCounts, left.storageImageSampleCounts,
             left.maxSampleMaskWords, left.timestampComputeAndGraphics, left.timestampPeriod,
             left.maxClipDistances, left.maxCullDistances, left.maxCombinedClipAndCullDistances,
             left.discreteQueuePriorities, left.pointSizeRange, left.lineWidthRange,
             left.pointSizeGranularity, left.lineWidthGranularity, left.strictLines,
             left.standardSampleLocations, left.optimalBufferCopyOffsetAlignment,
             left.optimalBufferCopyRowPitchAlignment, left.nonCoherentAtomSize) ==
         std::tie(
             right.maxImageDimension1D, right.maxImageDimension2D, right.maxImageDimension3D,
             right.maxImageDimensionCube, right.maxImageArrayLayers, right.maxTexelBufferElements,
             right.maxUniformBufferRange, right.maxStorageBufferRange, right.maxPushConstantsSize,
             right.maxMemoryAllocationCount, right.maxSamplerAllocationCount,
             right.bufferImageGranularity, right.sparseAddressSpaceSize,
             right.maxBoundDescriptorSets, right.maxPerStageDescriptorSamplers,
             right.maxPerStageDescriptorUniformBuffers, right.maxPerStageDescriptorStorageBuffers,
             right.maxPerStageDescriptorSampledImages, right.maxPerStageDescriptorStorageImages,
             right.maxPerStageDescriptorInputAttachments, right.maxPerStageResources,
             right.maxDescriptorSetSamplers, right.maxDescriptorSetUniformBuffers,
             right.maxDescriptorSetUniformBuffersDynamic, right.maxDescriptorSetStorageBuffers,
             right.maxDescriptorSetStorageBuffersDynamic, right.maxDescriptorSetSampledImages,
             right.maxDescriptorSetStorageImages, right.maxDescriptorSetInputAttachments,
             right.maxVertexInputAttributes, right.maxVertexInputBindings,
             right.maxVertexInputAttributeOffset, right.maxVertexInputBindingStride,
             right.maxVertexOutputComponents, right.maxTessellationGenerationLevel,
             right.maxTessellationPatchSize, right.maxTessellationControlPerVertexInputComponents,
             right.maxTessellationControlPerVertexOutputComponents,
             right.maxTessellationControlPerPatchOutputComponents,
             right.maxTessellationControlTotalOutputComponents,
             right.maxTessellationEvaluationInputComponents,
             right.maxTessellationEvaluationOutputComponents, right.maxGeometryShaderInvocations,
             right.maxGeometryInputComponents, right.maxGeometryOutputComponents,
             right.maxGeometryOutputVertices, right.maxGeometryTotalOutputComponents,
             right.maxFragmentInputComponents, right.maxFragmentOutputAttachments,
             right.maxFragmentDualSrcAttachments, right.maxFragmentCombinedOutputResources,
             right.maxComputeSharedMemorySize, right.maxComputeWorkGroupCount,
             right.maxComputeWorkGroupInvocations, right.maxComputeWorkGroupSize,
             right.subPixelPrecisionBits, right.subTexelPrecisionBits, right.mipmapPrecisionBits,
             right.maxDrawIndexedIndexValue, right.maxDrawIndirectCount, right.maxSamplerLodBias,
             right.maxSamplerAnisotropy, right.maxViewports, right.maxViewportDimensions,
             right.viewportBoundsRange, right.viewportSubPixelBits, right.minMemoryMapAlignment,
             right.minTexelBufferOffsetAlignment, right.minUniformBufferOffsetAlignment,
              right.minStorageBufferOffsetAlignment, right.minTexelOffset,
              right.maxTexelOffset, right.minTexelGatherOffset, right.maxTexelGatherOffset,
              right.minInterpolationOffset, right.maxInterpolationOffset,
              right.subPixelInterpolationOffsetBits, right.maxFramebufferWidth,
              right.maxFramebufferHeight, right.maxFramebufferLayers,
             right.framebufferColorSampleCounts, right.framebufferDepthSampleCounts,
             right.framebufferStencilSampleCounts, right.framebufferNoAttachmentsSampleCounts,
             right.maxColorAttachments, right.sampledImageColorSampleCounts,
             right.sampledImageIntegerSampleCounts, right.sampledImageDepthSampleCounts,
             right.sampledImageStencilSampleCounts, right.storageImageSampleCounts,
             right.maxSampleMaskWords, right.timestampComputeAndGraphics, right.timestampPeriod,
             right.maxClipDistances, right.maxCullDistances, right.maxCombinedClipAndCullDistances,
             right.discreteQueuePriorities, right.pointSizeRange, right.lineWidthRange,
             right.pointSizeGranularity, right.lineWidthGranularity, right.strictLines,
             right.standardSampleLocations, right.optimalBufferCopyOffsetAlignment,
             right.optimalBufferCopyRowPitchAlignment, right.nonCoherentAtomSize);
}

[[nodiscard]] bool
same_physical_device_sparse_properties(const vk::PhysicalDeviceSparseProperties &left,
                                       const vk::PhysicalDeviceSparseProperties &right) noexcept {
  return same_named_members(
      left, right, &vk::PhysicalDeviceSparseProperties::residencyStandard2DBlockShape,
      &vk::PhysicalDeviceSparseProperties::residencyStandard2DMultisampleBlockShape,
      &vk::PhysicalDeviceSparseProperties::residencyStandard3DBlockShape,
      &vk::PhysicalDeviceSparseProperties::residencyAlignedMipSize,
      &vk::PhysicalDeviceSparseProperties::residencyNonResidentStrict);
}

[[nodiscard]] bool
same_physical_device_properties(const vk::PhysicalDeviceProperties &left,
                                const vk::PhysicalDeviceProperties &right) noexcept {
  return same_named_members(
             left, right, &vk::PhysicalDeviceProperties::apiVersion,
             &vk::PhysicalDeviceProperties::driverVersion,
             &vk::PhysicalDeviceProperties::vendorID, &vk::PhysicalDeviceProperties::deviceID,
             &vk::PhysicalDeviceProperties::deviceType, &vk::PhysicalDeviceProperties::deviceName,
             &vk::PhysicalDeviceProperties::pipelineCacheUUID) &&
         same_physical_device_limits(left.limits, right.limits) &&
         same_physical_device_sparse_properties(left.sparseProperties, right.sparseProperties);
}

[[nodiscard]] bool
same_physical_device_properties2(const vk::PhysicalDeviceProperties2 &left,
                                 const vk::PhysicalDeviceProperties2 &right) noexcept {
  return same_named_members(left, right, &vk::PhysicalDeviceProperties2::sType,
                            &vk::PhysicalDeviceProperties2::pNext) &&
         same_physical_device_properties(left.properties, right.properties);
}

[[nodiscard]] bool
same_physical_device_id_properties(const vk::PhysicalDeviceIDProperties &left,
                                   const vk::PhysicalDeviceIDProperties &right) noexcept {
  return same_named_members(
      left, right, &vk::PhysicalDeviceIDProperties::sType, &vk::PhysicalDeviceIDProperties::pNext,
      &vk::PhysicalDeviceIDProperties::deviceUUID, &vk::PhysicalDeviceIDProperties::driverUUID,
      &vk::PhysicalDeviceIDProperties::deviceLUID, &vk::PhysicalDeviceIDProperties::deviceNodeMask,
      &vk::PhysicalDeviceIDProperties::deviceLUIDValid);
}

[[nodiscard]] bool
same_physical_device_driver_properties(const vk::PhysicalDeviceDriverProperties &left,
                                       const vk::PhysicalDeviceDriverProperties &right) noexcept {
  return same_named_members(
      left, right,
       &vk::PhysicalDeviceDriverProperties::sType,
       &vk::PhysicalDeviceDriverProperties::pNext,
       &vk::PhysicalDeviceDriverProperties::driverID,
       &vk::PhysicalDeviceDriverProperties::driverName,
       &vk::PhysicalDeviceDriverProperties::driverInfo) &&
          left.conformanceVersion.major == right.conformanceVersion.major &&
          left.conformanceVersion.minor == right.conformanceVersion.minor &&
          left.conformanceVersion.subminor == right.conformanceVersion.subminor &&
          left.conformanceVersion.patch == right.conformanceVersion.patch;
}

[[nodiscard]] bool same_physical_device_features(const vk::PhysicalDeviceFeatures &left,
                                                 const vk::PhysicalDeviceFeatures &right) noexcept {
  return same_named_members(
      left, right, &vk::PhysicalDeviceFeatures::robustBufferAccess,
      &vk::PhysicalDeviceFeatures::fullDrawIndexUint32, &vk::PhysicalDeviceFeatures::imageCubeArray,
      &vk::PhysicalDeviceFeatures::independentBlend, &vk::PhysicalDeviceFeatures::geometryShader,
      &vk::PhysicalDeviceFeatures::tessellationShader,
      &vk::PhysicalDeviceFeatures::sampleRateShading, &vk::PhysicalDeviceFeatures::dualSrcBlend,
      &vk::PhysicalDeviceFeatures::logicOp, &vk::PhysicalDeviceFeatures::multiDrawIndirect,
      &vk::PhysicalDeviceFeatures::drawIndirectFirstInstance, &vk::PhysicalDeviceFeatures::depthClamp,
      &vk::PhysicalDeviceFeatures::depthBiasClamp, &vk::PhysicalDeviceFeatures::fillModeNonSolid,
      &vk::PhysicalDeviceFeatures::depthBounds, &vk::PhysicalDeviceFeatures::wideLines,
      &vk::PhysicalDeviceFeatures::largePoints, &vk::PhysicalDeviceFeatures::alphaToOne,
      &vk::PhysicalDeviceFeatures::multiViewport, &vk::PhysicalDeviceFeatures::samplerAnisotropy,
      &vk::PhysicalDeviceFeatures::textureCompressionETC2,
      &vk::PhysicalDeviceFeatures::textureCompressionASTC_LDR,
      &vk::PhysicalDeviceFeatures::textureCompressionBC,
      &vk::PhysicalDeviceFeatures::occlusionQueryPrecise,
      &vk::PhysicalDeviceFeatures::pipelineStatisticsQuery,
      &vk::PhysicalDeviceFeatures::vertexPipelineStoresAndAtomics,
      &vk::PhysicalDeviceFeatures::fragmentStoresAndAtomics,
      &vk::PhysicalDeviceFeatures::shaderTessellationAndGeometryPointSize,
      &vk::PhysicalDeviceFeatures::shaderImageGatherExtended,
      &vk::PhysicalDeviceFeatures::shaderStorageImageExtendedFormats,
      &vk::PhysicalDeviceFeatures::shaderStorageImageMultisample,
      &vk::PhysicalDeviceFeatures::shaderStorageImageReadWithoutFormat,
      &vk::PhysicalDeviceFeatures::shaderStorageImageWriteWithoutFormat,
      &vk::PhysicalDeviceFeatures::shaderUniformBufferArrayDynamicIndexing,
      &vk::PhysicalDeviceFeatures::shaderSampledImageArrayDynamicIndexing,
      &vk::PhysicalDeviceFeatures::shaderStorageBufferArrayDynamicIndexing,
      &vk::PhysicalDeviceFeatures::shaderStorageImageArrayDynamicIndexing,
      &vk::PhysicalDeviceFeatures::shaderClipDistance, &vk::PhysicalDeviceFeatures::shaderCullDistance,
      &vk::PhysicalDeviceFeatures::shaderFloat64, &vk::PhysicalDeviceFeatures::shaderInt64,
      &vk::PhysicalDeviceFeatures::shaderInt16, &vk::PhysicalDeviceFeatures::shaderResourceResidency,
      &vk::PhysicalDeviceFeatures::shaderResourceMinLod, &vk::PhysicalDeviceFeatures::sparseBinding,
      &vk::PhysicalDeviceFeatures::sparseResidencyBuffer,
      &vk::PhysicalDeviceFeatures::sparseResidencyImage2D,
      &vk::PhysicalDeviceFeatures::sparseResidencyImage3D,
      &vk::PhysicalDeviceFeatures::sparseResidency2Samples,
      &vk::PhysicalDeviceFeatures::sparseResidency4Samples,
      &vk::PhysicalDeviceFeatures::sparseResidency8Samples,
      &vk::PhysicalDeviceFeatures::sparseResidency16Samples,
      &vk::PhysicalDeviceFeatures::sparseResidencyAliased,
      &vk::PhysicalDeviceFeatures::variableMultisampleRate,
      &vk::PhysicalDeviceFeatures::inheritedQueries);
}

[[nodiscard]] bool same_physical_device_vulkan11_features(
    const vk::PhysicalDeviceVulkan11Features &left,
    const vk::PhysicalDeviceVulkan11Features &right) noexcept {
  return same_named_members(
      left, right, &vk::PhysicalDeviceVulkan11Features::sType,
      &vk::PhysicalDeviceVulkan11Features::pNext,
      &vk::PhysicalDeviceVulkan11Features::storageBuffer16BitAccess,
      &vk::PhysicalDeviceVulkan11Features::uniformAndStorageBuffer16BitAccess,
      &vk::PhysicalDeviceVulkan11Features::storagePushConstant16,
      &vk::PhysicalDeviceVulkan11Features::storageInputOutput16,
      &vk::PhysicalDeviceVulkan11Features::multiview,
      &vk::PhysicalDeviceVulkan11Features::multiviewGeometryShader,
      &vk::PhysicalDeviceVulkan11Features::multiviewTessellationShader,
      &vk::PhysicalDeviceVulkan11Features::variablePointersStorageBuffer,
      &vk::PhysicalDeviceVulkan11Features::variablePointers,
      &vk::PhysicalDeviceVulkan11Features::protectedMemory,
      &vk::PhysicalDeviceVulkan11Features::samplerYcbcrConversion,
      &vk::PhysicalDeviceVulkan11Features::shaderDrawParameters);
}

[[nodiscard]] bool same_physical_device_vulkan12_features(
    const vk::PhysicalDeviceVulkan12Features &left,
    const vk::PhysicalDeviceVulkan12Features &right) noexcept {
  return same_named_members(
      left, right, &vk::PhysicalDeviceVulkan12Features::sType,
      &vk::PhysicalDeviceVulkan12Features::pNext,
      &vk::PhysicalDeviceVulkan12Features::samplerMirrorClampToEdge,
      &vk::PhysicalDeviceVulkan12Features::drawIndirectCount,
      &vk::PhysicalDeviceVulkan12Features::storageBuffer8BitAccess,
      &vk::PhysicalDeviceVulkan12Features::uniformAndStorageBuffer8BitAccess,
      &vk::PhysicalDeviceVulkan12Features::storagePushConstant8,
      &vk::PhysicalDeviceVulkan12Features::shaderBufferInt64Atomics,
      &vk::PhysicalDeviceVulkan12Features::shaderSharedInt64Atomics,
      &vk::PhysicalDeviceVulkan12Features::shaderFloat16,
      &vk::PhysicalDeviceVulkan12Features::shaderInt8,
      &vk::PhysicalDeviceVulkan12Features::descriptorIndexing,
      &vk::PhysicalDeviceVulkan12Features::shaderInputAttachmentArrayDynamicIndexing,
      &vk::PhysicalDeviceVulkan12Features::shaderUniformTexelBufferArrayDynamicIndexing,
      &vk::PhysicalDeviceVulkan12Features::shaderStorageTexelBufferArrayDynamicIndexing,
      &vk::PhysicalDeviceVulkan12Features::shaderUniformBufferArrayNonUniformIndexing,
      &vk::PhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing,
      &vk::PhysicalDeviceVulkan12Features::shaderStorageBufferArrayNonUniformIndexing,
      &vk::PhysicalDeviceVulkan12Features::shaderStorageImageArrayNonUniformIndexing,
      &vk::PhysicalDeviceVulkan12Features::shaderInputAttachmentArrayNonUniformIndexing,
      &vk::PhysicalDeviceVulkan12Features::shaderUniformTexelBufferArrayNonUniformIndexing,
      &vk::PhysicalDeviceVulkan12Features::shaderStorageTexelBufferArrayNonUniformIndexing,
      &vk::PhysicalDeviceVulkan12Features::descriptorBindingUniformBufferUpdateAfterBind,
      &vk::PhysicalDeviceVulkan12Features::descriptorBindingSampledImageUpdateAfterBind,
      &vk::PhysicalDeviceVulkan12Features::descriptorBindingStorageImageUpdateAfterBind,
      &vk::PhysicalDeviceVulkan12Features::descriptorBindingStorageBufferUpdateAfterBind,
      &vk::PhysicalDeviceVulkan12Features::descriptorBindingUniformTexelBufferUpdateAfterBind,
      &vk::PhysicalDeviceVulkan12Features::descriptorBindingStorageTexelBufferUpdateAfterBind,
      &vk::PhysicalDeviceVulkan12Features::descriptorBindingUpdateUnusedWhilePending,
      &vk::PhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound,
      &vk::PhysicalDeviceVulkan12Features::descriptorBindingVariableDescriptorCount,
      &vk::PhysicalDeviceVulkan12Features::runtimeDescriptorArray,
      &vk::PhysicalDeviceVulkan12Features::samplerFilterMinmax,
      &vk::PhysicalDeviceVulkan12Features::scalarBlockLayout,
      &vk::PhysicalDeviceVulkan12Features::imagelessFramebuffer,
      &vk::PhysicalDeviceVulkan12Features::uniformBufferStandardLayout,
      &vk::PhysicalDeviceVulkan12Features::shaderSubgroupExtendedTypes,
      &vk::PhysicalDeviceVulkan12Features::separateDepthStencilLayouts,
      &vk::PhysicalDeviceVulkan12Features::hostQueryReset,
      &vk::PhysicalDeviceVulkan12Features::timelineSemaphore,
      &vk::PhysicalDeviceVulkan12Features::bufferDeviceAddress,
      &vk::PhysicalDeviceVulkan12Features::bufferDeviceAddressCaptureReplay,
      &vk::PhysicalDeviceVulkan12Features::bufferDeviceAddressMultiDevice,
      &vk::PhysicalDeviceVulkan12Features::vulkanMemoryModel,
      &vk::PhysicalDeviceVulkan12Features::vulkanMemoryModelDeviceScope,
      &vk::PhysicalDeviceVulkan12Features::vulkanMemoryModelAvailabilityVisibilityChains,
      &vk::PhysicalDeviceVulkan12Features::shaderOutputViewportIndex,
      &vk::PhysicalDeviceVulkan12Features::shaderOutputLayer,
      &vk::PhysicalDeviceVulkan12Features::subgroupBroadcastDynamicId);
}

[[nodiscard]] bool same_physical_device_vulkan13_features(
    const vk::PhysicalDeviceVulkan13Features &left,
    const vk::PhysicalDeviceVulkan13Features &right) noexcept {
  return same_named_members(
      left, right, &vk::PhysicalDeviceVulkan13Features::sType,
      &vk::PhysicalDeviceVulkan13Features::pNext,
      &vk::PhysicalDeviceVulkan13Features::robustImageAccess,
      &vk::PhysicalDeviceVulkan13Features::inlineUniformBlock,
      &vk::PhysicalDeviceVulkan13Features::descriptorBindingInlineUniformBlockUpdateAfterBind,
      &vk::PhysicalDeviceVulkan13Features::pipelineCreationCacheControl,
      &vk::PhysicalDeviceVulkan13Features::privateData,
      &vk::PhysicalDeviceVulkan13Features::shaderDemoteToHelperInvocation,
      &vk::PhysicalDeviceVulkan13Features::shaderTerminateInvocation,
      &vk::PhysicalDeviceVulkan13Features::subgroupSizeControl,
      &vk::PhysicalDeviceVulkan13Features::computeFullSubgroups,
      &vk::PhysicalDeviceVulkan13Features::synchronization2,
      &vk::PhysicalDeviceVulkan13Features::textureCompressionASTC_HDR,
      &vk::PhysicalDeviceVulkan13Features::shaderZeroInitializeWorkgroupMemory,
      &vk::PhysicalDeviceVulkan13Features::dynamicRendering,
      &vk::PhysicalDeviceVulkan13Features::shaderIntegerDotProduct,
      &vk::PhysicalDeviceVulkan13Features::maintenance4);
}

[[nodiscard]] bool same_physical_device_features2(const vk::PhysicalDeviceFeatures2 &left,
                                                  const vk::PhysicalDeviceFeatures2 &right) noexcept {
  return same_named_members(left, right, &vk::PhysicalDeviceFeatures2::sType,
                            &vk::PhysicalDeviceFeatures2::pNext) &&
         same_physical_device_features(left.features, right.features);
}

[[nodiscard]] bool same_queue_family_properties(const vk::QueueFamilyProperties &left,
                                                const vk::QueueFamilyProperties &right) noexcept {
  return same_named_members(left, right, &vk::QueueFamilyProperties::queueFlags,
                            &vk::QueueFamilyProperties::queueCount,
                            &vk::QueueFamilyProperties::timestampValidBits) &&
         same_named_members(left.minImageTransferGranularity,
                            right.minImageTransferGranularity, &vk::Extent3D::width,
                            &vk::Extent3D::height, &vk::Extent3D::depth);
}

[[nodiscard]] bool same_queue_family_properties(
    const std::vector<vk::QueueFamilyProperties> &left,
    const std::vector<vk::QueueFamilyProperties> &right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!same_queue_family_properties(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

/// Compare only the populated portions of the Vulkan memory-property arrays.
/// Both counts are validated before indexing so public synthetic snapshots
/// cannot turn a malformed count into an out-of-bounds read.
[[nodiscard]] bool same_physical_device_memory_properties(
    const vk::PhysicalDeviceMemoryProperties &left,
    const vk::PhysicalDeviceMemoryProperties &right) noexcept {
  if (left.memoryTypeCount > VK_MAX_MEMORY_TYPES ||
      right.memoryTypeCount > VK_MAX_MEMORY_TYPES ||
      left.memoryHeapCount > VK_MAX_MEMORY_HEAPS ||
      right.memoryHeapCount > VK_MAX_MEMORY_HEAPS ||
      left.memoryTypeCount != right.memoryTypeCount ||
      left.memoryHeapCount != right.memoryHeapCount) {
    return false;
  }

  for (std::uint32_t index = 0; index < left.memoryTypeCount; ++index) {
    if (left.memoryTypes[index].propertyFlags != right.memoryTypes[index].propertyFlags ||
        left.memoryTypes[index].heapIndex != right.memoryTypes[index].heapIndex) {
      return false;
    }
  }
  for (std::uint32_t index = 0; index < left.memoryHeapCount; ++index) {
    if (left.memoryHeaps[index].size != right.memoryHeaps[index].size ||
        left.memoryHeaps[index].flags != right.memoryHeaps[index].flags) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool same_physical_device_memory_budget(
    const vk::PhysicalDeviceMemoryBudgetPropertiesEXT &left,
    const vk::PhysicalDeviceMemoryBudgetPropertiesEXT &right,
    std::uint32_t memory_heap_count) noexcept {
  if (!same_named_members(left, right, &vk::PhysicalDeviceMemoryBudgetPropertiesEXT::sType,
                          &vk::PhysicalDeviceMemoryBudgetPropertiesEXT::pNext)) {
    return false;
  }
  for (std::uint32_t index = 0; index < memory_heap_count; ++index) {
    if (left.heapBudget[index] != right.heapBudget[index] ||
        left.heapUsage[index] != right.heapUsage[index]) {
      return false;
    }
  }
  return true;
}

// clang-format on

/// Compare the complete value-owned observation rather than a digest or raw
/// object representation.  Native extension observations are canonicalised
/// before a token is created, and every canonical sequence element plus every
/// Vulkan structure member (including pNext, which must remain null) is checked.
[[nodiscard]] bool
same_physical_device_capabilities(const PhysicalDeviceCapabilities &left,
                                  const PhysicalDeviceCapabilities &right) noexcept {
  if (left.api_version != right.api_version ||
      left.properties2_available != right.properties2_available ||
      left.id_properties_available != right.id_properties_available ||
      left.driver_properties_available != right.driver_properties_available ||
      left.features2_available != right.features2_available ||
      left.features_11_available != right.features_11_available ||
      left.features_12_available != right.features_12_available ||
      left.features_13_available != right.features_13_available ||
      left.memory_properties2_available != right.memory_properties2_available) {
    return false;
  }
  if (!same_physical_device_properties(left.properties, right.properties) ||
      !same_physical_device_properties2(left.properties2, right.properties2) ||
      !same_physical_device_id_properties(left.id_properties, right.id_properties) ||
      !same_physical_device_driver_properties(left.driver_properties, right.driver_properties)) {
    return false;
  }
  if (left.device_uuid != right.device_uuid || left.driver_uuid != right.driver_uuid ||
      left.driver_id != right.driver_id || left.driver_name != right.driver_name ||
      left.driver_info != right.driver_info ||
      !same_physical_device_features(left.features_10, right.features_10) ||
      !same_physical_device_vulkan11_features(left.features_11, right.features_11) ||
      !same_physical_device_vulkan12_features(left.features_12, right.features_12) ||
      !same_physical_device_vulkan13_features(left.features_13, right.features_13)) {
    return false;
  }
  if (!same_physical_device_features2(left.features2, right.features2)) {
    return false;
  }
  if (left.extensions != right.extensions ||
      !same_extension_properties(left.extension_properties, right.extension_properties) ||
      !same_queue_family_properties(left.queue_families, right.queue_families) ||
      !same_physical_device_memory_properties(left.memory_properties, right.memory_properties)) {
    return false;
  }
  if (left.memory_budget.has_value() != right.memory_budget.has_value()) {
    return false;
  }
  return !left.memory_budget ||
         same_physical_device_memory_budget(*left.memory_budget, *right.memory_budget,
                                            left.memory_properties.memoryHeapCount);
}

[[nodiscard]] terreate::Error selection_error(PhysicalDeviceError error, std::string context,
                                              std::string detail) {
  return terreate::Error{make_error_code(error), std::move(context), std::move(detail)};
}

template <typename T>
[[nodiscard]] terreate::Result<T> selection_fail(PhysicalDeviceError error, std::string context,
                                                 std::string detail) {
  return std::unexpected(selection_error(error, std::move(context), std::move(detail)));
}

[[nodiscard]] bool selection_same_uuid(const PhysicalDeviceUuid &left,
                                       const PhysicalDeviceUuid &right) noexcept {
  return left == right;
}

[[nodiscard]] bool selection_matches_available_uuid(const PhysicalDeviceCapabilities &capabilities,
                                                    const PhysicalDeviceUuid &uuid) noexcept {
  // An unavailable ID-properties query leaves the value-owned UUID at its
  // zero-initialised value.  That value is not an observation and must never
  // satisfy an identity requirement or selection policy.
  return capabilities.id_properties_available &&
         selection_same_uuid(capabilities.device_uuid, uuid);
}

struct PhysicalDeviceSelectionNameRequirements {
  std::span<const std::string> required;
  std::span<const std::string> optional;
};

[[nodiscard]] std::optional<std::string>
validate_selection_extension_names(PhysicalDeviceSelectionNameRequirements names) {
  for (const auto &name : names.required) {
    if (name.empty() || name.find('\0') != std::string::npos) {
      return "a required physical-device extension name must be non-empty and must not contain an "
             "embedded NUL byte";
    }
  }
  for (const auto &name : names.optional) {
    if (name.empty() || name.find('\0') != std::string::npos) {
      return "an optional physical-device extension name must be non-empty and must not contain "
             "an embedded NUL byte";
    }
  }
  for (const auto &name : names.required) {
    if (std::count(names.optional.begin(), names.optional.end(), name) != 0) {
      return "physical-device extension '" + name + "' was declared both required and optional";
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string>
validate_selection_requirements(const PhysicalDeviceRequirements &requirements) {
  if (requirements.minimum_api_version && *requirements.minimum_api_version == 0) {
    return "minimum_api_version must be non-zero";
  }
  const PhysicalDeviceSelectionNameRequirements extension_names{requirements.required_extensions,
                                                                requirements.optional_extensions};
  if (const auto error = validate_selection_extension_names(extension_names)) {
    return error;
  }
  const auto validate_queue_requirements =
      [](const auto &queue_requirements) -> std::optional<std::string> {
    for (const auto &requirement : queue_requirements) {
      if (requirement.min_queue_count == 0 || static_cast<VkQueueFlags>(requirement.flags) == 0) {
        return "queue requirements must contain a non-zero flag mask and queue count";
      }
    }
    return std::nullopt;
  };
  if (const auto error = validate_queue_requirements(requirements.required_queue_families)) {
    return error;
  }
  if (const auto error = validate_queue_requirements(requirements.optional_queue_families)) {
    return error;
  }
  return std::nullopt;
}

} // namespace

const std::error_category &instance_error_category() noexcept { return error_category(); }

std::error_code make_error_code(InstanceError error) noexcept {
  return {static_cast<int>(error), instance_error_category()};
}

namespace {

[[nodiscard]] std::unique_ptr<vk::raii::Context> make_instance_context() {
  return std::make_unique<vk::raii::Context>();
}

[[nodiscard]] auto debug_message_types() {
  auto message_types = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
#ifdef VK_EXT_device_address_binding
  // Do not name the generated Hpp enumerator here.  The semantic category is
  // optional in older Vulkan-Hpp surfaces even though the raw extension bit
  // can be observed by a newer loader.
  message_types |= static_cast<vk::DebugUtilsMessageTypeFlagBitsEXT>(
      VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT);
#endif
  return message_types;
}

} // namespace

namespace {

[[nodiscard]] Result<InstanceCapabilities>
query_instance_capabilities_from_context(const vk::raii::Context *context) {
  InstanceCapabilities capabilities;

  // Vulkan 1.0 has no vkEnumerateInstanceVersion entry point.  Read the
  // function from the actual Hpp dispatcher and let the private adapter apply
  // the specification-defined fallback without entering Hpp's asserting
  // Context::enumerateInstanceVersion wrapper.
  capabilities.loader_api_version =
      detail::query_instance_api_version(context->getDispatcher()->vkEnumerateInstanceVersion);

  const auto native_extensions = context->enumerateInstanceExtensionProperties();
  const auto native_layers = context->enumerateInstanceLayerProperties();

  capabilities.available_extensions.reserve(native_extensions.size());
  for (const auto &extension : native_extensions) {
    capabilities.available_extensions.push_back(copy_native_name(extension.extensionName));
  }
  capabilities.available_layers.reserve(native_layers.size());
  for (const auto &layer : native_layers) {
    capabilities.available_layers.push_back(copy_native_name(layer.layerName));
  }

  capabilities.available_extensions = canonical_names(std::move(capabilities.available_extensions));
  capabilities.available_layers = canonical_names(std::move(capabilities.available_layers));
  return capabilities;
}

} // namespace

Result<InstanceCapabilities> queryInstanceCapabilities() {
  return detail::query_instance_capabilities(&make_instance_context,
                                             &query_instance_capabilities_from_context);
}

Result<InstancePlan> resolveInstance(const InstanceDescription &description,
                                     const InstanceCapabilities &capabilities) {
  switch (description.debug_utils) {
  case DebugUtilsMode::disabled:
  case DebugUtilsMode::optional:
  case DebugUtilsMode::required:
    break;
  default:
    return semantic_failure<InstancePlan>(
        InstanceError::invalid_description, "resolve Vulkan instance",
        "debug_utils contains an invalid underlying DebugUtilsMode value");
  }

  if (description.application_name.find('\0') != std::string::npos) {
    return semantic_failure<InstancePlan>(InstanceError::invalid_description,
                                          "resolve Vulkan instance",
                                          "application_name must not contain an embedded NUL byte");
  }
  if (description.engine_name.find('\0') != std::string::npos) {
    return semantic_failure<InstancePlan>(InstanceError::invalid_description,
                                          "resolve Vulkan instance",
                                          "engine_name must not contain an embedded NUL byte");
  }

  const auto explicit_api_version = description.api_version;
  if (explicit_api_version && *explicit_api_version == 0) {
    return semantic_failure<InstancePlan>(InstanceError::invalid_description,
                                          "resolve Vulkan instance",
                                          "requested Vulkan API version must be non-zero");
  }

  const auto loader_api_version = capabilities.loader_api_version;
  const auto effective_api_version = explicit_api_version.value_or(default_instance_api_version);
  if (effective_api_version > loader_api_version) {
    return semantic_failure<InstancePlan>(
        InstanceError::unsupported_api_version, "resolve Vulkan instance",
        version_detail(effective_api_version, loader_api_version));
  }

  RequirementMap extension_requirements;
  const ExplicitRequirementLists extension_lists{description.required_extensions,
                                                 description.optional_extensions};
  if (const auto error = add_explicit_requirements(extension_requirements, extension_lists)) {
    return semantic_failure<InstancePlan>(
        error->error, "resolve Vulkan instance",
        error->detail + (error->name.empty() ? std::string{} : " ('" + error->name + "')"));
  }

  RequirementMap layer_requirements;
  const ExplicitRequirementLists layer_lists{description.required_layers,
                                             description.optional_layers};
  if (const auto error = add_explicit_requirements(layer_requirements, layer_lists)) {
    return semantic_failure<InstancePlan>(
        error->error, "resolve Vulkan instance",
        error->detail + (error->name.empty() ? std::string{} : " ('" + error->name + "')"));
  }

  if (const auto error =
          embedded_nul_capability_error(capabilities.available_extensions, "extension")) {
    return semantic_failure<InstancePlan>(error->error, "resolve Vulkan instance", error->detail);
  }
  if (const auto error = embedded_nul_capability_error(capabilities.available_layers, "layer")) {
    return semantic_failure<InstancePlan>(error->error, "resolve Vulkan instance", error->detail);
  }

  constexpr std::string_view debug_utils_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (description.debug_utils != DebugUtilsMode::disabled) {
    const auto debug_strength = description.debug_utils == DebugUtilsMode::required
                                    ? RequirementStrength::required
                                    : RequirementStrength::optional;
    if (const auto error = add_requirement(
            extension_requirements, std::string{debug_utils_extension}, debug_strength, true)) {
      return semantic_failure<InstancePlan>(error->error, "resolve Vulkan instance",
                                            error->detail + " ('" + error->name + "')");
    }
  }

  const auto available_extensions = canonical_names(capabilities.available_extensions);
  const auto available_layers = canonical_names(capabilities.available_layers);

  const auto has_extension = [&](const std::string &name) {
    return std::binary_search(available_extensions.begin(), available_extensions.end(), name);
  };
  const auto has_layer = [&](const std::string &name) {
    return std::binary_search(available_layers.begin(), available_layers.end(), name);
  };

  std::vector<std::string> enabled_extensions;
  std::vector<std::string> enabled_layers;
  std::vector<InstanceDecision> extension_decisions;
  std::vector<InstanceDecision> layer_decisions;

  for (const auto &[name, requirement] : extension_requirements) {
    if (!has_extension(name)) {
      if (requirement.strength == RequirementStrength::required) {
        return semantic_failure<InstancePlan>(
            InstanceError::missing_required_extension, "resolve Vulkan instance",
            requirement_detail("required instance extension", name));
      }
      if (requirement.derived && description.debug_utils != DebugUtilsMode::disabled) {
        // An optional Debug Utils prerequisite may be declined.  The decision
        // is retained in the successful plan rather than disappearing as an
        // implicit feature fallback.
        extension_decisions.push_back(make_decision(name, requirement,
                                                    InstanceDecisionOutcome::declined,
                                                    InstanceDecisionReason::unsupported));
        continue;
      }
      extension_decisions.push_back(make_decision(name, requirement,
                                                  InstanceDecisionOutcome::declined,
                                                  InstanceDecisionReason::unsupported));
      continue;
    }

    enabled_extensions.push_back(name);
    extension_decisions.push_back(
        make_decision(name, requirement, InstanceDecisionOutcome::accepted,
                      requirement.derived ? InstanceDecisionReason::derived_prerequisite
                                          : (requirement.strength == RequirementStrength::optional
                                                 ? InstanceDecisionReason::supported
                                                 : InstanceDecisionReason::requested)));
  }

  for (const auto &[name, requirement] : layer_requirements) {
    if (!has_layer(name)) {
      if (requirement.strength == RequirementStrength::required) {
        return semantic_failure<InstancePlan>(InstanceError::missing_required_layer,
                                              "resolve Vulkan instance",
                                              requirement_detail("required instance layer", name));
      }
      const auto decision = make_decision(name, requirement, InstanceDecisionOutcome::declined,
                                          InstanceDecisionReason::unsupported);
      layer_decisions.push_back(decision);
      continue;
    }

    enabled_layers.push_back(name);
    const auto decision = make_decision(name, requirement, InstanceDecisionOutcome::accepted,
                                        requirement.strength == RequirementStrength::optional
                                            ? InstanceDecisionReason::supported
                                            : InstanceDecisionReason::requested);
    layer_decisions.push_back(decision);
  }

  const bool debug_utils_enabled =
      description.debug_utils != DebugUtilsMode::disabled &&
      std::binary_search(enabled_extensions.begin(), enabled_extensions.end(),
                         std::string{debug_utils_extension});
  const auto debug_decision = std::find_if(
      extension_decisions.begin(), extension_decisions.end(),
      [&](const InstanceDecision &decision) { return decision.name == debug_utils_extension; });
  const bool debug_utils_derived =
      debug_utils_enabled && debug_decision != extension_decisions.end() && debug_decision->derived;

  // The only construction path is this successful resolver boundary.  The
  // input snapshots are copied exactly once into the plan; effective values
  // and decisions are the separate canonical collections built above.
  return InstancePlan{description,
                      capabilities,
                      effective_api_version,
                      explicit_api_version ? InstanceApiVersionProvenance::explicit_request
                                           : InstanceApiVersionProvenance::default_policy,
                      std::move(enabled_extensions),
                      std::move(enabled_layers),
                      std::move(extension_decisions),
                      std::move(layer_decisions),
                      debug_utils_enabled,
                      debug_utils_derived};
}

Instance::Impl::Impl(std::unique_ptr<vk::raii::Context> context_owner,
                     std::unique_ptr<CallbackState> callback_owner,
                     vk::raii::Instance instance_owner,
                     std::optional<vk::raii::DebugUtilsMessengerEXT> messenger_owner,
                     const InstancePlan &instance_plan)
    : lifetime(std::make_unique<Instance::LifetimeToken>()), context(std::move(context_owner)),
      callback(std::move(callback_owner)), instance(std::move(instance_owner)),
      messenger(std::move(messenger_owner)), plan(instance_plan) {}

Instance::Impl::~Impl() noexcept = default;

Instance::Instance(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

Instance::Instance(Instance &&other) noexcept
    : implementation_(std::move(other.implementation_)),
      retired_lifetime_tokens_(std::move(other.retired_lifetime_tokens_)),
      retired_physical_device_tokens_(std::move(other.retired_physical_device_tokens_)) {}

Instance &Instance::operator=(Instance &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (implementation_ != nullptr) {
    auto retired_devices = std::move(implementation_->physical_device_tokens);
    if (retired_devices != nullptr) {
      for (auto *token = retired_devices.get(); token != nullptr; token = token->next.get()) {
        token->active = false;
      }
      auto *tail = retired_devices.get();
      while (tail->next != nullptr) {
        tail = tail->next.get();
      }
      tail->next = std::move(retired_physical_device_tokens_);
      retired_physical_device_tokens_ = std::move(retired_devices);
    }

    auto retired = std::move(implementation_->lifetime);
    if (retired != nullptr) {
      retired->active = false;
      retired->next = std::move(retired_lifetime_tokens_);
      retired_lifetime_tokens_ = std::move(retired);
    }
    // The old Impl is now safe to destroy: all borrowed views retain only its
    // inactive private tokens, never its native resources or owning wrapper.
    implementation_.reset();
  }

  implementation_ = std::move(other.implementation_);

  // Preserve tokens retired by the source before moving its live Impl.  The
  // intrusive unique_ptr chains avoid allocation in this noexcept operation.
  if (other.retired_lifetime_tokens_ != nullptr) {
    auto source_retired = std::move(other.retired_lifetime_tokens_);
    auto *tail = source_retired.get();
    while (tail->next != nullptr) {
      tail = tail->next.get();
    }
    tail->next = std::move(retired_lifetime_tokens_);
    retired_lifetime_tokens_ = std::move(source_retired);
  }
  if (other.retired_physical_device_tokens_ != nullptr) {
    auto source_retired = std::move(other.retired_physical_device_tokens_);
    auto *tail = source_retired.get();
    while (tail->next != nullptr) {
      tail = tail->next.get();
    }
    tail->next = std::move(retired_physical_device_tokens_);
    retired_physical_device_tokens_ = std::move(source_retired);
  }

  return *this;
}

Instance::~Instance() = default;

Instance::operator bool() const noexcept { return valid(); }

bool Instance::valid() const noexcept {
  return implementation_ != nullptr && implementation_->lifetime != nullptr &&
         implementation_->lifetime->active &&
         static_cast<VkInstance>(*implementation_->instance) != VK_NULL_HANDLE;
}

vk::Instance Instance::nativeHandle() const noexcept {
  if (implementation_ == nullptr) {
    return {};
  }
  return *implementation_->instance;
}

const InstancePlan *Instance::plan() const noexcept {
  return implementation_ == nullptr ? nullptr : &implementation_->plan;
}

const PhysicalDevice::QueryAuthority *PhysicalDevice::query_authority() noexcept {
  static const QueryAuthority authority;
  return &authority;
}

PhysicalDevice::PhysicalDevice(vk::PhysicalDevice native_handle,
                               const Instance::PhysicalDeviceToken *device_token,
                               const QueryAuthority *query_authority_value) noexcept
    : native_handle_(native_handle), device_token_(device_token) {
  if (query_authority_value != query_authority()) {
    native_handle_ = vk::PhysicalDevice{};
    device_token_ = nullptr;
  }
}

PhysicalDevice::operator bool() const noexcept { return valid(); }

bool PhysicalDevice::valid() const noexcept {
  return static_cast<VkPhysicalDevice>(native_handle_) != VK_NULL_HANDLE &&
         device_token_ != nullptr && device_token_->active &&
         device_token_->parent_identity != nullptr &&
         static_cast<VkPhysicalDevice>(device_token_->native_handle) ==
             static_cast<VkPhysicalDevice>(native_handle_);
}

vk::PhysicalDevice PhysicalDevice::nativeHandle() const noexcept {
  return valid() ? native_handle_ : vk::PhysicalDevice{};
}

bool PhysicalDevice::correlatedWith(const Instance &instance) const noexcept {
  return valid() && instance.valid() &&
         device_token_->parent_identity == instance.implementation_.get();
}

bool PhysicalDevice::capabilitiesMatch(
    const PhysicalDeviceCapabilities &capabilities) const noexcept {
  if (!valid() || device_token_ == nullptr) {
    return false;
  }
  const auto *token_identity = static_cast<const void *>(device_token_);
  return capabilities.query_identity == token_identity &&
         device_token_->capabilities.query_identity == token_identity &&
         same_physical_device_capabilities(device_token_->capabilities, capabilities);
}

Result<Instance> createInstance(const InstancePlan &plan, terreate::DiagnosticSinkView sink) {
  // Native apply is deliberately a consumer of the successful resolver
  // output.  It performs only structural/ABI checks; semantic policy and
  // capability resolution never re-enter this boundary.
  const auto &requested = plan.requested();
  const auto &enabled_extensions = plan.enabledExtensions();
  const auto &enabled_layers = plan.enabledLayers();
  const auto abi_counts = checked_instance_abi_counts(enabled_extensions, enabled_layers);
  if (!abi_counts) {
    return invalid_plan_failure(
        "enabled Vulkan extension or layer count exceeds the uint32_t ABI limit");
  }

  std::vector<const char *> extension_names;
  extension_names.reserve(enabled_extensions.size());
  for (const auto &name : enabled_extensions) {
    extension_names.push_back(name.c_str());
  }
  std::vector<const char *> layer_names;
  layer_names.reserve(enabled_layers.size());
  for (const auto &name : enabled_layers) {
    layer_names.push_back(name.c_str());
  }

  const char *native_operation = "create Vulkan instance";
  auto context_result = detail::create_instance_context(&make_instance_context);
  if (!context_result) {
    return std::unexpected(context_result.error());
  }

  try {
    auto context = std::move(*context_result);
    auto callback = std::make_unique<CallbackState>();
    callback->sink = sink;

    vk::ApplicationInfo application_info{};
    application_info.pApplicationName =
        requested.application_name.empty() ? nullptr : requested.application_name.c_str();
    application_info.applicationVersion = requested.application_version;
    application_info.pEngineName =
        requested.engine_name.empty() ? nullptr : requested.engine_name.c_str();
    application_info.engineVersion = requested.engine_version;
    application_info.apiVersion = plan.effectiveApiVersion();

    vk::InstanceCreateInfo create_info{};
    create_info.pApplicationInfo = &application_info;
    create_info.enabledExtensionCount = abi_counts->extension_count;
    create_info.ppEnabledExtensionNames = extension_names.data();
    create_info.enabledLayerCount = abi_counts->layer_count;
    create_info.ppEnabledLayerNames = layer_names.data();

    vk::DebugUtilsMessengerCreateInfoEXT debug_create_info{};
    if (plan.debugUtilsEnabled()) {
      debug_create_info.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                                          vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                                          vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                                          vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
      debug_create_info.messageType = debug_message_types();
      // This thunk has the exact Vulkan-Hpp callback signature, including its
      // VKAPI calling convention and non-throwing boundary, so no function
      // pointer reinterpretation is needed.
      debug_create_info.pfnUserCallback = &instance_debug_callback;
      debug_create_info.pUserData = callback.get();
      create_info.pNext = &debug_create_info;
    }

    vk::raii::Instance native_instance{*context, create_info};
    std::optional<vk::raii::DebugUtilsMessengerEXT> messenger;
    if (plan.debugUtilsEnabled()) {
      native_operation = "create Vulkan Debug Utils messenger";
      messenger.emplace(native_instance, debug_create_info);
    }

    auto implementation =
        std::make_unique<Instance::Impl>(std::move(context), std::move(callback),
                                         std::move(native_instance), std::move(messenger), plan);
    return Instance{std::move(implementation)};
  } catch (const vk::SystemError &error) {
    return std::unexpected(terreate::Error{error.code(), native_operation, error.what()});
  }
}

// Keep production selection in this indispensable Instance object together
// with the PhysicalDevice authority and validators below.  A static consumer
// that asks only for selection must therefore extract the same authoritative
// object instead of satisfying these private member references itself.
terreate::Result<PhysicalDeviceSelection>
selectPhysicalDevice(const PhysicalDeviceCandidate &candidate,
                     const PhysicalDeviceRequirements &requirements) {
  if (!candidate.device.valid()) {
    return selection_fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::invalid_view, "select Vulkan physical device",
        "the manually supplied physical-device candidate has no valid native view "
        "and private device token");
  }
  if (!candidate.device.capabilitiesMatch(candidate.capabilities)) {
    return selection_fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::invalid_view, "select Vulkan physical device",
        "the manually supplied physical-device candidate does not contain the exact "
        "capability snapshot bound to its private device token");
  }
  const auto evaluation = evaluatePhysicalDevice(candidate, requirements);
  if (!evaluation) {
    return std::unexpected(evaluation.error());
  }
  if (!evaluation->matches) {
    return selection_fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::no_match, "select Vulkan physical device",
        "the manually supplied physical-device candidate does not satisfy the requirements");
  }
  return PhysicalDeviceSelection{
      .candidate = candidate,
      .reason = PhysicalDeviceSelectionReason::explicit_candidate,
      .policy_index = std::nullopt,
  };
}

terreate::Result<PhysicalDeviceSelection>
selectPhysicalDevice(std::span<const PhysicalDeviceCandidate> candidates,
                     const PhysicalDeviceRequirements &requirements,
                     const PhysicalDeviceSelectionPolicy &policy) {
  if (const auto error = validate_selection_requirements(requirements)) {
    return selection_fail<PhysicalDeviceSelection>(PhysicalDeviceError::invalid_requirements,
                                                   "select Vulkan physical device", *error);
  }
  if (policy.explicit_uuid && !policy.uuid_order.empty()) {
    return selection_fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::invalid_requirements, "select Vulkan physical device",
        "explicit UUID selection cannot be combined with UUID ordering");
  }
  if (policy.explicit_uuid && requirements.required_device_uuid &&
      !selection_same_uuid(*policy.explicit_uuid, *requirements.required_device_uuid)) {
    return selection_fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::invalid_requirements, "select Vulkan physical device",
        "explicit UUID selection conflicts with required_device_uuid");
  }
  for (std::size_t left = 0; left < policy.uuid_order.size(); ++left) {
    if (std::find(policy.uuid_order.begin() + static_cast<std::ptrdiff_t>(left + 1),
                  policy.uuid_order.end(), policy.uuid_order[left]) != policy.uuid_order.end()) {
      return selection_fail<PhysicalDeviceSelection>(PhysicalDeviceError::invalid_requirements,
                                                     "select Vulkan physical device",
                                                     "uuid_order must not contain duplicate UUIDs");
    }
  }

  std::vector<const PhysicalDeviceCandidate *> matches;
  matches.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    if (!candidate.device.valid()) {
      return selection_fail<PhysicalDeviceSelection>(
          PhysicalDeviceError::invalid_view, "select Vulkan physical device",
          "an enumerated physical-device candidate has no valid native view and private device "
          "token");
    }
    if (!candidate.device.capabilitiesMatch(candidate.capabilities)) {
      return selection_fail<PhysicalDeviceSelection>(
          PhysicalDeviceError::invalid_view, "select Vulkan physical device",
          "an enumerated physical-device candidate does not contain the exact capability "
          "snapshot bound to its private device token");
    }
    const auto evaluation = evaluatePhysicalDevice(candidate, requirements);
    if (!evaluation) {
      return std::unexpected(evaluation.error());
    }
    if (evaluation->matches) {
      matches.push_back(&candidate);
    }
  }

  if (matches.empty()) {
    return selection_fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::no_match, "select Vulkan physical device",
        "no enumerated physical-device candidate satisfies the requirements");
  }

  if (policy.explicit_uuid) {
    std::vector<const PhysicalDeviceCandidate *> uuid_matches;
    for (const auto *candidate : matches) {
      if (selection_matches_available_uuid(candidate->capabilities, *policy.explicit_uuid)) {
        uuid_matches.push_back(candidate);
      }
    }
    if (uuid_matches.empty()) {
      return selection_fail<PhysicalDeviceSelection>(
          PhysicalDeviceError::no_match, "select Vulkan physical device",
          "the explicitly selected device UUID is not a matching candidate");
    }
    if (uuid_matches.size() != 1) {
      return selection_fail<PhysicalDeviceSelection>(
          PhysicalDeviceError::ambiguous_match, "select Vulkan physical device",
          "the explicitly selected UUID identifies multiple matching candidates");
    }
    return PhysicalDeviceSelection{
        .candidate = *uuid_matches.front(),
        .reason = PhysicalDeviceSelectionReason::explicit_uuid,
        .policy_index = std::nullopt,
    };
  }

  if (!policy.uuid_order.empty()) {
    for (std::size_t policy_index = 0; policy_index < policy.uuid_order.size(); ++policy_index) {
      const auto &uuid = policy.uuid_order[policy_index];
      std::vector<const PhysicalDeviceCandidate *> uuid_matches;
      for (const auto *candidate : matches) {
        if (selection_matches_available_uuid(candidate->capabilities, uuid)) {
          uuid_matches.push_back(candidate);
        }
      }
      if (uuid_matches.size() > 1) {
        return selection_fail<PhysicalDeviceSelection>(
            PhysicalDeviceError::ambiguous_match, "select Vulkan physical device",
            "the explicit UUID ordering contains a UUID shared by multiple candidates");
      }
      if (uuid_matches.size() == 1) {
        return PhysicalDeviceSelection{
            .candidate = *uuid_matches.front(),
            .reason = PhysicalDeviceSelectionReason::uuid_order,
            .policy_index = policy_index,
        };
      }
    }
    return selection_fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::no_match, "select Vulkan physical device",
        "the explicit UUID ordering contains no matching candidate");
  }

  if (matches.size() != 1) {
    return selection_fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::ambiguous_match, "select Vulkan physical device",
        "multiple candidates matched and no explicit UUID selection policy was supplied");
  }
  return PhysicalDeviceSelection{
      .candidate = *matches.front(),
      .reason = PhysicalDeviceSelectionReason::sole_match,
      .policy_index = std::nullopt,
  };
}

} // namespace terreate::graphics

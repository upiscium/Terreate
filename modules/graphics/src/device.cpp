#include <terreate/graphics/device.hpp>

// This translation unit is kept in the same explicit Vulkan field/chain order
// as the public plan; formatting it as generated data obscures that ordering.
// clang-format off

#include "instance_impl.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace terreate::graphics {

struct DevicePlanBuilder {
  static DevicePlan build(
      DeviceDescription requested, PhysicalDevice physical_device,
       PhysicalDeviceCapabilities capabilities, std::size_t enumeration_index,
       std::uint32_t effective_api_version, std::vector<std::string> enabled_extensions,
       DeviceApiVersionProvenance api_version_provenance,
       vk::PhysicalDeviceFeatures enabled_features_10,
      vk::PhysicalDeviceVulkan11Features enabled_features_11,
      vk::PhysicalDeviceVulkan12Features enabled_features_12,
      vk::PhysicalDeviceVulkan13Features enabled_features_13,
      std::vector<DeviceFeatureDecision> feature_decisions,
      std::vector<DeviceExtensionDecision> extension_decisions,
      std::vector<DeviceQueueDecision> queue_decisions,
      std::vector<DeviceQueueAllocation> queue_allocations,
      std::vector<DeviceQueueAssignment> queue_assignments,
      std::vector<DeviceQueueAliasGroup> queue_alias_groups,
      std::vector<float> queue_priorities) {
    return DevicePlan{std::move(requested),
                      physical_device,
                       std::move(capabilities),
                       enumeration_index,
                       effective_api_version,
                       std::move(enabled_extensions),
                       api_version_provenance,
                       enabled_features_10,
                      enabled_features_11,
                      enabled_features_12,
                      enabled_features_13,
                      std::move(feature_decisions),
                      std::move(extension_decisions),
                      std::move(queue_decisions),
                      std::move(queue_allocations),
                      std::move(queue_assignments),
                      std::move(queue_alias_groups),
                      std::move(queue_priorities)};
  }
};

namespace {

class DeviceErrorCategory final : public std::error_category {
public:
  [[nodiscard]] const char *name() const noexcept override {
    return "terreate.graphics.device";
  }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<DeviceError>(value)) {
    case DeviceError::invalid_description:
      return "invalid logical-device description";
    case DeviceError::contradictory_requirements:
      return "contradictory logical-device requirements";
    case DeviceError::unsupported_api_version:
      return "requested logical-device API version is unavailable";
    case DeviceError::missing_required_extension:
      return "required Vulkan device extension is unavailable";
    case DeviceError::unsupported_required_feature:
      return "required Vulkan device feature is unavailable";
    case DeviceError::invalid_feature_chain:
      return "logical-device feature chain is invalid";
    case DeviceError::invalid_queue_request:
      return "invalid logical-device queue request";
    case DeviceError::insufficient_queue_count:
      return "logical-device queue request cannot be allocated";
    case DeviceError::ambiguous_queue:
      return "logical-device queue family selection is ambiguous";
    case DeviceError::invalid_instance:
      return "the Vulkan instance is not a valid logical-device parent";
    case DeviceError::invalid_physical_device:
      return "the logical-device plan has no valid physical-device view";
    case DeviceError::parent_mismatch:
      return "the logical-device physical device belongs to another instance";
    case DeviceError::invalid_plan:
      return "the logical-device plan is structurally invalid";
    }
    return "unknown Terreate Graphics logical-device error";
  }
};

[[nodiscard]] const DeviceErrorCategory &device_category() noexcept {
  static const DeviceErrorCategory category;
  return category;
}

[[nodiscard]] terreate::Error device_error(DeviceError error, std::string context,
                                            std::string detail) {
  return terreate::Error{make_error_code(error), std::move(context), std::move(detail)};
}

template <typename T>
[[nodiscard]] terreate::Result<T> fail(DeviceError error, std::string context,
                                        std::string detail) {
  return std::unexpected(device_error(error, std::move(context), std::move(detail)));
}

template <typename Struct, typename Member> struct FeatureField {
  const char *name;
  Member Struct::*member;
};

using FeatureField10 =
    FeatureField<vk::PhysicalDeviceFeatures,
                 decltype(vk::PhysicalDeviceFeatures::robustBufferAccess)>;
using FeatureField11 =
    FeatureField<vk::PhysicalDeviceVulkan11Features,
                 decltype(vk::PhysicalDeviceVulkan11Features::storageBuffer16BitAccess)>;
using FeatureField12 =
    FeatureField<vk::PhysicalDeviceVulkan12Features,
                 decltype(vk::PhysicalDeviceVulkan12Features::samplerMirrorClampToEdge)>;
using FeatureField13 =
    FeatureField<vk::PhysicalDeviceVulkan13Features,
                 decltype(vk::PhysicalDeviceVulkan13Features::robustImageAccess)>;

#define TERREATE_FEATURE_FIELD(type, member) FeatureField##type{#member, &vk::PhysicalDevice##type##Features::member}

// Vulkan-Hpp exposes feature members with the same spelling as the Vulkan C
// structures.  Keeping this list in one canonical order makes decisions and
// synthetic replay stable without depending on native structure padding.
constexpr std::array feature_fields_10{
    FeatureField10{"robustBufferAccess", &vk::PhysicalDeviceFeatures::robustBufferAccess},
    FeatureField10{"fullDrawIndexUint32", &vk::PhysicalDeviceFeatures::fullDrawIndexUint32},
    FeatureField10{"imageCubeArray", &vk::PhysicalDeviceFeatures::imageCubeArray},
    FeatureField10{"independentBlend", &vk::PhysicalDeviceFeatures::independentBlend},
    FeatureField10{"geometryShader", &vk::PhysicalDeviceFeatures::geometryShader},
    FeatureField10{"tessellationShader", &vk::PhysicalDeviceFeatures::tessellationShader},
    FeatureField10{"sampleRateShading", &vk::PhysicalDeviceFeatures::sampleRateShading},
    FeatureField10{"dualSrcBlend", &vk::PhysicalDeviceFeatures::dualSrcBlend},
    FeatureField10{"logicOp", &vk::PhysicalDeviceFeatures::logicOp},
    FeatureField10{"multiDrawIndirect", &vk::PhysicalDeviceFeatures::multiDrawIndirect},
    FeatureField10{"drawIndirectFirstInstance", &vk::PhysicalDeviceFeatures::drawIndirectFirstInstance},
    FeatureField10{"depthClamp", &vk::PhysicalDeviceFeatures::depthClamp},
    FeatureField10{"depthBiasClamp", &vk::PhysicalDeviceFeatures::depthBiasClamp},
    FeatureField10{"fillModeNonSolid", &vk::PhysicalDeviceFeatures::fillModeNonSolid},
    FeatureField10{"depthBounds", &vk::PhysicalDeviceFeatures::depthBounds},
    FeatureField10{"wideLines", &vk::PhysicalDeviceFeatures::wideLines},
    FeatureField10{"largePoints", &vk::PhysicalDeviceFeatures::largePoints},
    FeatureField10{"alphaToOne", &vk::PhysicalDeviceFeatures::alphaToOne},
    FeatureField10{"multiViewport", &vk::PhysicalDeviceFeatures::multiViewport},
    FeatureField10{"samplerAnisotropy", &vk::PhysicalDeviceFeatures::samplerAnisotropy},
    FeatureField10{"textureCompressionETC2", &vk::PhysicalDeviceFeatures::textureCompressionETC2},
    FeatureField10{"textureCompressionASTC_LDR", &vk::PhysicalDeviceFeatures::textureCompressionASTC_LDR},
    FeatureField10{"textureCompressionBC", &vk::PhysicalDeviceFeatures::textureCompressionBC},
    FeatureField10{"occlusionQueryPrecise", &vk::PhysicalDeviceFeatures::occlusionQueryPrecise},
    FeatureField10{"pipelineStatisticsQuery", &vk::PhysicalDeviceFeatures::pipelineStatisticsQuery},
    FeatureField10{"vertexPipelineStoresAndAtomics", &vk::PhysicalDeviceFeatures::vertexPipelineStoresAndAtomics},
    FeatureField10{"fragmentStoresAndAtomics", &vk::PhysicalDeviceFeatures::fragmentStoresAndAtomics},
    FeatureField10{"shaderTessellationAndGeometryPointSize", &vk::PhysicalDeviceFeatures::shaderTessellationAndGeometryPointSize},
    FeatureField10{"shaderImageGatherExtended", &vk::PhysicalDeviceFeatures::shaderImageGatherExtended},
    FeatureField10{"shaderStorageImageExtendedFormats", &vk::PhysicalDeviceFeatures::shaderStorageImageExtendedFormats},
    FeatureField10{"shaderStorageImageMultisample", &vk::PhysicalDeviceFeatures::shaderStorageImageMultisample},
    FeatureField10{"shaderStorageImageReadWithoutFormat", &vk::PhysicalDeviceFeatures::shaderStorageImageReadWithoutFormat},
    FeatureField10{"shaderStorageImageWriteWithoutFormat", &vk::PhysicalDeviceFeatures::shaderStorageImageWriteWithoutFormat},
    FeatureField10{"shaderUniformBufferArrayDynamicIndexing", &vk::PhysicalDeviceFeatures::shaderUniformBufferArrayDynamicIndexing},
    FeatureField10{"shaderSampledImageArrayDynamicIndexing", &vk::PhysicalDeviceFeatures::shaderSampledImageArrayDynamicIndexing},
    FeatureField10{"shaderStorageBufferArrayDynamicIndexing", &vk::PhysicalDeviceFeatures::shaderStorageBufferArrayDynamicIndexing},
    FeatureField10{"shaderStorageImageArrayDynamicIndexing", &vk::PhysicalDeviceFeatures::shaderStorageImageArrayDynamicIndexing},
    FeatureField10{"shaderClipDistance", &vk::PhysicalDeviceFeatures::shaderClipDistance},
    FeatureField10{"shaderCullDistance", &vk::PhysicalDeviceFeatures::shaderCullDistance},
    FeatureField10{"shaderFloat64", &vk::PhysicalDeviceFeatures::shaderFloat64},
    FeatureField10{"shaderInt64", &vk::PhysicalDeviceFeatures::shaderInt64},
    FeatureField10{"shaderInt16", &vk::PhysicalDeviceFeatures::shaderInt16},
    FeatureField10{"shaderResourceResidency", &vk::PhysicalDeviceFeatures::shaderResourceResidency},
    FeatureField10{"shaderResourceMinLod", &vk::PhysicalDeviceFeatures::shaderResourceMinLod},
    FeatureField10{"sparseBinding", &vk::PhysicalDeviceFeatures::sparseBinding},
    FeatureField10{"sparseResidencyBuffer", &vk::PhysicalDeviceFeatures::sparseResidencyBuffer},
    FeatureField10{"sparseResidencyImage2D", &vk::PhysicalDeviceFeatures::sparseResidencyImage2D},
    FeatureField10{"sparseResidencyImage3D", &vk::PhysicalDeviceFeatures::sparseResidencyImage3D},
    FeatureField10{"sparseResidency2Samples", &vk::PhysicalDeviceFeatures::sparseResidency2Samples},
    FeatureField10{"sparseResidency4Samples", &vk::PhysicalDeviceFeatures::sparseResidency4Samples},
    FeatureField10{"sparseResidency8Samples", &vk::PhysicalDeviceFeatures::sparseResidency8Samples},
    FeatureField10{"sparseResidency16Samples", &vk::PhysicalDeviceFeatures::sparseResidency16Samples},
    FeatureField10{"sparseResidencyAliased", &vk::PhysicalDeviceFeatures::sparseResidencyAliased},
    FeatureField10{"variableMultisampleRate", &vk::PhysicalDeviceFeatures::variableMultisampleRate},
    FeatureField10{"inheritedQueries", &vk::PhysicalDeviceFeatures::inheritedQueries},
};

constexpr std::array feature_fields_11{
    FeatureField11{"storageBuffer16BitAccess", &vk::PhysicalDeviceVulkan11Features::storageBuffer16BitAccess},
    FeatureField11{"uniformAndStorageBuffer16BitAccess", &vk::PhysicalDeviceVulkan11Features::uniformAndStorageBuffer16BitAccess},
    FeatureField11{"storagePushConstant16", &vk::PhysicalDeviceVulkan11Features::storagePushConstant16},
    FeatureField11{"storageInputOutput16", &vk::PhysicalDeviceVulkan11Features::storageInputOutput16},
    FeatureField11{"multiview", &vk::PhysicalDeviceVulkan11Features::multiview},
    FeatureField11{"multiviewGeometryShader", &vk::PhysicalDeviceVulkan11Features::multiviewGeometryShader},
    FeatureField11{"multiviewTessellationShader", &vk::PhysicalDeviceVulkan11Features::multiviewTessellationShader},
    FeatureField11{"variablePointersStorageBuffer", &vk::PhysicalDeviceVulkan11Features::variablePointersStorageBuffer},
    FeatureField11{"variablePointers", &vk::PhysicalDeviceVulkan11Features::variablePointers},
    FeatureField11{"protectedMemory", &vk::PhysicalDeviceVulkan11Features::protectedMemory},
    FeatureField11{"samplerYcbcrConversion", &vk::PhysicalDeviceVulkan11Features::samplerYcbcrConversion},
    FeatureField11{"shaderDrawParameters", &vk::PhysicalDeviceVulkan11Features::shaderDrawParameters},
};

constexpr std::array feature_fields_12{
    FeatureField12{"samplerMirrorClampToEdge", &vk::PhysicalDeviceVulkan12Features::samplerMirrorClampToEdge},
    FeatureField12{"drawIndirectCount", &vk::PhysicalDeviceVulkan12Features::drawIndirectCount},
    FeatureField12{"storageBuffer8BitAccess", &vk::PhysicalDeviceVulkan12Features::storageBuffer8BitAccess},
    FeatureField12{"uniformAndStorageBuffer8BitAccess", &vk::PhysicalDeviceVulkan12Features::uniformAndStorageBuffer8BitAccess},
    FeatureField12{"storagePushConstant8", &vk::PhysicalDeviceVulkan12Features::storagePushConstant8},
    FeatureField12{"shaderBufferInt64Atomics", &vk::PhysicalDeviceVulkan12Features::shaderBufferInt64Atomics},
    FeatureField12{"shaderSharedInt64Atomics", &vk::PhysicalDeviceVulkan12Features::shaderSharedInt64Atomics},
    FeatureField12{"shaderFloat16", &vk::PhysicalDeviceVulkan12Features::shaderFloat16},
    FeatureField12{"shaderInt8", &vk::PhysicalDeviceVulkan12Features::shaderInt8},
    FeatureField12{"descriptorIndexing", &vk::PhysicalDeviceVulkan12Features::descriptorIndexing},
    FeatureField12{"shaderInputAttachmentArrayDynamicIndexing", &vk::PhysicalDeviceVulkan12Features::shaderInputAttachmentArrayDynamicIndexing},
    FeatureField12{"shaderUniformTexelBufferArrayDynamicIndexing", &vk::PhysicalDeviceVulkan12Features::shaderUniformTexelBufferArrayDynamicIndexing},
    FeatureField12{"shaderStorageTexelBufferArrayDynamicIndexing", &vk::PhysicalDeviceVulkan12Features::shaderStorageTexelBufferArrayDynamicIndexing},
    FeatureField12{"shaderUniformBufferArrayNonUniformIndexing", &vk::PhysicalDeviceVulkan12Features::shaderUniformBufferArrayNonUniformIndexing},
    FeatureField12{"shaderSampledImageArrayNonUniformIndexing", &vk::PhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing},
    FeatureField12{"shaderStorageBufferArrayNonUniformIndexing", &vk::PhysicalDeviceVulkan12Features::shaderStorageBufferArrayNonUniformIndexing},
    FeatureField12{"shaderStorageImageArrayNonUniformIndexing", &vk::PhysicalDeviceVulkan12Features::shaderStorageImageArrayNonUniformIndexing},
    FeatureField12{"shaderInputAttachmentArrayNonUniformIndexing", &vk::PhysicalDeviceVulkan12Features::shaderInputAttachmentArrayNonUniformIndexing},
    FeatureField12{"shaderUniformTexelBufferArrayNonUniformIndexing", &vk::PhysicalDeviceVulkan12Features::shaderUniformTexelBufferArrayNonUniformIndexing},
    FeatureField12{"shaderStorageTexelBufferArrayNonUniformIndexing", &vk::PhysicalDeviceVulkan12Features::shaderStorageTexelBufferArrayNonUniformIndexing},
    FeatureField12{"descriptorBindingUniformBufferUpdateAfterBind", &vk::PhysicalDeviceVulkan12Features::descriptorBindingUniformBufferUpdateAfterBind},
    FeatureField12{"descriptorBindingSampledImageUpdateAfterBind", &vk::PhysicalDeviceVulkan12Features::descriptorBindingSampledImageUpdateAfterBind},
    FeatureField12{"descriptorBindingStorageImageUpdateAfterBind", &vk::PhysicalDeviceVulkan12Features::descriptorBindingStorageImageUpdateAfterBind},
    FeatureField12{"descriptorBindingStorageBufferUpdateAfterBind", &vk::PhysicalDeviceVulkan12Features::descriptorBindingStorageBufferUpdateAfterBind},
    FeatureField12{"descriptorBindingUniformTexelBufferUpdateAfterBind", &vk::PhysicalDeviceVulkan12Features::descriptorBindingUniformTexelBufferUpdateAfterBind},
    FeatureField12{"descriptorBindingStorageTexelBufferUpdateAfterBind", &vk::PhysicalDeviceVulkan12Features::descriptorBindingStorageTexelBufferUpdateAfterBind},
    FeatureField12{"descriptorBindingUpdateUnusedWhilePending", &vk::PhysicalDeviceVulkan12Features::descriptorBindingUpdateUnusedWhilePending},
    FeatureField12{"descriptorBindingPartiallyBound", &vk::PhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound},
    FeatureField12{"descriptorBindingVariableDescriptorCount", &vk::PhysicalDeviceVulkan12Features::descriptorBindingVariableDescriptorCount},
    FeatureField12{"runtimeDescriptorArray", &vk::PhysicalDeviceVulkan12Features::runtimeDescriptorArray},
    FeatureField12{"samplerFilterMinmax", &vk::PhysicalDeviceVulkan12Features::samplerFilterMinmax},
    FeatureField12{"scalarBlockLayout", &vk::PhysicalDeviceVulkan12Features::scalarBlockLayout},
    FeatureField12{"imagelessFramebuffer", &vk::PhysicalDeviceVulkan12Features::imagelessFramebuffer},
    FeatureField12{"uniformBufferStandardLayout", &vk::PhysicalDeviceVulkan12Features::uniformBufferStandardLayout},
    FeatureField12{"shaderSubgroupExtendedTypes", &vk::PhysicalDeviceVulkan12Features::shaderSubgroupExtendedTypes},
    FeatureField12{"separateDepthStencilLayouts", &vk::PhysicalDeviceVulkan12Features::separateDepthStencilLayouts},
    FeatureField12{"hostQueryReset", &vk::PhysicalDeviceVulkan12Features::hostQueryReset},
    FeatureField12{"timelineSemaphore", &vk::PhysicalDeviceVulkan12Features::timelineSemaphore},
    FeatureField12{"bufferDeviceAddress", &vk::PhysicalDeviceVulkan12Features::bufferDeviceAddress},
    FeatureField12{"bufferDeviceAddressCaptureReplay", &vk::PhysicalDeviceVulkan12Features::bufferDeviceAddressCaptureReplay},
    FeatureField12{"bufferDeviceAddressMultiDevice", &vk::PhysicalDeviceVulkan12Features::bufferDeviceAddressMultiDevice},
    FeatureField12{"vulkanMemoryModel", &vk::PhysicalDeviceVulkan12Features::vulkanMemoryModel},
    FeatureField12{"vulkanMemoryModelDeviceScope", &vk::PhysicalDeviceVulkan12Features::vulkanMemoryModelDeviceScope},
    FeatureField12{"vulkanMemoryModelAvailabilityVisibilityChains", &vk::PhysicalDeviceVulkan12Features::vulkanMemoryModelAvailabilityVisibilityChains},
    FeatureField12{"shaderOutputViewportIndex", &vk::PhysicalDeviceVulkan12Features::shaderOutputViewportIndex},
    FeatureField12{"shaderOutputLayer", &vk::PhysicalDeviceVulkan12Features::shaderOutputLayer},
    FeatureField12{"subgroupBroadcastDynamicId", &vk::PhysicalDeviceVulkan12Features::subgroupBroadcastDynamicId},
};

constexpr std::array feature_fields_13{
    FeatureField13{"robustImageAccess", &vk::PhysicalDeviceVulkan13Features::robustImageAccess},
    FeatureField13{"inlineUniformBlock", &vk::PhysicalDeviceVulkan13Features::inlineUniformBlock},
    FeatureField13{"descriptorBindingInlineUniformBlockUpdateAfterBind", &vk::PhysicalDeviceVulkan13Features::descriptorBindingInlineUniformBlockUpdateAfterBind},
    FeatureField13{"pipelineCreationCacheControl", &vk::PhysicalDeviceVulkan13Features::pipelineCreationCacheControl},
    FeatureField13{"privateData", &vk::PhysicalDeviceVulkan13Features::privateData},
    FeatureField13{"shaderDemoteToHelperInvocation", &vk::PhysicalDeviceVulkan13Features::shaderDemoteToHelperInvocation},
    FeatureField13{"shaderTerminateInvocation", &vk::PhysicalDeviceVulkan13Features::shaderTerminateInvocation},
    FeatureField13{"subgroupSizeControl", &vk::PhysicalDeviceVulkan13Features::subgroupSizeControl},
    FeatureField13{"computeFullSubgroups", &vk::PhysicalDeviceVulkan13Features::computeFullSubgroups},
    FeatureField13{"synchronization2", &vk::PhysicalDeviceVulkan13Features::synchronization2},
    FeatureField13{"textureCompressionASTC_HDR", &vk::PhysicalDeviceVulkan13Features::textureCompressionASTC_HDR},
    FeatureField13{"shaderZeroInitializeWorkgroupMemory", &vk::PhysicalDeviceVulkan13Features::shaderZeroInitializeWorkgroupMemory},
    FeatureField13{"dynamicRendering", &vk::PhysicalDeviceVulkan13Features::dynamicRendering},
    FeatureField13{"shaderIntegerDotProduct", &vk::PhysicalDeviceVulkan13Features::shaderIntegerDotProduct},
    FeatureField13{"maintenance4", &vk::PhysicalDeviceVulkan13Features::maintenance4},
};

#undef TERREATE_FEATURE_FIELD

template <typename Struct, typename Fields>
[[nodiscard]] bool has_requested_feature(const Struct &features, const Fields &fields) noexcept {
  return std::any_of(fields.begin(), fields.end(), [&](const auto &field) {
    return (features.*(field.member)) != VK_FALSE;
  });
}

template <typename Struct, typename Fields>
[[nodiscard]] bool has_invalid_boolean(const Struct &features, const Fields &fields) noexcept {
  return std::any_of(fields.begin(), fields.end(), [&](const auto &field) {
    const auto value = static_cast<std::uint32_t>(features.*(field.member));
    return value != VK_FALSE && value != VK_TRUE;
  });
}

template <typename Struct, typename Fields>
void clear_feature_values(Struct &features, const Fields &fields) noexcept {
  for (const auto &field : fields) {
    features.*(field.member) = VK_FALSE;
  }
}

template <typename Struct, typename Fields>
void merge_feature_values(Struct &destination, const Struct &source, const Fields &fields) noexcept {
  for (const auto &field : fields) {
    if ((source.*(field.member)) != VK_FALSE) {
      destination.*(field.member) = VK_TRUE;
    }
  }
}

template <typename Struct, typename Fields>
[[nodiscard]] bool has_feature_value(const Struct &features, const char *name, const Fields &fields,
                                     VkBool32 &value) noexcept {
  for (const auto &field : fields) {
    if (std::string_view{field.name} == name) {
      value = features.*(field.member);
      return true;
    }
  }
  return false;
}

template <typename Struct, typename Fields>
[[nodiscard]] std::optional<std::string>
validate_feature_struct(const Struct &features, const Fields &fields, std::string_view version,
                        std::optional<vk::StructureType> expected_s_type = std::nullopt) {
  if constexpr (requires { features.sType; }) {
    if (expected_s_type && features.sType != *expected_s_type) {
      return "Vulkan " + std::string{version} + " feature structure has an unexpected sType";
    }
  } else if (expected_s_type) {
    return "Vulkan " + std::string{version} + " feature structure has no sType member";
  }
  if constexpr (requires { features.pNext; }) {
    if (features.pNext != nullptr) {
      return "Vulkan " + std::string{version} +
             " feature structure must not carry a borrowed pNext chain";
    }
  }
  if (has_invalid_boolean(features, fields)) {
    return "Vulkan " + std::string{version} +
           " feature members must be VK_FALSE or VK_TRUE";
  }
  return std::nullopt;
}

template <typename Struct, typename Fields>
void canonical_feature_header(Struct &features, vk::StructureType s_type,
                             const Fields &fields) noexcept {
  features.sType = s_type;
  features.pNext = nullptr;
  clear_feature_values(features, fields);
}

struct MergedFeatureMasks {
  vk::PhysicalDeviceFeatures required_10{};
  vk::PhysicalDeviceFeatures optional_10{};
  vk::PhysicalDeviceVulkan11Features required_11{};
  vk::PhysicalDeviceVulkan11Features optional_11{};
  vk::PhysicalDeviceVulkan12Features required_12{};
  vk::PhysicalDeviceVulkan12Features optional_12{};
  vk::PhysicalDeviceVulkan13Features required_13{};
  vk::PhysicalDeviceVulkan13Features optional_13{};
};

enum class FeatureRequestFailureKind : std::uint8_t {
  invalid_chain,
  contradictory,
};

struct FeatureRequestFailure {
  FeatureRequestFailureKind kind = FeatureRequestFailureKind::invalid_chain;
  std::string detail{};
};

[[nodiscard]] bool same_or_zero(const vk::PhysicalDeviceFeatures &left,
                                const vk::PhysicalDeviceFeatures &right) noexcept {
  for (const auto &field : feature_fields_10) {
    if ((left.*(field.member)) != VK_FALSE && (right.*(field.member)) != VK_FALSE) {
      return true;
    }
  }
  return false;
}

template <typename Struct, typename Fields>
[[nodiscard]] bool has_overlap(const Struct &left, const Struct &right,
                               const Fields &fields) noexcept {
  for (const auto &field : fields) {
    if ((left.*(field.member)) != VK_FALSE && (right.*(field.member)) != VK_FALSE) {
      return true;
    }
  }
  return false;
}

template <typename Struct, typename Fields>
void merge_many(Struct &destination, const std::vector<const Struct *> &sources,
                const Fields &fields) noexcept {
  for (const auto *source : sources) {
    merge_feature_values(destination, *source, fields);
  }
}

[[nodiscard]] std::optional<FeatureRequestFailure>
merge_feature_requests(const DeviceDescription &description, MergedFeatureMasks &masks) {
  const auto check10 = [&](const auto &features, std::string_view label) {
    return validate_feature_struct(features, feature_fields_10, label);
  };
  const auto check11 = [&](const auto &features, std::string_view label) {
    return validate_feature_struct(features, feature_fields_11, label,
                                  vk::StructureType::ePhysicalDeviceVulkan11Features);
  };
  const auto check12 = [&](const auto &features, std::string_view label) {
    return validate_feature_struct(features, feature_fields_12, label,
                                  vk::StructureType::ePhysicalDeviceVulkan12Features);
  };
  const auto check13 = [&](const auto &features, std::string_view label) {
    return validate_feature_struct(features, feature_fields_13, label,
                                  vk::StructureType::ePhysicalDeviceVulkan13Features);
  };

  if (const auto error = check10(description.required_features, "1.0")) {
    return FeatureRequestFailure{.kind = FeatureRequestFailureKind::invalid_chain,
                                 .detail = *error};
  }
  if (const auto error = check10(description.optional_features, "1.0")) {
    return FeatureRequestFailure{.kind = FeatureRequestFailureKind::invalid_chain,
                                 .detail = *error};
  }
  merge_feature_values(masks.required_10, description.required_features, feature_fields_10);
  merge_feature_values(masks.optional_10, description.optional_features, feature_fields_10);

  if (const auto error = check11(description.required_features_11, "1.1")) {
    return FeatureRequestFailure{.kind = FeatureRequestFailureKind::invalid_chain,
                                 .detail = *error};
  }
  if (const auto error = check11(description.optional_features_11, "1.1")) {
    return FeatureRequestFailure{.kind = FeatureRequestFailureKind::invalid_chain,
                                 .detail = *error};
  }
  merge_feature_values(masks.required_11, description.required_features_11, feature_fields_11);
  merge_feature_values(masks.optional_11, description.optional_features_11, feature_fields_11);

  if (const auto error = check12(description.required_features_12, "1.2")) {
    return FeatureRequestFailure{.kind = FeatureRequestFailureKind::invalid_chain,
                                 .detail = *error};
  }
  if (const auto error = check12(description.optional_features_12, "1.2")) {
    return FeatureRequestFailure{.kind = FeatureRequestFailureKind::invalid_chain,
                                 .detail = *error};
  }
  merge_feature_values(masks.required_12, description.required_features_12, feature_fields_12);
  merge_feature_values(masks.optional_12, description.optional_features_12, feature_fields_12);

  if (const auto error = check13(description.required_features_13, "1.3")) {
    return FeatureRequestFailure{.kind = FeatureRequestFailureKind::invalid_chain,
                                 .detail = *error};
  }
  if (const auto error = check13(description.optional_features_13, "1.3")) {
    return FeatureRequestFailure{.kind = FeatureRequestFailureKind::invalid_chain,
                                 .detail = *error};
  }
  merge_feature_values(masks.required_13, description.required_features_13, feature_fields_13);
  merge_feature_values(masks.optional_13, description.optional_features_13, feature_fields_13);

  if (has_overlap(masks.required_10, masks.optional_10, feature_fields_10) ||
      has_overlap(masks.required_11, masks.optional_11, feature_fields_11) ||
      has_overlap(masks.required_12, masks.optional_12, feature_fields_12) ||
      has_overlap(masks.required_13, masks.optional_13, feature_fields_13)) {
    return FeatureRequestFailure{
        .kind = FeatureRequestFailureKind::contradictory,
        .detail = "a Vulkan feature was declared both required and optional",
    };
  }
  return std::nullopt;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
template <typename Struct, typename Fields>
void append_feature_decisions(DeviceFeatureVersion version, std::string_view structure,
                              RequirementStrength strength, const Struct &requested,
                              const Struct &supported, bool available, const Fields &fields,
                              Struct &enabled, std::vector<DeviceFeatureDecision> &decisions,
                              std::optional<std::string> &required_failure) {
  for (const auto &field : fields) {
    if ((requested.*(field.member)) == VK_FALSE) {
      continue;
    }
    const bool supported_value = available && (supported.*(field.member)) == VK_TRUE;
    const auto outcome = supported_value ? DeviceDecisionOutcome::accepted
                                         : DeviceDecisionOutcome::declined;
    const auto reason = supported_value
                            ? (strength == RequirementStrength::required
                                   ? DeviceDecisionReason::requested
                                   : DeviceDecisionReason::supported)
                            : DeviceDecisionReason::unsupported;
    decisions.push_back(DeviceFeatureDecision{
        .version = version,
        .structure = std::string{structure},
        .member = field.name,
        .strength = strength,
        .requested = true,
        .supported = supported_value,
        .effective = supported_value,
        .outcome = outcome,
        .reason = reason,
    });
    if (supported_value) {
      enabled.*(field.member) = VK_TRUE;
    } else if (strength == RequirementStrength::required && !required_failure) {
      required_failure = std::string{"required Vulkan feature '"} + field.name + "' is unsupported";
    }
  }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] std::optional<std::string>
validate_capability_feature_snapshots(const PhysicalDeviceCapabilities &capabilities) {
  if (capabilities.features2.sType != vk::StructureType::ePhysicalDeviceFeatures2 ||
      capabilities.features_11.sType != vk::StructureType::ePhysicalDeviceVulkan11Features ||
      capabilities.features_12.sType != vk::StructureType::ePhysicalDeviceVulkan12Features ||
      capabilities.features_13.sType != vk::StructureType::ePhysicalDeviceVulkan13Features) {
    return "physical-device feature snapshots have an unexpected sType";
  }
  if (capabilities.features2.pNext != nullptr || capabilities.features_11.pNext != nullptr ||
      capabilities.features_12.pNext != nullptr || capabilities.features_13.pNext != nullptr) {
    return "physical-device feature snapshots must have null pNext members";
  }
  if (has_invalid_boolean(capabilities.features_10, feature_fields_10) ||
      has_invalid_boolean(capabilities.features_11, feature_fields_11) ||
      has_invalid_boolean(capabilities.features_12, feature_fields_12) ||
      has_invalid_boolean(capabilities.features_13, feature_fields_13)) {
    return "physical-device feature snapshots contain a non-boolean feature value";
  }
  return std::nullopt;
}

[[nodiscard]] std::string extension_detail(std::string_view name) {
  return "device extension '" + std::string{name} + "' is not reported by the physical device";
}

struct NormalizedQueueRequest {
  std::string caller_id{};
  RequirementStrength strength = RequirementStrength::required;
  vk::QueueFlags required_flags{};
  std::vector<std::size_t> allowed_families{};
  std::vector<std::size_t> preferred_families{};
  float priority = 1.0F;
  DeviceQueueAliasPolicy alias_policy = DeviceQueueAliasPolicy::forbid_alias;
  std::string alias_target{};
};

[[nodiscard]] bool queue_family_matches(const vk::QueueFamilyProperties &family,
                                         vk::QueueFlags required) noexcept {
  return (static_cast<VkQueueFlags>(family.queueFlags) & static_cast<VkQueueFlags>(required)) ==
         static_cast<VkQueueFlags>(required);
}

[[nodiscard]] bool queue_contains(const std::vector<std::size_t> &values,
                                  std::size_t value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

[[nodiscard]] std::optional<std::string>
normalize_queue_request(const DeviceQueueRequest &input, NormalizedQueueRequest &output,
                        std::size_t index) {
  output.caller_id = input.caller_id;
  if (output.caller_id.empty() || output.caller_id.find('\0') != std::string::npos) {
    return "queue request " + std::to_string(index) +
           " needs a non-empty caller_id without embedded NUL bytes";
  }
  if (static_cast<VkQueueFlags>(input.required_flags) == 0) {
    return "queue request '" + output.caller_id + "' needs non-zero required_flags";
  }
  if (!std::isfinite(input.priority) || input.priority < 0.0F || input.priority > 1.0F) {
    return "queue request '" + output.caller_id +
           "' priority must be finite and in [0, 1]";
  }
  output.allowed_families = input.allowed_family_indices;
  output.preferred_families = input.preferred_family_indices;
  for (std::size_t list_index = 0; list_index < output.allowed_families.size(); ++list_index) {
    const auto previous_end = output.allowed_families.begin() +
                               static_cast<std::vector<std::size_t>::difference_type>(list_index);
    if (std::find(output.allowed_families.begin(), previous_end,
                  output.allowed_families[list_index]) != previous_end) {
      return "queue request '" + output.caller_id +
             "' allowed_family_indices contains a duplicate family index";
    }
  }
  for (std::size_t list_index = 0; list_index < output.preferred_families.size(); ++list_index) {
    const auto previous_end = output.preferred_families.begin() +
                               static_cast<std::vector<std::size_t>::difference_type>(list_index);
    if (std::find(output.preferred_families.begin(), previous_end,
                  output.preferred_families[list_index]) != previous_end) {
      return "queue request '" + output.caller_id +
             "' preferred_family_indices contains a duplicate family index";
    }
    if (!queue_contains(output.allowed_families, output.preferred_families[list_index])) {
      return "queue request '" + output.caller_id +
             "' preferred_family_indices must be contained in allowed_family_indices";
    }
  }
  switch (input.strength) {
  case RequirementStrength::required:
  case RequirementStrength::optional:
    break;
  default:
    return "queue request '" + output.caller_id + "' has an invalid requirement strength";
  }
  output.strength = input.strength;
  if (output.allowed_families.empty() && output.strength == RequirementStrength::required) {
    return "queue request '" + output.caller_id +
           "' must provide an explicit allowed_family_indices whitelist";
  }
  output.priority = input.priority;
  output.alias_policy = input.alias_policy;
  output.alias_target = input.alias_target;
  switch (output.alias_policy) {
  case DeviceQueueAliasPolicy::forbid_alias:
    if (!output.alias_target.empty()) {
      return "queue request '" + output.caller_id +
             "' cannot name an alias target when aliasing is forbidden";
    }
    break;
  case DeviceQueueAliasPolicy::require_alias:
  case DeviceQueueAliasPolicy::prefer_alias_with_distinct_fallback:
  case DeviceQueueAliasPolicy::prefer_distinct_with_alias_fallback:
    if (output.alias_target.empty() || output.alias_target.find('\0') != std::string::npos) {
      return "queue request '" + output.caller_id +
             "' needs a non-empty alias_target for its alias policy";
    }
    if (output.alias_target == output.caller_id) {
      return "queue request '" + output.caller_id + "' cannot alias itself";
    }
    break;
  default:
    return "queue request '" + output.caller_id + "' has an invalid alias policy";
  }
  output.required_flags = input.required_flags;
  return std::nullopt;
}

struct QueueSearchChoice {
  bool accepted = false;
  bool aliased = false;
  bool alias_priority_conflict = false;
  std::size_t family_index = 0;
  std::size_t queue_index = 0;
};

struct QueueSearchState {
  std::vector<QueueSearchChoice> choices{};
  std::vector<std::pair<std::size_t, std::size_t>> used_coordinates{};
};

struct QueueSearchSolution {
  std::vector<QueueSearchChoice> choices{};
};

struct QueueResolutionFailure {
  enum class Kind : std::uint8_t {
    invalid,
    contradictory,
    insufficient,
    ambiguous,
  } kind = Kind::invalid;
  std::string detail{};
};

[[nodiscard]] bool coordinate_used(const QueueSearchState &state, std::size_t family_index,
                                   std::size_t queue_index) {
  return std::find(state.used_coordinates.begin(), state.used_coordinates.end(),
                   std::pair{family_index, queue_index}) != state.used_coordinates.end();
}

[[nodiscard]] std::optional<std::size_t>
first_free_queue(const PhysicalDeviceCapabilities &capabilities, const QueueSearchState &state,
                 std::size_t family_index) {
  if (family_index >= capabilities.queue_families.size()) {
    return std::nullopt;
  }
  for (std::size_t queue_index = 0;
       queue_index < capabilities.queue_families[family_index].queueCount; ++queue_index) {
    if (!coordinate_used(state, family_index, queue_index)) {
      return queue_index;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::vector<std::size_t>
candidate_families(const PhysicalDeviceCapabilities &capabilities,
                   const NormalizedQueueRequest &request, const QueueSearchState &state) {
  std::vector<std::size_t> candidates;
  const auto append = [&](std::size_t family_index) {
    if (family_index >= capabilities.queue_families.size() ||
        !queue_contains(request.allowed_families, family_index) ||
        !queue_family_matches(capabilities.queue_families[family_index],
                              request.required_flags) ||
        !first_free_queue(capabilities, state, family_index) ||
        queue_contains(candidates, family_index)) {
      return;
    }
    candidates.push_back(family_index);
  };
  for (const auto family_index : request.preferred_families) {
    append(family_index);
  }
  std::vector<std::size_t> remaining = request.allowed_families;
  std::sort(remaining.begin(), remaining.end());
  for (const auto family_index : remaining) {
    append(family_index);
  }
  return candidates;
}

[[nodiscard]] std::optional<std::size_t>
find_queue_request(const std::vector<NormalizedQueueRequest> &requests, std::string_view caller_id) {
  for (std::size_t index = 0; index < requests.size(); ++index) {
    if (requests[index].caller_id == caller_id) {
      return index;
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool alias_target_compatible(const PhysicalDeviceCapabilities &capabilities,
                                           const NormalizedQueueRequest &request,
                                           const NormalizedQueueRequest &target,
                                           const QueueSearchChoice &target_choice) {
  return target_choice.accepted && target_choice.family_index < capabilities.queue_families.size() &&
         queue_contains(request.allowed_families, target_choice.family_index) &&
         queue_family_matches(capabilities.queue_families[target_choice.family_index],
                              request.required_flags) &&
         target.priority == request.priority;
}

[[nodiscard]] bool queue_choice_less(const QueueSearchChoice &left,
                                     const QueueSearchChoice &right) noexcept {
  if (left.accepted != right.accepted) {
    return left.accepted > right.accepted;
  }
  if (!left.accepted) {
    return false;
  }
  if (left.family_index != right.family_index) {
    return left.family_index < right.family_index;
  }
  if (left.queue_index != right.queue_index) {
    return left.queue_index < right.queue_index;
  }
  return left.aliased < right.aliased;
}

[[nodiscard]] bool solution_less(const QueueSearchSolution &left,
                                 const QueueSearchSolution &right) noexcept {
  for (std::size_t index = 0; index < left.choices.size(); ++index) {
    if (left.choices[index].accepted != right.choices[index].accepted) {
      return left.choices[index].accepted > right.choices[index].accepted;
    }
    if (left.choices[index].accepted &&
        queue_choice_less(left.choices[index], right.choices[index])) {
      return true;
    }
    if (left.choices[index].accepted &&
        queue_choice_less(right.choices[index], left.choices[index])) {
      return false;
    }
  }
  return false;
}

struct QueueScore {
  std::size_t optional_accepted = 0;
  std::size_t alias_preference = 0;
  std::size_t preferred_penalty = 0;
};

[[nodiscard]] QueueScore score_solution(const std::vector<NormalizedQueueRequest> &requests,
                                         const QueueSearchSolution &solution) {
  QueueScore score;
  for (std::size_t index = 0; index < requests.size(); ++index) {
    const auto &request = requests[index];
    const auto &choice = solution.choices[index];
    if (!choice.accepted) {
      continue;
    }
    if (request.strength == RequirementStrength::optional) {
      ++score.optional_accepted;
    }
    switch (request.alias_policy) {
    case DeviceQueueAliasPolicy::prefer_alias_with_distinct_fallback:
      score.alias_preference += choice.aliased ? 2U : 1U;
      break;
    case DeviceQueueAliasPolicy::prefer_distinct_with_alias_fallback:
      score.alias_preference += choice.aliased ? 1U : 2U;
      break;
    case DeviceQueueAliasPolicy::forbid_alias:
    case DeviceQueueAliasPolicy::require_alias:
      break;
    }
    if (!request.preferred_families.empty()) {
      const auto preferred = std::find(request.preferred_families.begin(),
                                       request.preferred_families.end(), choice.family_index);
      score.preferred_penalty +=
          preferred == request.preferred_families.end()
              ? request.preferred_families.size() + 1U
              : static_cast<std::size_t>(preferred - request.preferred_families.begin());
    }
  }
  return score;
}

[[nodiscard]] bool score_better(const QueueScore &left, const QueueScore &right) noexcept {
  if (left.optional_accepted != right.optional_accepted) {
    return left.optional_accepted > right.optional_accepted;
  }
  if (left.alias_preference != right.alias_preference) {
    return left.alias_preference > right.alias_preference;
  }
  return left.preferred_penalty < right.preferred_penalty;
}

[[nodiscard]] bool score_equal(const QueueScore &left, const QueueScore &right) noexcept {
  return left.optional_accepted == right.optional_accepted &&
         left.alias_preference == right.alias_preference &&
         left.preferred_penalty == right.preferred_penalty;
}

void search_queue_solutions(const PhysicalDeviceCapabilities &capabilities,
                            const std::vector<NormalizedQueueRequest> &requests,
                            const std::vector<std::size_t> &order, std::size_t order_index,
                            QueueSearchState &state,
                            std::vector<QueueSearchSolution> &solutions) {
  if (order_index == order.size()) {
    solutions.push_back(QueueSearchSolution{.choices = state.choices});
    return;
  }
  const auto request_index = order[order_index];
  const auto &request = requests[request_index];
  const auto target_index = request.alias_target.empty()
                                ? std::optional<std::size_t>{}
                                : find_queue_request(requests, request.alias_target);

  const auto visit = [&](QueueSearchChoice choice) {
    state.choices[request_index] = choice;
    if (choice.accepted && !choice.aliased) {
      state.used_coordinates.emplace_back(choice.family_index, choice.queue_index);
      search_queue_solutions(capabilities, requests, order, order_index + 1, state, solutions);
      state.used_coordinates.pop_back();
    } else {
      search_queue_solutions(capabilities, requests, order, order_index + 1, state, solutions);
    }
    state.choices[request_index] = QueueSearchChoice{};
  };

  const auto can_alias = [&]() -> bool {
    if (!target_index || !state.choices[*target_index].accepted) {
      return false;
    }
    return alias_target_compatible(capabilities, request, requests[*target_index],
                                   state.choices[*target_index]);
  };
  const auto alias_choice = [&]() {
    if (!target_index || !can_alias()) {
      return;
    }
    const auto &target_choice = state.choices[*target_index];
    visit(QueueSearchChoice{.accepted = true,
                            .aliased = true,
                            .family_index = target_choice.family_index,
                            .queue_index = target_choice.queue_index});
  };
  const auto distinct_choices = [&]() {
    for (const auto family_index : candidate_families(capabilities, request, state)) {
      const auto queue_index = first_free_queue(capabilities, state, family_index);
      if (!queue_index) {
        continue;
      }
      const bool priority_conflict =
          target_index && state.choices[*target_index].accepted &&
          state.choices[*target_index].family_index == family_index &&
          requests[*target_index].priority != request.priority;
      visit(QueueSearchChoice{.accepted = true,
                              .aliased = false,
                              .alias_priority_conflict = priority_conflict,
                              .family_index = family_index,
                              .queue_index = *queue_index});
    }
  };

  switch (request.alias_policy) {
  case DeviceQueueAliasPolicy::require_alias:
    alias_choice();
    break;
  case DeviceQueueAliasPolicy::prefer_alias_with_distinct_fallback:
    alias_choice();
    distinct_choices();
    break;
  case DeviceQueueAliasPolicy::prefer_distinct_with_alias_fallback:
    distinct_choices();
    alias_choice();
    break;
  case DeviceQueueAliasPolicy::forbid_alias:
    distinct_choices();
    break;
  }
  if (request.strength == RequirementStrength::optional) {
    visit(QueueSearchChoice{});
  }
}

[[nodiscard]] std::optional<std::string>
validate_queue_relations(const std::vector<NormalizedQueueRequest> &requests,
                         std::vector<std::size_t> &order) {
  std::map<std::string, std::size_t> indices;
  for (std::size_t index = 0; index < requests.size(); ++index) {
    const auto [iterator, inserted] = indices.emplace(requests[index].caller_id, index);
    if (!inserted) {
      return "queue caller_id '" + requests[index].caller_id + "' was requested more than once";
    }
    (void)iterator;
  }
  for (const auto &request : requests) {
    if (!request.alias_target.empty() && !indices.contains(request.alias_target)) {
      return "queue request '" + request.caller_id + "' names unknown alias_target '" +
             request.alias_target + "'";
    }
  }

  std::vector<std::uint8_t> marks(requests.size(), 0);
  const auto visit = [&](const auto &self, std::size_t index) -> std::optional<std::string> {
    if (marks[index] == 1) {
      return "queue alias relations contain a cycle involving '" + requests[index].caller_id +
             "'";
    }
    if (marks[index] == 2) {
      return std::nullopt;
    }
    marks[index] = 1;
    if (!requests[index].alias_target.empty()) {
      const auto target = indices.find(requests[index].alias_target);
      if (target != indices.end()) {
        if (const auto error = self(self, target->second)) {
          return error;
        }
      }
    }
    marks[index] = 2;
    order.push_back(index);
    return std::nullopt;
  };
  for (const auto &[caller_id, index] : indices) {
    (void)caller_id;
    if (const auto error = visit(visit, index)) {
      return error;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<QueueResolutionFailure>
resolve_queues(const PhysicalDeviceCapabilities &capabilities,
               const DeviceDescription &description, std::vector<DeviceQueueDecision> &decisions,
               std::vector<DeviceQueueAllocation> &allocations,
               std::vector<DeviceQueueAssignment> &assignments,
               std::vector<DeviceQueueAliasGroup> &alias_groups) {
  std::vector<NormalizedQueueRequest> requests;
  requests.reserve(description.queue_requests.size());
  for (std::size_t index = 0; index < description.queue_requests.size(); ++index) {
    NormalizedQueueRequest request;
    if (const auto error = normalize_queue_request(description.queue_requests[index], request, index)) {
      return QueueResolutionFailure{.kind = QueueResolutionFailure::Kind::invalid,
                                    .detail = *error};
    }
    requests.push_back(std::move(request));
  }
  std::sort(requests.begin(), requests.end(), [](const auto &left, const auto &right) {
    return left.caller_id < right.caller_id;
  });
  std::vector<std::size_t> order;
  if (const auto error = validate_queue_relations(requests, order)) {
    return QueueResolutionFailure{.kind = QueueResolutionFailure::Kind::contradictory,
                                  .detail = *error};
  }

  QueueSearchState state;
  state.choices.resize(requests.size());
  std::vector<QueueSearchSolution> solutions;
  search_queue_solutions(capabilities, requests, order, 0, state, solutions);
  if (solutions.empty()) {
    for (const auto &request : requests) {
      if (request.strength != RequirementStrength::required) {
        continue;
      }
      if (!request.alias_target.empty()) {
        const auto target_index = find_queue_request(requests, request.alias_target);
        if (target_index && requests[*target_index].priority != request.priority) {
          return QueueResolutionFailure{
              .kind = QueueResolutionFailure::Kind::insufficient,
              .detail = "required queue request '" + request.caller_id +
                        "' cannot alias '" + request.alias_target +
                        "' because native queue priorities conflict"};
        }
      }
      const bool supported = std::any_of(
          request.allowed_families.begin(), request.allowed_families.end(), [&](std::size_t family) {
            return family < capabilities.queue_families.size() &&
                   queue_family_matches(capabilities.queue_families[family], request.required_flags);
          });
      return QueueResolutionFailure{
          .kind = QueueResolutionFailure::Kind::insufficient,
          .detail = supported ? "required queue request '" + request.caller_id +
                                     "' cannot be allocated under the declared alias/capacity policy"
                              : "required queue request '" + request.caller_id +
                                     "' has no allowed family supporting required_flags"};
    }
    return QueueResolutionFailure{.kind = QueueResolutionFailure::Kind::insufficient,
                                  .detail = "required queue requests cannot be allocated"};
  }

  QueueScore best_score = score_solution(requests, solutions.front());
  for (const auto &solution : solutions) {
    const auto score = score_solution(requests, solution);
    if (score_better(score, best_score)) {
      best_score = score;
    }
  }
  std::vector<QueueSearchSolution> best_solutions;
  for (const auto &solution : solutions) {
    if (score_equal(score_solution(requests, solution), best_score)) {
      best_solutions.push_back(solution);
    }
  }
  std::sort(best_solutions.begin(), best_solutions.end(), solution_less);
  auto selected = best_solutions.front();

  std::vector<bool> optional_ambiguity(requests.size(), false);
  for (std::size_t index = 0; index < requests.size(); ++index) {
    for (const auto &solution : best_solutions) {
      const auto &left = selected.choices[index];
      const auto &right = solution.choices[index];
      if (left.accepted != right.accepted || left.aliased != right.aliased ||
          (left.accepted && (left.family_index != right.family_index ||
                             left.queue_index != right.queue_index))) {
        if (requests[index].strength == RequirementStrength::required) {
          return QueueResolutionFailure{
              .kind = QueueResolutionFailure::Kind::ambiguous,
              .detail = "required queue request '" + requests[index].caller_id +
                        "' has multiple equally valid global allocations"};
        }
        optional_ambiguity[index] = true;
        break;
      }
    }
  }
  bool dependent_changed = true;
  while (dependent_changed) {
    dependent_changed = false;
    for (std::size_t index = 0; index < requests.size(); ++index) {
      if (!selected.choices[index].accepted || !selected.choices[index].aliased ||
          requests[index].alias_target.empty()) {
        continue;
      }
      const auto target_index = find_queue_request(requests, requests[index].alias_target);
      if (!target_index || !optional_ambiguity[*target_index] || optional_ambiguity[index]) {
        continue;
      }
      if (requests[index].strength == RequirementStrength::required) {
        return QueueResolutionFailure{
            .kind = QueueResolutionFailure::Kind::ambiguous,
            .detail = "required queue request '" + requests[index].caller_id +
                      "' depends on an ambiguous optional alias target"};
      }
      optional_ambiguity[index] = true;
      dependent_changed = true;
    }
  }
  for (std::size_t index = 0; index < requests.size(); ++index) {
    if (optional_ambiguity[index]) {
      selected.choices[index] = QueueSearchChoice{};
    }
  }

  struct CoordinateAllocation {
    std::size_t family_index = 0;
    std::size_t queue_index = 0;
    float priority = 1.0F;
    std::vector<std::size_t> request_indices{};
  };
  std::vector<CoordinateAllocation> coordinate_allocations;
  for (std::size_t request_index = 0; request_index < requests.size(); ++request_index) {
    const auto &choice = selected.choices[request_index];
    if (!choice.accepted) {
      continue;
    }
    const auto coordinate = std::pair{choice.family_index, choice.queue_index};
    const auto allocation = std::find_if(
        coordinate_allocations.begin(), coordinate_allocations.end(), [&](const auto &candidate) {
          return std::pair{candidate.family_index, candidate.queue_index} == coordinate;
        });
    if (allocation == coordinate_allocations.end()) {
      coordinate_allocations.push_back(CoordinateAllocation{
          .family_index = choice.family_index,
          .queue_index = choice.queue_index,
          .priority = requests[request_index].priority,
          .request_indices = {request_index},
      });
    } else {
      allocation->request_indices.push_back(request_index);
    }
  }
  std::sort(coordinate_allocations.begin(), coordinate_allocations.end(), [](const auto &left,
                                                                              const auto &right) {
    return std::pair{left.family_index, left.queue_index} <
           std::pair{right.family_index, right.queue_index};
  });
  for (std::size_t allocation_index = 0; allocation_index < coordinate_allocations.size();
       ++allocation_index) {
    const auto &coordinate = coordinate_allocations[allocation_index];
    const auto alias_group = alias_groups.size();
    allocations.push_back(DeviceQueueAllocation{
        .family_index = coordinate.family_index,
        .queue_index = coordinate.queue_index,
        .priority = coordinate.priority,
        .alias_group = alias_group,
    });
    DeviceQueueAliasGroup group{
        .index = alias_group,
        .allocation_index = allocation_index,
        .family_index = coordinate.family_index,
        .queue_index = coordinate.queue_index,
    };
    for (const auto request_index : coordinate.request_indices) {
      group.caller_ids.push_back(requests[request_index].caller_id);
    }
    alias_groups.push_back(std::move(group));
  }
  const auto allocation_index_for = [&](const QueueSearchChoice &choice) {
    for (std::size_t index = 0; index < allocations.size(); ++index) {
      if (allocations[index].family_index == choice.family_index &&
          allocations[index].queue_index == choice.queue_index) {
        return index;
      }
    }
    return allocations.size();
  };

  for (std::size_t request_index = 0; request_index < requests.size(); ++request_index) {
    const auto &request = requests[request_index];
    const auto &choice = selected.choices[request_index];
    DeviceQueueDecision decision{
        .caller_id = request.caller_id,
        .strength = request.strength,
        .requested = true,
        .supported = false,
        .effective = choice.accepted,
        .outcome = choice.accepted ? DeviceDecisionOutcome::accepted
                                   : DeviceDecisionOutcome::declined,
        .reason = DeviceDecisionReason::unsupported,
        .provenance = request.strength == RequirementStrength::required
                          ? DeviceQueueProvenance::explicit_required
                          : DeviceQueueProvenance::explicit_optional,
        .alias_target = request.alias_target.empty()
                            ? std::optional<std::string>{}
                            : std::optional<std::string>{request.alias_target},
    };
    decision.supported = std::any_of(
        request.allowed_families.begin(), request.allowed_families.end(), [&](std::size_t family) {
          return family < capabilities.queue_families.size() &&
                 queue_family_matches(capabilities.queue_families[family], request.required_flags);
        });
    if (choice.accepted) {
      decision.family_index = choice.family_index;
      decision.queue_index = choice.queue_index;
      decision.alias_group = allocations[allocation_index_for(choice)].alias_group;
      if (choice.aliased) {
        decision.reason = DeviceDecisionReason::accepted_by_aliasing;
        decision.provenance = DeviceQueueProvenance::alias;
      } else if (choice.alias_priority_conflict) {
        decision.reason = DeviceDecisionReason::conflicting_priority;
      } else if (!request.preferred_families.empty() &&
                 request.preferred_families.front() == choice.family_index) {
        decision.reason = DeviceDecisionReason::preferred_family;
        decision.provenance = DeviceQueueProvenance::preferred_family;
      } else if (!request.preferred_families.empty()) {
        decision.reason = DeviceDecisionReason::fallback_family;
        decision.provenance = DeviceQueueProvenance::fallback_family;
      } else {
        decision.reason = DeviceDecisionReason::requested;
      }
      assignments.push_back(DeviceQueueAssignment{
          .caller_id = request.caller_id,
          .strength = request.strength,
          .provenance = decision.provenance,
          .allocation_index = allocation_index_for(choice),
          .family_index = choice.family_index,
          .queue_index = choice.queue_index,
          .alias_group = decision.alias_group,
          .alias_target = choice.aliased
                              ? std::optional<std::string>{request.alias_target}
                              : std::optional<std::string>{},
      });
    } else if (optional_ambiguity[request_index]) {
      decision.reason = DeviceDecisionReason::ambiguous;
    } else if (!request.alias_target.empty()) {
      const auto target_index = find_queue_request(requests, request.alias_target);
      const bool priority_conflict =
          target_index && selected.choices[*target_index].accepted &&
          selected.choices[*target_index].family_index < capabilities.queue_families.size() &&
          queue_contains(request.allowed_families,
                         selected.choices[*target_index].family_index) &&
          queue_family_matches(
              capabilities.queue_families[selected.choices[*target_index].family_index],
              request.required_flags) &&
          requests[*target_index].priority != request.priority;
      decision.reason = priority_conflict ? DeviceDecisionReason::conflicting_priority
                                          : DeviceDecisionReason::insufficient_queue_count;
    } else if (!decision.supported) {
      decision.reason = DeviceDecisionReason::unsupported;
    } else {
      decision.reason = DeviceDecisionReason::insufficient_queue_count;
    }
    decisions.push_back(std::move(decision));
  }
  return std::nullopt;
}

[[nodiscard]] bool has_any_enabled(const vk::PhysicalDeviceFeatures &features) noexcept {
  return has_requested_feature(features, feature_fields_10);
}

[[nodiscard]] bool has_any_enabled(const vk::PhysicalDeviceVulkan11Features &features) noexcept {
  return has_requested_feature(features, feature_fields_11);
}

[[nodiscard]] bool has_any_enabled(const vk::PhysicalDeviceVulkan12Features &features) noexcept {
  return has_requested_feature(features, feature_fields_12);
}

[[nodiscard]] bool has_any_enabled(const vk::PhysicalDeviceVulkan13Features &features) noexcept {
  return has_requested_feature(features, feature_fields_13);
}

[[nodiscard]] std::optional<std::string>
validate_description_versions(const DeviceDescription &description) {
  if (description.api_version && *description.api_version == 0) {
    return "api_version must be non-zero";
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t>
requested_api_version(const DeviceDescription &description) noexcept {
  return description.api_version;
}

struct CanonicalExtensionRequest {
  std::map<std::string, RequirementStrength> requirements{};
};

enum class ExtensionRequestFailureKind : std::uint8_t {
  invalid_description,
  contradictory,
};

struct ExtensionRequestFailure {
  ExtensionRequestFailureKind kind = ExtensionRequestFailureKind::invalid_description;
  std::string detail{};
};

[[nodiscard]] std::optional<ExtensionRequestFailure>
canonicalize_extensions(const DeviceDescription &description, CanonicalExtensionRequest &request) {
  const auto add = [&](const std::vector<std::string> &names,
                       RequirementStrength strength) -> std::optional<ExtensionRequestFailure> {
    for (const auto &name : names) {
      if (name.empty() || name.find('\0') != std::string::npos) {
        return ExtensionRequestFailure{
            .kind = ExtensionRequestFailureKind::invalid_description,
            .detail = "device extension names must be non-empty and contain no embedded NUL bytes",
        };
      }
      const auto inserted = request.requirements.emplace(name, strength).second;
      if (!inserted) {
        return ExtensionRequestFailure{
            .kind = ExtensionRequestFailureKind::contradictory,
            .detail = "device extension '" + name +
                      "' was declared more than once, including duplicate strength declarations",
        };
      }
    }
    return std::nullopt;
  };
  if (const auto error = add(description.required_extensions, RequirementStrength::required)) {
    return error;
  }
  return add(description.optional_extensions, RequirementStrength::optional);
}

[[nodiscard]] bool extension_available(const std::vector<std::string> &extensions,
                                       std::string_view name) {
  return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
}

[[nodiscard]] std::optional<std::string>
validate_capability_names(const PhysicalDeviceCapabilities &capabilities) {
  for (const auto &name : capabilities.extensions) {
    if (name.empty() || name.find('\0') != std::string::npos) {
      return "physical-device extension observations must be non-empty and contain no embedded "
             "NUL bytes";
    }
  }
  auto names = capabilities.extensions;
  std::sort(names.begin(), names.end());
  if (std::adjacent_find(names.begin(), names.end()) != names.end()) {
    return "physical-device extension observations contain duplicate names";
  }
  return std::nullopt;
}

[[nodiscard]] terreate::Result<DevicePlan>
resolve_device_impl(const PhysicalDevice &physical_device,
                    const PhysicalDeviceCapabilities &input_capabilities, std::size_t index,
                    const DeviceDescription &description) {
  if (const auto error = validate_description_versions(description)) {
    return fail<DevicePlan>(DeviceError::invalid_description, "resolve Vulkan device", *error);
  }
  MergedFeatureMasks requested_features;
  if (const auto error = merge_feature_requests(description, requested_features)) {
    const auto device_error = error->kind == FeatureRequestFailureKind::contradictory
                                  ? DeviceError::contradictory_requirements
                                  : DeviceError::invalid_feature_chain;
    return fail<DevicePlan>(device_error, "resolve Vulkan device", error->detail);
  }
  if (const auto error = validate_capability_names(input_capabilities)) {
    return fail<DevicePlan>(DeviceError::invalid_description, "resolve Vulkan device", *error);
  }
  if (const auto error = validate_capability_feature_snapshots(input_capabilities)) {
    return fail<DevicePlan>(DeviceError::invalid_feature_chain, "resolve Vulkan device", *error);
  }
  const auto requested_version = requested_api_version(description);
  if (requested_version && input_capabilities.api_version < *requested_version) {
    return fail<DevicePlan>(DeviceError::unsupported_api_version, "resolve Vulkan device",
                            "requested Vulkan API version " + std::to_string(*requested_version) +
                                " exceeds the physical device's advertised version");
  }

  std::vector<std::string> available_extensions = input_capabilities.extensions;
  std::sort(available_extensions.begin(), available_extensions.end());
  available_extensions.erase(std::unique(available_extensions.begin(), available_extensions.end()),
                             available_extensions.end());
  CanonicalExtensionRequest extension_request;
  if (const auto error = canonicalize_extensions(description, extension_request)) {
    const auto device_error = error->kind == ExtensionRequestFailureKind::contradictory
                                  ? DeviceError::contradictory_requirements
                                  : DeviceError::invalid_description;
    return fail<DevicePlan>(device_error, "resolve Vulkan device", error->detail);
  }
  std::vector<std::string> enabled_extensions;
  std::vector<DeviceExtensionDecision> extension_decisions;
  for (const auto &[name, strength] : extension_request.requirements) {
    const bool supported = extension_available(available_extensions, name);
    const auto outcome = supported ? DeviceDecisionOutcome::accepted
                                   : DeviceDecisionOutcome::declined;
    const auto reason = supported
                            ? (strength == RequirementStrength::required
                                   ? DeviceDecisionReason::requested
                                   : DeviceDecisionReason::supported)
                            : DeviceDecisionReason::unsupported;
    extension_decisions.push_back(DeviceExtensionDecision{
        .name = name,
        .strength = strength,
        .requested = true,
        .supported = supported,
        .effective = supported,
        .outcome = outcome,
        .reason = reason,
    });
    if (!supported && strength == RequirementStrength::required) {
      return fail<DevicePlan>(DeviceError::missing_required_extension, "resolve Vulkan device",
                              extension_detail(name));
    }
    if (supported) {
      enabled_extensions.push_back(name);
    }
  }

  vk::PhysicalDeviceFeatures enabled10{};
  vk::PhysicalDeviceVulkan11Features enabled11{};
  vk::PhysicalDeviceVulkan12Features enabled12{};
  vk::PhysicalDeviceVulkan13Features enabled13{};
  canonical_feature_header(enabled11, vk::StructureType::ePhysicalDeviceVulkan11Features,
                           feature_fields_11);
  canonical_feature_header(enabled12, vk::StructureType::ePhysicalDeviceVulkan12Features,
                           feature_fields_12);
  canonical_feature_header(enabled13, vk::StructureType::ePhysicalDeviceVulkan13Features,
                           feature_fields_13);
  std::vector<DeviceFeatureDecision> feature_decisions;
  std::optional<std::string> required_feature_failure;
   append_feature_decisions(DeviceFeatureVersion::vulkan_1_0, "VkPhysicalDeviceFeatures",
                            RequirementStrength::required, requested_features.required_10,
                            input_capabilities.features_10, true, feature_fields_10, enabled10,
                            feature_decisions, required_feature_failure);
   append_feature_decisions(DeviceFeatureVersion::vulkan_1_0, "VkPhysicalDeviceFeatures",
                            RequirementStrength::optional, requested_features.optional_10,
                            input_capabilities.features_10, true, feature_fields_10, enabled10,
                            feature_decisions, required_feature_failure);
   append_feature_decisions(DeviceFeatureVersion::vulkan_1_1,
                            "VkPhysicalDeviceVulkan11Features", RequirementStrength::required,
                            requested_features.required_11,
                            input_capabilities.features_11, input_capabilities.features_11_available,
                            feature_fields_11, enabled11, feature_decisions, required_feature_failure);
   append_feature_decisions(DeviceFeatureVersion::vulkan_1_1,
                            "VkPhysicalDeviceVulkan11Features", RequirementStrength::optional,
                            requested_features.optional_11,
                            input_capabilities.features_11, input_capabilities.features_11_available,
                            feature_fields_11, enabled11, feature_decisions, required_feature_failure);
   append_feature_decisions(DeviceFeatureVersion::vulkan_1_2,
                            "VkPhysicalDeviceVulkan12Features", RequirementStrength::required,
                            requested_features.required_12,
                            input_capabilities.features_12, input_capabilities.features_12_available,
                            feature_fields_12, enabled12, feature_decisions, required_feature_failure);
   append_feature_decisions(DeviceFeatureVersion::vulkan_1_2,
                            "VkPhysicalDeviceVulkan12Features", RequirementStrength::optional,
                            requested_features.optional_12,
                            input_capabilities.features_12, input_capabilities.features_12_available,
                            feature_fields_12, enabled12, feature_decisions, required_feature_failure);
   append_feature_decisions(DeviceFeatureVersion::vulkan_1_3,
                            "VkPhysicalDeviceVulkan13Features", RequirementStrength::required,
                            requested_features.required_13,
                            input_capabilities.features_13, input_capabilities.features_13_available,
                            feature_fields_13, enabled13, feature_decisions, required_feature_failure);
   append_feature_decisions(DeviceFeatureVersion::vulkan_1_3,
                            "VkPhysicalDeviceVulkan13Features", RequirementStrength::optional,
                            requested_features.optional_13,
                            input_capabilities.features_13, input_capabilities.features_13_available,
                            feature_fields_13, enabled13, feature_decisions, required_feature_failure);
  if (required_feature_failure) {
    return fail<DevicePlan>(DeviceError::unsupported_required_feature, "resolve Vulkan device",
                            *required_feature_failure);
  }

  std::vector<DeviceQueueDecision> queue_decisions;
  std::vector<DeviceQueueAllocation> queue_allocations;
  std::vector<DeviceQueueAssignment> queue_assignments;
  std::vector<DeviceQueueAliasGroup> queue_alias_groups;
  if (const auto error = resolve_queues(input_capabilities, description, queue_decisions,
                                        queue_allocations, queue_assignments,
                                        queue_alias_groups)) {
    const auto queue_error = [&] {
       switch (error->kind) {
       case QueueResolutionFailure::Kind::invalid:
         return DeviceError::invalid_queue_request;
       case QueueResolutionFailure::Kind::contradictory:
         return DeviceError::contradictory_requirements;
       case QueueResolutionFailure::Kind::insufficient:
         return DeviceError::insufficient_queue_count;
       case QueueResolutionFailure::Kind::ambiguous:
         return DeviceError::ambiguous_queue;
      }
      return DeviceError::invalid_queue_request;
    }();
    return fail<DevicePlan>(queue_error, "resolve Vulkan device", error->detail);
  }
  std::vector<float> queue_priorities;
  queue_priorities.reserve(queue_allocations.size());
  for (const auto &allocation : queue_allocations) {
    queue_priorities.push_back(allocation.priority);
  }

  // The successful plan owns a value snapshot, never the caller's pNext
  // pointers.  Validation above has already rejected non-null chains; clear
  // the validated fields in the owned copy so the lifetime contract remains
  // true even for a synthetic caller-built description.
  auto requested_snapshot = description;
  requested_snapshot.required_features_11.pNext = nullptr;
  requested_snapshot.optional_features_11.pNext = nullptr;
  requested_snapshot.required_features_12.pNext = nullptr;
  requested_snapshot.optional_features_12.pNext = nullptr;
  requested_snapshot.required_features_13.pNext = nullptr;
  requested_snapshot.optional_features_13.pNext = nullptr;

  return DevicePlanBuilder::build(
       std::move(requested_snapshot), physical_device, input_capabilities, index,
       requested_version.value_or(input_capabilities.api_version),
       std::move(enabled_extensions),
       requested_version ? DeviceApiVersionProvenance::explicit_request
                         : DeviceApiVersionProvenance::capability_snapshot,
       enabled10, enabled11, enabled12, enabled13,
      std::move(feature_decisions), std::move(extension_decisions), std::move(queue_decisions),
      std::move(queue_allocations), std::move(queue_assignments), std::move(queue_alias_groups),
      std::move(queue_priorities));
}

struct NativeQueueGroup {
  std::uint32_t family_index = 0;
  std::uint32_t count = 0;
  std::size_t priority_offset = 0;
};

[[nodiscard]] std::optional<std::string>
validate_native_plan(const Instance &instance, const DevicePlan &plan) {
  if (!instance.valid() || instance.plan() == nullptr) {
    return "the supplied Instance is moved-from, invalid, or has no retained plan";
  }
  if (!plan.hasPhysicalDevice()) {
    return "the DevicePlan was resolved from a synthetic capability snapshot and has no native "
           "physical-device view";
  }
  if (!plan.physicalDevice().correlatedWith(instance)) {
    return "the plan's physical device is correlated with a different Instance";
  }
  const auto requested_version = requested_api_version(plan.requested());
  if (requested_version && instance.plan()->effectiveApiVersion() < *requested_version) {
    return "the parent Instance API version is below the DevicePlan minimum";
  }
  if (plan.queueAllocations().size() != plan.queuePriorities().size()) {
    return "queue allocation and priority storage lengths differ";
  }
  if (plan.queueAllocations().empty()) {
    return "DevicePlan contains no effective queue allocation; Vulkan device creation requires at least one queue";
  }
  const auto &families = plan.capabilities().queue_families;
  if (plan.requested().required_features_11.pNext != nullptr ||
      plan.requested().optional_features_11.pNext != nullptr ||
      plan.requested().required_features_12.pNext != nullptr ||
      plan.requested().optional_features_12.pNext != nullptr ||
      plan.requested().required_features_13.pNext != nullptr ||
      plan.requested().optional_features_13.pNext != nullptr ||
      plan.enabledFeatures11().pNext != nullptr || plan.enabledFeatures12().pNext != nullptr ||
      plan.enabledFeatures13().pNext != nullptr) {
    return "DevicePlan contains a borrowed feature pNext pointer";
  }
  for (std::size_t index = 0; index < plan.queueAllocations().size(); ++index) {
    const auto &allocation = plan.queueAllocations()[index];
    if (allocation.family_index > std::numeric_limits<std::uint32_t>::max() ||
        allocation.queue_index > std::numeric_limits<std::uint32_t>::max() ||
        allocation.family_index >= families.size() ||
        allocation.queue_index >= families[allocation.family_index].queueCount ||
        !std::isfinite(allocation.priority) || allocation.priority < 0.0F ||
        allocation.priority > 1.0F) {
      return "DevicePlan contains an out-of-range queue allocation";
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      const auto &other = plan.queueAllocations()[previous];
      if (other.family_index == allocation.family_index &&
          other.queue_index == allocation.queue_index) {
        return "DevicePlan contains duplicate native queue allocations";
      }
    }
  }
  for (const auto &assignment : plan.queueAssignments()) {
    if (assignment.allocation_index >= plan.queueAllocations().size()) {
      return "DevicePlan contains an out-of-range queue assignment";
    }
    const auto &allocation = plan.queueAllocations()[assignment.allocation_index];
    if (assignment.family_index != allocation.family_index ||
        assignment.queue_index != allocation.queue_index) {
      return "DevicePlan queue assignment coordinates do not match its allocation";
    }
  }
  return std::nullopt;
}

} // namespace

const std::error_category &device_error_category() noexcept { return device_category(); }

std::error_code make_error_code(DeviceError error) noexcept {
  return {static_cast<int>(error), device_error_category()};
}

terreate::Result<DevicePlan> resolveDevice(const PhysicalDeviceCandidate &candidate,
                                           const DeviceDescription &description) {
  return resolve_device_impl(candidate.device(), candidate.capabilities(), candidate.enumerationIndex(),
                             description);
}

terreate::Result<DevicePlan> resolveDevice(const PhysicalDeviceCapabilities &capabilities,
                                           const DeviceDescription &description) {
  return resolve_device_impl(PhysicalDevice{}, capabilities, 0, description);
}

struct Device::Impl {
  vk::raii::Device device;
  DevicePlan plan;
  const Instance::Impl *parent_identity = nullptr;
  std::vector<Queue> queues{};

  Impl(vk::raii::Device device_owner, DevicePlan device_plan,
       const Instance::Impl *parent_identity_owner)
      : device(std::move(device_owner)), plan(std::move(device_plan)),
        parent_identity(parent_identity_owner) {}

  void appendQueue(vk::Queue native_handle, std::size_t family_index, std::size_t queue_index) {
    Queue borrowed{native_handle, this, family_index, queue_index};
    queues.push_back(borrowed);
  }
};

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
Queue::Queue(vk::Queue native_handle, const Device::Impl *parent_identity,
             std::size_t family_index, std::size_t queue_index) noexcept
    : native_handle_(native_handle), parent_identity_(parent_identity),
      family_index_(family_index), queue_index_(queue_index) {}
// NOLINTEND(bugprone-easily-swappable-parameters)

Queue::operator bool() const noexcept { return valid(); }

bool Queue::valid() const noexcept {
  return static_cast<VkQueue>(native_handle_) != VK_NULL_HANDLE;
}

vk::Queue Queue::nativeHandle() const noexcept { return native_handle_; }

bool Queue::correlatedWith(const Device &device) const noexcept {
  return valid() && parent_identity_ != nullptr && device.valid() &&
         parent_identity_ == device.implementation_.get();
}

Device::Device(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

Device::Device(Device &&other) noexcept : implementation_(std::move(other.implementation_)) {}

Device &Device::operator=(Device &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  implementation_ = std::move(other.implementation_);
  return *this;
}

Device::~Device() = default;

Device::operator bool() const noexcept { return valid(); }

bool Device::valid() const noexcept {
  return implementation_ != nullptr &&
         static_cast<VkDevice>(*implementation_->device) != VK_NULL_HANDLE;
}

vk::Device Device::nativeHandle() const noexcept {
  return implementation_ == nullptr ? vk::Device{} : *implementation_->device;
}

const DevicePlan *Device::plan() const noexcept {
  return implementation_ == nullptr ? nullptr : &implementation_->plan;
}

bool Device::correlatedWith(const Instance &instance) const noexcept {
  return valid() && instance.valid() && implementation_->parent_identity == instance.implementation_.get();
}

Queue Device::queue(std::string_view caller_id) const noexcept {
  if (implementation_ == nullptr) {
    return {};
  }
  const auto assignment = std::find_if(
      implementation_->plan.queueAssignments().begin(), implementation_->plan.queueAssignments().end(),
      [&](const auto &candidate) { return candidate.caller_id == caller_id; });
  if (assignment == implementation_->plan.queueAssignments().end()) {
    return {};
  }
  const auto allocation_index = assignment->allocation_index;
  if (allocation_index >= implementation_->plan.queueAllocations().size()) {
    return {};
  }
  const auto &allocation = implementation_->plan.queueAllocations()[allocation_index];
  const auto queue = std::find_if(
      implementation_->queues.begin(), implementation_->queues.end(), [&](const Queue &candidate) {
        return candidate.familyIndex() == allocation.family_index &&
               candidate.queueIndex() == allocation.queue_index;
      });
  return queue == implementation_->queues.end() ? Queue{} : *queue;
}

std::span<const Queue> Device::queues() const noexcept {
  if (implementation_ == nullptr) {
    return {};
  }
  return implementation_->queues;
}

terreate::Result<Device> createDevice(const Instance &instance, const DevicePlan &plan) {
  // This boundary consumes the already-resolved plan.  It deliberately does
  // not call resolveDevice or query any mutable physical-device state.
  if (!instance.valid() || instance.plan() == nullptr) {
    return fail<Device>(DeviceError::invalid_instance, "create Vulkan device",
                        "the supplied Instance is moved-from, invalid, or has no retained plan");
  }
  if (const auto error = validate_native_plan(instance, plan)) {
    const auto category = error->find("different Instance") != std::string::npos
                              ? DeviceError::parent_mismatch
                              : (error->find("synthetic") != std::string::npos
                                     ? DeviceError::invalid_physical_device
                                     : DeviceError::invalid_plan);
    return fail<Device>(category, "create Vulkan device", *error);
  }

  const auto &allocations = plan.queueAllocations();
  std::vector<std::size_t> ordered_indices(allocations.size());
  for (std::size_t index = 0; index < ordered_indices.size(); ++index) {
    ordered_indices[index] = index;
  }
  std::sort(ordered_indices.begin(), ordered_indices.end(), [&](std::size_t left, std::size_t right) {
    const auto &lhs = allocations[left];
    const auto &rhs = allocations[right];
    if (lhs.family_index != rhs.family_index) {
      return lhs.family_index < rhs.family_index;
    }
    return lhs.queue_index < rhs.queue_index;
  });

  std::vector<float> native_priorities;
  native_priorities.reserve(ordered_indices.size());
  std::vector<NativeQueueGroup> groups;
  for (const auto allocation_index : ordered_indices) {
    const auto &allocation = allocations[allocation_index];
    if (groups.empty() || groups.back().family_index != allocation.family_index) {
      if (allocation.family_index > std::numeric_limits<std::uint32_t>::max()) {
        return fail<Device>(DeviceError::invalid_plan, "create Vulkan device",
                            "queue family index exceeds the Vulkan uint32_t ABI");
      }
      groups.push_back(NativeQueueGroup{
          .family_index = static_cast<std::uint32_t>(allocation.family_index),
          .count = 0,
          .priority_offset = native_priorities.size(),
      });
    } else if (allocation.queue_index != groups.back().count) {
      return fail<Device>(DeviceError::invalid_plan, "create Vulkan device",
                          "queue allocations for a family are not contiguous from queue zero");
    }
    if (groups.back().count == std::numeric_limits<std::uint32_t>::max()) {
      return fail<Device>(DeviceError::invalid_plan, "create Vulkan device",
                          "per-family queue count exceeds the Vulkan uint32_t ABI");
    }
    native_priorities.push_back(allocation.priority);
    ++groups.back().count;
  }

  std::vector<vk::DeviceQueueCreateInfo> queue_create_infos;
  if (groups.empty()) {
    return fail<Device>(DeviceError::invalid_plan, "create Vulkan device",
                        "the native queue-create-info list cannot be empty");
  }
  if (groups.size() > std::numeric_limits<std::uint32_t>::max()) {
    return fail<Device>(DeviceError::invalid_plan, "create Vulkan device",
                        "queue-create-info count exceeds the Vulkan uint32_t ABI");
  }
  queue_create_infos.reserve(groups.size());
  for (const auto &group : groups) {
    vk::DeviceQueueCreateInfo queue_info{};
    queue_info.queueFamilyIndex = group.family_index;
    queue_info.queueCount = group.count;
    queue_info.pQueuePriorities = native_priorities.data() + group.priority_offset;
    queue_create_infos.push_back(queue_info);
  }

  std::vector<const char *> extension_names;
  extension_names.reserve(plan.enabledExtensions().size());
  for (const auto &extension : plan.enabledExtensions()) {
    extension_names.push_back(extension.c_str());
  }
  if (extension_names.size() > std::numeric_limits<std::uint32_t>::max()) {
    return fail<Device>(DeviceError::invalid_plan, "create Vulkan device",
                        "enabled device extension count exceeds the Vulkan uint32_t ABI");
  }

  vk::PhysicalDeviceFeatures features10 = plan.enabledFeatures10();
  vk::PhysicalDeviceVulkan11Features features11 = plan.enabledFeatures11();
  vk::PhysicalDeviceVulkan12Features features12 = plan.enabledFeatures12();
  vk::PhysicalDeviceVulkan13Features features13 = plan.enabledFeatures13();
  features11.pNext = nullptr;
  features12.pNext = nullptr;
  features13.pNext = nullptr;

  void *feature_chain = nullptr;
  if (has_any_enabled(features13)) {
    feature_chain = &features13;
  }
  if (has_any_enabled(features12)) {
    features12.pNext = feature_chain;
    feature_chain = &features12;
  }
  if (has_any_enabled(features11)) {
    features11.pNext = feature_chain;
    feature_chain = &features11;
  }

  vk::DeviceCreateInfo create_info{};
  create_info.pNext = feature_chain;
  // queue_create_infos was checked against the Vulkan ABI immediately before
  // this conversion.  VkDeviceCreateInfo requires at least one queue-create
  // info; validate_native_plan rejects an empty effective allocation set.
  create_info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_create_infos.size());
  create_info.pQueueCreateInfos = queue_create_infos.data();
  create_info.enabledExtensionCount = static_cast<std::uint32_t>(extension_names.size());
  create_info.ppEnabledExtensionNames = extension_names.data();
  create_info.pEnabledFeatures = has_any_enabled(features10) ? &features10 : nullptr;

  try {
    vk::raii::PhysicalDevice physical_device{
        instance.implementation_->instance,
        static_cast<VkPhysicalDevice>(plan.physicalDevice().nativeHandle())};
    vk::raii::Device native_device{physical_device, create_info};
    auto implementation =
        std::make_unique<Device::Impl>(std::move(native_device), plan, instance.implementation_.get());
    implementation->queues.reserve(ordered_indices.size());
    for (const auto allocation_index : ordered_indices) {
      const auto &allocation = allocations[allocation_index];
      auto native_queue = implementation->device.getQueue(
           static_cast<std::uint32_t>(allocation.family_index),
           static_cast<std::uint32_t>(allocation.queue_index));
      implementation->appendQueue(vk::Queue{*native_queue}, allocation.family_index,
                                    allocation.queue_index);
    }
    return Device{std::move(implementation)};
  } catch (const vk::SystemError &error) {
    return std::unexpected(
        terreate::Error{error.code(), "create Vulkan device", error.what()});
  }
}

terreate::Result<Device> createDevice(const DevicePlan &plan, const Instance &instance) {
  return createDevice(instance, plan);
}

} // namespace terreate::graphics

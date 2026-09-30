#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

#include "physical_device_query.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace terreate::graphics;
namespace graphics_detail = terreate::graphics::detail;

static_assert(std::copy_constructible<PhysicalDevice>);
static_assert(std::is_nothrow_copy_constructible_v<PhysicalDevice>);
static_assert(!std::is_trivially_copyable_v<PhysicalDevice>);
static_assert(std::is_nothrow_move_constructible_v<PhysicalDevice>);
static_assert(std::is_nothrow_move_assignable_v<PhysicalDevice>);
static_assert(!std::is_constructible_v<PhysicalDevice, vk::PhysicalDevice, const void *>);
static_assert(!std::is_constructible_v<PhysicalDevice, vk::PhysicalDevice, vk::Instance,
                                       std::shared_ptr<const void>>);
static_assert(!std::is_convertible_v<PhysicalDevice, vk::PhysicalDevice>);
static_assert(std::copy_constructible<PhysicalDeviceCapabilities>);
static_assert(std::copy_constructible<PhysicalDeviceCandidate>);

[[nodiscard]] bool check(bool condition, const char *message) {
  if (!condition) {
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
  }
  return condition;
}

void emit_runtime_marker(const char *marker) {
  std::fputs(marker, stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

[[nodiscard]] PhysicalDeviceUuid uuid(std::uint8_t first) {
  PhysicalDeviceUuid value{};
  value[0] = first;
  value[VK_UUID_SIZE - 1] = static_cast<std::uint8_t>(0xffu - first);
  return value;
}

[[nodiscard]] PhysicalDeviceCapabilities
synthetic_capabilities(std::uint8_t uuid_first, bool anisotropy, bool shader_int64) {
  PhysicalDeviceCapabilities capabilities;
  capabilities.api_version = VK_API_VERSION_1_3;
  capabilities.id_properties_available = true;
  capabilities.device_uuid = uuid(uuid_first);
  capabilities.properties.vendorID = 0x1000u + uuid_first;
  capabilities.properties.deviceID = 0x2000u + uuid_first;
  const std::string synthetic_name = "synthetic-device-" + std::to_string(uuid_first);
  std::copy(synthetic_name.begin(), synthetic_name.end(),
            capabilities.properties.deviceName.begin());
  const auto extension_properties = graphics_detail::canonicalize_physical_device_extensions({
      PhysicalDeviceExtensionProperty{.name = "VK_KHR_swapchain", .spec_version = 70},
      PhysicalDeviceExtensionProperty{.name = "VK_EXT_memory_budget", .spec_version = 1},
      PhysicalDeviceExtensionProperty{.name = "VK_KHR_swapchain", .spec_version = 69},
  });
  capabilities.extension_properties = extension_properties;
  for (const auto &extension : extension_properties) {
    capabilities.extensions.push_back(extension.name);
  }
  capabilities.features_10.samplerAnisotropy = anisotropy ? VK_TRUE : VK_FALSE;
  capabilities.features_10.shaderInt64 = shader_int64 ? VK_TRUE : VK_FALSE;
  capabilities.features_11.shaderDrawParameters = VK_TRUE;
  capabilities.features_12.timelineSemaphore = VK_TRUE;
  capabilities.features_13.dynamicRendering = VK_TRUE;
  capabilities.properties.deviceType = vk::PhysicalDeviceType::eDiscreteGpu;
  vk::QueueFamilyProperties queue_family{};
  queue_family.queueFlags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
  queue_family.queueCount = 1;
  capabilities.queue_families.push_back(queue_family);
  capabilities.memory_properties.memoryHeapCount = 1;
  capabilities.memory_properties.memoryHeaps[0].size =
      static_cast<vk::DeviceSize>(512u) * 1024u * 1024u;
  capabilities.memory_properties.memoryHeaps[0].flags = vk::MemoryHeapFlagBits::eDeviceLocal;
  capabilities.memory_budget.emplace();
  return capabilities;
}

[[nodiscard]] PhysicalDeviceCandidate
synthetic_candidate(std::uint8_t uuid_first, bool anisotropy = true, bool shader_int64 = false,
                    std::size_t index = 0, PhysicalDevice device = {}) {
  return PhysicalDeviceCandidate{
      .device = std::move(device),
      .capabilities = synthetic_capabilities(uuid_first, anisotropy, shader_int64),
      .enumeration_index = index,
  };
}

[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
synthetic_query_failure(const graphics_detail::PhysicalDeviceQueryInput &) {
  throw vk::SystemError{vk::Result::eErrorInitializationFailed};
}

[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
synthetic_empty_query(const graphics_detail::PhysicalDeviceQueryInput &) {
  return PhysicalDeviceInventory{};
}

[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
synthetic_temporary_query(const graphics_detail::PhysicalDeviceQueryInput &) {
  // Every value below is local to this adapter.  A successful query must
  // return copies that remain usable after these temporaries are destroyed.
  auto temporary = synthetic_capabilities(0x33u, true, true);
  temporary.driver_name = "temporary-driver-name";
  temporary.driver_info = "temporary-driver-info";
  temporary.extension_properties = graphics_detail::canonicalize_physical_device_extensions({
      PhysicalDeviceExtensionProperty{.name = "VK_EXT_temporary", .spec_version = 4},
      PhysicalDeviceExtensionProperty{.name = "VK_EXT_temporary", .spec_version = 3},
  });
  temporary.extensions = {temporary.extension_properties.front().name};

  PhysicalDeviceInventory inventory;
  inventory.candidates.push_back(PhysicalDeviceCandidate{
      .device = PhysicalDevice{},
      .capabilities = std::move(temporary),
      .enumeration_index = 7,
  });
  return inventory;
}

[[nodiscard]] bool same_evaluation(const PhysicalDeviceEvaluation &left,
                                   const PhysicalDeviceEvaluation &right) {
  if (left.candidate_identity.uuid != right.candidate_identity.uuid ||
      left.candidate_identity.vendor_id != right.candidate_identity.vendor_id ||
      left.candidate_identity.device_id != right.candidate_identity.device_id ||
      left.candidate_identity.type != right.candidate_identity.type ||
      left.candidate_identity.device_name != right.candidate_identity.device_name ||
      left.matches != right.matches || left.decisions.size() != right.decisions.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.decisions.size(); ++index) {
    const auto &left_decision = left.decisions[index];
    const auto &right_decision = right.decisions[index];
    if (left_decision.kind != right_decision.kind ||
        left_decision.strength != right_decision.strength ||
        left_decision.outcome != right_decision.outcome ||
        left_decision.reason != right_decision.reason) {
      return false;
    }
    if (left_decision.evidence.index() != right_decision.evidence.index()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool test_synthetic_evaluation_and_optional_decisions() {
  const auto candidate = synthetic_candidate(0x21u);
  PhysicalDeviceRequirements requirements;
  requirements.minimum_api_version = VK_API_VERSION_1_2;
  requirements.required_extensions = {"VK_KHR_swapchain"};
  requirements.optional_extensions = {"VK_EXT_not_present"};
  requirements.required_queue_families = {PhysicalDeviceQueueRequirement{
      .flags = vk::QueueFlagBits::eGraphics,
      .min_queue_count = 1,
  }};
  requirements.optional_queue_families = {
      PhysicalDeviceQueueRequirement{
          .flags = vk::QueueFlagBits::eCompute,
          .min_queue_count = 1,
      },
      PhysicalDeviceQueueRequirement{
          .flags = vk::QueueFlagBits::eTransfer,
          .min_queue_count = 1,
      },
  };
  const auto result = evaluatePhysicalDevice(candidate, requirements);
  bool passed = check(result.has_value(), "synthetic physical-device evaluation failed");
  if (!result) {
    return false;
  }
  passed &= check(result->matches, "supported synthetic requirements did not match");
  passed &= check(result->decisions.size() == 6,
                  "synthetic evaluation did not retain every explicit decision");
  const auto &identity = result->candidate_identity;
  const auto expected_name = "synthetic-device-" + std::to_string(0x21u);
  passed &= check(identity.uuid ==
                          std::optional<PhysicalDeviceUuid>{candidate.capabilities.device_uuid} &&
                      identity.vendor_id == candidate.capabilities.properties.vendorID &&
                      identity.device_id == candidate.capabilities.properties.deviceID &&
                      identity.type == candidate.capabilities.properties.deviceType &&
                      identity.device_name == expected_name,
                  "synthetic evaluation did not retain the complete owned candidate identity");

  const auto optional_queue_accepted =
      std::find_if(result->decisions.begin(), result->decisions.end(), [](const auto &decision) {
        if (decision.kind != PhysicalDeviceRequirementKind::queue ||
            decision.strength != RequirementStrength::optional ||
            decision.outcome != PhysicalDeviceRequirementOutcome::accepted ||
            decision.reason != PhysicalDeviceRequirementReason::supported) {
          return false;
        }
        const auto *evidence = std::get_if<PhysicalDeviceQueueEvidence>(&decision.evidence);
        return evidence != nullptr && evidence->requirement.flags == vk::QueueFlagBits::eCompute &&
               evidence->matching_queue_indices == std::vector<std::size_t>{0} &&
               evidence->requirement.min_queue_count == 1;
      });
  passed &= check(optional_queue_accepted != result->decisions.end(),
                  "supported optional queue capability was not accepted");

  const auto optional_queue_declined =
      std::find_if(result->decisions.begin(), result->decisions.end(), [](const auto &decision) {
        if (decision.kind != PhysicalDeviceRequirementKind::queue ||
            decision.strength != RequirementStrength::optional ||
            decision.outcome != PhysicalDeviceRequirementOutcome::declined ||
            decision.reason != PhysicalDeviceRequirementReason::unsupported) {
          return false;
        }
        const auto *evidence = std::get_if<PhysicalDeviceQueueEvidence>(&decision.evidence);
        return evidence != nullptr && evidence->requirement.flags == vk::QueueFlagBits::eTransfer &&
               evidence->matching_queue_indices.empty();
      });
  passed &= check(optional_queue_declined != result->decisions.end(),
                  "unsupported optional queue capability was not declined");

  const auto repeated = evaluatePhysicalDevice(candidate, requirements);
  passed &= check(repeated.has_value(), "repeated synthetic evaluation failed");
  if (repeated) {
    passed &= check(same_evaluation(*result, *repeated),
                    "repeated physical-device evaluation was not deterministic");
  }

  const auto no_intent = evaluatePhysicalDevice(candidate, PhysicalDeviceRequirements{});
  passed &= check(candidate.capabilities.features_10.samplerAnisotropy == VK_TRUE &&
                      no_intent.has_value() && no_intent->matches && no_intent->decisions.empty(),
                  "supported but unrequested observations implied enable intent");

  PhysicalDeviceRequirements typed_requirements;
  typed_requirements.minimum_api_version = VK_API_VERSION_1_2;
  typed_requirements.required_device_uuid = candidate.capabilities.device_uuid;
  typed_requirements.required_device_type = candidate.capabilities.properties.deviceType;
  typed_requirements.required_extensions = {"VK_KHR_swapchain"};
  const auto typed_evaluation = evaluatePhysicalDevice(candidate, typed_requirements);
  passed &= check(typed_evaluation.has_value() && typed_evaluation->matches &&
                      typed_evaluation->decisions.size() == 4,
                  "typed physical-device requirement evidence was not retained");
  if (typed_evaluation && typed_evaluation->decisions.size() == 4) {
    const auto *api_evidence =
        std::get_if<PhysicalDeviceApiEvidence>(&typed_evaluation->decisions[0].evidence);
    const auto *uuid_evidence =
        std::get_if<PhysicalDeviceUuidEvidence>(&typed_evaluation->decisions[1].evidence);
    const auto *type_evidence =
        std::get_if<PhysicalDeviceTypeEvidence>(&typed_evaluation->decisions[2].evidence);
    const auto *extension_evidence =
        std::get_if<PhysicalDeviceExtensionEvidence>(&typed_evaluation->decisions[3].evidence);
    passed &= check(api_evidence != nullptr, "missing typed API evidence");
    if (api_evidence != nullptr) {
      passed &= check(api_evidence->required_api_version == VK_API_VERSION_1_2 &&
                          api_evidence->available_api_version == candidate.capabilities.api_version,
                      "API evidence lost requested or observed values");
    }
    passed &= check(uuid_evidence != nullptr, "missing typed UUID evidence");
    if (uuid_evidence != nullptr) {
      passed &= check(uuid_evidence->required_uuid == candidate.capabilities.device_uuid &&
                          uuid_evidence->available_uuid ==
                              std::optional<PhysicalDeviceUuid>{candidate.capabilities.device_uuid},
                      "UUID evidence lost requested or observed values");
    }
    passed &= check(type_evidence != nullptr, "missing typed device-type evidence");
    if (type_evidence != nullptr) {
      passed &= check(type_evidence->required_type == candidate.capabilities.properties.deviceType,
                      "device-type evidence lost the requested value");
      passed &= check(type_evidence->available_type == candidate.capabilities.properties.deviceType,
                      "device-type evidence lost the observed value");
    }
    passed &= check(extension_evidence != nullptr, "missing typed extension evidence");
    if (extension_evidence != nullptr) {
      passed &= check(extension_evidence->extension == "VK_KHR_swapchain" &&
                          extension_evidence->available,
                      "extension evidence lost requested or observed values");
    }
  }

  auto no_match_requirements = requirements;
  no_match_requirements.required_extensions = {"VK_EXT_missing"};
  const auto no_match_evaluation = evaluatePhysicalDevice(candidate, no_match_requirements);
  passed &= check(no_match_evaluation.has_value() && !no_match_evaluation->matches,
                  "unsupported required extension did not produce a non-match evaluation");

  PhysicalDeviceRequirements contradictory = requirements;
  contradictory.required_extensions = {"VK_KHR_swapchain"};
  contradictory.optional_extensions = {"VK_KHR_swapchain"};
  const auto invalid = evaluatePhysicalDevice(candidate, contradictory);
  const bool contradictory_rejected =
      !invalid &&
      invalid.error().code() == make_error_code(PhysicalDeviceError::invalid_requirements);
  passed &= check(contradictory_rejected, "contradictory requirements were accepted");

  return passed;
}

[[nodiscard]] bool test_extension_canonicalization_and_explicit_intent() {
  const auto canonical = graphics_detail::canonicalize_physical_device_extensions({
      PhysicalDeviceExtensionProperty{.name = "VK_EXT_zeta", .spec_version = 1},
      PhysicalDeviceExtensionProperty{.name = "VK_EXT_alpha", .spec_version = 2},
      PhysicalDeviceExtensionProperty{.name = "VK_EXT_zeta", .spec_version = 9},
      PhysicalDeviceExtensionProperty{.name = "VK_EXT_alpha", .spec_version = 1},
  });

  bool passed = true;
  const bool canonical_size_is_expected = canonical.size() == 2;
  passed &= check(canonical_size_is_expected,
                  "physical-device extension observations were not deduplicated");
  if (canonical.size() == 2) {
    passed &= check(canonical[0].name == "VK_EXT_alpha" && canonical[0].spec_version == 2,
                    "physical-device extension observations were not ordered by name/version");
    passed &= check(canonical[1].name == "VK_EXT_zeta" && canonical[1].spec_version == 9,
                    "duplicate extension metadata did not retain the highest version");
  }

  const auto candidate = synthetic_candidate(0x23u);
  passed &= check(std::is_sorted(candidate.capabilities.extensions.begin(),
                                 candidate.capabilities.extensions.end()),
                  "synthetic physical-device extension names were not canonicalised");
  passed &= check(std::adjacent_find(candidate.capabilities.extensions.begin(),
                                     candidate.capabilities.extensions.end()) ==
                      candidate.capabilities.extensions.end(),
                  "synthetic physical-device extension names contained duplicates");

  const auto no_request = evaluatePhysicalDevice(candidate, PhysicalDeviceRequirements{});
  passed &= check(no_request.has_value() && no_request->matches && no_request->decisions.empty(),
                  "supported-but-unrequested extension acquired enable intent");
  return passed;
}

[[nodiscard]] bool test_query_gating_and_snapshot_boundaries() {
  bool passed = true;

  const auto lower_instance_and_device = graphics_detail::physical_device_query_availability(
      graphics_detail::PhysicalDeviceQueryApiVersions{
          .effective_instance_api = VK_API_VERSION_1_0,
          .physical_device_api = VK_API_VERSION_1_0,
      },
      false, false, false);
  const bool lower_members_gated =
      !lower_instance_and_device.properties2 && !lower_instance_and_device.id_properties &&
      !lower_instance_and_device.driver_properties && !lower_instance_and_device.features_11 &&
      !lower_instance_and_device.features_12 && !lower_instance_and_device.features_13 &&
      !lower_instance_and_device.memory_properties2;
  passed &= check(lower_members_gated,
                  "lower-version Instance/device query did not gate all pNext members");

  const auto extension_backed = graphics_detail::physical_device_query_availability(
      graphics_detail::PhysicalDeviceQueryApiVersions{
          .effective_instance_api = VK_API_VERSION_1_0,
          .physical_device_api = VK_API_VERSION_1_0,
      },
      true, true, true);
  const bool extension_members_gated =
      extension_backed.properties2 && extension_backed.id_properties &&
      extension_backed.driver_properties && !extension_backed.features_11 &&
      !extension_backed.features_12 && !extension_backed.features_13 &&
      extension_backed.memory_properties2;
  passed &= check(extension_members_gated,
                  "extension-backed pNext availability was not gated independently");

  const auto vulkan11 = graphics_detail::physical_device_query_availability(
      graphics_detail::PhysicalDeviceQueryApiVersions{
          .effective_instance_api = VK_API_VERSION_1_1,
          .physical_device_api = VK_API_VERSION_1_1,
      },
      false, false, false);
  const bool vulkan11_members_gated = vulkan11.properties2 && vulkan11.id_properties &&
                                      !vulkan11.driver_properties && vulkan11.features_11 &&
                                      !vulkan11.features_12 && !vulkan11.features_13;
  passed &= check(vulkan11_members_gated,
                  "Vulkan 1.1 pNext availability was not gated by both API versions");

  const auto vulkan12 = graphics_detail::physical_device_query_availability(
      graphics_detail::PhysicalDeviceQueryApiVersions{
          .effective_instance_api = VK_API_VERSION_1_2,
          .physical_device_api = VK_API_VERSION_1_2,
      },
      false, false, false);
  const bool vulkan12_members_gated = vulkan12.driver_properties && vulkan12.features_11 &&
                                      vulkan12.features_12 && !vulkan12.features_13;
  passed &= check(vulkan12_members_gated,
                  "Vulkan 1.2 pNext availability was not gated by the effective/device API");

  const auto device_only_vulkan12 = graphics_detail::physical_device_query_availability(
      graphics_detail::PhysicalDeviceQueryApiVersions{
          .effective_instance_api = VK_API_VERSION_1_1,
          .physical_device_api = VK_API_VERSION_1_2,
      },
      false, false, false);
  passed &= check(!device_only_vulkan12.driver_properties,
                  "core driver properties ignored the effective Instance API gate");

  const auto device_extension_driver_properties =
      graphics_detail::physical_device_query_availability(
          graphics_detail::PhysicalDeviceQueryApiVersions{
              .effective_instance_api = VK_API_VERSION_1_1,
              .physical_device_api = VK_API_VERSION_1_0,
          },
          false, false, true);
  passed &= check(device_extension_driver_properties.properties2 &&
                      device_extension_driver_properties.driver_properties,
                  "device driver-properties extension was not accepted through Properties2");

  const auto vulkan13 = graphics_detail::physical_device_query_availability(
      graphics_detail::PhysicalDeviceQueryApiVersions{
          .effective_instance_api = VK_API_VERSION_1_3,
          .physical_device_api = VK_API_VERSION_1_3,
      },
      false, false, false);
  const bool vulkan13_members_gated = vulkan13.driver_properties && vulkan13.features_11 &&
                                      vulkan13.features_12 && vulkan13.features_13;
  passed &= check(vulkan13_members_gated,
                  "Vulkan 1.3 pNext availability did not expose the complete valid chain");

  const auto candidate = synthetic_candidate(0x24u);

  auto unsorted = candidate;
  unsorted.capabilities.extensions = {"VK_EXT_memory_budget", "VK_KHR_swapchain"};
  const auto unsorted_evaluation = evaluatePhysicalDevice(
      unsorted, PhysicalDeviceRequirements{.required_extensions = {"VK_KHR_swapchain"}});
  passed &= check(unsorted_evaluation.has_value() && unsorted_evaluation->matches,
                  "unsorted public extension snapshots were not evaluated safely");

  return passed;
}

[[nodiscard]] bool test_query_snapshot_ownership(const Instance &owner, const InstancePlan &plan) {
  const graphics_detail::PhysicalDeviceQueryInput input{
      .instance = owner,
      .native_instance = nullptr,
      .plan = plan,
  };
  const auto native_failure =
      graphics_detail::query_physical_devices_from_adapter(&synthetic_query_failure, input);
  bool passed = check(!native_failure, "synthetic physical-device query failure was swallowed");
  if (!native_failure) {
    const vk::SystemError expected{vk::Result::eErrorInitializationFailed};
    passed &= check(native_failure.error().code() == expected.code(),
                    "synthetic physical-device query failure lost its native error code");
    passed &= check(native_failure.error().context() == "query Vulkan physical devices",
                    "synthetic physical-device query failure lost its query context");
  }

  const auto empty_inventory =
      graphics_detail::query_physical_devices_from_adapter(&synthetic_empty_query, input);
  passed &= check(empty_inventory.has_value() && empty_inventory->empty(),
                  "successful empty physical-device inventory was converted into an error");
  if (empty_inventory) {
    const auto no_match = selectPhysicalDevice(empty_inventory->candidates, {});
    passed &= check(!no_match &&
                        no_match.error().code() == make_error_code(PhysicalDeviceError::no_match),
                    "empty inventory was not distinct from a no-match selection");
  }

  const auto temporary_inventory =
      graphics_detail::query_physical_devices_from_adapter(&synthetic_temporary_query, input);
  passed &= check(temporary_inventory.has_value() && temporary_inventory->size() == 1,
                  "synthetic capability query did not return its value-owned snapshot");
  if (temporary_inventory) {
    const auto &snapshot = temporary_inventory->front().capabilities;
    passed &= check(snapshot.driver_name == "temporary-driver-name" &&
                        snapshot.driver_info == "temporary-driver-info",
                    "capability strings did not survive query temporaries vanishing");
    passed &= check(snapshot.extensions == std::vector<std::string>{"VK_EXT_temporary"},
                    "capability extension names did not survive query temporaries vanishing");
    passed &= check(snapshot.extension_properties.size() == 1 &&
                        snapshot.extension_properties.front().name == "VK_EXT_temporary" &&
                        snapshot.extension_properties.front().spec_version == 4,
                    "capability extension metadata did not survive query temporaries vanishing");
    passed &= check(snapshot.features_10.samplerAnisotropy == VK_TRUE &&
                        snapshot.features_11.shaderDrawParameters == VK_TRUE &&
                        snapshot.features_12.timelineSemaphore == VK_TRUE &&
                        snapshot.features_13.dynamicRendering == VK_TRUE,
                    "native Vulkan feature snapshots did not survive query temporaries vanishing");
  }

  auto source = synthetic_candidate(0x34u);
  const auto copied = source;
  const auto copied_extension_names = copied.capabilities.extensions;
  source.capabilities.extensions.clear();
  source.capabilities.extension_properties.clear();
  source.capabilities.queue_families.clear();
  source.capabilities.memory_properties.memoryHeapCount = 0;
  passed &= check(copied.capabilities.extensions == copied_extension_names &&
                      !copied.capabilities.queue_families.empty() &&
                      copied.capabilities.memory_properties.memoryHeapCount == 1,
                  "capability snapshots shared mutable source/test input storage");
  return passed;
}

[[nodiscard]] bool
test_synthetic_selection_is_explicit_and_deterministic(const PhysicalDevice &prototype) {
  const auto first = synthetic_candidate(0x01u, true, true, 0, prototype);
  const auto second = synthetic_candidate(0x02u, true, true, 1, prototype);
  const std::array candidates{second, first};
  const PhysicalDeviceRequirements requirements{};

  bool passed = true;
  const auto ambiguous = selectPhysicalDevice(candidates, requirements);
  const bool ambiguity_reported =
      !ambiguous &&
      ambiguous.error().code() == make_error_code(PhysicalDeviceError::ambiguous_match);
  passed &= check(ambiguity_reported, "multiple candidates selected enumeration front");

  PhysicalDeviceSelectionPolicy uuid_order;
  uuid_order.uuid_order = {first.capabilities.device_uuid, second.capabilities.device_uuid};
  const auto ordered = selectPhysicalDevice(candidates, requirements, uuid_order);
  passed &= check(ordered.has_value(), "explicit UUID ordering failed to select a candidate");
  if (ordered) {
    passed &= check(ordered->candidate.capabilities.device_uuid == first.capabilities.device_uuid &&
                        ordered->reason == PhysicalDeviceSelectionReason::uuid_order &&
                        ordered->policy_index == std::optional<std::size_t>{0},
                    "explicit UUID ordering did not select the first requested UUID");
  }

  PhysicalDeviceSelectionPolicy manual;
  manual.explicit_uuid = second.capabilities.device_uuid;
  const auto manually_selected = selectPhysicalDevice(candidates, requirements, manual);
  passed &= check(manually_selected.has_value(), "manual UUID selection failed");
  if (manually_selected) {
    const bool manual_match =
        manually_selected->candidate.capabilities.device_uuid == second.capabilities.device_uuid &&
        manually_selected->reason == PhysicalDeviceSelectionReason::explicit_uuid &&
        !manually_selected->policy_index;
    passed &= check(manual_match, "manual UUID selection returned a different candidate");
  }

  const auto direct = selectPhysicalDevice(first, requirements);
  const bool direct_match =
      direct.has_value() && direct->reason == PhysicalDeviceSelectionReason::sole_match;
  passed &= check(direct_match, "single-candidate manual selection failed");

  PhysicalDeviceSelectionPolicy conflicting_policy;
  conflicting_policy.explicit_uuid = first.capabilities.device_uuid;
  conflicting_policy.uuid_order = {first.capabilities.device_uuid};
  const auto conflicting = selectPhysicalDevice(candidates, requirements, conflicting_policy);
  const bool conflicting_policy_rejected =
      !conflicting &&
      conflicting.error().code() == make_error_code(PhysicalDeviceError::invalid_requirements);
  passed &=
      check(conflicting_policy_rejected, "explicit UUID and UUID ordering policies were combined");

  PhysicalDeviceSelectionPolicy conflicting_requirement;
  conflicting_requirement.explicit_uuid = first.capabilities.device_uuid;
  const auto requirement_conflict = selectPhysicalDevice(
      candidates,
      PhysicalDeviceRequirements{.required_device_uuid = second.capabilities.device_uuid},
      conflicting_requirement);
  const bool conflicting_requirement_rejected =
      !requirement_conflict && requirement_conflict.error().code() ==
                                   make_error_code(PhysicalDeviceError::invalid_requirements);
  passed &= check(conflicting_requirement_rejected,
                  "explicit UUID selection conflicted with a required UUID");

  const PhysicalDeviceCandidate invalid_view{
      .device = PhysicalDevice{},
      .capabilities = first.capabilities,
      .enumeration_index = first.enumeration_index,
  };
  const auto invalid_selection = selectPhysicalDevice(invalid_view, requirements);
  const bool invalid_view_rejected =
      !invalid_selection &&
      invalid_selection.error().code() == make_error_code(PhysicalDeviceError::invalid_view);
  passed &= check(invalid_view_rejected, "selection accepted a default physical-device view");

  const auto missing = selectPhysicalDevice(
      candidates, PhysicalDeviceRequirements{.required_extensions = {"VK_EXT_missing"}});
  const bool no_match_reported =
      !missing && missing.error().code() == make_error_code(PhysicalDeviceError::no_match);
  passed &= check(no_match_reported, "missing extension did not produce no-match");
  return passed;
}

[[nodiscard]] bool test_unavailable_uuid_is_never_a_match(const PhysicalDevice &prototype) {
  const PhysicalDeviceUuid zero_uuid{};
  auto unavailable = synthetic_candidate(0x00u, true, false, 0, prototype);
  unavailable.capabilities.id_properties_available = false;
  unavailable.capabilities.device_uuid = zero_uuid;
  const auto available = synthetic_candidate(0x03u, true, false, 1, prototype);

  bool passed = true;
  const PhysicalDeviceRequirements uuid_requirement{.required_device_uuid = zero_uuid};
  const auto evaluation = evaluatePhysicalDevice(unavailable, uuid_requirement);
  passed &= check(evaluation.has_value() && !evaluation->matches,
                  "unavailable UUID properties satisfied a required zero UUID");
  if (evaluation) {
    passed &= check(!evaluation->candidate_identity.uuid &&
                        evaluation->candidate_identity.vendor_id ==
                            unavailable.capabilities.properties.vendorID &&
                        evaluation->candidate_identity.device_id ==
                            unavailable.capabilities.properties.deviceID &&
                        evaluation->candidate_identity.type ==
                            unavailable.capabilities.properties.deviceType &&
                        !evaluation->candidate_identity.device_name.empty(),
                    "unavailable UUID evaluation did not retain the owned non-UUID identity");
    bool clear_decline = evaluation->decisions.size() == 1;
    if (clear_decline) {
      const auto &decision = evaluation->decisions.front();
      const auto *evidence = std::get_if<PhysicalDeviceUuidEvidence>(&decision.evidence);
      clear_decline = decision.kind == PhysicalDeviceRequirementKind::uuid &&
                      decision.outcome == PhysicalDeviceRequirementOutcome::declined &&
                      decision.reason == PhysicalDeviceRequirementReason::unsupported &&
                      evidence != nullptr && !evidence->available_uuid;
    }
    passed &= check(clear_decline,
                    "unavailable UUID properties did not produce a clear declined decision");
  }

  const auto manual = selectPhysicalDevice(unavailable, uuid_requirement);
  const bool manual_no_match =
      !manual && manual.error().code() == make_error_code(PhysicalDeviceError::no_match);
  passed &= check(manual_no_match, "manual UUID selection accepted unavailable ID properties");

  const std::array candidates{unavailable, available};
  const auto no_match_code = make_error_code(PhysicalDeviceError::no_match);
  PhysicalDeviceSelectionPolicy explicit_zero;
  explicit_zero.explicit_uuid = zero_uuid;
  const auto explicit_selection =
      selectPhysicalDevice(candidates, PhysicalDeviceRequirements{}, explicit_zero);
  passed &= check(!explicit_selection, "explicit zero UUID policy selected unavailable UUID");
  if (!explicit_selection) {
    passed &= check(explicit_selection.error().code() == no_match_code,
                    "explicit zero UUID policy returned an unexpected error");
  }

  PhysicalDeviceSelectionPolicy zero_then_available;
  zero_then_available.uuid_order = {zero_uuid, available.capabilities.device_uuid};
  const auto ordered_selection =
      selectPhysicalDevice(candidates, PhysicalDeviceRequirements{}, zero_then_available);
  const bool ordered_available =
      ordered_selection.has_value() &&
      ordered_selection->candidate.capabilities.id_properties_available &&
      ordered_selection->candidate.capabilities.device_uuid == available.capabilities.device_uuid;
  passed &= check(ordered_available,
                  "UUID ordering did not skip unavailable zero UUID properties deterministically");

  PhysicalDeviceSelectionPolicy only_zero;
  only_zero.uuid_order = {zero_uuid};
  const auto zero_selection =
      selectPhysicalDevice(candidates, PhysicalDeviceRequirements{}, only_zero);
  passed &= check(!zero_selection, "UUID ordering selected an unavailable zero UUID candidate");
  if (!zero_selection) {
    passed &= check(zero_selection.error().code() == no_match_code,
                    "UUID ordering returned an unexpected error");
  }
  return passed;
}

[[nodiscard]] bool test_headless_query_and_instance_move_correlation() {
  const auto capabilities_result = queryInstanceCapabilities();
  const bool capabilities_available = capabilities_result.has_value();
  if (!check(capabilities_available, "physical-device runtime could not query Vulkan loader")) {
    return false;
  }

  InstanceDescription description;
  description.api_version = capabilities_result->loader_api_version;
  description.application_name = "terreate-physical-device-test";
  const auto plan = resolveInstance(description, *capabilities_result);
  if (!check(plan.has_value(), "physical-device runtime could not resolve Instance")) {
    return false;
  }
  auto instance_result = createInstance(*plan);
  if (!check(instance_result.has_value(), "physical-device runtime could not create Instance")) {
    return false;
  }

  Instance owner = std::move(*instance_result);
  bool passed = test_query_snapshot_ownership(owner, *plan);
  const auto inventory_result = queryPhysicalDevices(owner);
  if (!check(inventory_result.has_value(), "physical-device runtime query failed")) {
    return false;
  }
  const bool has_devices = !inventory_result->empty();
  passed &= check(has_devices, "headless Vulkan runtime exposed no physical devices");
  if (inventory_result->empty()) {
    return passed;
  }

  const auto &candidate = inventory_result->candidates.front();
  passed &= test_synthetic_selection_is_explicit_and_deterministic(candidate.device);
  passed &= test_unavailable_uuid_is_never_a_match(candidate.device);
  // nativeHandle() is borrowed: ownership never transfers, and callers must
  // never destroy the physical device through it.  Keep this observation only
  // while its parent Instance is in its original state.
  const auto native_before_move = candidate.device.nativeHandle();
  passed &= check(candidate.device.correlatedWith(owner),
                  "PhysicalDevice view was not correlated with its parent Instance");
  passed &= check(native_before_move != vk::PhysicalDevice{},
                  "PhysicalDevice view did not retain its native handle");

  const auto &snapshot = candidate.capabilities;
  passed &= check(candidate.device.valid(),
                  "headless PhysicalDevice view did not contain a valid borrowed handle");
  passed &= check(snapshot.properties2_available && snapshot.features2_available &&
                      snapshot.memory_properties2_available,
                  "headless query did not observe Vulkan 1.1+ capability chains");
  passed &= check(snapshot.properties.apiVersion != 0 && snapshot.properties.deviceName[0] != '\0',
                  "headless query did not observe physical-device properties");
  PhysicalDeviceUuid observed_device_uuid{};
  std::copy(snapshot.id_properties.deviceUUID.begin(), snapshot.id_properties.deviceUUID.end(),
            observed_device_uuid.begin());
  PhysicalDeviceUuid observed_driver_uuid{};
  std::copy(snapshot.id_properties.driverUUID.begin(), snapshot.id_properties.driverUUID.end(),
            observed_driver_uuid.begin());
  passed &= check(snapshot.device_uuid == observed_device_uuid &&
                      snapshot.driver_uuid == observed_driver_uuid,
                  "headless query did not preserve physical-device UUID observations");
  const bool properties_are_owned = snapshot.properties2.pNext == nullptr &&
                                    snapshot.id_properties.pNext == nullptr &&
                                    snapshot.driver_properties.pNext == nullptr;
  passed &= check(properties_are_owned, "properties snapshots retained borrowed pNext links");
  passed &= check(snapshot.properties2.properties.apiVersion == snapshot.properties.apiVersion,
                  "properties2 snapshot did not retain the base properties observation");
  const bool features_are_owned =
      snapshot.features2.pNext == nullptr && snapshot.features_11.pNext == nullptr &&
      snapshot.features_12.pNext == nullptr && snapshot.features_13.pNext == nullptr;
  passed &= check(features_are_owned, "feature snapshots retained borrowed pNext links");
  const bool feature_snapshot_matches =
      snapshot.features2.features.samplerAnisotropy == snapshot.features_10.samplerAnisotropy;
  passed &= check(feature_snapshot_matches,
                  "feature snapshots did not retain the Vulkan 1.0 feature observation");
  passed &= check(std::is_sorted(snapshot.extensions.begin(), snapshot.extensions.end()),
                  "device extension names were not canonicalised");
  passed &= check(std::adjacent_find(snapshot.extensions.begin(), snapshot.extensions.end()) ==
                      snapshot.extensions.end(),
                  "device extension names contained duplicates");
  bool extension_metadata_matches =
      snapshot.extension_properties.size() == snapshot.extensions.size();
  for (std::size_t index = 0; index < snapshot.extensions.size() && extension_metadata_matches;
       ++index) {
    extension_metadata_matches =
        snapshot.extension_properties[index].name == snapshot.extensions[index];
  }
  passed &= check(extension_metadata_matches,
                  "device extension metadata did not match canonical extension names");
  const bool queues_are_observed =
      !snapshot.queue_families.empty() &&
      std::all_of(snapshot.queue_families.begin(), snapshot.queue_families.end(),
                  [](const auto &family) { return family.queueCount != 0; });
  passed &= check(queues_are_observed, "headless query did not observe queue families");
  const bool memory_is_observed = snapshot.memory_properties.memoryHeapCount != 0 &&
                                  snapshot.memory_properties.memoryHeaps[0].size != 0;
  passed &= check(memory_is_observed, "headless query did not observe device memory properties");
  if (snapshot.memory_budget) {
    passed &= check(snapshot.memory_budget->pNext == nullptr,
                    "memory-budget snapshot retained a borrowed pNext link");
  }

  const auto empty_evaluation =
      evaluatePhysicalDevice(inventory_result->candidates.front(), PhysicalDeviceRequirements{});
  passed &= check(empty_evaluation.has_value() && empty_evaluation->matches,
                  "empty explicit requirements did not match the runtime candidate");

  // Selection above consumes the borrowed view while its parent is current.
  // A borrowed native handle is not retained across this move; reacquire one
  // from a fresh query if native observation is needed afterward.
  PhysicalDeviceSelectionPolicy explicit_order;
  explicit_order.uuid_order.reserve(inventory_result->size());
  for (const auto &runtime_candidate : *inventory_result) {
    explicit_order.uuid_order.push_back(runtime_candidate.capabilities.device_uuid);
  }
  const auto selected = selectPhysicalDevice(inventory_result->candidates,
                                             PhysicalDeviceRequirements{}, explicit_order);
  passed &= check(selected.has_value(),
                  "explicit UUID ordering could not select the runtime physical device");

  Instance moved = std::move(owner);
  // A moved-from Instance is intentionally inspected to verify its documented empty state.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  passed &= check(!owner.valid(), "Instance move did not empty the source owner");
  passed &= check(candidate.device.correlatedWith(moved),
                  "PhysicalDevice view lost correlation after Instance move");
  if (passed) {
    emit_runtime_marker("graphics.physical-device: headless-query=PASS");
  }
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= test_synthetic_evaluation_and_optional_decisions();
  passed &= test_extension_canonicalization_and_explicit_intent();
  passed &= test_query_gating_and_snapshot_boundaries();
  passed &= test_headless_query_and_instance_move_correlation();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

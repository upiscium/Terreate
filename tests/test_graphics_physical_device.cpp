#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

#include "physical_device_query.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
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
static_assert(std::is_trivially_copyable_v<PhysicalDevice>);
static_assert(sizeof(PhysicalDevice) <= 2 * sizeof(void *));
static_assert(std::is_nothrow_move_constructible_v<PhysicalDevice>);
static_assert(std::is_nothrow_move_assignable_v<PhysicalDevice>);
static_assert(!std::is_constructible_v<PhysicalDevice, vk::PhysicalDevice, const void *>);
static_assert(!std::is_convertible_v<PhysicalDevice, vk::PhysicalDevice>);
static_assert(std::copy_constructible<PhysicalDeviceCapabilities>);
static_assert(std::copy_constructible<PhysicalDeviceCandidate>);
static_assert(std::is_move_constructible_v<PhysicalDeviceCandidate>);
static_assert(!std::is_copy_assignable_v<PhysicalDeviceCandidate>);
static_assert(!std::is_move_assignable_v<PhysicalDeviceCandidate>);
static_assert(!std::is_default_constructible_v<PhysicalDeviceCandidate>);
static_assert(!std::is_constructible_v<PhysicalDeviceCandidate, PhysicalDevice,
                                       PhysicalDeviceCapabilities, std::size_t>);
static_assert(!std::is_constructible_v<PhysicalDeviceCandidate, vk::PhysicalDevice,
                                       PhysicalDeviceCapabilities, std::size_t>);
static_assert(std::is_same_v<decltype(std::declval<PhysicalDeviceCandidate &>().device()),
                             const PhysicalDevice &>);
static_assert(std::is_same_v<decltype(std::declval<PhysicalDeviceCandidate &>().capabilities()),
                             const PhysicalDeviceCapabilities &>);
static_assert(!std::is_assignable_v<decltype(std::declval<PhysicalDeviceCandidate &>().device()),
                                    PhysicalDevice>);
static_assert(
    !std::is_assignable_v<decltype(std::declval<PhysicalDeviceCandidate &>().capabilities()),
                          PhysicalDeviceCapabilities>);
static_assert(std::is_copy_constructible_v<PhysicalDeviceInventory>);
static_assert(std::is_move_constructible_v<PhysicalDeviceInventory>);
static_assert(std::is_default_constructible_v<PhysicalDeviceInventory>);
static_assert(
    !std::is_constructible_v<PhysicalDeviceInventory, std::vector<PhysicalDeviceCandidate>>);
static_assert(!std::is_copy_assignable_v<PhysicalDeviceInventory>);
static_assert(!std::is_move_assignable_v<PhysicalDeviceInventory>);
static_assert(std::is_same_v<decltype(std::declval<PhysicalDeviceInventory &>()[0]),
                             const PhysicalDeviceCandidate &>);
static_assert(std::is_copy_constructible_v<PhysicalDeviceSelection>);
static_assert(std::is_move_constructible_v<PhysicalDeviceSelection>);
static_assert(!std::is_copy_assignable_v<PhysicalDeviceSelection>);
static_assert(!std::is_move_assignable_v<PhysicalDeviceSelection>);

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

[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
synthetic_query_failure(const graphics_detail::PhysicalDeviceQueryInput &) {
  throw vk::SystemError{vk::Result::eErrorInitializationFailed};
}

[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
synthetic_empty_query(const graphics_detail::PhysicalDeviceQueryInput &) {
  return PhysicalDeviceInventory{};
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
  const auto candidate = synthetic_capabilities(0x21u, true, false);
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
  const bool identity_matches =
      identity.uuid == std::optional<PhysicalDeviceUuid>{candidate.device_uuid} &&
      identity.vendor_id == candidate.properties.vendorID &&
      identity.device_id == candidate.properties.deviceID &&
      identity.type == candidate.properties.deviceType && identity.device_name == expected_name;
  passed &= check(identity_matches,
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
  const bool no_intent_matches = candidate.features_10.samplerAnisotropy == VK_TRUE &&
                                 no_intent.has_value() && no_intent->matches &&
                                 no_intent->decisions.empty();
  passed &= check(no_intent_matches, "unrequested observations implied enable intent");

  PhysicalDeviceRequirements typed_requirements;
  typed_requirements.minimum_api_version = VK_API_VERSION_1_2;
  typed_requirements.required_device_uuid = candidate.device_uuid;
  typed_requirements.required_device_type = candidate.properties.deviceType;
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
      const auto required_api_version = api_evidence->required_api_version;
      const auto available_api_version = api_evidence->available_api_version;
      const bool api_values_match = required_api_version == VK_API_VERSION_1_2 &&
                                    available_api_version == candidate.api_version;
      passed &= check(api_values_match, "API evidence lost requested or observed values");
    }
    passed &= check(uuid_evidence != nullptr, "missing typed UUID evidence");
    if (uuid_evidence != nullptr) {
      const auto required_uuid = uuid_evidence->required_uuid;
      const auto available_uuid = uuid_evidence->available_uuid;
      const bool uuid_values_match =
          required_uuid == candidate.device_uuid &&
          available_uuid == std::optional<PhysicalDeviceUuid>{candidate.device_uuid};
      passed &= check(uuid_values_match, "UUID evidence lost requested or observed values");
    }
    passed &= check(type_evidence != nullptr, "missing typed device-type evidence");
    if (type_evidence != nullptr) {
      passed &= check(type_evidence->required_type == candidate.properties.deviceType,
                      "device-type evidence lost the requested value");
      passed &= check(type_evidence->available_type == candidate.properties.deviceType,
                      "device-type evidence lost the observed value");
    }
    passed &= check(extension_evidence != nullptr, "missing typed extension evidence");
    if (extension_evidence != nullptr) {
      passed &= check(extension_evidence->extension == "VK_KHR_swapchain" &&
                          extension_evidence->available,
                      "extension evidence lost requested or observed values");
    }
  }

  const PhysicalDeviceRequirements insufficient_queue_requirements{
      .required_queue_families = {PhysicalDeviceQueueRequirement{
          .flags = vk::QueueFlagBits::eGraphics,
          .min_queue_count = 2,
      }},
  };
  const auto insufficient_queue =
      evaluatePhysicalDevice(candidate, insufficient_queue_requirements);
  passed &= check(insufficient_queue.has_value() && !insufficient_queue->matches,
                  "one graphics queue incorrectly satisfied a two-queue requirement");
  if (insufficient_queue && insufficient_queue->decisions.size() == 1) {
    const auto &decision = insufficient_queue->decisions.front();
    const auto *evidence = std::get_if<PhysicalDeviceQueueEvidence>(&decision.evidence);
    passed &= check(decision.outcome == PhysicalDeviceRequirementOutcome::declined &&
                        decision.reason == PhysicalDeviceRequirementReason::unsupported &&
                        evidence != nullptr && evidence->requirement.min_queue_count == 2 &&
                        evidence->matching_queue_indices.empty(),
                    "insufficient graphics queue count did not retain declined evidence");
  }

  const PhysicalDeviceQueueRequirement zero_queue_requirement{
      .flags = vk::QueueFlagBits::eGraphics,
      .min_queue_count = 0,
  };
  const PhysicalDeviceRequirements zero_queue_requirements{
      .required_queue_families = {zero_queue_requirement},
  };
  const auto zero_queue_count = evaluatePhysicalDevice(candidate, zero_queue_requirements);
  const bool zero_queue_rejected =
      !zero_queue_count &&
      zero_queue_count.error().code() == make_error_code(PhysicalDeviceError::invalid_requirements);
  passed &= check(zero_queue_rejected, "zero minimum queue count was not rejected");

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

  const auto candidate = synthetic_capabilities(0x23u, true, false);
  passed &= check(std::is_sorted(candidate.extensions.begin(), candidate.extensions.end()),
                  "synthetic physical-device extension names were not canonicalised");
  passed &= check(std::adjacent_find(candidate.extensions.begin(), candidate.extensions.end()) ==
                      candidate.extensions.end(),
                  "synthetic physical-device extension names contained duplicates");

  const auto no_request = evaluatePhysicalDevice(candidate, PhysicalDeviceRequirements{});
  passed &= check(no_request.has_value() && no_request->matches && no_request->decisions.empty(),
                  "supported-but-unrequested extension acquired enable intent");
  return passed;
}

[[nodiscard]] bool test_query_gating_and_snapshot_boundaries() {
  bool passed = true;
  const auto route_call_counts = [](graphics_detail::PhysicalDeviceQueryRoute route) {
    std::array<std::size_t, 2> calls{};
    graphics_detail::dispatch_physical_device_query(
        route, [&] { ++calls[0]; }, [&] { ++calls[1]; });
    return calls;
  };

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
  const bool lower_route_is_unavailable =
      lower_instance_and_device.properties2_route ==
          graphics_detail::PhysicalDeviceQueryRoute::unavailable &&
      route_call_counts(lower_instance_and_device.properties2_route) ==
          std::array<std::size_t, 2>{0, 0};
  passed &= check(lower_route_is_unavailable,
                  "unavailable Properties2 route made a native query callback");

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
  const auto extension_properties_route = route_call_counts(extension_backed.properties2_route);
  const auto extension_features_route = route_call_counts(extension_backed.properties2_route);
  const auto extension_memory_route = route_call_counts(extension_backed.properties2_route);
  const bool extension_route_is_khr =
      extension_backed.properties2_route == graphics_detail::PhysicalDeviceQueryRoute::khr &&
      extension_properties_route == std::array<std::size_t, 2>{0, 1} &&
      extension_features_route == std::array<std::size_t, 2>{0, 1} &&
      extension_memory_route == std::array<std::size_t, 2>{0, 1};
  passed &= check(extension_route_is_khr,
                  "Vulkan 1.0 extension-backed properties/features/memory queries did not select "
                  "KHR");

  const auto vulkan10_external_memory_without_properties2 =
      graphics_detail::physical_device_query_availability(
          graphics_detail::PhysicalDeviceQueryApiVersions{
              .effective_instance_api = VK_API_VERSION_1_0,
              .physical_device_api = VK_API_VERSION_1_0,
          },
          false, true, false);
  passed &= check(!vulkan10_external_memory_without_properties2.properties2 &&
                      !vulkan10_external_memory_without_properties2.id_properties,
                  "Vulkan 1.0 external-memory ID properties bypassed the Properties2 gate");

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
  const bool vulkan11_route_is_core =
      vulkan11.properties2_route == graphics_detail::PhysicalDeviceQueryRoute::core &&
      route_call_counts(vulkan11.properties2_route) == std::array<std::size_t, 2>{1, 0};
  passed &= check(vulkan11_route_is_core, "Vulkan 1.1 Properties2 queries did not select core");

  const auto vulkan11_instance_vulkan10_device_with_external_ids =
      graphics_detail::physical_device_query_availability(
          graphics_detail::PhysicalDeviceQueryApiVersions{
              .effective_instance_api = VK_API_VERSION_1_1,
              .physical_device_api = VK_API_VERSION_1_0,
          },
          false, true, false);
  passed &= check(vulkan11_instance_vulkan10_device_with_external_ids.properties2 &&
                      vulkan11_instance_vulkan10_device_with_external_ids.id_properties &&
                      !vulkan11_instance_vulkan10_device_with_external_ids.driver_properties &&
                      vulkan11_instance_vulkan10_device_with_external_ids.properties2_route ==
                          graphics_detail::PhysicalDeviceQueryRoute::core,
                  "external-memory ID properties were not available for a Vulkan 1.1 Instance "
                  "with a Vulkan 1.0 device");

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

  auto unsorted = synthetic_capabilities(0x24u, true, false);
  unsorted.extensions = {"VK_EXT_memory_budget", "VK_KHR_swapchain"};
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
    const auto no_match = selectPhysicalDevice(empty_inventory->candidates(), {});
    passed &= check(!no_match &&
                        no_match.error().code() == make_error_code(PhysicalDeviceError::no_match),
                    "empty inventory was not distinct from a no-match selection");
  }

  auto source = synthetic_capabilities(0x34u, true, true);
  source.driver_name = "temporary-driver-name";
  source.driver_info = "temporary-driver-info";
  const auto copied = source;
  const auto copied_extension_names = copied.extensions;
  source.extensions.clear();
  source.extension_properties.clear();
  source.queue_families.clear();
  source.memory_properties.memoryHeapCount = 0;
  const bool copied_strings = copied.driver_name == "temporary-driver-name" &&
                              copied.driver_info == "temporary-driver-info";
  const bool copied_extensions = copied.extensions == copied_extension_names;
  const bool copied_storage =
      !copied.queue_families.empty() && copied.memory_properties.memoryHeapCount == 1;
  const bool copied_snapshot_is_independent = copied_strings && copied_extensions && copied_storage;
  passed &= check(copied_snapshot_is_independent,
                  "capability snapshots shared mutable source/test input storage");

  return passed;
}

[[nodiscard]] bool test_synthetic_evaluation_is_explicit_and_deterministic() {
  const auto first = synthetic_capabilities(0x01u, true, false);
  const auto second = synthetic_capabilities(0x02u, true, false);
  const PhysicalDeviceRequirements requirements{};

  bool passed = true;
  const auto first_evaluation = evaluatePhysicalDevice(first, requirements);
  const auto repeated_capability_evaluation = evaluatePhysicalDevice(first, requirements);
  passed &= check(first_evaluation.has_value() && repeated_capability_evaluation.has_value() &&
                      first_evaluation->matches && repeated_capability_evaluation->matches,
                  "synthetic capability evaluation did not accept explicit empty requirements");
  if (first_evaluation && repeated_capability_evaluation) {
    passed &= check(same_evaluation(*first_evaluation, *repeated_capability_evaluation),
                    "repeated capability evaluation was not deterministic");
  }

  const auto second_evaluation = evaluatePhysicalDevice(second, requirements);
  passed &= check(first_evaluation && second_evaluation.has_value() && second_evaluation->matches &&
                      second_evaluation->candidate_identity.uuid !=
                          first_evaluation->candidate_identity.uuid,
                  "synthetic evaluation did not remain a pure value operation");

  const auto repeated = evaluatePhysicalDevice(first, requirements);
  if (first_evaluation && repeated) {
    passed &= check(same_evaluation(*first_evaluation, *repeated),
                    "repeated synthetic evaluation was not deterministic");
  }
  return passed;
}

[[nodiscard]] bool test_unavailable_uuid_is_never_a_match() {
  const PhysicalDeviceUuid zero_uuid{};
  auto unavailable = synthetic_capabilities(0x00u, true, false);
  unavailable.id_properties_available = false;
  unavailable.device_uuid = zero_uuid;
  const auto available = synthetic_capabilities(0x03u, true, false);

  bool passed = true;
  const PhysicalDeviceRequirements uuid_requirement{.required_device_uuid = zero_uuid};
  const auto evaluation = evaluatePhysicalDevice(unavailable, uuid_requirement);
  passed &= check(evaluation.has_value() && !evaluation->matches,
                  "unavailable UUID properties satisfied a required zero UUID");
  if (evaluation) {
    const auto &identity = evaluation->candidate_identity;
    const bool identity_matches =
        !identity.uuid && identity.vendor_id == unavailable.properties.vendorID &&
        identity.device_id == unavailable.properties.deviceID &&
        identity.type == unavailable.properties.deviceType && !identity.device_name.empty();
    passed &= check(identity_matches,
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
  const PhysicalDeviceRequirements available_uuid_requirement{
      .required_device_uuid = available.device_uuid,
  };
  const auto available_evaluation = evaluatePhysicalDevice(available, available_uuid_requirement);
  passed &= check(available_evaluation.has_value() && available_evaluation->matches,
                  "available UUID properties did not satisfy a pure evaluation");
  return passed;
}

[[nodiscard]] bool test_native_vulkan10_khr_route(const InstanceCapabilities &loader_capabilities) {
  const std::string properties2_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
  if (!std::binary_search(loader_capabilities.available_extensions.begin(),
                          loader_capabilities.available_extensions.end(), properties2_extension)) {
    emit_runtime_marker("graphics.physical-device: khr-route=NOT-AVAILABLE");
    return true;
  }

  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_0;
  description.required_extensions = {properties2_extension};
  description.application_name = "terreate-physical-device-khr-route-test";
  const auto plan = resolveInstance(description, loader_capabilities);
  if (!check(plan.has_value(), "Vulkan 1.0 KHR route could not resolve Instance")) {
    return false;
  }
  auto instance_result = createInstance(*plan);
  if (!check(instance_result.has_value(), "Vulkan 1.0 KHR route could not create Instance")) {
    return false;
  }

  Instance owner = std::move(*instance_result);
  const auto inventory_result = queryPhysicalDevices(owner);
  if (!check(inventory_result.has_value(), "Vulkan 1.0 KHR route query failed")) {
    return false;
  }
  if (!check(!inventory_result->empty(), "Vulkan 1.0 KHR route exposed no physical devices")) {
    return false;
  }

  const auto &snapshot = inventory_result->front().capabilities();
  const auto route = graphics_detail::physical_device_query_route(
      graphics_detail::PhysicalDeviceQueryApiVersions{
          .effective_instance_api = plan->effectiveApiVersion(),
          .physical_device_api = snapshot.api_version,
      },
      true);
  bool passed = true;
  passed &= check(route == graphics_detail::PhysicalDeviceQueryRoute::khr,
                  "Vulkan 1.0 Instance did not select the KHR Properties2 route natively");
  passed &= check(snapshot.properties2_available && snapshot.features2_available &&
                      snapshot.memory_properties2_available,
                  "native KHR route did not execute properties/features/memory queries");
  passed &= check(snapshot.properties2.pNext == nullptr && snapshot.features2.pNext == nullptr,
                  "native KHR route retained borrowed pNext links");
  const bool memory_budget_supported =
      std::find(snapshot.extensions.begin(), snapshot.extensions.end(),
                VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) != snapshot.extensions.end();
  if (memory_budget_supported) {
    passed &= check(snapshot.memory_budget.has_value(),
                    "native KHR memory-properties2 query did not observe memory budget");
  }
  if (passed) {
    const char *route_marker = memory_budget_supported
                                   ? "graphics.physical-device: khr-route=PASS"
                                   : "graphics.physical-device: khr-route=PASS memory-budget="
                                     "NOT-AVAILABLE";
    emit_runtime_marker(route_marker);
  }
  return passed;
}

[[nodiscard]] bool test_headless_query_and_instance_move_correlation() {
  const auto capabilities_result = queryInstanceCapabilities();
  const bool capabilities_available = capabilities_result.has_value();
  if (!check(capabilities_available, "physical-device runtime could not query Vulkan loader")) {
    return false;
  }

  bool passed = test_native_vulkan10_khr_route(*capabilities_result);

  InstanceDescription description;
  description.api_version = capabilities_result->loader_api_version;
  const std::string properties2_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
  if (capabilities_result->loader_api_version < VK_API_VERSION_1_1 &&
      std::binary_search(capabilities_result->available_extensions.begin(),
                         capabilities_result->available_extensions.end(), properties2_extension)) {
    description.required_extensions = {properties2_extension};
  }
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
  passed &= test_query_snapshot_ownership(owner, *plan);
  const auto inventory_result = queryPhysicalDevices(owner);
  if (!check(inventory_result.has_value(), "physical-device runtime query failed")) {
    return false;
  }
  const bool has_devices = !inventory_result->empty();
  passed &= check(has_devices, "headless Vulkan runtime exposed no physical devices");
  if (inventory_result->empty()) {
    return passed;
  }

  const auto &candidate = inventory_result->front();
  passed &= test_synthetic_evaluation_is_explicit_and_deterministic();
  passed &= test_unavailable_uuid_is_never_a_match();
  const auto copied_candidate = candidate;
  passed &= check(copied_candidate.device().nativeHandle() == candidate.device().nativeHandle() &&
                      copied_candidate.capabilities().properties.deviceID ==
                          candidate.capabilities().properties.deviceID &&
                      copied_candidate.enumerationIndex() == candidate.enumerationIndex(),
                  "PhysicalDeviceCandidate copy did not preserve its immutable value state");
  // nativeHandle() is a borrowed observation: ownership never transfers, and
  // callers must never destroy the physical device through it.  Use it only
  // while the parent Instance is live.
  const auto native_before_move = candidate.device().nativeHandle();
  passed &= check(candidate.device().correlatedWith(owner),
                  "PhysicalDevice view was not correlated with its parent Instance");
  passed &= check(native_before_move != vk::PhysicalDevice{},
                  "PhysicalDevice view did not retain its native handle");

  const auto &snapshot = candidate.capabilities();
  passed &= check(candidate.device().valid(),
                  "headless PhysicalDevice view did not contain a valid borrowed handle");
  const bool properties2_extension_enabled = std::binary_search(
      plan->enabledExtensions().begin(), plan->enabledExtensions().end(), properties2_extension);
  const bool external_memory_extension_enabled =
      std::binary_search(plan->enabledExtensions().begin(), plan->enabledExtensions().end(),
                         std::string{VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME});
  const bool driver_properties_extension_supported =
      std::find(snapshot.extensions.begin(), snapshot.extensions.end(),
                VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME) != snapshot.extensions.end();
  const auto expected_availability = graphics_detail::physical_device_query_availability(
      graphics_detail::PhysicalDeviceQueryApiVersions{
          .effective_instance_api = plan->effectiveApiVersion(),
          .physical_device_api = snapshot.api_version,
      },
      properties2_extension_enabled, external_memory_extension_enabled,
      driver_properties_extension_supported);
  const bool availability_matches =
      snapshot.properties2_available == expected_availability.properties2 &&
      snapshot.features2_available == expected_availability.properties2 &&
      snapshot.memory_properties2_available == expected_availability.memory_properties2;
  passed &= check(availability_matches,
                  "headless query did not retain the selected Properties2 route availability");
  passed &= check(snapshot.properties.apiVersion != 0 && snapshot.properties.deviceName[0] != '\0',
                  "headless query did not observe physical-device properties");
  if (snapshot.id_properties_available) {
    PhysicalDeviceUuid observed_device_uuid{};
    std::copy(snapshot.id_properties.deviceUUID.begin(), snapshot.id_properties.deviceUUID.end(),
              observed_device_uuid.begin());
    PhysicalDeviceUuid observed_driver_uuid{};
    std::copy(snapshot.id_properties.driverUUID.begin(), snapshot.id_properties.driverUUID.end(),
              observed_driver_uuid.begin());
    passed &= check(snapshot.device_uuid == observed_device_uuid &&
                        snapshot.driver_uuid == observed_driver_uuid,
                    "headless query did not preserve physical-device UUID observations");
  }
  if (snapshot.properties2_available) {
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
  }
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
  const auto graphics_queue = std::find_if(
      snapshot.queue_families.begin(), snapshot.queue_families.end(), [](const auto &family) {
        return (static_cast<VkQueueFlags>(family.queueFlags) &
                static_cast<VkQueueFlags>(vk::QueueFlagBits::eGraphics)) != 0;
      });
  if (graphics_queue != snapshot.queue_families.end() &&
      graphics_queue->queueCount < std::numeric_limits<std::uint32_t>::max()) {
    const PhysicalDeviceRequirements insufficient_graphics_queue{
        .required_queue_families = {PhysicalDeviceQueueRequirement{
            .flags = vk::QueueFlagBits::eGraphics,
            .min_queue_count = graphics_queue->queueCount + 1,
        }},
    };
    const auto queue_evaluation = evaluatePhysicalDevice(candidate, insufficient_graphics_queue);
    passed &= check(queue_evaluation.has_value() && !queue_evaluation->matches,
                    "an insufficient graphics queue count was evaluated as a match");
    const auto queue_selection = selectPhysicalDevice(candidate, insufficient_graphics_queue);
    const bool queue_no_match =
        !queue_selection &&
        queue_selection.error().code() == make_error_code(PhysicalDeviceError::no_match);
    passed &= check(queue_no_match, "insufficient graphics queue count did not produce no-match");
  }
  const bool memory_is_observed = snapshot.memory_properties.memoryHeapCount != 0 &&
                                  snapshot.memory_properties.memoryHeaps[0].size != 0;
  passed &= check(memory_is_observed, "headless query did not observe device memory properties");
  if (snapshot.memory_budget) {
    passed &= check(snapshot.memory_budget->pNext == nullptr,
                    "memory-budget snapshot retained a borrowed pNext link");
  }

  const auto empty_evaluation = evaluatePhysicalDevice(candidate, PhysicalDeviceRequirements{});
  passed &= check(empty_evaluation.has_value() && empty_evaluation->matches,
                  "empty explicit requirements did not match the runtime candidate");

  // Selection above observes the borrowed view while its parent Instance is
  // live.  Reacquire a native handle from the current owner after an Instance
  // move if native observation is needed again.
  PhysicalDeviceSelectionPolicy explicit_order;
  explicit_order.uuid_order.reserve(inventory_result->size());
  for (const auto &runtime_candidate : *inventory_result) {
    if (runtime_candidate.capabilities().id_properties_available) {
      explicit_order.uuid_order.push_back(runtime_candidate.capabilities().device_uuid);
    }
  }
  const auto selected = selectPhysicalDevice(inventory_result->candidates(),
                                             PhysicalDeviceRequirements{}, explicit_order);
  if (!explicit_order.uuid_order.empty()) {
    passed &= check(selected.has_value(),
                    "explicit UUID ordering could not select the runtime physical device");
  }

  Instance moved = std::move(owner);
  // A moved-from Instance is intentionally inspected to verify its documented empty state.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  passed &= check(!owner.valid(), "Instance move did not empty the source owner");
  passed &= check(candidate.device().correlatedWith(moved),
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

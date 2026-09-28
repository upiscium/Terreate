#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

#include "physical_device_query.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace terreate::graphics;
namespace graphics_detail = terreate::graphics::detail;

static_assert(std::copy_constructible<PhysicalDevice>);
static_assert(std::is_nothrow_copy_constructible_v<PhysicalDevice>);
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

[[nodiscard]] const void *synthetic_identity(std::uint8_t first) noexcept {
  static const std::array<std::uint8_t, 256> identities{};
  return &identities[first];
}

[[nodiscard]] PhysicalDeviceCandidate synthetic_candidate(std::uint8_t uuid_first,
                                                          bool anisotropy = true,
                                                          bool shader_int64 = false,
                                                          std::size_t index = 0) {
  return PhysicalDeviceCandidate{
      .device = PhysicalDevice{vk::PhysicalDevice{}, synthetic_identity(uuid_first)},
      .capabilities = synthetic_capabilities(uuid_first, anisotropy, shader_int64),
      .enumeration_index = index,
  };
}

[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
synthetic_query_failure(const Instance &, const vk::raii::Instance *, const InstancePlan &) {
  throw vk::SystemError{vk::Result::eErrorInitializationFailed};
}

[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
synthetic_empty_query(const Instance &, const vk::raii::Instance *, const InstancePlan &) {
  return PhysicalDeviceInventory{};
}

[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
synthetic_temporary_query(const Instance &, const vk::raii::Instance *, const InstancePlan &) {
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
      .device = PhysicalDevice{vk::PhysicalDevice{}, synthetic_identity(0x33u)},
      .capabilities = std::move(temporary),
      .enumeration_index = 7,
  });
  return inventory;
}

[[nodiscard]] bool same_evaluation(const PhysicalDeviceEvaluation &left,
                                   const PhysicalDeviceEvaluation &right) {
  if (left.matches != right.matches || left.decisions.size() != right.decisions.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.decisions.size(); ++index) {
    const auto &left_decision = left.decisions[index];
    const auto &right_decision = right.decisions[index];
    if (left_decision.name != right_decision.name ||
        left_decision.strength != right_decision.strength ||
        left_decision.outcome != right_decision.outcome ||
        left_decision.reason != right_decision.reason) {
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

  const auto optional_queue_accepted =
      std::find_if(result->decisions.begin(), result->decisions.end(), [](const auto &decision) {
        return decision.name ==
                   "queue_flags=" +
                       std::to_string(static_cast<VkQueueFlags>(vk::QueueFlagBits::eCompute)) &&
               decision.strength == RequirementStrength::optional &&
               decision.outcome == PhysicalDeviceRequirementOutcome::accepted &&
               decision.reason == PhysicalDeviceRequirementReason::supported;
      });
  passed &= check(optional_queue_accepted != result->decisions.end(),
                  "supported optional queue capability was not accepted");

  const auto optional_queue_declined =
      std::find_if(result->decisions.begin(), result->decisions.end(), [](const auto &decision) {
        return decision.name ==
                   "queue_flags=" +
                       std::to_string(static_cast<VkQueueFlags>(vk::QueueFlagBits::eTransfer)) &&
               decision.strength == RequirementStrength::optional &&
               decision.outcome == PhysicalDeviceRequirementOutcome::declined &&
               decision.reason == PhysicalDeviceRequirementReason::unsupported;
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
  passed &= check(candidate.capabilities.features10().samplerAnisotropy == VK_TRUE &&
                      no_intent.has_value() && no_intent->matches && no_intent->decisions.empty(),
                  "supported but unrequested observations implied enable intent");

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
  const auto native_failure = graphics_detail::query_physical_devices_from_adapter(
      &synthetic_query_failure, owner, nullptr, plan);
  bool passed = check(!native_failure, "synthetic physical-device query failure was swallowed");
  if (!native_failure) {
    const vk::SystemError expected{vk::Result::eErrorInitializationFailed};
    passed &= check(native_failure.error().code() == expected.code(),
                    "synthetic physical-device query failure lost its native error code");
    passed &= check(native_failure.error().context() == "query Vulkan physical devices",
                    "synthetic physical-device query failure lost its query context");
  }

  const auto empty_inventory = graphics_detail::query_physical_devices_from_adapter(
      &synthetic_empty_query, owner, nullptr, plan);
  passed &= check(empty_inventory.has_value() && empty_inventory->empty(),
                  "successful empty physical-device inventory was converted into an error");
  if (empty_inventory) {
    const auto no_match = selectPhysicalDevice(empty_inventory->view(), {});
    passed &= check(!no_match &&
                        no_match.error().code() == make_error_code(PhysicalDeviceError::no_match),
                    "empty inventory was not distinct from a no-match selection");
  }

  const auto temporary_inventory = graphics_detail::query_physical_devices_from_adapter(
      &synthetic_temporary_query, owner, nullptr, plan);
  passed &= check(temporary_inventory.has_value() && temporary_inventory->size() == 1,
                  "synthetic capability query did not return its value-owned snapshot");
  if (temporary_inventory) {
    const auto &snapshot = temporary_inventory->front().capabilities;
    passed &= check(snapshot.driverName() == "temporary-driver-name" &&
                        snapshot.driverInfo() == "temporary-driver-info",
                    "capability strings did not survive query temporaries vanishing");
    passed &= check(snapshot.extensionNames() == std::vector<std::string>{"VK_EXT_temporary"},
                    "capability extension names did not survive query temporaries vanishing");
    passed &= check(snapshot.extensionProperties().size() == 1 &&
                        snapshot.extensionProperties().front().name == "VK_EXT_temporary" &&
                        snapshot.extensionProperties().front().spec_version == 4,
                    "capability extension metadata did not survive query temporaries vanishing");
    passed &= check(snapshot.features10().samplerAnisotropy == VK_TRUE &&
                        snapshot.features11().shaderDrawParameters == VK_TRUE &&
                        snapshot.features12().timelineSemaphore == VK_TRUE &&
                        snapshot.features13().dynamicRendering == VK_TRUE,
                    "native Vulkan feature snapshots did not survive query temporaries vanishing");
  }

  auto source = synthetic_candidate(0x34u);
  const auto copied = source;
  const auto copied_extension_names = copied.capabilities.extensionNames();
  source.capabilities.extensions.clear();
  source.capabilities.extension_properties.clear();
  source.capabilities.queue_families.clear();
  source.capabilities.memory_properties.memoryHeapCount = 0;
  passed &= check(copied.capabilities.extensionNames() == copied_extension_names &&
                      !copied.capabilities.queueFamilies().empty() &&
                      copied.capabilities.memoryProperties().memoryHeapCount == 1,
                  "capability snapshots shared mutable source/test input storage");
  return passed;
}

[[nodiscard]] bool test_synthetic_selection_is_explicit_and_deterministic() {
  const auto first = synthetic_candidate(0x01u, true, true, 0);
  const auto second = synthetic_candidate(0x02u, true, true, 1);
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
    passed &= check(ordered->capabilities.device_uuid == first.capabilities.device_uuid,
                    "explicit UUID ordering did not select the first requested UUID");
  }

  PhysicalDeviceSelectionPolicy manual;
  manual.explicit_uuid = second.capabilities.device_uuid;
  const auto manually_selected = selectPhysicalDevice(candidates, requirements, manual);
  passed &= check(manually_selected.has_value(), "manual UUID selection failed");
  if (manually_selected) {
    passed &= check(manually_selected->capabilities.device_uuid == second.capabilities.device_uuid,
                    "manual UUID selection returned a different candidate");
  }

  const auto direct = selectPhysicalDevice(first, requirements);
  passed &= check(direct.has_value(), "single-candidate manual selection failed");

  const auto missing = selectPhysicalDevice(
      candidates, PhysicalDeviceRequirements{.required_extensions = {"VK_EXT_missing"}});
  const bool no_match_reported =
      !missing && missing.error().code() == make_error_code(PhysicalDeviceError::no_match);
  passed &= check(no_match_reported, "missing extension did not produce no-match");
  return passed;
}

[[nodiscard]] bool test_unavailable_uuid_is_never_a_match() {
  const PhysicalDeviceUuid zero_uuid{};
  auto unavailable = synthetic_candidate(0x00u);
  unavailable.capabilities.id_properties_available = false;
  unavailable.capabilities.device_uuid = zero_uuid;
  const auto available = synthetic_candidate(0x03u);

  bool passed = true;
  const PhysicalDeviceRequirements uuid_requirement{.required_device_uuid = zero_uuid};
  const auto evaluation = evaluatePhysicalDevice(unavailable, uuid_requirement);
  passed &= check(evaluation.has_value() && !evaluation->matches,
                  "unavailable UUID properties satisfied a required zero UUID");
  if (evaluation) {
    bool clear_decline = evaluation->decisions.size() == 1;
    if (clear_decline) {
      const auto &decision = evaluation->decisions.front();
      clear_decline = decision.name == "device_uuid=00000000000000000000000000000000" &&
                      decision.outcome == PhysicalDeviceRequirementOutcome::declined &&
                      decision.reason == PhysicalDeviceRequirementReason::unsupported;
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
      ordered_selection.has_value() && ordered_selection->capabilities.id_properties_available &&
      ordered_selection->capabilities.device_uuid == available.capabilities.device_uuid;
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
  const auto identity = owner.implementationIdentity();
  const auto native_before_move = candidate.device.nativeHandle();
  passed &= check(identity != nullptr && candidate.device.instanceIdentity() == identity,
                  "PhysicalDevice view did not retain the Instance implementation identity");
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

  Instance moved = std::move(owner);
  // A moved-from Instance is intentionally inspected to verify its documented empty state.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  passed &= check(owner.implementationIdentity() == nullptr && !owner.valid(),
                  "Instance move did not empty the source owner");
  passed &= check(moved.implementationIdentity() == identity,
                  "Instance move changed the implementation identity token");
  passed &= check(candidate.device.correlatedWith(moved),
                  "PhysicalDevice view lost correlation after Instance move");
  passed &= check(candidate.device.nativeHandle() == native_before_move,
                  "PhysicalDevice native handle changed after Instance move");

  PhysicalDeviceSelectionPolicy explicit_order;
  explicit_order.uuid_order.reserve(inventory_result->size());
  for (const auto &candidate : *inventory_result) {
    explicit_order.uuid_order.push_back(candidate.capabilities.device_uuid);
  }
  const auto selected =
      selectPhysicalDevice(*inventory_result, PhysicalDeviceRequirements{}, explicit_order);
  passed &= check(selected.has_value(),
                  "explicit UUID ordering could not select the runtime physical device");
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
  passed &= test_synthetic_selection_is_explicit_and_deterministic();
  passed &= test_unavailable_uuid_is_never_a_match();
  passed &= test_headless_query_and_instance_move_correlation();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

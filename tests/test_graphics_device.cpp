#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <terreate/graphics/device.hpp>

// The designated-initializer-heavy contract matrix is intentionally kept in
// the same visual field order as the public request structures.
// clang-format off

namespace {

using namespace terreate::graphics;

static_assert(!std::default_initializable<DevicePlan>);
static_assert(std::copy_constructible<DevicePlan>);
static_assert(std::is_nothrow_move_constructible_v<DevicePlan>);
static_assert(std::is_nothrow_move_assignable_v<DevicePlan>);
static_assert(!std::copy_constructible<Device>);
static_assert(std::movable<Device>);
static_assert(!std::default_initializable<Device>);
static_assert(std::copy_constructible<Queue>);
static_assert(std::is_nothrow_copy_constructible_v<Queue>);
static_assert(std::is_trivially_copyable_v<Queue>);
static_assert(!std::is_constructible_v<Queue, vk::Queue, const void *>);
static_assert(!std::is_convertible_v<Queue, vk::Queue>);

[[nodiscard]] bool check(bool condition, const char *message) {
  if (!condition) {
    std::fputs("graphics.device: assertion=FAIL: ", stderr);
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

[[nodiscard]] PhysicalDeviceCapabilities synthetic_capabilities(std::uint32_t queue_count = 1,
                                                                bool add_second_family = false) {
  PhysicalDeviceCapabilities capabilities;
  capabilities.api_version = VK_API_VERSION_1_3;
  capabilities.features2_available = true;
  capabilities.features_11_available = true;
  capabilities.features_12_available = true;
  capabilities.features_13_available = true;
  capabilities.features2.sType = vk::StructureType::ePhysicalDeviceFeatures2;
  capabilities.features_11.sType = vk::StructureType::ePhysicalDeviceVulkan11Features;
  capabilities.features_12.sType = vk::StructureType::ePhysicalDeviceVulkan12Features;
  capabilities.features_13.sType = vk::StructureType::ePhysicalDeviceVulkan13Features;
  capabilities.features_10.samplerAnisotropy = VK_TRUE;
  capabilities.features_11.shaderDrawParameters = VK_TRUE;
  capabilities.features_12.timelineSemaphore = VK_TRUE;
  capabilities.features_13.dynamicRendering = VK_TRUE;
  capabilities.features_13.shaderIntegerDotProduct = VK_TRUE;
  capabilities.extensions = {"VK_EXT_synthetic_supported", "VK_EXT_synthetic_unrequested"};
  vk::QueueFamilyProperties family{};
  family.queueFlags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
  family.queueCount = queue_count;
  capabilities.queue_families.push_back(family);
  if (add_second_family) {
    vk::QueueFamilyProperties second{};
    second.queueFlags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
    second.queueCount = 1;
    capabilities.queue_families.push_back(second);
  }
  return capabilities;
}

[[nodiscard]] DeviceQueueRequest queue_request(std::string caller_id, RequirementStrength strength,
                                               vk::QueueFlags flags, std::size_t family,
                                               float priority = 0.75F) {
  return DeviceQueueRequest{
      .caller_id = std::move(caller_id),
      .strength = strength,
      .required_flags = flags,
      .allowed_family_indices = {family},
      .preferred_family_indices = {family},
      .priority = priority,
  };
}

[[nodiscard]] const DeviceQueueDecision *queue_decision(const DevicePlan &plan,
                                                         const std::string &caller_id) {
  const auto iterator = std::find_if(
      plan.queueDecisions().begin(), plan.queueDecisions().end(), [&](const auto &decision) {
        return decision.caller_id == caller_id;
      });
  return iterator == plan.queueDecisions().end() ? nullptr : &*iterator;
}

[[nodiscard]] const DeviceExtensionDecision *extension_decision(const DevicePlan &plan,
                                                                 const char *name) {
  const auto iterator = std::find_if(
      plan.extensionDecisions().begin(), plan.extensionDecisions().end(), [&](const auto &decision) {
        return decision.name == name;
      });
  return iterator == plan.extensionDecisions().end() ? nullptr : &*iterator;
}

[[nodiscard]] bool test_device_error_codes() {
  constexpr std::array expected_codes{
      std::pair{DeviceError::invalid_description, 1},
      std::pair{DeviceError::contradictory_requirements, 2},
      std::pair{DeviceError::unsupported_api_version, 3},
      std::pair{DeviceError::missing_required_extension, 4},
      std::pair{DeviceError::unsupported_required_feature, 5},
      std::pair{DeviceError::invalid_feature_chain, 6},
      std::pair{DeviceError::invalid_queue_request, 7},
      std::pair{DeviceError::insufficient_queue_count, 8},
      std::pair{DeviceError::ambiguous_queue, 9},
      std::pair{DeviceError::invalid_instance, 10},
      std::pair{DeviceError::invalid_physical_device, 11},
      std::pair{DeviceError::parent_mismatch, 12},
      std::pair{DeviceError::invalid_plan, 13},
  };

  bool passed = true;
  for (const auto &[error, value] : expected_codes) {
    const auto code = make_error_code(error);
    passed &= check(static_cast<bool>(code), "DeviceError produced a false error_code");
    passed &= check(code.value() == value, "DeviceError numeric value changed");
    passed &= check(code.category() == device_error_category(),
                    "DeviceError used the wrong error category");
  }
  return passed;
}

[[nodiscard]] bool test_alias_fallback_and_decisions() {
  const auto capabilities = synthetic_capabilities();
  DeviceDescription description;
  description.required_extensions = {"VK_EXT_synthetic_supported"};
  description.optional_extensions = {"VK_EXT_synthetic_optional"};
  description.required_features.samplerAnisotropy = VK_TRUE;
  description.required_features_11.shaderDrawParameters = VK_TRUE;
  description.optional_features_12.timelineSemaphore = VK_TRUE;
  description.optional_features_13.dynamicRendering = VK_TRUE;
  description.required_features_13.shaderIntegerDotProduct = VK_TRUE;

  auto graphics = queue_request("graphics", RequirementStrength::required,
                                vk::QueueFlagBits::eGraphics, 0);
  auto compute = queue_request("compute", RequirementStrength::optional,
                               vk::QueueFlagBits::eCompute, 0);
  compute.alias_policy = DeviceQueueAliasPolicy::prefer_alias_with_distinct_fallback;
  compute.alias_target = "graphics";
  description.queue_requests = {graphics, compute};
  const auto original = description;

  const auto result = resolveDevice(capabilities, description);
  bool passed = check(result.has_value(), "synthetic logical-device resolution failed");
  if (!result) {
    return false;
  }
  const auto &plan = *result;
  passed &= check(description.queue_requests.size() == original.queue_requests.size() &&
                      description.queue_requests[1].alias_target == "graphics",
                  "resolution mutated the queue request input");
  passed &= check(!plan.hasPhysicalDevice(), "synthetic resolution fabricated a native device");
  passed &= check(plan.enabledExtensions() == std::vector<std::string>{"VK_EXT_synthetic_supported"},
                  "optional unsupported extension was enabled");
  passed &= check(plan.enabledFeatures10().samplerAnisotropy == VK_TRUE &&
                      plan.enabledFeatures11().shaderDrawParameters == VK_TRUE &&
                      plan.enabledFeatures12().timelineSemaphore == VK_TRUE &&
                      plan.enabledFeatures13().dynamicRendering == VK_TRUE &&
                      plan.enabledFeatures13().shaderIntegerDotProduct == VK_TRUE,
                   "feature enablement did not follow explicit requests");
  passed &= check(plan.requested().required_features_11.pNext == nullptr &&
                      plan.requested().optional_features_13.pNext == nullptr,
                  "owned request snapshot retained a pNext pointer");
  passed &= check(plan.queueAllocations().size() == 1 && plan.queueAliasGroups().size() == 1 &&
                      plan.queueAliasGroups().front().caller_ids.size() == 2,
                  "explicit alias fallback did not produce one unique allocation");
  const auto *compute_decision = queue_decision(plan, "compute");
  const auto *graphics_decision = queue_decision(plan, "graphics");
  passed &= check(compute_decision != nullptr && graphics_decision != nullptr &&
                      compute_decision->outcome == DeviceDecisionOutcome::accepted &&
                      compute_decision->reason == DeviceDecisionReason::accepted_by_aliasing &&
                      compute_decision->effective && compute_decision->supported &&
                      compute_decision->queue_index == 0 && compute_decision->alias_target ==
                                                               std::optional<std::string>{"graphics"},
                  "alias decision did not expose effective family/index provenance");
  passed &= check(graphics_decision != nullptr && graphics_decision->effective &&
                      graphics_decision->family_index == 0 && graphics_decision->queue_index == 0,
                  "required graphics decision was not effective");
  const auto *required_extension = extension_decision(plan, "VK_EXT_synthetic_supported");
  const auto *optional_extension = extension_decision(plan, "VK_EXT_synthetic_optional");
  constexpr std::array expected_extension_names{
      "VK_EXT_synthetic_optional",
      "VK_EXT_synthetic_supported",
  };
  passed &= check(
      plan.extensionDecisions().size() == expected_extension_names.size() &&
          std::equal(plan.extensionDecisions().begin(), plan.extensionDecisions().end(),
                     expected_extension_names.begin(), expected_extension_names.end(),
                     [](const auto &decision, const auto name) { return decision.name == name; }),
      "extension decisions were not in canonical lexical order");
  passed &= check(plan.extensionDecisions().size() == 2,
                  "extension decisions did not retain one record per explicit request");
  passed &= check(required_extension != nullptr &&
                      required_extension->strength == RequirementStrength::required &&
                      required_extension->requested && required_extension->supported &&
                      required_extension->effective &&
                      required_extension->outcome == DeviceDecisionOutcome::accepted,
                  "required extension decision did not expose requested/supported/effective");
  passed &= check(optional_extension != nullptr &&
                      optional_extension->strength == RequirementStrength::optional &&
                      optional_extension->requested && !optional_extension->supported &&
                      !optional_extension->effective &&
                      optional_extension->outcome == DeviceDecisionOutcome::declined,
                  "optional extension decision did not expose requested/supported/effective");
  const auto unrequested_capability = std::find(
      plan.capabilities().extensions.begin(), plan.capabilities().extensions.end(),
      "VK_EXT_synthetic_unrequested");
  passed &= check(unrequested_capability != plan.capabilities().extensions.end() &&
                      extension_decision(plan, "VK_EXT_synthetic_unrequested") == nullptr &&
                      std::find(plan.enabledExtensions().begin(), plan.enabledExtensions().end(),
                                "VK_EXT_synthetic_unrequested") == plan.enabledExtensions().end(),
                  "supported but unrequested extension was enabled or recorded as requested");
   passed &= check(plan.featureDecisions().size() == 5 &&
                       plan.featureDecisions().front().requested &&
                      plan.featureDecisions().front().supported &&
                      plan.featureDecisions().front().effective &&
                      !plan.featureDecisions().front().structure.empty() &&
                      !plan.featureDecisions().front().member.empty(),
                   "feature decisions did not expose structure/member evidence");
   passed &= check(std::any_of(plan.featureDecisions().begin(), plan.featureDecisions().end(),
                               [](const auto &decision) {
                                 return decision.version == DeviceFeatureVersion::vulkan_1_3 &&
                                        decision.member == "shaderIntegerDotProduct" &&
                                        decision.effective;
                               }),
                   "Vulkan 1.3 shaderIntegerDotProduct was not resolved");
  return passed;
}

[[nodiscard]] bool test_queue_contract() {
  const auto capabilities = synthetic_capabilities(1, true);
  bool passed = true;

  DeviceDescription duplicate_id;
  duplicate_id.queue_requests = {
      queue_request("same", RequirementStrength::required, vk::QueueFlagBits::eGraphics, 0),
      queue_request("same", RequirementStrength::optional, vk::QueueFlagBits::eCompute, 0),
  };
  auto result = resolveDevice(capabilities, duplicate_id);
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::contradictory_requirements),
                  "duplicate queue caller IDs were not rejected");

  DeviceDescription invalid_flags;
  auto invalid = queue_request("invalid", RequirementStrength::required, {}, 0);
  invalid_flags.queue_requests.push_back(std::move(invalid));
  result = resolveDevice(capabilities, invalid_flags);
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::invalid_queue_request),
                  "zero required queue flags were not rejected");

  DeviceDescription invalid_whitelist;
  invalid_whitelist.queue_requests.push_back(
      queue_request("invalid-whitelist", RequirementStrength::required,
                    vk::QueueFlagBits::eGraphics, 0));
  invalid_whitelist.queue_requests.front().allowed_family_indices = {};
  result = resolveDevice(capabilities, invalid_whitelist);
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::invalid_queue_request),
                  "missing strict family whitelist was not rejected");

  DeviceDescription optional_decline;
  auto unsupported = queue_request("optional-compute", RequirementStrength::optional,
                                  vk::QueueFlagBits::eTransfer, 1);
  optional_decline.queue_requests.push_back(std::move(unsupported));
  result = resolveDevice(capabilities, optional_decline);
  passed &= check(result.has_value() && result->queueAllocations().empty() &&
                      queue_decision(*result, "optional-compute") != nullptr &&
                      queue_decision(*result, "optional-compute")->outcome ==
                          DeviceDecisionOutcome::declined &&
                      queue_decision(*result, "optional-compute")->reason ==
                          DeviceDecisionReason::unsupported,
                  "unsupported optional queue was not explicitly declined");

  DeviceDescription empty_optional_whitelist;
  empty_optional_whitelist.queue_requests.push_back(DeviceQueueRequest{
      .caller_id = "empty-optional",
      .strength = RequirementStrength::optional,
      .required_flags = vk::QueueFlagBits::eCompute,
      .priority = 0.5F,
  });
  result = resolveDevice(capabilities, empty_optional_whitelist);
  passed &= check(result.has_value() && queue_decision(*result, "empty-optional") != nullptr &&
                      queue_decision(*result, "empty-optional")->outcome ==
                          DeviceDecisionOutcome::declined &&
                      queue_decision(*result, "empty-optional")->reason ==
                          DeviceDecisionReason::unsupported,
                   "optional request without a whitelist was not declined");

  DeviceDescription distinct_required;
  distinct_required.queue_requests = {
      queue_request("graphics", RequirementStrength::required, vk::QueueFlagBits::eGraphics, 0),
      queue_request("compute", RequirementStrength::required, vk::QueueFlagBits::eCompute, 0),
  };
  result = resolveDevice(synthetic_capabilities(), distinct_required);
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::insufficient_queue_count),
                  "distinct-required one-queue case was accepted");

  DeviceDescription ambiguous;
  DeviceQueueRequest ambiguous_request{
      .caller_id = "ambiguous",
      .strength = RequirementStrength::required,
      .required_flags = vk::QueueFlagBits::eGraphics,
      .allowed_family_indices = {0, 1},
      .priority = 0.5F,
  };
  ambiguous.queue_requests.push_back(ambiguous_request);
  result = resolveDevice(capabilities, ambiguous);
  passed &= check(!result && result.error().code() == make_error_code(DeviceError::ambiguous_queue),
                  "equal unpreferred families did not report ambiguity");
  ambiguous.queue_requests.front().preferred_family_indices = {1, 0};
  result = resolveDevice(capabilities, ambiguous);
  passed &= check(result.has_value() && queue_decision(*result, "ambiguous")->family_index == 1,
                  "ordered preferred families did not select the declared first family");

  DeviceDescription conflicting_priority;
  auto priority_graphics = queue_request("graphics", RequirementStrength::required,
                                         vk::QueueFlagBits::eGraphics, 0, 0.75F);
  auto priority_compute = queue_request("compute", RequirementStrength::optional,
                                       vk::QueueFlagBits::eCompute, 0, 0.25F);
  priority_compute.alias_policy = DeviceQueueAliasPolicy::prefer_alias_with_distinct_fallback;
  priority_compute.alias_target = "graphics";
  conflicting_priority.queue_requests = {priority_graphics, priority_compute};
  result = resolveDevice(synthetic_capabilities(2), conflicting_priority);
  passed &= check(result.has_value() && result->queueAllocations().size() == 2 &&
                      queue_decision(*result, "compute")->reason ==
                          DeviceDecisionReason::conflicting_priority,
                  "conflicting alias priorities did not use the distinct fallback");

  DeviceDescription alias_fallback;
  auto fallback_graphics = queue_request("graphics", RequirementStrength::required,
                                         vk::QueueFlagBits::eGraphics, 0);
  auto fallback_compute = queue_request("compute", RequirementStrength::required,
                                        vk::QueueFlagBits::eCompute, 0);
  fallback_compute.alias_policy = DeviceQueueAliasPolicy::prefer_distinct_with_alias_fallback;
  fallback_compute.alias_target = "graphics";
  alias_fallback.queue_requests = {fallback_graphics, fallback_compute};
  result = resolveDevice(synthetic_capabilities(), alias_fallback);
  passed &= check(result.has_value() && result->queueAllocations().size() == 1 &&
                      queue_decision(*result, "compute")->reason ==
                          DeviceDecisionReason::accepted_by_aliasing,
                  "distinct-preferred alias fallback was not accepted");
  return passed;
}

[[nodiscard]] bool test_extension_and_feature_rejections() {
  const auto capabilities = synthetic_capabilities();
  bool passed = true;

  DeviceDescription empty_extension;
  empty_extension.required_extensions = {""};
  auto result = resolveDevice(capabilities, empty_extension);
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::invalid_description),
                  "empty device extension names were not rejected as invalid descriptions");

  DeviceDescription embedded_nul_extension;
  embedded_nul_extension.optional_extensions = {std::string{"bad\0name", 8}};
  result = resolveDevice(capabilities, embedded_nul_extension);
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::invalid_description),
                  "embedded-NUL device extension names were not rejected as invalid descriptions");

  DeviceDescription duplicate_extension;
  duplicate_extension.required_extensions = {"VK_EXT_synthetic_supported",
                                             "VK_EXT_synthetic_supported"};
  result = resolveDevice(capabilities, duplicate_extension);
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::contradictory_requirements),
                  "same-strength duplicate extensions were not rejected");

  duplicate_extension.required_extensions = {"VK_EXT_synthetic_supported"};
  duplicate_extension.optional_extensions = {"VK_EXT_synthetic_supported"};
  result = resolveDevice(capabilities, duplicate_extension);
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::contradictory_requirements),
                  "required/optional duplicate extensions were not rejected");

  auto bad_capabilities = capabilities;
  bad_capabilities.extensions.push_back("VK_EXT_synthetic_supported");
  result = resolveDevice(bad_capabilities, DeviceDescription{});
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::invalid_description),
                  "duplicate capability extensions were not rejected");

  DeviceDescription required_feature;
  required_feature.required_features.samplerAnisotropy = VK_TRUE;
  auto missing_feature = capabilities;
  missing_feature.features_10.samplerAnisotropy = VK_FALSE;
  result = resolveDevice(missing_feature, required_feature);
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::unsupported_required_feature),
                   "unsupported required feature was not rejected");

  DeviceDescription overlapping_feature;
  overlapping_feature.required_features_13.shaderIntegerDotProduct = VK_TRUE;
  overlapping_feature.optional_features_13.shaderIntegerDotProduct = VK_TRUE;
  result = resolveDevice(capabilities, overlapping_feature);
  passed &= check(!result &&
                       result.error().code() == make_error_code(DeviceError::contradictory_requirements),
                   "required/optional feature overlap was not classified as contradictory");

  auto bad_chain = capabilities;
  bad_chain.features_11.pNext = &bad_chain.features_10;
  result = resolveDevice(bad_chain, DeviceDescription{});
  passed &= check(!result &&
                      result.error().code() == make_error_code(DeviceError::invalid_feature_chain),
                  "borrowed capability pNext chain was not rejected");
  return passed;
}

[[nodiscard]] bool test_shuffle_stability() {
  const auto capabilities = synthetic_capabilities(2, true);
  DeviceQueueRequest graphics{
      .caller_id = "graphics",
      .strength = RequirementStrength::required,
      .required_flags = vk::QueueFlagBits::eGraphics,
      .allowed_family_indices = {0, 1},
      .preferred_family_indices = {1, 0},
      .priority = 0.75F,
  };
  DeviceQueueRequest compute{
      .caller_id = "compute",
      .strength = RequirementStrength::optional,
      .required_flags = vk::QueueFlagBits::eCompute,
      .allowed_family_indices = {0, 1},
      .preferred_family_indices = {0, 1},
      .priority = 0.75F,
      .alias_policy = DeviceQueueAliasPolicy::prefer_distinct_with_alias_fallback,
      .alias_target = "graphics",
  };
  DeviceDescription first;
  first.queue_requests = {graphics, compute};
  DeviceDescription second;
  second.queue_requests = {compute, graphics};
  const auto first_result = resolveDevice(capabilities, first);
  const auto second_result = resolveDevice(capabilities, second);
  bool passed = check(first_result.has_value() && second_result.has_value(),
                      "shuffled queue requests did not resolve");
  if (!first_result || !second_result) {
    return false;
  }
  passed &= check(first_result->queueAllocations().size() == second_result->queueAllocations().size() &&
                      first_result->queueAssignments().size() == second_result->queueAssignments().size() &&
                      std::equal(first_result->queueAssignments().begin(),
                                 first_result->queueAssignments().end(),
                                 second_result->queueAssignments().begin(),
                                 [](const auto &left, const auto &right) {
                                   return left.caller_id == right.caller_id &&
                                          left.family_index == right.family_index &&
                                          left.queue_index == right.queue_index &&
                                          left.provenance == right.provenance;
                                 }),
                  "queue global solution changed when request order changed");
  return passed;
}

[[nodiscard]] std::optional<std::pair<std::size_t, vk::QueueFlags>>
first_usable_queue_family(const PhysicalDeviceCapabilities &capabilities) {
  for (std::size_t index = 0; index < capabilities.queue_families.size(); ++index) {
    const auto flags = capabilities.queue_families[index].queueFlags;
    if (capabilities.queue_families[index].queueCount == 0 ||
        static_cast<VkQueueFlags>(flags) == 0) {
      continue;
    }
    if ((static_cast<VkQueueFlags>(flags) &
         static_cast<VkQueueFlags>(vk::QueueFlagBits::eGraphics)) != 0) {
      return std::pair{index, vk::QueueFlagBits::eGraphics};
    }
    if ((static_cast<VkQueueFlags>(flags) &
         static_cast<VkQueueFlags>(vk::QueueFlagBits::eCompute)) != 0) {
      return std::pair{index, vk::QueueFlagBits::eCompute};
    }
    return std::pair{index, vk::QueueFlagBits::eTransfer};
  }
  return std::nullopt;
}

[[nodiscard]] bool test_headless_native_device() {
  const auto instance_capabilities = queryInstanceCapabilities();
  if (!check(instance_capabilities.has_value(),
             "headless logical-device test could not query the Vulkan loader")) {
    return false;
  }
  InstanceDescription instance_description;
  instance_description.api_version = instance_capabilities->loader_api_version;
  const auto instance_plan = resolveInstance(instance_description, *instance_capabilities);
  if (!check(instance_plan.has_value(), "headless logical-device instance resolution failed")) {
    return false;
  }
  auto instance = createInstance(*instance_plan);
  if (!check(instance.has_value(), "headless logical-device instance creation failed")) {
    return false;
  }
  const auto inventory = queryPhysicalDevices(*instance);
  if (!check(inventory.has_value() && !inventory->empty(),
             "headless logical-device test found no physical device")) {
    return false;
  }
  const auto &candidate = inventory->front();
  bool passed = true;
  const auto empty_queue_plan = resolveDevice(candidate, DeviceDescription{});
  passed &= check(empty_queue_plan.has_value() && empty_queue_plan->queueAllocations().empty(),
                  "empty queue description did not produce an observable zero-allocation plan");
  if (empty_queue_plan) {
    const auto empty_queue_device = createDevice(*instance, *empty_queue_plan);
    passed &= check(!empty_queue_device &&
                        empty_queue_device.error().code() == make_error_code(DeviceError::invalid_plan),
                    "zero-allocation native plan was not rejected before Vulkan creation");
  }
  const auto queue_family = first_usable_queue_family(candidate.capabilities());
  if (!check(queue_family.has_value(), "headless physical device exposed no usable queue family")) {
    return false;
  }
  if (!queue_family) {
    return false;
  }
  const auto selected_queue = *queue_family;
  DeviceDescription device_description;
  device_description.queue_requests.push_back(DeviceQueueRequest{
      .caller_id = "main",
      .strength = RequirementStrength::required,
      .required_flags = selected_queue.second,
       .allowed_family_indices = {selected_queue.first},
       .preferred_family_indices = {selected_queue.first},
       .priority = 1.0F,
   });
  bool requested_extended_feature = false;
  if (candidate.capabilities().features_11_available &&
      candidate.capabilities().features_11.shaderDrawParameters == VK_TRUE) {
    device_description.optional_features_11.shaderDrawParameters = VK_TRUE;
    requested_extended_feature = true;
  } else if (candidate.capabilities().features_12_available &&
             candidate.capabilities().features_12.timelineSemaphore == VK_TRUE) {
    device_description.optional_features_12.timelineSemaphore = VK_TRUE;
    requested_extended_feature = true;
  } else if (candidate.capabilities().features_13_available &&
             candidate.capabilities().features_13.dynamicRendering == VK_TRUE) {
    device_description.optional_features_13.dynamicRendering = VK_TRUE;
    requested_extended_feature = true;
  }
  const auto device_plan = resolveDevice(candidate, device_description);
  if (!check(device_plan.has_value(), "headless logical-device resolution failed")) {
    return false;
  }
  passed &= check(device_plan->hasPhysicalDevice(),
                  "native candidate resolution lost the physical-device view");
  passed &= check(
      device_plan->enabledFeatures11().sType == vk::StructureType::ePhysicalDeviceVulkan11Features &&
          device_plan->enabledFeatures11().pNext == nullptr &&
          device_plan->enabledFeatures12().sType == vk::StructureType::ePhysicalDeviceVulkan12Features &&
          device_plan->enabledFeatures12().pNext == nullptr &&
          device_plan->enabledFeatures13().sType == vk::StructureType::ePhysicalDeviceVulkan13Features &&
          device_plan->enabledFeatures13().pNext == nullptr,
      "native device plan did not retain canonical extended-feature headers");
  if (requested_extended_feature) {
    passed &= check(
        (device_description.optional_features_11.shaderDrawParameters == VK_TRUE &&
         device_plan->enabledFeatures11().shaderDrawParameters == VK_TRUE) ||
            (device_description.optional_features_12.timelineSemaphore == VK_TRUE &&
             device_plan->enabledFeatures12().timelineSemaphore == VK_TRUE) ||
            (device_description.optional_features_13.dynamicRendering == VK_TRUE &&
             device_plan->enabledFeatures13().dynamicRendering == VK_TRUE),
        "supported optional extended feature was not enabled in the native plan");
  }
  auto created = createDevice(*instance, *device_plan);
  if (!check(created.has_value(), "headless logical-device creation failed")) {
    return false;
  }
  passed &= check(created->valid() && created->nativeHandle() != vk::Device{},
                  "created Device did not retain a valid native handle");
  passed &= check(created->correlatedWith(*instance),
                  "created Device lost its parent Instance correlation");
  passed &= check(created->queues().size() == 1 && created->queue("main").valid(),
                  "created Device did not expose the requested queue");
  Queue borrowed = created->queue("main");
  passed &= check(borrowed.correlatedWith(*created) && borrowed.familyIndex() == selected_queue.first &&
                      borrowed.queueIndex() == 0,
                  "Queue view lost its Device correlation or native coordinates");
  const auto copied_queue = borrowed;
  Device moved = std::move(*created);
  passed &= check(!created->valid() && moved.valid() && borrowed.correlatedWith(moved) &&
                      copied_queue.correlatedWith(moved),
                  "Device move did not preserve non-owning queue correlation");
  passed &= check(!moved.queue("unknown").valid(), "unknown queue caller ID returned a queue");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= test_device_error_codes();
  passed &= test_alias_fallback_and_decisions();
  passed &= test_queue_contract();
  passed &= test_extension_and_feature_rejections();
  passed &= test_shuffle_stability();
  passed &= test_headless_native_device();
  if (passed) {
    emit_runtime_marker("graphics.device: headless-device=PASS");
  }
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

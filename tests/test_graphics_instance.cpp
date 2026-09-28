#include <terreate/graphics/instance.hpp>

#include "instance_query.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace terreate::graphics;
namespace graphics_detail = terreate::graphics::detail;
using NativeCategory = graphics_detail::NativeDiagnosticCategory;
using NativeCategorySpan = std::span<const NativeCategory>;

static_assert(!std::default_initializable<InstancePlan>);
static_assert(!std::default_initializable<Instance>);
static_assert(std::copy_constructible<InstancePlan>);
static_assert(std::is_nothrow_move_constructible_v<InstancePlan>);
static_assert(std::is_nothrow_move_assignable_v<InstancePlan>);
static_assert(std::is_same_v<decltype(std::declval<const InstancePlan &>().requested()),
                             const InstanceDescription &>);
static_assert(std::is_same_v<decltype(std::declval<const InstancePlan &>().capabilities()),
                             const InstanceCapabilities &>);
static_assert(std::is_same_v<decltype(std::declval<const InstancePlan &>().enabledExtensions()),
                             const std::vector<std::string> &>);
static_assert(std::is_same_v<decltype(std::declval<const InstancePlan &>().enabledLayers()),
                             const std::vector<std::string> &>);
static_assert(std::is_same_v<decltype(std::declval<const InstancePlan &>().extensionDecisions()),
                             const std::vector<InstanceDecision> &>);
static_assert(std::is_same_v<decltype(std::declval<const InstancePlan &>().layerDecisions()),
                             const std::vector<InstanceDecision> &>);

[[nodiscard]] std::unique_ptr<vk::raii::Context> throwing_context_factory() {
  throw std::runtime_error{"synthetic loader open failure"};
}

[[nodiscard]] std::unique_ptr<vk::raii::Context> throwing_system_error_context_factory() {
  throw vk::SystemError{vk::Result::eErrorInitializationFailed};
}

[[nodiscard]] std::unique_ptr<vk::raii::Context> throwing_logic_error_context_factory() {
  throw std::logic_error{"synthetic apply context logic failure"};
}

[[nodiscard]] terreate::Result<InstanceCapabilities>
system_error_capability_adapter(const vk::raii::Context *) {
  throw vk::SystemError{vk::Result::eErrorInitializationFailed};
}

[[nodiscard]] terreate::Result<InstanceCapabilities>
logic_error_capability_adapter(const vk::raii::Context *) {
  throw std::logic_error{"synthetic query logic failure"};
}

VKAPI_ATTR VkResult VKAPI_CALL fake_enumerate_instance_version(std::uint32_t *version) noexcept {
  *version = VK_API_VERSION_1_2;
  return VK_SUCCESS;
}

struct CapabilityFixture {
  std::uint32_t loader_version = VK_API_VERSION_1_0;
  std::vector<std::string> extensions{};
  std::vector<std::string> layers{};
  std::optional<std::string> identity{};
};

[[nodiscard]] InstanceCapabilities capabilities_for(CapabilityFixture fixture) {
  InstanceCapabilities capabilities;
  capabilities.loader_api_version = fixture.loader_version;
  capabilities.loader_identity = std::move(fixture.identity);
  capabilities.available_extensions = std::move(fixture.extensions);
  capabilities.available_layers = std::move(fixture.layers);
  return capabilities;
}

[[nodiscard]] bool contains_name(const std::vector<std::string> &names, const std::string &name) {
  return std::binary_search(names.begin(), names.end(), name);
}

[[nodiscard]] bool check(bool condition, const char *message) {
  if (!condition) {
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
  }
  return condition;
}

[[nodiscard]] bool same_description(const InstanceDescription &left,
                                    const InstanceDescription &right) {
  return left.api_version == right.api_version &&
         left.required_extensions == right.required_extensions &&
         left.optional_extensions == right.optional_extensions &&
         left.required_layers == right.required_layers &&
         left.optional_layers == right.optional_layers && left.debug_utils == right.debug_utils &&
         left.application_name == right.application_name &&
         left.application_version == right.application_version &&
         left.engine_name == right.engine_name && left.engine_version == right.engine_version;
}

[[nodiscard]] bool same_capabilities(const InstanceCapabilities &left,
                                     const InstanceCapabilities &right) {
  return left.loader_api_version == right.loader_api_version &&
         left.loader_identity == right.loader_identity &&
         left.available_extensions == right.available_extensions &&
         left.available_layers == right.available_layers;
}

[[nodiscard]] bool same_decisions(const std::vector<InstanceDecision> &left,
                                  const std::vector<InstanceDecision> &right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].name != right[index].name || left[index].strength != right[index].strength ||
        left[index].outcome != right[index].outcome || left[index].reason != right[index].reason ||
        left[index].derived != right[index].derived) {
      return false;
    }
  }
  return true;
}

// NOLINTBEGIN(clang-analyzer-cplusplus.Move)
[[nodiscard]] bool same_plan_values(const InstancePlan &left, const InstancePlan &right) {
  return same_description(left.requested(), right.requested()) &&
         same_capabilities(left.capabilities(), right.capabilities()) &&
         left.effectiveApiVersion() == right.effectiveApiVersion() &&
         left.apiVersionProvenance() == right.apiVersionProvenance() &&
         left.enabledExtensions() == right.enabledExtensions() &&
         left.enabledLayers() == right.enabledLayers() &&
         same_decisions(left.extensionDecisions(), right.extensionDecisions()) &&
         same_decisions(left.layerDecisions(), right.layerDecisions()) &&
         left.debugUtilsEnabled() == right.debugUtilsEnabled() &&
         left.debugUtilsDerived() == right.debugUtilsDerived();
}
// NOLINTEND(clang-analyzer-cplusplus.Move)

void emit_runtime_marker(const char *marker) {
  std::fputs(marker, stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

[[nodiscard]] bool test_instance_error_codes_are_stable_and_truthy() {
  constexpr std::array expected_codes{
      std::pair{InstanceError::invalid_description, 1},
      std::pair{InstanceError::contradictory_requirements, 2},
      std::pair{InstanceError::unsupported_api_version, 3},
      std::pair{InstanceError::missing_required_extension, 4},
      std::pair{InstanceError::missing_required_layer, 5},
      std::pair{InstanceError::loader_unavailable, 6},
  };

  bool passed = true;
  for (const auto &[error, expected_value] : expected_codes) {
    const auto code = make_error_code(error);
    passed &= check(static_cast<bool>(code), "InstanceError produced a false error_code");
    passed &= check(code.value() == expected_value,
                    "InstanceError numeric value was not stable and explicit");
    passed &= check(code.category() == instance_error_category(),
                    "InstanceError used the wrong error category");
  }
  return passed;
}

[[nodiscard]] bool test_vulkan_10_version_fallback() {
  const auto fallback = graphics_detail::query_instance_api_version(nullptr);
  const auto queried =
      graphics_detail::query_instance_api_version(&fake_enumerate_instance_version);

  bool passed = true;
  passed &= check(fallback == VK_API_VERSION_1_0,
                  "missing vkEnumerateInstanceVersion did not use Vulkan 1.0 fallback");
  passed &= check(queried == VK_API_VERSION_1_2, "available version query was not called");
  return passed;
}

[[nodiscard]] bool check_message_type_mapping(VkDebugUtilsMessageTypeFlagsEXT message_types,
                                              NativeCategorySpan expected,
                                              const char *description) {
  const auto result = graphics_detail::map_vulkan_message_types(message_types);
  if (!result) {
    return check(false, description);
  }

  const auto categories = result->view();
  const bool matches = categories.size() == expected.size() &&
                       std::equal(categories.begin(), categories.end(), expected.begin());
  return check(matches, description);
}

[[nodiscard]] bool check_unsupported_message_types(VkDebugUtilsMessageTypeFlagsEXT message_types,
                                                   const char *description) {
  const auto result = graphics_detail::map_vulkan_message_types(message_types);
  const bool unsupported =
      !result && result.error() == graphics_detail::NativeDiagnosticMessageTypeError::unsupported;
  return check(unsupported, description);
}

[[nodiscard]] bool test_vulkan_message_type_mapping() {
  constexpr VkDebugUtilsMessageTypeFlagsEXT general = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
  constexpr VkDebugUtilsMessageTypeFlagsEXT validation =
      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
  constexpr VkDebugUtilsMessageTypeFlagsEXT performance =
      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  constexpr VkDebugUtilsMessageTypeFlagsEXT unknown =
      static_cast<VkDebugUtilsMessageTypeFlagsEXT>(1u << 31);

  const std::array<NativeCategory, 1> expected_general{NativeCategory::general};
  const std::array<NativeCategory, 2> expected_validation_performance{
      NativeCategory::validation,
      NativeCategory::performance,
  };
  const std::array<NativeCategory, 3> expected_all{
      NativeCategory::general,
      NativeCategory::validation,
      NativeCategory::performance,
  };

  bool passed = true;
  passed &= check_message_type_mapping(general, expected_general,
                                       "GENERAL message type was not mapped by the raw adapter");
  passed &= check_message_type_mapping(
      validation | performance, expected_validation_performance,
      "VALIDATION/PERFORMANCE message types were not mapped by the raw adapter");
  passed &= check_message_type_mapping(
      performance | validation | general, expected_all,
      "known message types were not mapped in semantic order by the raw adapter");

#ifdef VK_EXT_device_address_binding
  constexpr auto device_address_binding =
      VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
  const std::array<NativeCategory, 2> expected_general_device{
      NativeCategory::general,
      NativeCategory::device_address_binding,
  };
  const std::array<NativeCategory, 4> expected_all_with_device{
      NativeCategory::general,
      NativeCategory::validation,
      NativeCategory::performance,
      NativeCategory::device_address_binding,
  };
  passed &= check_message_type_mapping(
      device_address_binding | general, expected_general_device,
      "GENERAL/DEVICE_ADDRESS_BINDING order was not preserved by the raw adapter");
  passed &= check_message_type_mapping(
      device_address_binding | performance | validation | general, expected_all_with_device,
      "optional DEVICE_ADDRESS_BINDING was not mapped in semantic order");
#endif

  passed &= check_unsupported_message_types(0, "zero message-type mask was not rejected");
  passed &= check_unsupported_message_types(unknown, "unknown-only message-type mask was accepted");
  passed &= check_unsupported_message_types(
      general | unknown, "mixed known/unknown message-type mask was truncated or accepted");
  return passed;
}

[[nodiscard]] bool test_query_boundary_failures_are_results() {
  const auto loader_failure = graphics_detail::query_instance_capabilities(
      &throwing_context_factory, &system_error_capability_adapter);
  bool passed = true;
  const bool loader_failure_is_result = !loader_failure;
  passed &= check(loader_failure_is_result,
                  "loader construction failure unexpectedly produced capabilities");
  if (!loader_failure) {
    const bool loader_code_preserved =
        loader_failure.error().code() == make_error_code(InstanceError::loader_unavailable);
    passed &= check(loader_code_preserved, "loader failure code did not identify the loader");
    passed &= check(loader_failure.error().context() == "query Vulkan instance capabilities",
                    "loader construction failure lost query context");
    passed &= check(loader_failure.error().detail() == "synthetic loader open failure",
                    "loader construction failure lost its detail");
  }

  const auto context_native_error = graphics_detail::query_instance_capabilities(
      &throwing_system_error_context_factory, &system_error_capability_adapter);
  const vk::SystemError expected_context_error{vk::Result::eErrorInitializationFailed};
  passed &= check(!context_native_error, "native context failure was not a Result");
  if (!context_native_error) {
    passed &= check(context_native_error.error().code() == expected_context_error.code(),
                    "native context failure did not preserve its vk::SystemError code");
  }

  const auto native_error = graphics_detail::query_instance_capabilities_from_adapter(
      &system_error_capability_adapter, nullptr);
  const vk::SystemError expected_native_error{vk::Result::eErrorInitializationFailed};
  passed &= check(!native_error, "native query failure unexpectedly produced capabilities");
  if (!native_error) {
    passed &= check(native_error.error().code() == expected_native_error.code(),
                    "native query failure did not preserve its vk::SystemError code");
  }

  bool logic_error_propagated = false;
  try {
    const auto logic_result = graphics_detail::query_instance_capabilities_from_adapter(
        &logic_error_capability_adapter, nullptr);
    (void)logic_result;
    passed &= check(false, "query adapter converted a non-native logic error into a Result");
  } catch (const std::logic_error &error) {
    logic_error_propagated = std::string{error.what()} == "synthetic query logic failure";
  } catch (...) {
    passed &= check(false, "query adapter propagated the wrong exception type");
  }
  passed &= check(logic_error_propagated, "query adapter did not propagate logic_error");
  return passed;
}

[[nodiscard]] bool test_apply_context_boundary_failures_are_narrow() {
  const auto loader_failure = graphics_detail::create_instance_context(&throwing_context_factory);
  bool passed = true;
  passed &= check(!loader_failure, "apply loader construction failure was not a Result");
  if (!loader_failure) {
    const bool loader_code_preserved =
        loader_failure.error().code() == make_error_code(InstanceError::loader_unavailable);
    passed &= check(loader_code_preserved, "apply loader failure code did not identify the loader");
    passed &= check(loader_failure.error().context() == "create Vulkan instance",
                    "apply loader failure lost its native operation context");
    passed &= check(loader_failure.error().detail() == "synthetic loader open failure",
                    "apply loader failure lost its detail");
  }

  const auto native_failure =
      graphics_detail::create_instance_context(&throwing_system_error_context_factory);
  const vk::SystemError expected_native_error{vk::Result::eErrorInitializationFailed};
  passed &= check(!native_failure, "apply native context failure was not a Result");
  if (!native_failure) {
    passed &= check(native_failure.error().code() == expected_native_error.code(),
                    "apply native context failure did not preserve its vk::SystemError code");
  }

  bool logic_error_propagated = false;
  try {
    const auto logic_result =
        graphics_detail::create_instance_context(&throwing_logic_error_context_factory);
    (void)logic_result;
    passed &= check(false, "apply context converted a non-native logic error into a Result");
  } catch (const std::logic_error &error) {
    logic_error_propagated = std::string{error.what()} == "synthetic apply context logic failure";
  } catch (...) {
    passed &= check(false, "apply context propagated the wrong exception type");
  }
  passed &= check(logic_error_propagated, "apply context did not propagate logic_error");
  return passed;
}

[[nodiscard]] bool test_synthetic_resolution_and_immutable_observers() {
  const auto capabilities = capabilities_for(CapabilityFixture{
      VK_API_VERSION_1_3,
      {"VK_EXT_unrequested", "VK_EXT_required", "VK_EXT_optional", "VK_EXT_optional"},
      {"VK_LAYER_optional", "VK_LAYER_optional"},
      "synthetic-loader",
  });
  const auto original_capabilities = capabilities_for(CapabilityFixture{
      VK_API_VERSION_1_3,
      {"VK_EXT_unrequested", "VK_EXT_required", "VK_EXT_optional", "VK_EXT_optional"},
      {"VK_LAYER_optional", "VK_LAYER_optional"},
      "synthetic-loader",
  });

  InstanceDescription description;
  description.required_extensions = {"VK_EXT_required"};
  description.optional_extensions = {"VK_EXT_optional", "VK_EXT_absent", "VK_EXT_optional"};
  description.optional_layers = {"VK_LAYER_optional", "VK_LAYER_absent", "VK_LAYER_optional"};
  const auto original_description = description;

  const auto result = resolveInstance(description, capabilities);
  if (!check(result.has_value(), "synthetic instance resolution failed")) {
    return false;
  }

  const auto &plan = *result;
  bool passed = true;
  passed &= check(same_description(description, original_description),
                  "instance description was mutated during resolution");
  passed &= check(same_capabilities(capabilities, original_capabilities),
                  "instance capabilities were mutated during resolution");
  passed &= check(same_description(plan.requested(), original_description),
                  "requested snapshot did not preserve the description input");
  passed &= check(same_capabilities(plan.capabilities(), original_capabilities),
                  "capability snapshot did not preserve the capabilities input");
  const bool identity_preserved =
      plan.capabilities().loader_identity == std::optional<std::string>{"synthetic-loader"};
  passed &= check(identity_preserved, "optional loader identity was not preserved by resolution");
  passed &= check(plan.effectiveApiVersion() == VK_API_VERSION_1_3,
                  "unspecified API version did not select Vulkan 1.3");
  passed &= check(plan.apiVersionProvenance() == InstanceApiVersionProvenance::default_policy,
                  "default API version provenance was not observable");
  const bool enabled_extensions_are_canonical =
      plan.enabledExtensions() == std::vector<std::string>{"VK_EXT_optional", "VK_EXT_required"};
  passed &= check(enabled_extensions_are_canonical,
                  "enabled extensions were not canonical or explicit-only");
  passed &= check(plan.enabledLayers() == std::vector<std::string>{"VK_LAYER_optional"},
                  "enabled layers were not canonical");
  const auto &enabled_extensions = plan.enabledExtensions();
  passed &= check(std::find(enabled_extensions.begin(), enabled_extensions.end(),
                            "VK_EXT_unrequested") == enabled_extensions.end(),
                  "an observed but unrequested extension was enabled");
  passed &= check(plan.extensionDecisions().size() == 3,
                  "optional extension decisions were not retained");
  passed &= check(plan.layerDecisions().size() == 2, "optional layer decisions were not retained");
  passed &= check(!plan.debugUtilsEnabled() && !plan.debugUtilsDerived(),
                  "disabled Debug Utils policy was not observable");

  const auto repeated = resolveInstance(description, capabilities);
  passed &= check(repeated.has_value(), "repeated synthetic instance resolution failed");
  if (repeated) {
    passed &= check(same_plan_values(plan, *repeated),
                    "repeated instance resolution was not deterministic");
  }

  // Plans are ordinary copyable values, but there is no public construction or
  // mutation path.  Copy and assignment preserve exactly the resolver output.
  auto copied_plan = *result;
  passed &= check(same_plan_values(copied_plan, plan),
                  "copying a resolver-produced plan changed observer values");
  auto assigned_plan = *result;
  assigned_plan = copied_plan;
  std::swap(copied_plan, assigned_plan);
  const bool assignment_preserved = same_plan_values(assigned_plan, plan);
  passed &= check(assignment_preserved, "copy assignment changed observer values");

  auto moved_source = plan;
  auto moved_plan = std::move(moved_source);
  passed &= check(same_plan_values(moved_plan, plan),
                  "default InstancePlan move changed its destination value");
  // A moved-from plan remains a valid object that can be assigned a new value;
  // its owned collections are otherwise intentionally unspecified.
  moved_source = plan;
  passed &= check(same_plan_values(moved_source, plan),
                  "moved-from InstancePlan could not be assigned a new value");
  auto move_assigned_source = plan;
  auto move_assigned_plan = *result;
  move_assigned_plan = std::move(move_assigned_source);
  passed &= check(same_plan_values(move_assigned_plan, plan),
                  "default InstancePlan move assignment changed its destination value");
  return passed;
}

[[nodiscard]] bool test_contradictions_and_required_failures() {
  const auto capabilities = capabilities_for(CapabilityFixture{VK_API_VERSION_1_3, {}, {}});

  InstanceDescription contradictory;
  contradictory.required_extensions = {"VK_EXT_same"};
  contradictory.optional_extensions = {"VK_EXT_same"};
  const auto contradiction = resolveInstance(contradictory, capabilities);
  bool passed = true;
  const bool contradiction_rejected =
      !contradiction &&
      contradiction.error().code() == make_error_code(InstanceError::contradictory_requirements);
  passed &= check(contradiction_rejected, "required/optional contradiction was accepted");

  InstanceDescription missing;
  missing.required_extensions = {"VK_EXT_missing"};
  const auto missing_result = resolveInstance(missing, capabilities);
  const bool missing_extension_rejected =
      !missing_result &&
      missing_result.error().code() == make_error_code(InstanceError::missing_required_extension);
  passed &= check(missing_extension_rejected, "missing extension rejection failed");

  InstanceDescription optional_debug;
  optional_debug.debug_utils = DebugUtilsMode::optional;
  const auto declined = resolveInstance(optional_debug, capabilities);
  passed &= check(declined.has_value(), "optional Debug Utils was not allowed to decline");
  if (declined) {
    passed &= check(!declined->debugUtilsEnabled(), "declined optional Debug Utils was enabled");
  }

  const auto debug_capabilities = capabilities_for(
      CapabilityFixture{VK_API_VERSION_1_3, {VK_EXT_DEBUG_UTILS_EXTENSION_NAME}, {}});
  InstanceDescription explicit_required_optional_policy;
  explicit_required_optional_policy.required_extensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  explicit_required_optional_policy.debug_utils = DebugUtilsMode::optional;
  const auto explicit_required_optional_result =
      resolveInstance(explicit_required_optional_policy, debug_capabilities);
  passed &= check(explicit_required_optional_result.has_value(),
                  "explicit required Debug Utils extension conflicted with optional policy");
  if (explicit_required_optional_result) {
    passed &= check(explicit_required_optional_result->debugUtilsEnabled() &&
                        !explicit_required_optional_result->debugUtilsDerived(),
                    "explicit Debug Utils requirement was mislabeled as derived");
  }

  InstanceDescription policy_optional_only;
  policy_optional_only.debug_utils = DebugUtilsMode::optional;
  const auto policy_optional_only_result =
      resolveInstance(policy_optional_only, debug_capabilities);
  passed &= check(policy_optional_only_result.has_value(),
                  "optional Debug Utils policy did not resolve with a supported extension");
  if (policy_optional_only_result) {
    passed &= check(policy_optional_only_result->debugUtilsEnabled() &&
                        policy_optional_only_result->debugUtilsDerived(),
                    "derived Debug Utils prerequisite provenance was not retained");
  }

  InstanceDescription explicit_optional_required_policy;
  explicit_optional_required_policy.optional_extensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  explicit_optional_required_policy.debug_utils = DebugUtilsMode::required;
  const auto explicit_optional_required_result =
      resolveInstance(explicit_optional_required_policy, debug_capabilities);
  const bool explicit_optional_required_rejected =
      !explicit_optional_required_result &&
      explicit_optional_required_result.error().code() ==
          make_error_code(InstanceError::contradictory_requirements);
  passed &= check(explicit_optional_required_rejected,
                  "optional explicit Debug Utils extension did not reject required policy");

  InstanceDescription required_debug;
  required_debug.debug_utils = DebugUtilsMode::required;
  const auto required_debug_result = resolveInstance(required_debug, capabilities);
  const bool required_debug_rejected = !required_debug_result;
  passed &= check(required_debug_rejected, "required Debug Utils prerequisite did not fail");
  if (required_debug_rejected) {
    passed &= check(required_debug_result.error().code() ==
                        make_error_code(InstanceError::missing_required_extension),
                    "required Debug Utils failure had the wrong error code");
  }

  const auto old_loader = capabilities_for(CapabilityFixture{VK_API_VERSION_1_2, {}, {}});
  const auto default_version_result = resolveInstance(InstanceDescription{}, old_loader);
  const bool default_version_rejected = !default_version_result;
  passed &= check(default_version_rejected, "default Vulkan 1.3 was silently downgraded");
  if (default_version_rejected) {
    passed &= check(default_version_result.error().code() ==
                        make_error_code(InstanceError::unsupported_api_version),
                    "default API rejection had the wrong error code");
  }
  InstanceDescription explicit_version;
  explicit_version.api_version = VK_API_VERSION_1_2;
  const auto explicit_result = resolveInstance(explicit_version, capabilities);
  if (explicit_result) {
    passed &= check(explicit_result->effectiveApiVersion() == VK_API_VERSION_1_2 &&
                        explicit_result->apiVersionProvenance() ==
                            InstanceApiVersionProvenance::explicit_request,
                    "explicit API version was not retained over the default");
  } else {
    passed &= check(false, "explicit API version unexpectedly failed");
  }
  return passed;
}

[[nodiscard]] bool rejects_invalid_description(const terreate::Result<InstancePlan> &result) {
  return !result && result.error().code() == make_error_code(InstanceError::invalid_description);
}

[[nodiscard]] bool test_rejects_embedded_nul_names() {
  const auto capabilities = capabilities_for(
      CapabilityFixture{VK_API_VERSION_1_3, {"VK_EXT_supported"}, {"VK_LAYER_supported"}});
  const std::string malformed_extension = std::string{"VK_EXT_embedded"} + '\0' + "suffix";
  const std::string malformed_layer = std::string{"VK_LAYER_embedded"} + '\0' + "suffix";

  bool passed = true;

  InstanceDescription required;
  required.required_extensions = {malformed_extension};
  passed &= check(rejects_invalid_description(resolveInstance(required, capabilities)),
                  "required extension names containing NUL were accepted");

  InstanceDescription optional;
  optional.optional_layers = {malformed_layer};
  passed &= check(rejects_invalid_description(resolveInstance(optional, capabilities)),
                  "optional layer names containing NUL were accepted");

  InstanceDescription malformed_application;
  malformed_application.application_name = std::string{"terreate-application"} + '\0' + "suffix";
  passed &= check(rejects_invalid_description(resolveInstance(malformed_application, capabilities)),
                  "application names containing NUL were accepted");

  InstanceDescription malformed_engine;
  malformed_engine.engine_name = std::string{"terreate-engine"} + '\0' + "suffix";
  passed &= check(rejects_invalid_description(resolveInstance(malformed_engine, capabilities)),
                  "engine names containing NUL were accepted");

  auto malformed_extension_capabilities = capabilities;
  malformed_extension_capabilities.available_extensions = {malformed_extension};
  passed &= check(rejects_invalid_description(
                      resolveInstance(InstanceDescription{}, malformed_extension_capabilities)),
                  "available extension capability names containing NUL were accepted");

  auto malformed_layer_capabilities = capabilities;
  malformed_layer_capabilities.available_layers = {malformed_layer};
  passed &= check(rejects_invalid_description(
                      resolveInstance(InstanceDescription{}, malformed_layer_capabilities)),
                  "available layer capability names containing NUL were accepted");

  return passed;
}

[[nodiscard]] bool test_rejects_invalid_debug_utils_modes() {
  const auto capabilities = capabilities_for(CapabilityFixture{VK_API_VERSION_1_3, {}, {}});
  constexpr std::uint16_t first_invalid_mode = 3;

  bool every_invalid_mode_rejected = true;
  bool every_invalid_mode_contextualized = true;
  bool every_invalid_mode_described = true;
  for (std::uint16_t raw_mode = first_invalid_mode; raw_mode < 0x100u; ++raw_mode) {
    InstanceDescription description;
    description.debug_utils = static_cast<DebugUtilsMode>(raw_mode);
    const auto result = resolveInstance(description, capabilities);

    const bool rejected =
        !result && result.error().code() == make_error_code(InstanceError::invalid_description);
    every_invalid_mode_rejected &= rejected;
    if (rejected) {
      every_invalid_mode_contextualized &= result.error().context() == "resolve Vulkan instance";
      every_invalid_mode_described &= result.error().detail() ==
                                      "debug_utils contains an invalid underlying DebugUtilsMode "
                                      "value";
    }
  }

  bool passed = true;
  passed &= check(every_invalid_mode_rejected, "invalid DebugUtilsMode rejection failed");
  passed &= check(every_invalid_mode_contextualized,
                  "invalid DebugUtilsMode rejection lost its resolver context");
  passed &= check(every_invalid_mode_described,
                  "invalid DebugUtilsMode rejection lost its diagnostic detail");
  return passed;
}

[[nodiscard]] bool
test_native_apply_failure_preserves_plan(const InstanceCapabilities &loader_capabilities) {
  constexpr char synthetic_unavailable_extension[] =
      "VK_EXT_terreate_synthetic_unavailable_for_test";

  InstanceDescription description;
  description.api_version = loader_capabilities.loader_api_version;
  description.required_extensions = {synthetic_unavailable_extension};
  auto synthetic_capabilities = loader_capabilities;
  synthetic_capabilities.available_extensions = {synthetic_unavailable_extension};
  synthetic_capabilities.available_layers.clear();
  const auto resolved = resolveInstance(description, synthetic_capabilities);
  if (!check(resolved.has_value(), "synthetic unavailable extension did not resolve")) {
    return false;
  }

  const auto before_native_apply = std::make_unique<InstancePlan>(*resolved);
  // Every collection observer is a borrowed const reference.  Repeated
  // observation must not allocate a copy or expose a mutation path.
  const auto *observed_requested = std::addressof(resolved->requested());
  const auto *observed_capabilities = std::addressof(resolved->capabilities());
  const auto *observed_enabled_extensions = std::addressof(resolved->enabledExtensions());
  const auto *observed_enabled_layers = std::addressof(resolved->enabledLayers());
  const auto *observed_extension_decisions = std::addressof(resolved->extensionDecisions());
  const auto *observed_layer_decisions = std::addressof(resolved->layerDecisions());

  bool passed = true;
  passed &= check(observed_requested == std::addressof(resolved->requested()),
                  "request observer was not a zero-copy borrowed view");
  passed &= check(observed_capabilities == std::addressof(resolved->capabilities()),
                  "capability observer was not a zero-copy borrowed view");
  passed &= check(observed_enabled_extensions == std::addressof(resolved->enabledExtensions()),
                  "extension observer was not a zero-copy borrowed view");
  passed &= check(observed_enabled_layers == std::addressof(resolved->enabledLayers()),
                  "layer observer was not a zero-copy borrowed view");
  passed &= check(observed_extension_decisions == std::addressof(resolved->extensionDecisions()),
                  "extension decision observer was not a zero-copy borrowed view");
  passed &= check(observed_layer_decisions == std::addressof(resolved->layerDecisions()),
                  "layer decision observer was not a zero-copy borrowed view");
  passed &= check(same_plan_values(*resolved, *before_native_apply),
                  "read-only observer access changed the resolved plan");
  const auto failed_creation = createInstance(*resolved);
  passed &= check(!failed_creation, "synthetic unavailable extension unexpectedly created");
  if (!failed_creation) {
    passed &= check(failed_creation.error().code().value() == VK_ERROR_EXTENSION_NOT_PRESENT,
                    "synthetic unavailable extension did not reach native extension rejection");
    passed &= check(failed_creation.error().context() == "create Vulkan instance",
                    "native extension rejection did not retain the native operation context");
  }
  passed &= check(same_plan_values(*resolved, *before_native_apply),
                  "observer mutation or native apply failure mutated the resolved plan");
  passed &= check(resolved->enabledExtensions() ==
                      std::vector<std::string>{synthetic_unavailable_extension},
                  "native apply failure discarded the effective extension collection");
  return passed;
}

struct Recorder {
  int calls = 0;
  terreate::DiagnosticEvent event{};

  void operator()(const terreate::DiagnosticEvent &observed) noexcept {
    ++calls;
    event = observed;
  }
};

[[nodiscard]] bool test_headless_native_instance() {
  const auto capabilities_result = queryInstanceCapabilities();
  if (!check(capabilities_result.has_value(),
             "canonical headless runtime could not query the Vulkan loader")) {
    return false;
  }

  const auto &capabilities = *capabilities_result;
  bool passed = check(!capabilities.loader_identity, "loader identity was fabricated");
  passed &= test_native_apply_failure_preserves_plan(capabilities);
  const bool debug_utils_available =
      contains_name(capabilities.available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  const bool validation_available =
      contains_name(capabilities.available_layers, "VK_LAYER_KHRONOS_validation");

  InstanceDescription description;
  description.api_version = capabilities.loader_api_version;
  description.application_name = "terreate-instance-test";
  description.engine_name = "terreate-test";
  description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  if (debug_utils_available) {
    description.debug_utils = DebugUtilsMode::required;
  }

  Recorder recorder;
  const auto plan_result = resolveInstance(description, capabilities);
  if (!check(plan_result.has_value(), "native capability snapshot did not resolve")) {
    return false;
  }
  const auto &original_plan = *plan_result;
  const auto sink = terreate::DiagnosticSinkView::bind(recorder);

  auto first_result = createInstance(original_plan, sink);
  if (!check(first_result.has_value(), "headless Vulkan instance creation failed")) {
    return false;
  }
  passed &= check(first_result->valid(), "created Instance was not valid");
  passed &= check(first_result->nativeHandle() != vk::Instance{},
                  "created Instance did not expose a native handle");
  const bool plan_retained = first_result->plan() != nullptr;
  passed &= check(plan_retained, "created Instance did not retain its effective plan");
  if (first_result->plan() != nullptr) {
    passed &= check(same_plan_values(*first_result->plan(), original_plan),
                    "Instance did not retain its effective plan");
  }
  if (validation_available) {
    const auto &enabled_layers = original_plan.enabledLayers();
    const bool validation_enabled = contains_name(enabled_layers, "VK_LAYER_KHRONOS_validation");
    passed &= check(validation_enabled, "validation layer was not enabled");
    emit_runtime_marker("graphics.instance: validation-layer=PASS");
  } else {
    const auto &layer_decisions = original_plan.layerDecisions();
    bool validation_was_declined = false;
    for (const auto &decision : layer_decisions) {
      if (decision.name == "VK_LAYER_KHRONOS_validation" &&
          decision.outcome == InstanceDecisionOutcome::declined &&
          decision.reason == InstanceDecisionReason::unsupported) {
        validation_was_declined = true;
        break;
      }
    }
    passed &= check(validation_was_declined, "validation layer decline was not recorded");
    emit_runtime_marker(
        "graphics.instance: validation-layer=SKIP reason=VK_LAYER_KHRONOS_validation unavailable");
  }

  // Move assignment must operate on real owning Instances: there is no public
  // empty Instance value.  optional supplies the destination's engagement.
  auto replacement_result = createInstance(original_plan, sink);
  if (!check(replacement_result.has_value(), "replacement headless instance creation failed")) {
    return false;
  }
  std::optional<Instance> assigned{std::move(*first_result)};
  *assigned = std::move(*replacement_result);
  passed &= check(first_result->plan() == nullptr && !first_result->valid(),
                  "move assignment did not leave the source Instance empty");
  passed &= check(replacement_result->plan() == nullptr && !replacement_result->valid(),
                  "move assignment source retained native ownership");
  passed &= check(assigned.has_value() && assigned->valid() && assigned->plan() != nullptr,
                  "move assignment lost the real Instance ownership");

  if (!debug_utils_available) {
    emit_runtime_marker(
        "graphics.instance: debug-utils-callback=SKIP reason=VK_EXT_debug_utils unavailable");
    return passed;
  }

  if (!check(assigned->plan()->debugUtilsEnabled(),
             "VK_EXT_debug_utils was available but the Debug Utils messenger was not enabled")) {
    return false;
  }

  const auto raw_instance = static_cast<VkInstance>(assigned->nativeHandle());
  const auto submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
      vkGetInstanceProcAddr(raw_instance, "vkSubmitDebugUtilsMessageEXT"));
  if (!check(submit != nullptr,
             "VK_EXT_debug_utils was available but vkSubmitDebugUtilsMessageEXT was unavailable")) {
    return false;
  }

  constexpr char expected_message[] = "terreate debug utils callback after move assignment";
  std::string submitted_message{expected_message};
  const auto calls_before_submit = recorder.calls;
  VkDebugUtilsMessengerCallbackDataEXT callback_data{
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,
      .pMessageIdName = "terreate-instance-debug-utils-runtime",
      .messageIdNumber = 26812,
      .pMessage = submitted_message.c_str(),
  };
  submit(raw_instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
#ifdef VK_EXT_device_address_binding
         VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT |
             VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
#else
         VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
#endif
         &callback_data);
  submitted_message = "mutated after vkSubmitDebugUtilsMessageEXT returned";

  passed &= check(recorder.calls > calls_before_submit,
                  "Debug Utils callback did not reach the application sink after move assignment");
  passed &= check(recorder.event.message == expected_message,
                  "Debug Utils callback message was not copied into the application sink event");
  passed &= check(recorder.event.source == "vulkan",
                  "Debug Utils callback source was not copied into the application sink event");
  passed &= check(recorder.event.code &&
                      recorder.event.code->name == "terreate-instance-debug-utils-runtime" &&
                      recorder.event.code->value == 26812,
                  "Debug Utils callback message ID was not copied into the application sink event");
#ifdef VK_EXT_device_address_binding
  const std::vector<std::string> expected_categories{"GENERAL", "DEVICE_ADDRESS_BINDING"};
#else
  const std::vector<std::string> expected_categories{"GENERAL"};
#endif
  passed &= check(recorder.event.categories == expected_categories,
                  "Debug Utils callback categories were not mapped in semantic order");
  if (passed) {
    emit_runtime_marker("graphics.instance: debug-utils-callback=PASS");
  }
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= test_instance_error_codes_are_stable_and_truthy();
  passed &= test_vulkan_10_version_fallback();
  passed &= test_vulkan_message_type_mapping();
  passed &= test_query_boundary_failures_are_results();
  passed &= test_apply_context_boundary_failures_are_narrow();
  passed &= test_synthetic_resolution_and_immutable_observers();
  passed &= test_contradictions_and_required_failures();
  passed &= test_rejects_embedded_nul_names();
  passed &= test_rejects_invalid_debug_utils_modes();
  passed &= test_headless_native_instance();
  if (!passed) {
    return EXIT_FAILURE;
  }
  emit_runtime_marker("graphics.instance: headless-instance=PASS");
  return EXIT_SUCCESS;
}

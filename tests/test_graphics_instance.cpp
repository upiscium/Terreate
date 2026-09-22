#include <terreate/graphics/instance.hpp>

#include "instance_query.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace terreate::graphics;
namespace graphics_detail = terreate::graphics::detail;

[[nodiscard]] std::unique_ptr<vk::raii::Context> throwing_context_factory() {
  throw std::runtime_error{"synthetic loader open failure"};
}

[[nodiscard]] std::unique_ptr<vk::raii::Context> throwing_system_error_context_factory() {
  throw vk::SystemError{vk::Result::eErrorInitializationFailed};
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

[[nodiscard]] InstanceCapabilities capabilities_for(std::uint32_t loader_version,
                                                    std::vector<std::string> extensions,
                                                    std::vector<std::string> layers) {
  InstanceCapabilities capabilities;
  capabilities.loader_api_version = loader_version;
  capabilities.available_extensions = std::move(extensions);
  capabilities.available_layers = std::move(layers);
  return capabilities;
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

[[nodiscard]] bool same_plan_values(const InstancePlan &left, const InstancePlan &right) {
  return same_description(left.requested, right.requested) &&
         same_capabilities(left.capabilities, right.capabilities) &&
         left.effective_api_version == right.effective_api_version &&
         left.api_version_provenance == right.api_version_provenance &&
         left.enabled_extensions == right.enabled_extensions &&
         left.enabled_layers == right.enabled_layers &&
         same_decisions(left.extension_decisions, right.extension_decisions) &&
         same_decisions(left.layer_decisions, right.layer_decisions) &&
         left.debug_utils_enabled == right.debug_utils_enabled &&
         left.debug_utils_derived == right.debug_utils_derived;
}

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
  const bool queried_version_preserved = queried == VK_API_VERSION_1_2;
  passed &= check(queried_version_preserved, "available version query was not called");
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
  const bool context_failure_is_result = !context_native_error;
  passed &= check(context_failure_is_result, "native context failure was not a Result");
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
  const auto logic_error_adapter = &logic_error_capability_adapter;
  try {
    const auto logic_result =
        graphics_detail::query_instance_capabilities_from_adapter(logic_error_adapter, nullptr);
    logic_error_propagated = !logic_result;
  } catch (const std::logic_error &error) {
    logic_error_propagated = std::string{error.what()} == "synthetic query logic failure";
  } catch (...) {
    logic_error_propagated = false;
  }
  passed &= check(logic_error_propagated, "query adapter masked a non-native logic error");
  return passed;
}

[[nodiscard]] bool test_synthetic_resolution() {
  auto capabilities = capabilities_for(
      VK_API_VERSION_1_3,
      {"VK_EXT_unrequested", "VK_EXT_required", "VK_EXT_optional", "VK_EXT_optional"},
      {"VK_LAYER_optional", "VK_LAYER_optional"});
  const auto expected_capabilities = capabilities_for(
      VK_API_VERSION_1_3,
      {"VK_EXT_unrequested", "VK_EXT_required", "VK_EXT_optional", "VK_EXT_optional"},
      {"VK_LAYER_optional", "VK_LAYER_optional"});

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
  passed &= check(same_capabilities(capabilities, expected_capabilities),
                  "instance capabilities were mutated during resolution");
  passed &= check(same_description(plan.requested, original_description),
                  "requested snapshot did not preserve the description input");
  passed &= check(same_capabilities(plan.capabilities, expected_capabilities),
                  "capability snapshot did not preserve the capabilities input");
  passed &= check(plan.effective_api_version == VK_API_VERSION_1_3,
                  "unspecified API version did not select Vulkan 1.3");
  passed &= check(plan.api_version_provenance == InstanceApiVersionProvenance::default_policy,
                  "default API version provenance was not observable");
  passed &= check(plan.enabled_extensions ==
                      std::vector<std::string>{"VK_EXT_optional", "VK_EXT_required"},
                  "enabled extensions were not canonical or explicit-only");
  passed &= check(plan.enabled_layers == std::vector<std::string>{"VK_LAYER_optional"},
                  "enabled layers were not canonical");
  passed &= check(std::find(plan.enabled_extensions.begin(), plan.enabled_extensions.end(),
                            "VK_EXT_unrequested") == plan.enabled_extensions.end(),
                  "an observed but unrequested extension was enabled");
  passed &=
      check(plan.extension_decisions.size() == 3, "optional extension decisions were not retained");
  passed &= check(plan.layer_decisions.size() == 2, "optional layer decisions were not retained");

  const auto repeated = resolveInstance(description, capabilities);
  passed &= check(repeated.has_value(), "repeated synthetic instance resolution failed");
  if (repeated) {
    passed &= check(same_plan_values(plan, *repeated),
                    "repeated instance resolution was not deterministic");
    passed &= check(repeated->matchesCanonical(),
                    "repeated resolver result did not retain canonical provenance");
  }
  return passed;
}

[[nodiscard]] bool test_contradictions_and_required_failures() {
  const auto capabilities = capabilities_for(VK_API_VERSION_1_3, {}, {});

  InstanceDescription contradictory;
  contradictory.required_extensions = {"VK_EXT_same"};
  contradictory.optional_extensions = {"VK_EXT_same"};
  const auto contradiction = resolveInstance(contradictory, capabilities);
  bool passed = true;
  passed &= check(!contradiction && contradiction.error().code() ==
                                        make_error_code(InstanceError::contradictory_requirements),
                  "required/optional contradiction was accepted");

  InstanceDescription missing;
  missing.required_extensions = {"VK_EXT_missing"};
  const auto missing_result = resolveInstance(missing, capabilities);
  passed &= check(!missing_result && missing_result.error().code() ==
                                         make_error_code(InstanceError::missing_required_extension),
                  "missing required extension was not a resolution error");

  InstanceDescription optional_debug;
  optional_debug.debug_utils = DebugUtilsMode::optional;
  const auto declined = resolveInstance(optional_debug, capabilities);
  passed &= check(declined.has_value(), "optional Debug Utils was not allowed to decline");
  if (declined) {
    passed &= check(!declined->debug_utils_enabled, "declined optional Debug Utils was enabled");
  }

  const auto debug_capabilities =
      capabilities_for(VK_API_VERSION_1_3, {VK_EXT_DEBUG_UTILS_EXTENSION_NAME}, {});
  InstanceDescription explicit_required_optional_policy;
  explicit_required_optional_policy.required_extensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  explicit_required_optional_policy.debug_utils = DebugUtilsMode::optional;
  const auto explicit_required_optional_result =
      resolveInstance(explicit_required_optional_policy, debug_capabilities);
  passed &= check(explicit_required_optional_result.has_value(),
                  "explicit required Debug Utils extension conflicted with optional policy");
  if (explicit_required_optional_result) {
    const auto &plan = *explicit_required_optional_result;
    passed &= check(plan.debug_utils_enabled && !plan.debug_utils_derived,
                    "explicit Debug Utils requirement was mislabeled as derived");
    const bool explicit_decision =
        plan.extension_decisions.size() == 1 && !plan.extension_decisions.front().derived;
    passed &= check(explicit_decision, "explicit Debug Utils decision lost its provenance");
  }

  InstanceDescription explicit_required_required_policy;
  explicit_required_required_policy.required_extensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  explicit_required_required_policy.debug_utils = DebugUtilsMode::required;
  const auto explicit_required_required_result =
      resolveInstance(explicit_required_required_policy, debug_capabilities);
  passed &= check(explicit_required_required_result.has_value(),
                  "explicit required Debug Utils extension conflicted with required policy");
  if (explicit_required_required_result) {
    passed &= check(!explicit_required_required_result->debug_utils_derived &&
                        explicit_required_required_result->extension_decisions.size() == 1 &&
                        !explicit_required_required_result->extension_decisions.front().derived,
                    "explicit and required Debug Utils provenance was not stable");
  }

  InstanceDescription explicit_optional_optional_policy;
  explicit_optional_optional_policy.optional_extensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  explicit_optional_optional_policy.debug_utils = DebugUtilsMode::optional;
  const auto explicit_optional_optional_result =
      resolveInstance(explicit_optional_optional_policy, debug_capabilities);
  passed &= check(explicit_optional_optional_result.has_value(),
                  "explicit optional Debug Utils extension conflicted with optional policy");
  if (explicit_optional_optional_result) {
    passed &= check(explicit_optional_optional_result->debug_utils_enabled &&
                        !explicit_optional_optional_result->debug_utils_derived &&
                        explicit_optional_optional_result->extension_decisions.size() == 1 &&
                        !explicit_optional_optional_result->extension_decisions.front().derived,
                    "explicit optional Debug Utils provenance was mislabeled as derived");
  }

  InstanceDescription policy_optional_only;
  policy_optional_only.debug_utils = DebugUtilsMode::optional;
  const auto policy_optional_only_result =
      resolveInstance(policy_optional_only, debug_capabilities);
  passed &= check(policy_optional_only_result.has_value(),
                  "optional Debug Utils policy did not resolve with a supported extension");
  if (policy_optional_only_result) {
    passed &= check(policy_optional_only_result->debug_utils_enabled &&
                        policy_optional_only_result->debug_utils_derived &&
                        policy_optional_only_result->extension_decisions.size() == 1 &&
                        policy_optional_only_result->extension_decisions.front().derived,
                    "derived Debug Utils prerequisite provenance was not retained");
  }

  InstanceDescription explicit_optional_required_policy;
  explicit_optional_required_policy.optional_extensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  explicit_optional_required_policy.debug_utils = DebugUtilsMode::required;
  const auto explicit_optional_required_result =
      resolveInstance(explicit_optional_required_policy, debug_capabilities);
  passed &= check(!explicit_optional_required_result &&
                      explicit_optional_required_result.error().code() ==
                          make_error_code(InstanceError::contradictory_requirements),
                  "optional explicit Debug Utils extension did not reject required policy");

  InstanceDescription explicit_required_disabled_policy;
  explicit_required_disabled_policy.required_extensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  const auto explicit_required_disabled_result =
      resolveInstance(explicit_required_disabled_policy, debug_capabilities);
  passed &= check(explicit_required_disabled_result.has_value(),
                  "explicit Debug Utils extension failed with disabled policy");
  if (explicit_required_disabled_result) {
    passed &= check(!explicit_required_disabled_result->debug_utils_enabled &&
                        !explicit_required_disabled_result->debug_utils_derived,
                    "disabled Debug Utils policy enabled an explicit extension messenger");
  }

  InstanceDescription required_debug;
  required_debug.debug_utils = DebugUtilsMode::required;
  const auto without_debug = capabilities_for(VK_API_VERSION_1_3, {}, {});
  const auto required_debug_result = resolveInstance(required_debug, without_debug);
  passed &= check(!required_debug_result &&
                      required_debug_result.error().code() ==
                          make_error_code(InstanceError::missing_required_extension),
                  "required Debug Utils prerequisite did not fail");

  const auto old_loader = capabilities_for(VK_API_VERSION_1_2, {}, {});
  const auto default_version_result = resolveInstance(InstanceDescription{}, old_loader);
  passed &=
      check(!default_version_result && default_version_result.error().code() ==
                                           make_error_code(InstanceError::unsupported_api_version),
            "default Vulkan 1.3 was silently downgraded");
  InstanceDescription explicit_version;
  explicit_version.api_version = VK_API_VERSION_1_2;
  const auto explicit_result = resolveInstance(explicit_version, capabilities);
  bool explicit_version_is_preserved = false;
  if (explicit_result) {
    explicit_version_is_preserved =
        explicit_result->effective_api_version == VK_API_VERSION_1_2 &&
        explicit_result->api_version_provenance == InstanceApiVersionProvenance::explicit_request;
  }
  passed &= check(explicit_version_is_preserved,
                  "explicit API version was not retained over the default");
  return passed;
}

[[nodiscard]] bool rejects_invalid_description(const terreate::Result<InstancePlan> &result) {
  return !result && result.error().code() == make_error_code(InstanceError::invalid_description);
}

[[nodiscard]] bool test_rejects_embedded_nul_names() {
  const auto capabilities =
      capabilities_for(VK_API_VERSION_1_3, {"VK_EXT_supported"}, {"VK_LAYER_supported"});
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
  const auto malformed_extension_result =
      resolveInstance(InstanceDescription{}, malformed_extension_capabilities);
  passed &= check(rejects_invalid_description(malformed_extension_result),
                  "available extension capability names containing NUL were accepted");

  auto malformed_layer_capabilities = capabilities;
  malformed_layer_capabilities.available_layers = {malformed_layer};
  const auto malformed_layer_result =
      resolveInstance(InstanceDescription{}, malformed_layer_capabilities);
  passed &= check(rejects_invalid_description(malformed_layer_result),
                  "available layer capability names containing NUL were accepted");

  return passed;
}

[[nodiscard]] bool test_plan_validation() {
  const auto capabilities =
      capabilities_for(VK_API_VERSION_1_3, {"VK_EXT_supported"}, {"VK_LAYER_supported"});
  InstanceDescription description;
  description.required_extensions = {"VK_EXT_supported"};
  description.application_name = "plan-validation";

  const auto resolved = resolveInstance(description, capabilities);
  if (!check(resolved.has_value(), "plan validation fixture did not resolve")) {
    return false;
  }

  const auto rejects_before_native_apply = [](const InstancePlan &candidate) {
    const auto result = createInstance(candidate);
    return !result && result.error().code() == make_error_code(InstanceError::invalid_description);
  };

  bool passed = true;
  passed &= check(rejects_before_native_apply(InstancePlan{}),
                  "default-constructed InstancePlan reached native apply");

  auto copied_plan = *resolved;
  passed &= check(copied_plan.matchesCanonical(),
                  "copying a resolver-produced plan lost canonical provenance");
  passed &= check(same_plan_values(copied_plan, *resolved),
                  "copying a resolver-produced plan changed inspectable values");
  copied_plan.requested.application_name = "copy-only mutation";
  passed &= check(!copied_plan.matchesCanonical(),
                  "mutating a copied resolver-produced plan was not rejected");
  copied_plan.requested.application_name = resolved->requested.application_name;
  passed &= check(copied_plan.matchesCanonical(),
                  "restoring a copied resolver-produced plan did not restore provenance");
  InstancePlan assigned_copy;
  assigned_copy = copied_plan;
  passed &= check(assigned_copy.matchesCanonical(),
                  "copy assignment lost resolver-produced plan provenance");
  passed &= check(same_plan_values(assigned_copy, *resolved),
                  "copy assignment changed inspectable plan values");

  auto enabled_mutation = *resolved;
  enabled_mutation.enabled_extensions.push_back("VK_EXT_forged");
  passed &= check(rejects_before_native_apply(enabled_mutation),
                  "mutated enabled extensions were accepted for native apply");

  auto effective_version_mutation = *resolved;
  effective_version_mutation.effective_api_version = VK_API_VERSION_1_2;
  passed &= check(rejects_before_native_apply(effective_version_mutation),
                  "mutated effective API version was accepted for native apply");

  auto request_mutation = *resolved;
  request_mutation.requested.application_name = "mutated-after-resolution";
  passed &= check(rejects_before_native_apply(request_mutation),
                  "mutated requested description was accepted for native apply");

  auto capability_mutation = *resolved;
  capability_mutation.capabilities.available_extensions.clear();
  passed &= check(rejects_before_native_apply(capability_mutation),
                  "mutated capability snapshot was accepted for native apply");

  auto decision_mutation = *resolved;
  decision_mutation.extension_decisions.front().outcome = InstanceDecisionOutcome::declined;
  passed &= check(rejects_before_native_apply(decision_mutation),
                  "mutated resolution decision was accepted for native apply");

  return passed;
}

[[nodiscard]] bool
test_native_apply_failure_preserves_plan(const InstanceCapabilities &loader_capabilities) {
  constexpr char synthetic_unavailable_extension[] =
      "VK_EXT_terreate_synthetic_unavailable_for_test";

  InstanceDescription description;
  description.api_version = loader_capabilities.loader_api_version;
  description.required_extensions = {synthetic_unavailable_extension};
  InstanceCapabilities synthetic_capabilities;
  synthetic_capabilities.loader_api_version = loader_capabilities.loader_api_version;
  synthetic_capabilities.available_extensions = {synthetic_unavailable_extension};
  const auto resolved = resolveInstance(description, synthetic_capabilities);
  if (!check(resolved.has_value(), "synthetic unavailable extension did not resolve")) {
    return false;
  }

  auto before_native_apply = *resolved;
  const auto failed_creation = createInstance(*resolved);
  const bool plan_values_preserved = same_plan_values(*resolved, before_native_apply);
  before_native_apply.requested.application_name = "native-failure-snapshot-only";
  bool passed = true;
  passed &= check(!before_native_apply.matchesCanonical(),
                  "copied native-failure snapshot did not detach its provenance");
  passed &= check(!failed_creation, "synthetic unavailable extension unexpectedly created");
  if (!failed_creation) {
    passed &= check(failed_creation.error().code().value() == VK_ERROR_EXTENSION_NOT_PRESENT,
                    "synthetic unavailable extension did not reach native extension rejection");
    passed &= check(failed_creation.error().context() == "create Vulkan instance",
                    "native extension rejection did not retain the native operation context");
  }
  passed &= check(resolved->matchesCanonical(),
                  "native apply failure made the resolved plan uninspectable");
  passed &= check(plan_values_preserved, "native apply failure mutated the resolved plan");
  passed &= check(resolved->enabled_extensions ==
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
  if (!capabilities_result) {
    // A source build without a Vulkan loader is still useful for synthetic
    // resolution tests.  Native coverage is opportunistic and remains
    // display/GPU independent when a loader is present.
    emit_runtime_marker(
        "graphics.instance: debug-utils-callback=SKIP reason=Vulkan loader unavailable; "
        "validation layers are not required");
    return true;
  }

  const auto &capabilities = *capabilities_result;
  bool passed = test_native_apply_failure_preserves_plan(capabilities);
  const bool debug_utils_available = std::binary_search(
      capabilities.available_extensions.begin(), capabilities.available_extensions.end(),
      std::string{VK_EXT_DEBUG_UTILS_EXTENSION_NAME});
  InstanceDescription description;
  description.application_name = "terreate-instance-test";
  description.engine_name = "terreate-test";
  if (debug_utils_available) {
    // Once the extension is reported by the loader, this runtime test must
    // create the messenger rather than silently exercising a no-messenger
    // instance.  A missing validation layer is irrelevant to Debug Utils.
    description.debug_utils = DebugUtilsMode::required;
  }

  Recorder recorder;
  auto plan_result = resolveInstance(description, capabilities);
  if (!check(plan_result.has_value(), "native capability snapshot did not resolve")) {
    return false;
  }

  const auto &original_plan = *plan_result;

  const auto sink = terreate::DiagnosticSinkView::bind(recorder);
  const auto &copied_plan = *plan_result;
  auto instance_result = createInstance(copied_plan, sink);
  if (!check(instance_result.has_value(), "headless Vulkan instance creation failed")) {
    return false;
  }

  passed &= check(copied_plan.matchesCanonical(),
                  "resolver-produced plan lost provenance after native apply");
  passed &= check(instance_result->valid(), "created Instance was not valid");
  passed &= check(instance_result->nativeHandle() != vk::Instance{},
                  "created Instance did not expose a native handle");
  const bool retained_effective_api_version =
      instance_result->plan().effective_api_version == original_plan.effective_api_version;
  passed &= check(retained_effective_api_version, "Instance did not retain its effective plan");

  auto failing_plan = original_plan;
  failing_plan.enabled_extensions.push_back("VK_EXT_terreate_missing_for_test");
  const auto failing_extensions = failing_plan.enabled_extensions;
  const auto failed_creation = createInstance(failing_plan, sink);
  passed &= check(!failed_creation &&
                      failed_creation.error().code() ==
                          make_error_code(InstanceError::invalid_description) &&
                      failing_plan.enabled_extensions == failing_extensions,
                  "native apply failure did not retain the pre-native plan");

  Instance moved{std::move(*instance_result)};
  passed &= check(!*instance_result && moved.valid(), "Instance move construction lost ownership");
  const bool moved_was_valid = moved.valid();
  Instance assigned;
  assigned = std::move(moved);
  passed &= check(moved_was_valid && assigned.valid(), "Instance move assignment lost ownership");

  if (!debug_utils_available) {
    passed &= check(!assigned.plan().debug_utils_enabled,
                    "Debug Utils was enabled even though VK_EXT_debug_utils was unavailable");
    emit_runtime_marker(
        "graphics.instance: debug-utils-callback=SKIP reason=VK_EXT_debug_utils unavailable; "
        "validation layers are not required");
    return passed;
  }

  if (!check(assigned.plan().debug_utils_enabled,
             "VK_EXT_debug_utils was available but the Debug Utils messenger was not enabled")) {
    return false;
  }

  const auto raw_instance = static_cast<VkInstance>(assigned.nativeHandle());
  const auto submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
      vkGetInstanceProcAddr(raw_instance, "vkSubmitDebugUtilsMessageEXT"));
  if (submit == nullptr) {
    emit_runtime_marker(
        "graphics.instance: debug-utils-callback=SKIP reason="
        "vkSubmitDebugUtilsMessageEXT unavailable although VK_EXT_debug_utils is present; "
        "validation layers are not required");
    return passed;
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
         VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &callback_data);
  // Changing the source storage after the synchronous native call makes the
  // sink's owning-copy requirement observable instead of relying on a literal.
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
  passed &= check(recorder.event.categories == std::vector<std::string>{"GENERAL"},
                  "Debug Utils callback category was not copied into the application sink event");
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
  passed &= test_query_boundary_failures_are_results();
  passed &= test_synthetic_resolution();
  passed &= test_contradictions_and_required_failures();
  passed &= test_rejects_embedded_nul_names();
  passed &= test_plan_validation();
  passed &= test_headless_native_instance();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

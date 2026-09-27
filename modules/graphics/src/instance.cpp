#include <terreate/graphics/instance.hpp>

#include "graphics_diagnostics.hpp"
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
#include <utility>
#include <vector>

namespace terreate::graphics {

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

struct CallbackState {
  terreate::DiagnosticSinkView sink{};
};

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

} // namespace

const std::error_category &instance_error_category() noexcept { return error_category(); }

std::error_code make_error_code(InstanceError error) noexcept {
  return {static_cast<int>(error), instance_error_category()};
}

namespace {

[[nodiscard]] std::unique_ptr<vk::raii::Context> make_instance_context() {
  return std::make_unique<vk::raii::Context>();
}

} // namespace

std::uint32_t
detail::query_instance_api_version(detail::InstanceVersionFunction enumerate_instance_version) {
  if (enumerate_instance_version == nullptr) {
    return VK_API_VERSION_1_0;
  }

  std::uint32_t api_version = VK_API_VERSION_1_0;
  const auto result = enumerate_instance_version(&api_version);
  if (result != VK_SUCCESS) {
    throw vk::SystemError{static_cast<vk::Result>(result)};
  }
  return api_version;
}

Result<std::unique_ptr<vk::raii::Context>>
detail::create_instance_context(detail::InstanceContextFactory context_factory) {
  try {
    auto context = context_factory();
    if (context == nullptr) {
      return std::unexpected(semantic_error(InstanceError::loader_unavailable,
                                            "create Vulkan instance",
                                            "Vulkan loader context was not constructed"));
    }
    return context;
  } catch (const vk::SystemError &error) {
    return std::unexpected(terreate::Error{error.code(), "create Vulkan instance", error.what()});
  } catch (const std::runtime_error &error) {
    // vk::raii::Context reports an unavailable Vulkan loader as a
    // std::runtime_error.  Keep this catch limited to context construction so
    // later allocation, logic, and native operations retain their behavior.
    return std::unexpected(
        semantic_error(InstanceError::loader_unavailable, "create Vulkan instance", error.what()));
  }
}

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

Result<InstanceCapabilities> detail::query_instance_capabilities_from_adapter(
    detail::InstanceCapabilityAdapter capability_adapter, const vk::raii::Context *context) {
  try {
    return capability_adapter(context);
  } catch (const vk::SystemError &error) {
    return std::unexpected(
        terreate::Error{error.code(), "query Vulkan instance capabilities", error.what()});
  }
}

Result<InstanceCapabilities>
detail::query_instance_capabilities(detail::InstanceContextFactory context_factory,
                                    detail::InstanceCapabilityAdapter capability_adapter) {
  std::unique_ptr<vk::raii::Context> context;
  try {
    context = context_factory();
  } catch (const vk::SystemError &error) {
    return std::unexpected(
        terreate::Error{error.code(), "query Vulkan instance capabilities", error.what()});
  } catch (const std::runtime_error &error) {
    return std::unexpected(semantic_error(InstanceError::loader_unavailable,
                                          "query Vulkan instance capabilities", error.what()));
  }

  if (context == nullptr) {
    return std::unexpected(semantic_error(InstanceError::loader_unavailable,
                                          "query Vulkan instance capabilities",
                                          "Vulkan loader context was not constructed"));
  }

  return detail::query_instance_capabilities_from_adapter(capability_adapter, context.get());
}

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

struct Instance::Impl {
  std::unique_ptr<vk::raii::Context> context;
  std::unique_ptr<CallbackState> callback;
  vk::raii::Instance instance;
  std::optional<vk::raii::DebugUtilsMessengerEXT> messenger;
  InstancePlan plan;

  Impl(std::unique_ptr<vk::raii::Context> context_owner,
       std::unique_ptr<CallbackState> callback_owner, vk::raii::Instance instance_owner,
       std::optional<vk::raii::DebugUtilsMessengerEXT> messenger_owner, InstancePlan instance_plan)
      : context(std::move(context_owner)), callback(std::move(callback_owner)),
        instance(std::move(instance_owner)), messenger(std::move(messenger_owner)),
        plan(std::move(instance_plan)) {}

  void destroy() noexcept {
    // pUserData points into callback.  Destroy the messenger while that state
    // is still alive, then destroy the child instance, callback state, and its
    // parent Context.  This same path is used when unique_ptr move-assignment
    // releases an existing implementation.
    messenger.reset();
    instance.clear();
    callback.reset();
    context.reset();
  }

  ~Impl() noexcept { destroy(); }
};

Instance::Instance(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Instance::Instance(Instance &&other) noexcept : impl_(std::move(other.impl_)) {}

Instance &Instance::operator=(Instance &&other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
  }
  return *this;
}

Instance::~Instance() = default;

Instance::operator bool() const noexcept { return valid(); }

bool Instance::valid() const noexcept {
  return impl_ != nullptr && static_cast<bool>(*impl_->instance);
}

vk::Instance Instance::nativeHandle() const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  return *impl_->instance;
}

const InstancePlan *Instance::plan() const noexcept {
  return impl_ == nullptr ? nullptr : &impl_->plan;
}

Result<Instance> createInstance(const InstancePlan &plan, terreate::DiagnosticSinkView sink) {
  // Native apply is deliberately a consumer of the successful resolver
  // output.  It performs only structural/ABI checks; semantic policy and
  // capability resolution never re-enter this boundary.
  const auto requested = plan.requested();
  const auto enabled_extensions = plan.enabledExtensions();
  const auto enabled_layers = plan.enabledLayers();
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
      debug_create_info.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                                      vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                                      vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance |
                                      vk::DebugUtilsMessageTypeFlagBitsEXT::eDeviceAddressBinding;
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

} // namespace terreate::graphics

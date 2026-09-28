#include <terreate/graphics/instance.hpp>

#include "graphics_diagnostics.hpp"
#include "instance_query.hpp"
#include "physical_device_query.hpp"

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

detail::NativeDiagnosticCategoryMappingResult
detail::map_vulkan_message_types(VkDebugUtilsMessageTypeFlagsEXT message_types) noexcept {
  constexpr VkDebugUtilsMessageTypeFlagsEXT general = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
  constexpr VkDebugUtilsMessageTypeFlagsEXT validation =
      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
  constexpr VkDebugUtilsMessageTypeFlagsEXT performance =
      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

  constexpr VkDebugUtilsMessageTypeFlagsEXT known_message_type_mask =
#ifdef VK_EXT_device_address_binding
      general | validation | performance |
      VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
#else
      general | validation | performance;
#endif

  const auto native_message_types = static_cast<VkDebugUtilsMessageTypeFlagsEXT>(message_types);
  if (native_message_types == 0 || (native_message_types & ~known_message_type_mask) != 0) {
    return std::unexpected(NativeDiagnosticMessageTypeError::unsupported);
  }

  NativeDiagnosticCategoryMapping mapping{};
  if ((native_message_types & general) != 0) {
    mapping.categories[mapping.count++] = NativeDiagnosticCategory::general;
  }
  if ((native_message_types & validation) != 0) {
    mapping.categories[mapping.count++] = NativeDiagnosticCategory::validation;
  }
  if ((native_message_types & performance) != 0) {
    mapping.categories[mapping.count++] = NativeDiagnosticCategory::performance;
  }
#ifdef VK_EXT_device_address_binding
  if ((native_message_types & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) != 0) {
    mapping.categories[mapping.count++] = NativeDiagnosticCategory::device_address_binding;
  }
#endif
  return mapping;
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
       std::optional<vk::raii::DebugUtilsMessengerEXT> messenger_owner,
       const InstancePlan &instance_plan)
      : context(std::move(context_owner)), callback(std::move(callback_owner)),
        instance(std::move(instance_owner)), messenger(std::move(messenger_owner)),
        plan(instance_plan) {}

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

namespace {

class PhysicalDeviceErrorCategory final : public std::error_category {
public:
  [[nodiscard]] const char *name() const noexcept override {
    return "terreate.graphics.physical_device";
  }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<PhysicalDeviceError>(value)) {
    case PhysicalDeviceError::invalid_instance:
      return "the Vulkan instance is not a valid physical-device query parent";
    case PhysicalDeviceError::invalid_requirements:
      return "invalid physical-device requirements";
    case PhysicalDeviceError::no_match:
      return "no physical device satisfies the explicit requirements";
    case PhysicalDeviceError::ambiguous_match:
      return "more than one physical device satisfies the explicit requirements";
    }
    return "unknown Terreate Graphics physical-device error";
  }
};

[[nodiscard]] const PhysicalDeviceErrorCategory &physical_error_category() noexcept {
  static const PhysicalDeviceErrorCategory category;
  return category;
}

[[nodiscard]] terreate::Error p_error(PhysicalDeviceError error, std::string context,
                                      std::string detail) {
  return terreate::Error{make_error_code(error), std::move(context), std::move(detail)};
}

template <typename T>
[[nodiscard]] terreate::Result<T> fail(PhysicalDeviceError error, std::string context,
                                       std::string detail) {
  return std::unexpected(p_error(error, std::move(context), std::move(detail)));
}

[[nodiscard]] std::string fixed_name(const auto &characters) {
  const auto first = characters.begin();
  const auto last = std::find(first, characters.end(), '\0');
  return std::string{first, last};
}

[[nodiscard]] bool has_name(const std::vector<std::string> &names, std::string_view name) noexcept {
  // PhysicalDeviceCapabilities is a public, caller-constructible snapshot;
  // do not assume that its extension names were canonicalised by the native
  // query path before evaluating them.
  return std::find(names.begin(), names.end(), name) != names.end();
}

[[nodiscard]] bool instance_ext(const Instance &instance, std::string_view extension) noexcept {
  const auto *plan = instance.plan();
  if (plan == nullptr) {
    return false;
  }
  return std::binary_search(plan->enabledExtensions().begin(), plan->enabledExtensions().end(),
                            extension);
}

[[nodiscard]] bool queue_satisfies(const vk::QueueFamilyProperties &family,
                                   const PhysicalDeviceQueueRequirement &requirement) noexcept {
  const auto family_flags = static_cast<VkQueueFlags>(family.queueFlags);
  const auto required_flags = static_cast<VkQueueFlags>(requirement.flags);
  return (family_flags & required_flags) == required_flags &&
         family.queueCount >= requirement.min_queue_count;
}

[[nodiscard]] bool has_queue(const PhysicalDeviceCapabilities &c,
                             const PhysicalDeviceQueueRequirement &r) noexcept {
  return std::any_of(c.queue_families.begin(), c.queue_families.end(),
                     [&](const auto &family) { return queue_satisfies(family, r); });
}

[[nodiscard]] bool same_uuid(const PhysicalDeviceUuid &left,
                             const PhysicalDeviceUuid &right) noexcept {
  return std::equal(left.begin(), left.end(), right.begin(), right.end());
}

[[nodiscard]] bool matches_available_uuid(const PhysicalDeviceCapabilities &capabilities,
                                          const PhysicalDeviceUuid &uuid) noexcept {
  // An unavailable ID-properties query leaves the value-owned UUID at its
  // zero-initialised value.  That value is not an observation and must never
  // satisfy an identity requirement or selection policy.
  return capabilities.id_properties_available && same_uuid(capabilities.device_uuid, uuid);
}

[[nodiscard]] std::string uuid_detail(const PhysicalDeviceUuid &uuid) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string text;
  text.reserve(uuid.size() * 2);
  for (const auto byte : uuid) {
    text.push_back(digits[(byte >> 4u) & 0x0fu]);
    text.push_back(digits[byte & 0x0fu]);
  }
  return text;
}

[[nodiscard]] bool duplicate_name(std::span<const std::string> required,
                                  std::span<const std::string> optional,
                                  std::string_view name) noexcept {
  return std::count(required.begin(), required.end(), name) != 0 &&
         std::count(optional.begin(), optional.end(), name) != 0;
}

[[nodiscard]] std::optional<std::string> validate_names(std::span<const std::string> required,
                                                        std::span<const std::string> optional,
                                                        std::string_view kind) {
  for (const auto &name : required) {
    if (name.empty() || name.find('\0') != std::string::npos) {
      return std::string{"a required physical-device "} + std::string{kind} +
             " name must be non-empty and must not contain an embedded NUL byte";
    }
  }
  for (const auto &name : optional) {
    if (name.empty() || name.find('\0') != std::string::npos) {
      return std::string{"an optional physical-device "} + std::string{kind} +
             " name must be non-empty and must not contain an embedded NUL byte";
    }
  }
  for (const auto &name : required) {
    if (duplicate_name(required, optional, name)) {
      return "physical-device " + std::string{kind} + " '" + name +
             "' was declared both required and optional";
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string>
validate_physical_requirements(const PhysicalDeviceRequirements &requirements) {
  if (requirements.minimum_api_version && *requirements.minimum_api_version == 0) {
    return "minimum_api_version must be non-zero";
  }
  if (const auto error = validate_names(requirements.required_extensions,
                                        requirements.optional_extensions, "extension")) {
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
  for (const auto flags : requirements.required_queue_flags) {
    if (static_cast<VkQueueFlags>(flags) == 0) {
      return "required queue flag masks must be non-zero";
    }
  }
  for (const auto flags : requirements.optional_queue_flags) {
    if (static_cast<VkQueueFlags>(flags) == 0) {
      return "optional queue flag masks must be non-zero";
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::vector<PhysicalDeviceQueueRequirement>
queue_requirements_with_masks(const std::vector<PhysicalDeviceQueueRequirement> &families,
                              const std::vector<vk::QueueFlags> &masks) {
  auto result = families;
  result.reserve(result.size() + masks.size());
  for (const auto flags : masks) {
    result.push_back(PhysicalDeviceQueueRequirement{.flags = flags, .min_queue_count = 1});
  }
  return result;
}

[[nodiscard]] PhysicalDeviceRequirementDecision
make_physical_decision(std::string name, RequirementStrength strength,
                       PhysicalDeviceRequirementOutcome outcome,
                       PhysicalDeviceRequirementReason reason) {
  return PhysicalDeviceRequirementDecision{
      .name = std::move(name),
      .strength = strength,
      .outcome = outcome,
      .reason = reason,
  };
}

template <bool IncludeId, bool IncludeDriver, typename Chain>
void copy_properties2_chain(const Chain &chain, PhysicalDeviceCapabilities &capabilities) {
  auto properties2 = chain.template get<vk::PhysicalDeviceProperties2>();
  properties2.pNext = nullptr;
  capabilities.properties2 = properties2;

  if constexpr (IncludeId) {
    auto id_properties = chain.template get<vk::PhysicalDeviceIDProperties>();
    id_properties.pNext = nullptr;
    capabilities.id_properties = id_properties;
    std::copy(id_properties.deviceUUID.begin(), id_properties.deviceUUID.end(),
              capabilities.device_uuid.begin());
    std::copy(id_properties.driverUUID.begin(), id_properties.driverUUID.end(),
              capabilities.driver_uuid.begin());
  }

  if constexpr (IncludeDriver) {
    auto driver_properties = chain.template get<vk::PhysicalDeviceDriverProperties>();
    driver_properties.pNext = nullptr;
    capabilities.driver_properties = driver_properties;
    capabilities.driver_id = static_cast<std::uint32_t>(driver_properties.driverID);
    capabilities.driver_name = fixed_name(driver_properties.driverName);
    capabilities.driver_info = fixed_name(driver_properties.driverInfo);
  }
}

template <bool IncludeId, bool IncludeDriver>
void query_properties2_chain(const vk::raii::PhysicalDevice &native,
                             PhysicalDeviceCapabilities &capabilities) {
  if constexpr (IncludeId && IncludeDriver) {
    const auto chain =
        native.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties,
                              vk::PhysicalDeviceDriverProperties>();
    copy_properties2_chain<true, true>(chain, capabilities);
  } else if constexpr (IncludeId) {
    const auto chain =
        native.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
    copy_properties2_chain<true, false>(chain, capabilities);
  } else if constexpr (IncludeDriver) {
    const auto chain =
        native.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>();
    copy_properties2_chain<false, true>(chain, capabilities);
  } else {
    auto properties2 = native.getProperties2();
    properties2.pNext = nullptr;
    capabilities.properties2 = properties2;
  }
}

template <bool Include11, bool Include12, bool Include13, typename Chain>
void copy_features2_chain(const Chain &chain, PhysicalDeviceCapabilities &capabilities) {
  auto features2 = chain.template get<vk::PhysicalDeviceFeatures2>();
  features2.pNext = nullptr;
  capabilities.features2 = features2;

  if constexpr (Include11) {
    auto features11 = chain.template get<vk::PhysicalDeviceVulkan11Features>();
    features11.pNext = nullptr;
    capabilities.features_11 = features11;
  }
  if constexpr (Include12) {
    auto features12 = chain.template get<vk::PhysicalDeviceVulkan12Features>();
    features12.pNext = nullptr;
    capabilities.features_12 = features12;
  }
  if constexpr (Include13) {
    auto features13 = chain.template get<vk::PhysicalDeviceVulkan13Features>();
    features13.pNext = nullptr;
    capabilities.features_13 = features13;
  }
}

template <bool Include11, bool Include12, bool Include13>
// clang-format off
void query_features2_chain(const vk::raii::PhysicalDevice &native,
                           PhysicalDeviceCapabilities &capabilities) {
  if constexpr (Include11 && Include12 && Include13) {
    const auto chain =
        native.getFeatures2<
            vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
            vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>();
    copy_features2_chain<true, true, true>(chain, capabilities);
  } else if constexpr (Include11 && Include12) {
    const auto chain =
        native.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                            vk::PhysicalDeviceVulkan12Features>();
    copy_features2_chain<true, true, false>(chain, capabilities);
  } else if constexpr (Include11 && Include13) {
    const auto chain =
        native.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                            vk::PhysicalDeviceVulkan13Features>();
    copy_features2_chain<true, false, true>(chain, capabilities);
  } else if constexpr (Include12 && Include13) {
    const auto chain =
        native.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features,
                            vk::PhysicalDeviceVulkan13Features>();
    copy_features2_chain<false, true, true>(chain, capabilities);
  } else if constexpr (Include11) {
    const auto chain =
        native.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features>();
    copy_features2_chain<true, false, false>(chain, capabilities);
  } else if constexpr (Include12) {
    const auto chain =
        native.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
    copy_features2_chain<false, true, false>(chain, capabilities);
  } else if constexpr (Include13) {
    const auto chain =
        native.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features>();
    copy_features2_chain<false, false, true>(chain, capabilities);
  } else {
    auto features2 = native.getFeatures2();
    features2.pNext = nullptr;
    capabilities.features2 = features2;
  }
}
// clang-format on

// clang-format off
[[nodiscard]] PhysicalDeviceCapabilities snapshot_device(
    const vk::raii::PhysicalDevice &native, std::uint32_t effective_instance_api,
    bool properties2_extension, bool external_memory_extension, bool driver_properties_extension,
    const std::vector<std::string> &extensions)
// clang-format on
{
  PhysicalDeviceCapabilities capabilities;
  capabilities.extensions = extensions;

  capabilities.properties = native.getProperties();
  // The requirement evaluator must compare a device's advertised API support,
  // not merely the API version selected while creating its parent Instance.
  capabilities.api_version = capabilities.properties.apiVersion;
  // Feature structures are copied as native observations only; feature
  // selection and enable-chain construction belong to logical-device creation.
  capabilities.features_10 = native.getFeatures();
  capabilities.queue_families = native.getQueueFamilyProperties();
  capabilities.memory_properties = native.getMemoryProperties();

  const detail::PhysicalDeviceQueryApiVersions api_versions{
      .effective_instance_api = effective_instance_api,
      .physical_device_api = capabilities.api_version,
  };
  const auto availability = detail::physical_device_query_availability(
      api_versions, properties2_extension, external_memory_extension, driver_properties_extension);
  capabilities.properties2_available = availability.properties2;
  capabilities.id_properties_available = availability.id_properties;
  capabilities.driver_properties_available = availability.driver_properties;
  capabilities.features2_available = availability.properties2;
  capabilities.features_11_available = availability.features_11;
  capabilities.features_12_available = availability.features_12;
  capabilities.features_13_available = availability.features_13;
  capabilities.memory_properties2_available = availability.memory_properties2;

  if (availability.properties2) {
    if (availability.id_properties && availability.driver_properties) {
      query_properties2_chain<true, true>(native, capabilities);
    } else if (availability.id_properties) {
      query_properties2_chain<true, false>(native, capabilities);
    } else if (availability.driver_properties) {
      query_properties2_chain<false, true>(native, capabilities);
    } else {
      query_properties2_chain<false, false>(native, capabilities);
    }

    if (availability.features_11 && availability.features_12 && availability.features_13) {
      query_features2_chain<true, true, true>(native, capabilities);
    } else if (availability.features_11 && availability.features_12) {
      query_features2_chain<true, true, false>(native, capabilities);
    } else if (availability.features_11 && availability.features_13) {
      query_features2_chain<true, false, true>(native, capabilities);
    } else if (availability.features_12 && availability.features_13) {
      query_features2_chain<false, true, true>(native, capabilities);
    } else if (availability.features_11) {
      query_features2_chain<true, false, false>(native, capabilities);
    } else if (availability.features_12) {
      query_features2_chain<false, true, false>(native, capabilities);
    } else if (availability.features_13) {
      query_features2_chain<false, false, true>(native, capabilities);
    } else {
      query_features2_chain<false, false, false>(native, capabilities);
    }
  }

  capabilities.extension_properties.reserve(extensions.size());
  // The public query first canonicalises names.  Keep the same canonical
  // order for metadata; spec_version is populated by queryPhysicalDevices.
  for (const auto &name : extensions) {
    capabilities.extension_properties.push_back(
        PhysicalDeviceExtensionProperty{.name = name, .spec_version = 0});
  }
  return capabilities;
}

} // namespace

const std::error_category &physical_device_error_category() noexcept {
  return physical_error_category();
}

std::error_code make_error_code(PhysicalDeviceError error) noexcept {
  return {static_cast<int>(error), physical_device_error_category()};
}

detail::PhysicalDeviceQueryAvailability detail::physical_device_query_availability(
    detail::PhysicalDeviceQueryApiVersions api_versions, bool properties2_extension,
    bool external_memory_extension, bool driver_properties_extension) noexcept {
  const bool instance_supports_11 = api_versions.effective_instance_api >= VK_API_VERSION_1_1;
  const bool instance_supports_12 = api_versions.effective_instance_api >= VK_API_VERSION_1_2;
  const bool instance_supports_13 = api_versions.effective_instance_api >= VK_API_VERSION_1_3;
  const bool device_supports_11 = api_versions.physical_device_api >= VK_API_VERSION_1_1;
  const bool device_supports_12 = api_versions.physical_device_api >= VK_API_VERSION_1_2;
  const bool device_supports_13 = api_versions.physical_device_api >= VK_API_VERSION_1_3;

  detail::PhysicalDeviceQueryAvailability availability;
  const bool properties2_available = instance_supports_11 || properties2_extension;
  const bool core11_available = instance_supports_11 && device_supports_11;
  const bool core12_available = instance_supports_12 && device_supports_12;
  const bool core13_available = instance_supports_13 && device_supports_13;
  availability.properties2 = properties2_available;
  const bool id_properties_available = core11_available || external_memory_extension;
  availability.id_properties = availability.properties2 && id_properties_available;
  availability.driver_properties =
      availability.properties2 && (core12_available || driver_properties_extension);
  availability.features_11 = availability.properties2 && core11_available;
  availability.features_12 = availability.properties2 && core12_available;
  availability.features_13 = availability.properties2 && core13_available;
  availability.memory_properties2 = availability.properties2;
  return availability;
}

std::vector<PhysicalDeviceExtensionProperty> detail::canonicalize_physical_device_extensions(
    std::vector<PhysicalDeviceExtensionProperty> extension_properties) {
  std::sort(extension_properties.begin(), extension_properties.end(),
            [](const auto &left, const auto &right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.spec_version > right.spec_version;
            });
  extension_properties.erase(
      std::unique(extension_properties.begin(), extension_properties.end(),
                  [](const auto &left, const auto &right) { return left.name == right.name; }),
      extension_properties.end());
  return extension_properties;
}

bool PhysicalDevice::correlatedWith(const Instance &instance) const noexcept {
  return instance_identity_ != nullptr && instance_identity_ == instance.identityToken();
}

namespace {

[[nodiscard]] terreate::Result<PhysicalDeviceInventory> query_physical_devices_from_instance(
    const Instance &instance, const vk::raii::Instance *native_instance, const InstancePlan &plan) {
  if (!instance.valid() || instance.identityToken() == nullptr || native_instance == nullptr) {
    return fail<PhysicalDeviceInventory>(
        PhysicalDeviceError::invalid_instance, "query Vulkan physical devices",
        "the supplied Instance is moved-from, invalid, or has no retained plan");
  }

  const bool properties2_extension =
      instance_ext(instance, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
  const bool external_memory_extension =
      instance_ext(instance, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
  PhysicalDeviceInventory inventory;
  const auto native_devices = native_instance->enumeratePhysicalDevices();
  inventory.candidates.reserve(native_devices.size());
  for (std::size_t index = 0; index < native_devices.size(); ++index) {
    const auto &native = native_devices[index];
    std::vector<PhysicalDeviceExtensionProperty> extension_properties;
    const auto native_extensions = native.enumerateDeviceExtensionProperties();
    extension_properties.reserve(native_extensions.size());
    for (const auto &extension : native_extensions) {
      extension_properties.push_back(PhysicalDeviceExtensionProperty{
          .name = copy_native_name(extension.extensionName),
          .spec_version = extension.specVersion,
      });
    }
    extension_properties =
        detail::canonicalize_physical_device_extensions(std::move(extension_properties));

    std::vector<std::string> extension_names;
    extension_names.reserve(extension_properties.size());
    for (const auto &extension : extension_properties) {
      extension_names.push_back(extension.name);
    }

    const bool driver_properties_extension =
        has_name(extension_names, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME) ||
        instance_ext(instance, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    auto capabilities =
        snapshot_device(native, plan.effectiveApiVersion(), properties2_extension,
                        external_memory_extension, driver_properties_extension, extension_names);
    capabilities.extension_properties = std::move(extension_properties);

    const bool memory_budget_supported =
        has_name(capabilities.extensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (memory_budget_supported && capabilities.memory_properties2_available) {
      const auto memory_chain =
          native.getMemoryProperties2<vk::PhysicalDeviceMemoryProperties2,
                                      vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
      vk::PhysicalDeviceMemoryProperties2 memory_properties2 =
          memory_chain.get<vk::PhysicalDeviceMemoryProperties2>();
      vk::PhysicalDeviceMemoryBudgetPropertiesEXT memory_budget =
          memory_chain.get<vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
      memory_properties2.pNext = nullptr;
      memory_budget.pNext = nullptr;
      capabilities.memory_budget = memory_budget;
    }

    inventory.candidates.push_back(PhysicalDeviceCandidate{
        .device = PhysicalDevice{*native, instance.identityToken()},
        .capabilities = std::move(capabilities),
        .enumeration_index = index,
    });
  }
  return inventory;
}

} // namespace

terreate::Result<PhysicalDeviceInventory> detail::query_physical_devices_from_adapter(
    PhysicalDeviceQueryAdapter query_adapter, const Instance &instance,
    const vk::raii::Instance *native_instance, const InstancePlan &plan) {
  try {
    return query_adapter(instance, native_instance, plan);
  } catch (const vk::SystemError &error) {
    return std::unexpected(
        terreate::Error{error.code(), "query Vulkan physical devices", error.what()});
  }
}

terreate::Result<PhysicalDeviceInventory> queryPhysicalDevices(const Instance &instance) {
  const auto *plan = instance.plan();
  if (!instance.valid() || plan == nullptr || instance.identityToken() == nullptr) {
    return fail<PhysicalDeviceInventory>(
        PhysicalDeviceError::invalid_instance, "query Vulkan physical devices",
        "the supplied Instance is moved-from, invalid, or has no retained plan");
  }
  const auto *native_instance = &instance.impl_->instance;
  return detail::query_physical_devices_from_adapter(&query_physical_devices_from_instance,
                                                     instance, native_instance, *plan);
}

terreate::Result<PhysicalDeviceEvaluation>
evaluatePhysicalDevice(const PhysicalDeviceCapabilities &capabilities,
                       const PhysicalDeviceRequirements &requirements) {
  if (const auto error = validate_physical_requirements(requirements)) {
    return fail<PhysicalDeviceEvaluation>(PhysicalDeviceError::invalid_requirements,
                                          "evaluate Vulkan physical device", *error);
  }

  PhysicalDeviceEvaluation evaluation;
  bool matches = true;
  const auto add = [&](std::string name, RequirementStrength strength, bool supported) {
    const auto outcome = supported ? PhysicalDeviceRequirementOutcome::accepted
                                   : PhysicalDeviceRequirementOutcome::declined;
    const auto reason = supported ? (strength == RequirementStrength::required
                                         ? PhysicalDeviceRequirementReason::requested
                                         : PhysicalDeviceRequirementReason::supported)
                                  : PhysicalDeviceRequirementReason::unsupported;
    evaluation.decisions.push_back(
        make_physical_decision(std::move(name), strength, outcome, reason));
    if (!supported && strength == RequirementStrength::required) {
      matches = false;
    }
  };

  if (requirements.minimum_api_version) {
    add("minimum_api_version", RequirementStrength::required,
        capabilities.api_version >= *requirements.minimum_api_version);
  }
  if (requirements.required_device_uuid) {
    add("device_uuid=" + uuid_detail(*requirements.required_device_uuid),
        RequirementStrength::required,
        matches_available_uuid(capabilities, *requirements.required_device_uuid));
  }
  if (requirements.required_device_type) {
    add("device_type", RequirementStrength::required,
        capabilities.properties.deviceType == *requirements.required_device_type);
  }
  for (const auto &extension : requirements.required_extensions) {
    add(extension, RequirementStrength::required, has_name(capabilities.extensions, extension));
  }
  for (const auto &extension : requirements.optional_extensions) {
    add(extension, RequirementStrength::optional, has_name(capabilities.extensions, extension));
  }

  const auto &required_families = requirements.required_queue_families;
  const auto &required_flags = requirements.required_queue_flags;
  const auto &optional_families = requirements.optional_queue_families;
  const auto &optional_flags = requirements.optional_queue_flags;
  const auto required_queues = queue_requirements_with_masks(required_families, required_flags);
  const auto optional_queues = queue_requirements_with_masks(optional_families, optional_flags);
  for (const auto &queue : required_queues) {
    add("queue_flags=" + std::to_string(static_cast<VkQueueFlags>(queue.flags)),
        RequirementStrength::required, has_queue(capabilities, queue));
  }
  for (const auto &queue : optional_queues) {
    add("queue_flags=" + std::to_string(static_cast<VkQueueFlags>(queue.flags)),
        RequirementStrength::optional, has_queue(capabilities, queue));
  }

  evaluation.matches = matches;
  return evaluation;
}

terreate::Result<PhysicalDeviceEvaluation>
evaluatePhysicalDevice(const PhysicalDeviceCandidate &candidate,
                       const PhysicalDeviceRequirements &requirements) {
  return evaluatePhysicalDevice(candidate.capabilities, requirements);
}

terreate::Result<PhysicalDeviceCandidate>
selectPhysicalDevice(const PhysicalDeviceCandidate &candidate,
                     const PhysicalDeviceRequirements &requirements) {
  const auto evaluation = evaluatePhysicalDevice(candidate, requirements);
  if (!evaluation) {
    return std::unexpected(evaluation.error());
  }
  if (!evaluation->matches) {
    return fail<PhysicalDeviceCandidate>(
        PhysicalDeviceError::no_match, "select Vulkan physical device",
        "the manually supplied physical-device candidate does not satisfy the requirements");
  }
  return candidate;
}

terreate::Result<PhysicalDeviceCandidate>
selectPhysicalDevice(std::span<const PhysicalDeviceCandidate> candidates,
                     const PhysicalDeviceRequirements &requirements,
                     const PhysicalDeviceSelectionPolicy &policy) {
  if (const auto error = validate_physical_requirements(requirements)) {
    return fail<PhysicalDeviceCandidate>(PhysicalDeviceError::invalid_requirements,
                                         "select Vulkan physical device", *error);
  }
  if (policy.explicit_uuid && requirements.required_device_uuid &&
      !same_uuid(*policy.explicit_uuid, *requirements.required_device_uuid)) {
    return fail<PhysicalDeviceCandidate>(
        PhysicalDeviceError::invalid_requirements, "select Vulkan physical device",
        "explicit UUID selection conflicts with required_device_uuid");
  }
  for (std::size_t left = 0; left < policy.uuid_order.size(); ++left) {
    if (std::find(policy.uuid_order.begin() + static_cast<std::ptrdiff_t>(left + 1),
                  policy.uuid_order.end(), policy.uuid_order[left]) != policy.uuid_order.end()) {
      return fail<PhysicalDeviceCandidate>(PhysicalDeviceError::invalid_requirements,
                                           "select Vulkan physical device",
                                           "uuid_order must not contain duplicate UUIDs");
    }
  }

  std::vector<const PhysicalDeviceCandidate *> matches;
  matches.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    const auto evaluation = evaluatePhysicalDevice(candidate, requirements);
    if (!evaluation) {
      return std::unexpected(evaluation.error());
    }
    if (evaluation->matches) {
      matches.push_back(&candidate);
    }
  }

  if (matches.empty()) {
    return fail<PhysicalDeviceCandidate>(
        PhysicalDeviceError::no_match, "select Vulkan physical device",
        "no enumerated physical-device candidate satisfies the requirements");
  }

  if (policy.explicit_uuid) {
    std::vector<const PhysicalDeviceCandidate *> uuid_matches;
    for (const auto *candidate : matches) {
      if (matches_available_uuid(candidate->capabilities, *policy.explicit_uuid)) {
        uuid_matches.push_back(candidate);
      }
    }
    if (uuid_matches.empty()) {
      return fail<PhysicalDeviceCandidate>(
          PhysicalDeviceError::no_match, "select Vulkan physical device",
          "the explicitly selected device UUID is not a matching candidate");
    }
    if (uuid_matches.size() != 1) {
      return fail<PhysicalDeviceCandidate>(
          PhysicalDeviceError::ambiguous_match, "select Vulkan physical device",
          "the explicitly selected UUID identifies multiple matching candidates");
    }
    return *uuid_matches.front();
  }

  if (!policy.uuid_order.empty()) {
    for (const auto &uuid : policy.uuid_order) {
      std::vector<const PhysicalDeviceCandidate *> uuid_matches;
      for (const auto *candidate : matches) {
        if (matches_available_uuid(candidate->capabilities, uuid)) {
          uuid_matches.push_back(candidate);
        }
      }
      if (uuid_matches.size() > 1) {
        return fail<PhysicalDeviceCandidate>(
            PhysicalDeviceError::ambiguous_match, "select Vulkan physical device",
            "the explicit UUID ordering contains a UUID shared by multiple candidates");
      }
      if (uuid_matches.size() == 1) {
        return *uuid_matches.front();
      }
    }
    return fail<PhysicalDeviceCandidate>(
        PhysicalDeviceError::no_match, "select Vulkan physical device",
        "the explicit UUID ordering contains no matching candidate");
  }

  if (matches.size() != 1) {
    return fail<PhysicalDeviceCandidate>(
        PhysicalDeviceError::ambiguous_match, "select Vulkan physical device",
        "multiple candidates matched and no explicit UUID selection policy was supplied");
  }
  return *matches.front();
}

} // namespace terreate::graphics

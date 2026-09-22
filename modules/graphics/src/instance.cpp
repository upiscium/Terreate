#include <terreate/graphics/instance.hpp>

#include "graphics_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
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
checked_instance_abi_counts(const InstancePlan &plan) noexcept {
  const auto extension_count = checked_abi_count(plan.enabled_extensions.size());
  const auto layer_count = checked_abi_count(plan.enabled_layers.size());
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
    if (left[index].name != right[index].name) {
      return false;
    }
    if (left[index].strength != right[index].strength) {
      return false;
    }
    if (left[index].outcome != right[index].outcome) {
      return false;
    }
    if (left[index].reason != right[index].reason) {
      return false;
    }
    if (left[index].derived != right[index].derived) {
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
    VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
    VkDebugUtilsMessageTypeFlagsEXT message_types,
    const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data) noexcept {
  // Vulkan invokes this function from native code.  Neither a null user-data
  // pointer nor malformed optional callback data may escape into the bridge,
  // and no exception may cross the Vulkan ABI boundary.
  if (user_data == nullptr || callback_data == nullptr) {
    return VK_FALSE;
  }

  try {
    auto *state = static_cast<CallbackState *>(user_data);

    std::array<detail::NativeDiagnosticCategory, 3> categories{};
    std::size_t category_count = 0;
    if ((message_types & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) != 0) {
      categories[category_count++] = detail::NativeDiagnosticCategory::general;
    }
    if ((message_types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0) {
      categories[category_count++] = detail::NativeDiagnosticCategory::validation;
    }
    if ((message_types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0) {
      categories[category_count++] = detail::NativeDiagnosticCategory::performance;
    }
    if (category_count == 0) {
      categories[category_count++] = detail::NativeDiagnosticCategory::general;
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
        .severity = callback_severity(message_severity),
        .categories =
            std::span<const detail::NativeDiagnosticCategory>{categories.data(), category_count},
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

InstancePlan::InstancePlan(const InstancePlan &other)
    : requested(other.requested), capabilities(other.capabilities),
      effective_api_version(other.effective_api_version),
      api_version_provenance(other.api_version_provenance),
      enabled_extensions(other.enabled_extensions), enabled_layers(other.enabled_layers),
      extension_decisions(other.extension_decisions), layer_decisions(other.layer_decisions),
      debug_utils_enabled(other.debug_utils_enabled),
      debug_utils_derived(other.debug_utils_derived) {
  if (other.canonical_ != nullptr) {
    canonical_ = std::make_unique<InstancePlan>(*other.canonical_);
  }
}

InstancePlan &InstancePlan::operator=(const InstancePlan &other) {
  if (this == &other) {
    return *this;
  }

  InstancePlan copy{other};
  *this = std::move(copy);
  return *this;
}

bool InstancePlan::matchesCanonical() const {
  return canonical_ != nullptr && same_plan_values(*this, *canonical_);
}

const std::error_category &instance_error_category() noexcept { return error_category(); }

std::error_code make_error_code(InstanceError error) noexcept {
  return {static_cast<int>(error), instance_error_category()};
}

Result<InstanceCapabilities> queryInstanceCapabilities() {
  try {
    vk::raii::Context context;
    InstanceCapabilities capabilities;
    capabilities.loader_api_version = context.enumerateInstanceVersion();

    const auto native_extensions = context.enumerateInstanceExtensionProperties();
    const auto native_layers = context.enumerateInstanceLayerProperties();

    capabilities.available_extensions.reserve(native_extensions.size());
    for (const auto &extension : native_extensions) {
      capabilities.available_extensions.push_back(copy_native_name(extension.extensionName));
    }
    capabilities.available_layers.reserve(native_layers.size());
    for (const auto &layer : native_layers) {
      capabilities.available_layers.push_back(copy_native_name(layer.layerName));
    }

    capabilities.available_extensions =
        canonical_names(std::move(capabilities.available_extensions));
    capabilities.available_layers = canonical_names(std::move(capabilities.available_layers));
    return capabilities;
  } catch (const vk::SystemError &error) {
    return std::unexpected(
        terreate::Error{error.code(), "query Vulkan instance capabilities", error.what()});
  }
}

Result<InstancePlan> resolveInstance(const InstanceDescription &description,
                                     const InstanceCapabilities &capabilities) {
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

  InstancePlan plan;
  // These are immutable input snapshots.  Only the effective enabled
  // collections and decisions below are canonicalized; the public snapshots
  // retain the caller's original ordering and duplicates for inspection.
  plan.requested = description;
  plan.capabilities = capabilities;
  plan.effective_api_version = effective_api_version;
  plan.api_version_provenance = explicit_api_version
                                    ? InstanceApiVersionProvenance::explicit_request
                                    : InstanceApiVersionProvenance::default_policy;

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
        plan.extension_decisions.push_back(make_decision(name, requirement,
                                                         InstanceDecisionOutcome::declined,
                                                         InstanceDecisionReason::unsupported));
        continue;
      }
      plan.extension_decisions.push_back(make_decision(name, requirement,
                                                       InstanceDecisionOutcome::declined,
                                                       InstanceDecisionReason::unsupported));
      continue;
    }

    plan.enabled_extensions.push_back(name);
    plan.extension_decisions.push_back(
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
      plan.layer_decisions.push_back(make_decision(name, requirement,
                                                   InstanceDecisionOutcome::declined,
                                                   InstanceDecisionReason::unsupported));
      continue;
    }

    plan.enabled_layers.push_back(name);
    plan.layer_decisions.push_back(make_decision(
        name, requirement, InstanceDecisionOutcome::accepted,
        requirement.strength == RequirementStrength::optional ? InstanceDecisionReason::supported
                                                              : InstanceDecisionReason::requested));
  }

  plan.debug_utils_enabled =
      description.debug_utils != DebugUtilsMode::disabled &&
      std::binary_search(plan.enabled_extensions.begin(), plan.enabled_extensions.end(),
                         std::string{debug_utils_extension});
  const auto debug_decision = std::find_if(
      plan.extension_decisions.begin(), plan.extension_decisions.end(),
      [&](const InstanceDecision &decision) { return decision.name == debug_utils_extension; });
  plan.debug_utils_derived = plan.debug_utils_enabled &&
                             debug_decision != plan.extension_decisions.end() &&
                             debug_decision->derived;
  // Keep a private, exclusive immutable provenance snapshot.  The public
  // fields remain inspectable, but native apply must not accept a hand-built or
  // subsequently mutated plan as if it had passed through this resolver.
  // InstancePlan's explicit value-copy operations deep-copy this snapshot
  // instead of sharing it.
  plan.canonical_ = std::make_unique<InstancePlan>(plan);
  return plan;
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

Instance::Instance() noexcept = default;

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

const InstancePlan &Instance::plan() const noexcept {
  static const InstancePlan empty_plan{};
  return impl_ == nullptr ? empty_plan : impl_->plan;
}

Result<Instance> createInstance(const InstancePlan &plan, terreate::DiagnosticSinkView sink) {
  if (!plan.matchesCanonical()) {
    return invalid_plan_failure("instance plan was not produced by resolveInstance or was mutated");
  }

  // Re-resolve the caller's request and capability snapshot even after the
  // provenance check.  This keeps every effective value and decision coupled
  // to the resolver's canonical result before native Vulkan is touched.
  const auto resolved = resolveInstance(plan.requested, plan.capabilities);
  if (!resolved) {
    const auto validation_detail =
        "instance plan could not be revalidated: " + resolved.error().detail();
    return invalid_plan_failure(validation_detail);
  }
  if (!same_plan_values(plan, *resolved)) {
    return invalid_plan_failure("instance plan does not match resolveInstance output");
  }

  const auto &effective_plan = *resolved;
  const auto abi_counts = checked_instance_abi_counts(effective_plan);
  if (!abi_counts) {
    return invalid_plan_failure(
        "enabled Vulkan extension or layer count exceeds the uint32_t ABI limit");
  }

  std::vector<const char *> extension_names;
  extension_names.reserve(effective_plan.enabled_extensions.size());
  for (const auto &name : effective_plan.enabled_extensions) {
    extension_names.push_back(name.c_str());
  }
  std::vector<const char *> layer_names;
  layer_names.reserve(effective_plan.enabled_layers.size());
  for (const auto &name : effective_plan.enabled_layers) {
    layer_names.push_back(name.c_str());
  }

  const char *native_operation = "create Vulkan instance";
  try {
    auto context = std::make_unique<vk::raii::Context>();
    auto callback = std::make_unique<CallbackState>();
    callback->sink = sink;

    vk::ApplicationInfo application_info{};
    application_info.pApplicationName = effective_plan.requested.application_name.empty()
                                            ? nullptr
                                            : effective_plan.requested.application_name.c_str();
    application_info.applicationVersion = effective_plan.requested.application_version;
    application_info.pEngineName = effective_plan.requested.engine_name.empty()
                                       ? nullptr
                                       : effective_plan.requested.engine_name.c_str();
    application_info.engineVersion = effective_plan.requested.engine_version;
    application_info.apiVersion = effective_plan.effective_api_version;

    vk::InstanceCreateInfo create_info{};
    create_info.pApplicationInfo = &application_info;
    create_info.enabledExtensionCount = abi_counts->extension_count;
    create_info.ppEnabledExtensionNames = extension_names.data();
    create_info.enabledLayerCount = abi_counts->layer_count;
    create_info.ppEnabledLayerNames = layer_names.data();

    vk::DebugUtilsMessengerCreateInfoEXT debug_create_info{};
    if (effective_plan.debug_utils_enabled) {
      debug_create_info.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                                          vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                                          vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                                          vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
      debug_create_info.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                                      vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                                      vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
      // Vulkan-Hpp exposes a type-safe callback typedef using vk:: enum
      // wrappers.  The underlying ABI is the native VKAPI callback; retaining
      // this explicit cast keeps the callback's VKAPI/noexcept/null-safe
      // boundary visible and portable across Vulkan-Hpp's wrapper types.
      debug_create_info.pfnUserCallback =
          reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(&instance_debug_callback);
      debug_create_info.pUserData = callback.get();
      create_info.pNext = &debug_create_info;
    }

    vk::raii::Instance native_instance{*context, create_info};
    std::optional<vk::raii::DebugUtilsMessengerEXT> messenger;
    if (effective_plan.debug_utils_enabled) {
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

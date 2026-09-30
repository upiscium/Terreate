#include <terreate/graphics/physical_device.hpp>

#include "instance_query.hpp"
#include "physical_device_query.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace terreate::graphics {

struct PhysicalDevice::QueryTag {};

namespace {

struct PhysicalDeviceState final {
  vk::PhysicalDevice native_handle{};
  vk::Instance parent_instance{};
  const void *authority = nullptr;
};

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
    case PhysicalDeviceError::invalid_view:
      return "the physical-device candidate has an invalid borrowed view";
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

template <typename CharacterRange>
[[nodiscard]] std::string copy_native_name(const CharacterRange &characters) {
  const auto first = characters.begin();
  const auto last = std::find(first, characters.end(), '\0');
  return std::string{first, last};
}

[[nodiscard]] std::string fixed_name(const auto &characters) {
  return copy_native_name(characters);
}

[[nodiscard]] bool has_name(const std::vector<std::string> &names, std::string_view name) noexcept {
  // PhysicalDeviceCapabilities is a public, caller-constructible snapshot;
  // do not assume that its extension names were canonicalised by the native
  // query path before evaluating them.
  return std::find(names.begin(), names.end(), name) != names.end();
}

[[nodiscard]] bool instance_ext(const InstancePlan &plan, std::string_view extension) noexcept {
  return std::binary_search(plan.enabledExtensions().begin(), plan.enabledExtensions().end(),
                            extension);
}

[[nodiscard]] bool queue_satisfies(const vk::QueueFamilyProperties &family,
                                   const PhysicalDeviceQueueRequirement &requirement) noexcept {
  const auto family_flags = static_cast<VkQueueFlags>(family.queueFlags);
  const auto required_flags = static_cast<VkQueueFlags>(requirement.flags);
  return (family_flags & required_flags) == required_flags &&
         family.queueCount >= requirement.min_queue_count;
}

[[nodiscard]] std::vector<std::size_t>
matching_queue_indices(const PhysicalDeviceCapabilities &capabilities,
                       const PhysicalDeviceQueueRequirement &requirement) {
  std::vector<std::size_t> indices;
  for (std::size_t index = 0; index < capabilities.queue_families.size(); ++index) {
    if (queue_satisfies(capabilities.queue_families[index], requirement)) {
      indices.push_back(index);
    }
  }
  return indices;
}

[[nodiscard]] bool same_uuid(const PhysicalDeviceUuid &left,
                             const PhysicalDeviceUuid &right) noexcept {
  return left == right;
}

[[nodiscard]] bool matches_available_uuid(const PhysicalDeviceCapabilities &capabilities,
                                          const PhysicalDeviceUuid &uuid) noexcept {
  // An unavailable ID-properties query leaves the value-owned UUID at its
  // zero-initialised value.  That value is not an observation and must never
  // satisfy an identity requirement or selection policy.
  return capabilities.id_properties_available && same_uuid(capabilities.device_uuid, uuid);
}

struct PhysicalDeviceNameRequirements {
  std::span<const std::string> required;
  std::span<const std::string> optional;
};

[[nodiscard]] std::optional<std::string>
validate_extension_names(PhysicalDeviceNameRequirements names) {
  for (const auto &name : names.required) {
    if (name.empty() || name.find('\0') != std::string::npos) {
      return "a required physical-device extension name must be non-empty and must not contain an "
             "embedded NUL byte";
    }
  }
  for (const auto &name : names.optional) {
    if (name.empty() || name.find('\0') != std::string::npos) {
      return "an optional physical-device extension name must be non-empty and must not contain an "
             "embedded NUL byte";
    }
  }
  for (const auto &name : names.required) {
    if (std::count(names.optional.begin(), names.optional.end(), name) != 0) {
      return "physical-device extension '" + name + "' was declared both required and optional";
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string>
validate_physical_requirements(const PhysicalDeviceRequirements &requirements) {
  if (requirements.minimum_api_version && *requirements.minimum_api_version == 0) {
    return "minimum_api_version must be non-zero";
  }
  const PhysicalDeviceNameRequirements extension_names{requirements.required_extensions,
                                                       requirements.optional_extensions};
  if (const auto error = validate_extension_names(extension_names)) {
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
make_physical_decision(PhysicalDeviceRequirementKind kind, RequirementStrength strength,
                       PhysicalDeviceRequirementOutcome outcome,
                       PhysicalDeviceRequirementReason reason,
                       PhysicalDeviceRequirementEvidence evidence) {
  return PhysicalDeviceRequirementDecision{
      .kind = kind,
      .strength = strength,
      .outcome = outcome,
      .reason = reason,
      .evidence = std::move(evidence),
  };
}

template <typename Evidence>
void add_decision(PhysicalDeviceEvaluation &evaluation, bool &matches,
                  PhysicalDeviceRequirementKind kind, RequirementStrength strength, bool supported,
                  Evidence evidence) {
  const auto outcome = supported ? PhysicalDeviceRequirementOutcome::accepted
                                 : PhysicalDeviceRequirementOutcome::declined;
  const auto reason = supported ? (strength == RequirementStrength::required
                                       ? PhysicalDeviceRequirementReason::requested
                                       : PhysicalDeviceRequirementReason::supported)
                                : PhysicalDeviceRequirementReason::unsupported;
  evaluation.decisions.push_back(make_physical_decision(
      kind, strength, outcome, reason, PhysicalDeviceRequirementEvidence{std::move(evidence)}));
  if (!supported && strength == RequirementStrength::required) {
    matches = false;
  }
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
void query_properties2_chain(const vk::PhysicalDevice &native,
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
void query_features2_chain(const vk::PhysicalDevice &native,
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
    const vk::PhysicalDevice &native, std::uint32_t effective_instance_api,
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

const PhysicalDevice::QueryTag *PhysicalDevice::query_tag() noexcept {
  static const QueryTag tag;
  return &tag;
}

PhysicalDevice::PhysicalDevice(vk::PhysicalDevice native_handle, vk::Instance parent_instance,
                               const QueryTag *query_tag_value) noexcept {
  if (query_tag_value != query_tag()) {
    return;
  }
  auto state = std::make_shared<PhysicalDeviceState>();
  state->native_handle = native_handle;
  state->parent_instance = parent_instance;
  state->authority = query_tag_value;
  state_ = std::shared_ptr<const void>{std::move(state)};
}

PhysicalDevice::operator bool() const noexcept { return valid(); }

bool PhysicalDevice::valid() const noexcept {
  const auto *state = static_cast<const PhysicalDeviceState *>(state_.get());
  return state != nullptr && state->authority == static_cast<const void *>(query_tag()) &&
         static_cast<VkPhysicalDevice>(state->native_handle) != VK_NULL_HANDLE &&
         static_cast<VkInstance>(state->parent_instance) != VK_NULL_HANDLE;
}

vk::PhysicalDevice PhysicalDevice::nativeHandle() const noexcept {
  const auto *state = static_cast<const PhysicalDeviceState *>(state_.get());
  return state == nullptr ? vk::PhysicalDevice{} : state->native_handle;
}

bool PhysicalDevice::correlatedWith(const Instance &instance) const noexcept {
  const auto *state = static_cast<const PhysicalDeviceState *>(state_.get());
  return valid() && instance.valid() && state->parent_instance == instance.nativeHandle();
}

terreate::Result<PhysicalDeviceInventory> queryPhysicalDevices(const Instance &instance) {
  try {
    const auto native_instance = instance.nativeHandle();
    const auto *plan = instance.plan();
    if (!instance.valid() || static_cast<VkInstance>(native_instance) == VK_NULL_HANDLE ||
        plan == nullptr) {
      return fail<PhysicalDeviceInventory>(
          PhysicalDeviceError::invalid_instance, "query Vulkan physical devices",
          "the supplied Instance is moved-from, invalid, or has no retained plan");
    }

    const bool properties2_extension =
        instance_ext(*plan, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    const bool external_memory_extension =
        instance_ext(*plan, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
    PhysicalDeviceInventory inventory;
    const auto native_devices = native_instance.enumeratePhysicalDevices();
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

      // This is deliberately based only on the device extension inventory.  An
      // instance extension with a similarly themed name cannot authorize a
      // device Properties2 chain member.
      const bool driver_properties_extension =
          has_name(extension_names, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
      auto capabilities =
          snapshot_device(native, plan->effectiveApiVersion(), properties2_extension,
                          external_memory_extension, driver_properties_extension, extension_names);
      capabilities.extension_properties = std::move(extension_properties);

      const bool memory_budget_supported =
          has_name(capabilities.extensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
      if (memory_budget_supported && capabilities.memory_properties2_available) {
        const auto memory_chain =
            native.getMemoryProperties2<vk::PhysicalDeviceMemoryProperties2,
                                        vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
        vk::PhysicalDeviceMemoryBudgetPropertiesEXT memory_budget =
            memory_chain.get<vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
        memory_budget.pNext = nullptr;
        capabilities.memory_budget = memory_budget;
      }

      inventory.candidates.push_back(PhysicalDeviceCandidate{
          .device = PhysicalDevice{vk::PhysicalDevice{native}, native_instance,
                                   PhysicalDevice::query_tag()},
          .capabilities = std::move(capabilities),
          .enumeration_index = index,
      });
    }
    return inventory;
  } catch (const vk::SystemError &error) {
    return std::unexpected(
        terreate::Error{error.code(), "query Vulkan physical devices", error.what()});
  }
}

terreate::Result<PhysicalDeviceEvaluation>
evaluatePhysicalDevice(const PhysicalDeviceCapabilities &capabilities,
                       const PhysicalDeviceRequirements &requirements) {
  if (const auto error = validate_physical_requirements(requirements)) {
    return fail<PhysicalDeviceEvaluation>(PhysicalDeviceError::invalid_requirements,
                                          "evaluate Vulkan physical device", *error);
  }

  PhysicalDeviceEvaluation evaluation;
  evaluation.candidate_identity.type = capabilities.properties.deviceType;
  evaluation.candidate_identity.vendor_id = capabilities.properties.vendorID;
  evaluation.candidate_identity.device_id = capabilities.properties.deviceID;
  evaluation.candidate_identity.device_name = fixed_name(capabilities.properties.deviceName);
  if (capabilities.id_properties_available) {
    evaluation.candidate_identity.uuid = capabilities.device_uuid;
  }

  bool matches = true;
  if (requirements.minimum_api_version) {
    add_decision(evaluation, matches, PhysicalDeviceRequirementKind::api_version,
                 RequirementStrength::required,
                 capabilities.api_version >= *requirements.minimum_api_version,
                 PhysicalDeviceApiEvidence{
                     .required_api_version = *requirements.minimum_api_version,
                     .available_api_version = capabilities.api_version,
                 });
  }
  if (requirements.required_device_uuid) {
    add_decision(evaluation, matches, PhysicalDeviceRequirementKind::uuid,
                 RequirementStrength::required,
                 matches_available_uuid(capabilities, *requirements.required_device_uuid),
                 PhysicalDeviceUuidEvidence{
                     .required_uuid = *requirements.required_device_uuid,
                     .available_uuid = capabilities.id_properties_available
                                           ? std::optional{capabilities.device_uuid}
                                           : std::nullopt,
                 });
  }
  if (requirements.required_device_type) {
    add_decision(evaluation, matches, PhysicalDeviceRequirementKind::type,
                 RequirementStrength::required,
                 capabilities.properties.deviceType == *requirements.required_device_type,
                 PhysicalDeviceTypeEvidence{
                     .required_type = *requirements.required_device_type,
                     .available_type = capabilities.properties.deviceType,
                 });
  }
  for (const auto &extension : requirements.required_extensions) {
    add_decision(evaluation, matches, PhysicalDeviceRequirementKind::extension,
                 RequirementStrength::required, has_name(capabilities.extensions, extension),
                 PhysicalDeviceExtensionEvidence{
                     .extension = extension,
                     .available = has_name(capabilities.extensions, extension),
                 });
  }
  for (const auto &extension : requirements.optional_extensions) {
    add_decision(evaluation, matches, PhysicalDeviceRequirementKind::extension,
                 RequirementStrength::optional, has_name(capabilities.extensions, extension),
                 PhysicalDeviceExtensionEvidence{
                     .extension = extension,
                     .available = has_name(capabilities.extensions, extension),
                 });
  }

  const auto required_queues = queue_requirements_with_masks(requirements.required_queue_families,
                                                             requirements.required_queue_flags);
  const auto optional_queues = queue_requirements_with_masks(requirements.optional_queue_families,
                                                             requirements.optional_queue_flags);
  for (const auto &queue : required_queues) {
    auto indices = matching_queue_indices(capabilities, queue);
    const bool supported = !indices.empty();
    add_decision(evaluation, matches, PhysicalDeviceRequirementKind::queue,
                 RequirementStrength::required, supported,
                 PhysicalDeviceQueueEvidence{
                     .requirement = queue,
                     .matching_queue_indices = std::move(indices),
                 });
  }
  for (const auto &queue : optional_queues) {
    auto indices = matching_queue_indices(capabilities, queue);
    const bool supported = !indices.empty();
    add_decision(evaluation, matches, PhysicalDeviceRequirementKind::queue,
                 RequirementStrength::optional, supported,
                 PhysicalDeviceQueueEvidence{
                     .requirement = queue,
                     .matching_queue_indices = std::move(indices),
                 });
  }

  evaluation.matches = matches;
  return evaluation;
}

terreate::Result<PhysicalDeviceEvaluation>
evaluatePhysicalDevice(const PhysicalDeviceCandidate &candidate,
                       const PhysicalDeviceRequirements &requirements) {
  return evaluatePhysicalDevice(candidate.capabilities, requirements);
}

terreate::Result<PhysicalDeviceSelection>
selectPhysicalDevice(const PhysicalDeviceCandidate &candidate,
                     const PhysicalDeviceRequirements &requirements) {
  if (!candidate.device.valid()) {
    return fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::invalid_view, "select Vulkan physical device",
        "the manually supplied physical-device candidate has no valid native view "
        "and parent token");
  }
  const auto evaluation = evaluatePhysicalDevice(candidate, requirements);
  if (!evaluation) {
    return std::unexpected(evaluation.error());
  }
  if (!evaluation->matches) {
    return fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::no_match, "select Vulkan physical device",
        "the manually supplied physical-device candidate does not satisfy the requirements");
  }
  return PhysicalDeviceSelection{
      .candidate = candidate,
      .reason = PhysicalDeviceSelectionReason::sole_match,
      .policy_index = std::nullopt,
  };
}

terreate::Result<PhysicalDeviceSelection>
selectPhysicalDevice(std::span<const PhysicalDeviceCandidate> candidates,
                     const PhysicalDeviceRequirements &requirements,
                     const PhysicalDeviceSelectionPolicy &policy) {
  if (const auto error = validate_physical_requirements(requirements)) {
    return fail<PhysicalDeviceSelection>(PhysicalDeviceError::invalid_requirements,
                                         "select Vulkan physical device", *error);
  }
  if (policy.explicit_uuid && !policy.uuid_order.empty()) {
    return fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::invalid_requirements, "select Vulkan physical device",
        "explicit UUID selection cannot be combined with UUID ordering");
  }
  if (policy.explicit_uuid && requirements.required_device_uuid &&
      !same_uuid(*policy.explicit_uuid, *requirements.required_device_uuid)) {
    return fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::invalid_requirements, "select Vulkan physical device",
        "explicit UUID selection conflicts with required_device_uuid");
  }
  for (std::size_t left = 0; left < policy.uuid_order.size(); ++left) {
    if (std::find(policy.uuid_order.begin() + static_cast<std::ptrdiff_t>(left + 1),
                  policy.uuid_order.end(), policy.uuid_order[left]) != policy.uuid_order.end()) {
      return fail<PhysicalDeviceSelection>(PhysicalDeviceError::invalid_requirements,
                                           "select Vulkan physical device",
                                           "uuid_order must not contain duplicate UUIDs");
    }
  }

  std::vector<const PhysicalDeviceCandidate *> matches;
  matches.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    if (!candidate.device.valid()) {
      return fail<PhysicalDeviceSelection>(
          PhysicalDeviceError::invalid_view, "select Vulkan physical device",
          "an enumerated physical-device candidate has no valid native view and parent token");
    }
    const auto evaluation = evaluatePhysicalDevice(candidate, requirements);
    if (!evaluation) {
      return std::unexpected(evaluation.error());
    }
    if (evaluation->matches) {
      matches.push_back(&candidate);
    }
  }

  if (matches.empty()) {
    return fail<PhysicalDeviceSelection>(
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
      return fail<PhysicalDeviceSelection>(
          PhysicalDeviceError::no_match, "select Vulkan physical device",
          "the explicitly selected device UUID is not a matching candidate");
    }
    if (uuid_matches.size() != 1) {
      return fail<PhysicalDeviceSelection>(
          PhysicalDeviceError::ambiguous_match, "select Vulkan physical device",
          "the explicitly selected UUID identifies multiple matching candidates");
    }
    return PhysicalDeviceSelection{
        .candidate = *uuid_matches.front(),
        .reason = PhysicalDeviceSelectionReason::explicit_uuid,
        .policy_index = std::nullopt,
    };
  }

  if (!policy.uuid_order.empty()) {
    for (std::size_t policy_index = 0; policy_index < policy.uuid_order.size(); ++policy_index) {
      const auto &uuid = policy.uuid_order[policy_index];
      std::vector<const PhysicalDeviceCandidate *> uuid_matches;
      for (const auto *candidate : matches) {
        if (matches_available_uuid(candidate->capabilities, uuid)) {
          uuid_matches.push_back(candidate);
        }
      }
      if (uuid_matches.size() > 1) {
        return fail<PhysicalDeviceSelection>(
            PhysicalDeviceError::ambiguous_match, "select Vulkan physical device",
            "the explicit UUID ordering contains a UUID shared by multiple candidates");
      }
      if (uuid_matches.size() == 1) {
        return PhysicalDeviceSelection{
            .candidate = *uuid_matches.front(),
            .reason = PhysicalDeviceSelectionReason::uuid_order,
            .policy_index = policy_index,
        };
      }
    }
    return fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::no_match, "select Vulkan physical device",
        "the explicit UUID ordering contains no matching candidate");
  }

  if (matches.size() != 1) {
    return fail<PhysicalDeviceSelection>(
        PhysicalDeviceError::ambiguous_match, "select Vulkan physical device",
        "multiple candidates matched and no explicit UUID selection policy was supplied");
  }
  return PhysicalDeviceSelection{
      .candidate = *matches.front(),
      .reason = PhysicalDeviceSelectionReason::sole_match,
      .policy_index = std::nullopt,
  };
}

} // namespace terreate::graphics

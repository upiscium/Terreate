#ifndef TERREATE_GRAPHICS_PHYSICAL_DEVICE_HPP
#define TERREATE_GRAPHICS_PHYSICAL_DEVICE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <terreate/core/result.hpp>
#include <terreate/graphics/instance.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace terreate::graphics {

class Instance;

/// The UUID is a value-owned copy of VkPhysicalDeviceIDProperties::deviceUUID.
/// It is deliberately an array rather than a string: Vulkan UUIDs are binary
/// values and may contain zero bytes.
using PhysicalDeviceUuid = std::array<std::uint8_t, VK_UUID_SIZE>;
using PhysicalDeviceUUID = PhysicalDeviceUuid;

/// A borrowed PhysicalDevice handle.  Physical devices are owned by Vulkan's
/// parent instance, not by this value.  The opaque identity token is copied
/// from the owning Instance implementation allocation and is only compared;
/// it is never dereferenced and therefore remains correlated when Instance is
/// moved to another C++ object.
class PhysicalDevice {
public:
  constexpr PhysicalDevice() noexcept = default;

  /// Construct a borrowed view from an already-observed native handle.  The
  /// token is opaque and non-owning; callers must keep its parent Instance alive
  /// for every native-handle operation.
  constexpr PhysicalDevice(vk::PhysicalDevice native_handle, const void *instance_identity) noexcept
      : native_handle_(native_handle), instance_identity_(instance_identity) {}

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] constexpr bool valid() const noexcept {
    return static_cast<VkPhysicalDevice>(native_handle_) != VK_NULL_HANDLE &&
           instance_identity_ != nullptr;
  }

  /// Return a borrowed native handle.  This value never destroys the Vulkan
  /// physical device and must not outlive the parent Instance.
  [[nodiscard]] constexpr vk::PhysicalDevice nativeHandle() const noexcept {
    return native_handle_;
  }
  [[nodiscard]] constexpr operator vk::PhysicalDevice() const noexcept { return nativeHandle(); }

  /// Return the opaque parent implementation identity without dereferencing
  /// it.  The identity is stable across Instance move construction and move
  /// assignment, but becomes non-usable when the parent Instance is destroyed.
  [[nodiscard]] constexpr const void *instanceIdentity() const noexcept {
    return instance_identity_;
  }
  [[nodiscard]] constexpr const void *parentIdentity() const noexcept { return instanceIdentity(); }
  [[nodiscard]] constexpr const void *implementationIdentity() const noexcept {
    return instanceIdentity();
  }

  /// Compare only the stable opaque identity token.  No parent implementation
  /// is accessed by this operation.
  [[nodiscard]] bool correlatedWith(const Instance &instance) const noexcept;

private:
  vk::PhysicalDevice native_handle_{};
  const void *instance_identity_ = nullptr;
};

/// A copied device-extension property.  `name` is owned and the collection in
/// PhysicalDeviceCapabilities is canonicalised lexicographically without
/// duplicates.
struct PhysicalDeviceExtensionProperty {
  std::string name{};
  std::uint32_t spec_version = 0;
};

/// A queue-family constraint used by pure requirement evaluation.  `flags` is
/// a required subset of a family's queue flags and `min_queue_count` makes the
/// count requirement explicit rather than inferred from enumeration order.
struct PhysicalDeviceQueueRequirement {
  vk::QueueFlags flags{};
  std::uint32_t min_queue_count = 1;
};
using QueueFamilyRequirement = PhysicalDeviceQueueRequirement;

/// All observations belonging to one physical device.  Every Vulkan structure
/// is an eager value copy; pNext pointers in snapshots produced by the native
/// query are cleared before this object is returned.  The public members also
/// make synthetic, allocation-free capability fixtures straightforward.
struct PhysicalDeviceCapabilities {
  std::uint32_t api_version = VK_API_VERSION_1_0;

  /// Availability describes the valid native query chain, not merely whether
  /// the corresponding value object is present in this snapshot.
  bool properties2_available = false;
  bool id_properties_available = false;
  bool driver_properties_available = false;
  bool features2_available = false;
  bool features_11_available = false;
  bool features_12_available = false;
  bool features_13_available = false;
  bool memory_properties2_available = false;

  vk::PhysicalDeviceProperties properties{};
  vk::PhysicalDeviceProperties2 properties2{};
  vk::PhysicalDeviceIDProperties id_properties{};
  vk::PhysicalDeviceDriverProperties driver_properties{};

  /// UUID fields are valid identity observations only when ID properties are
  /// available.  Otherwise their zero-initialised values must not be matched.
  PhysicalDeviceUuid device_uuid{};
  PhysicalDeviceUuid driver_uuid{};
  std::uint32_t driver_id = 0;
  std::string driver_name{};
  std::string driver_info{};

  /// Native feature observations are retained as owned, pNext-free snapshots
  /// for callers that need to inspect them manually.  They are not generic
  /// requirement inputs and are never interpreted as enable requests here.
  vk::PhysicalDeviceFeatures features_10{};
  vk::PhysicalDeviceFeatures2 features2{};
  vk::PhysicalDeviceVulkan11Features features_11{};
  vk::PhysicalDeviceVulkan12Features features_12{};
  vk::PhysicalDeviceVulkan13Features features_13{};

  /// Canonical extension names used by pure requirement evaluation.
  std::vector<std::string> extensions{};
  /// The corresponding copied spec versions, sorted by name and deduplicated.
  std::vector<PhysicalDeviceExtensionProperty> extension_properties{};

  std::vector<vk::QueueFamilyProperties> queue_families{};
  vk::PhysicalDeviceMemoryProperties memory_properties{};
  /// This is a point-in-time observation.  It is absent when
  /// VK_EXT_memory_budget was not reported or the memory-properties2 query was
  /// not available for the parent instance API.
  std::optional<vk::PhysicalDeviceMemoryBudgetPropertiesEXT> memory_budget{};

  [[nodiscard]] const vk::PhysicalDeviceProperties &propertiesSnapshot() const noexcept {
    return properties;
  }
  [[nodiscard]] const vk::PhysicalDeviceProperties2 &properties2Snapshot() const noexcept {
    return properties2;
  }
  [[nodiscard]] const vk::PhysicalDeviceIDProperties &idProperties() const noexcept {
    return id_properties;
  }
  [[nodiscard]] const vk::PhysicalDeviceDriverProperties &driverProperties() const noexcept {
    return driver_properties;
  }
  [[nodiscard]] const PhysicalDeviceUuid &deviceUuid() const noexcept { return device_uuid; }
  [[nodiscard]] const PhysicalDeviceUuid &driverUuid() const noexcept { return driver_uuid; }
  [[nodiscard]] std::uint32_t driverId() const noexcept { return driver_id; }
  [[nodiscard]] const std::string &driverName() const noexcept { return driver_name; }
  [[nodiscard]] const std::string &driverInfo() const noexcept { return driver_info; }
  [[nodiscard]] const vk::PhysicalDeviceFeatures &features10() const noexcept {
    return features_10;
  }
  [[nodiscard]] const vk::PhysicalDeviceFeatures2 &featuresSnapshot() const noexcept {
    return features2;
  }
  [[nodiscard]] const vk::PhysicalDeviceVulkan11Features &features11() const noexcept {
    return features_11;
  }
  [[nodiscard]] const vk::PhysicalDeviceVulkan12Features &features12() const noexcept {
    return features_12;
  }
  [[nodiscard]] const vk::PhysicalDeviceVulkan13Features &features13() const noexcept {
    return features_13;
  }
  [[nodiscard]] const std::vector<std::string> &extensionNames() const noexcept {
    return extensions;
  }
  [[nodiscard]] const std::vector<PhysicalDeviceExtensionProperty> &
  extensionProperties() const noexcept {
    return extension_properties;
  }
  [[nodiscard]] const std::vector<vk::QueueFamilyProperties> &queueFamilies() const noexcept {
    return queue_families;
  }
  [[nodiscard]] const vk::PhysicalDeviceMemoryProperties &memoryProperties() const noexcept {
    return memory_properties;
  }
  [[nodiscard]] const std::optional<vk::PhysicalDeviceMemoryBudgetPropertiesEXT> &
  memoryBudget() const noexcept {
    return memory_budget;
  }
};

/// A query result item: the borrowed native view is correlated with the
/// eagerly-owned capability snapshot and carries the original enumeration
/// position only as provenance.  Selection never uses that position as a
/// hidden score or fallback policy.
struct PhysicalDeviceCandidate {
  PhysicalDevice device{};
  PhysicalDeviceCapabilities capabilities{};
  std::size_t enumeration_index = 0;

  [[nodiscard]] const PhysicalDevice &view() const noexcept { return device; }
  [[nodiscard]] const PhysicalDevice &physicalDevice() const noexcept { return device; }
  [[nodiscard]] const PhysicalDeviceCapabilities &snapshot() const noexcept { return capabilities; }
};

/// A successful enumeration, including the valid empty-inventory case.  The
/// vector-like observers keep the result useful in range-for and synthetic
/// tests without exposing a native ownership type.
struct PhysicalDeviceInventory {
  std::vector<PhysicalDeviceCandidate> candidates{};

  [[nodiscard]] bool empty() const noexcept { return candidates.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return candidates.size(); }
  [[nodiscard]] PhysicalDeviceCandidate &operator[](std::size_t index) noexcept {
    return candidates[index];
  }
  [[nodiscard]] const PhysicalDeviceCandidate &operator[](std::size_t index) const noexcept {
    return candidates[index];
  }
  [[nodiscard]] PhysicalDeviceCandidate &front() noexcept { return candidates.front(); }
  [[nodiscard]] const PhysicalDeviceCandidate &front() const noexcept { return candidates.front(); }
  [[nodiscard]] PhysicalDeviceCandidate &back() noexcept { return candidates.back(); }
  [[nodiscard]] const PhysicalDeviceCandidate &back() const noexcept { return candidates.back(); }
  [[nodiscard]] PhysicalDeviceCandidate &at(std::size_t index) { return candidates.at(index); }
  [[nodiscard]] const PhysicalDeviceCandidate &at(std::size_t index) const {
    return candidates.at(index);
  }
  [[nodiscard]] auto begin() noexcept { return candidates.begin(); }
  [[nodiscard]] auto end() noexcept { return candidates.end(); }
  [[nodiscard]] auto begin() const noexcept { return candidates.begin(); }
  [[nodiscard]] auto end() const noexcept { return candidates.end(); }
  [[nodiscard]] const std::vector<PhysicalDeviceCandidate> &view() const noexcept {
    return candidates;
  }
};

enum class PhysicalDeviceRequirementOutcome : std::uint8_t {
  accepted,
  declined,
};

enum class PhysicalDeviceRequirementReason : std::uint8_t {
  requested,
  supported,
  unsupported,
  accepted_by_uuid,
};

struct PhysicalDeviceRequirementDecision {
  std::string name{};
  RequirementStrength strength = RequirementStrength::optional;
  PhysicalDeviceRequirementOutcome outcome = PhysicalDeviceRequirementOutcome::accepted;
  PhysicalDeviceRequirementReason reason = PhysicalDeviceRequirementReason::supported;
};

/// Explicit requirements understood by the pure evaluator are limited to API
/// version, device identity/type, extensions, and queue criteria.  An empty
/// field is unspecified; observed support, including native feature snapshots,
/// is never promoted into an implicit request.  Feature selection and enabling
/// belong to logical-device creation, not physical-device evaluation.
struct PhysicalDeviceRequirements {
  std::optional<std::uint32_t> minimum_api_version{};
  std::optional<PhysicalDeviceUuid> required_device_uuid{};
  std::optional<vk::PhysicalDeviceType> required_device_type{};

  std::vector<std::string> required_extensions{};
  std::vector<std::string> optional_extensions{};

  std::vector<PhysicalDeviceQueueRequirement> required_queue_families{};
  std::vector<PhysicalDeviceQueueRequirement> optional_queue_families{};
  /// Convenience masks for callers that do not need a queue-count constraint.
  std::vector<vk::QueueFlags> required_queue_flags{};
  std::vector<vk::QueueFlags> optional_queue_flags{};
};

struct PhysicalDeviceEvaluation {
  bool matches = false;
  std::vector<PhysicalDeviceRequirementDecision> decisions{};

  [[nodiscard]] bool matched() const noexcept { return matches; }
  [[nodiscard]] const std::vector<PhysicalDeviceRequirementDecision> &
  requirementDecisions() const noexcept {
    return decisions;
  }
};

/// Selection policy is deliberately explicit.  With no UUID policy, more than
/// one matching candidate is an ambiguity.  A UUID order is a caller-owned
/// preference list copied/observed for this call; enumeration order is never a
/// fallback.  `explicit_uuid` is the manual-selection form.
struct PhysicalDeviceSelectionPolicy {
  std::optional<PhysicalDeviceUuid> explicit_uuid{};
  std::vector<PhysicalDeviceUuid> uuid_order{};
};

enum class PhysicalDeviceError : std::uint8_t {
  invalid_instance = 1,
  invalid_requirements = 2,
  no_match = 3,
  ambiguous_match = 4,

  // Short aliases keep the three semantic selection outcomes easy to inspect
  // without introducing a second error category.
  invalid = invalid_requirements,
  ambiguity = ambiguous_match,
};

[[nodiscard]] const std::error_category &physical_device_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(PhysicalDeviceError error) noexcept;

/// Enumerate physical devices and eagerly copy every supported observation.
/// A successful zero-device enumeration returns an empty inventory; it is not
/// converted into an error.  vk::SystemError codes from enumeration or any
/// per-device native query are preserved in the returned terreate::Error.
[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
queryPhysicalDevices(const Instance &instance);

[[nodiscard]] inline terreate::Result<PhysicalDeviceInventory>
queryPhysicalDeviceCapabilities(const Instance &instance) {
  return queryPhysicalDevices(instance);
}

[[nodiscard]] inline terreate::Result<PhysicalDeviceInventory>
queryPhysicalDeviceCandidates(const Instance &instance) {
  return queryPhysicalDevices(instance);
}

[[nodiscard]] inline terreate::Result<PhysicalDeviceInventory>
enumeratePhysicalDevices(const Instance &instance) {
  return queryPhysicalDevices(instance);
}

/// Evaluate only explicit requirements against one candidate.  No native
/// calls, global state, enumeration order, score, or mutation is involved.
[[nodiscard]] terreate::Result<PhysicalDeviceEvaluation>
evaluatePhysicalDevice(const PhysicalDeviceCandidate &candidate,
                       const PhysicalDeviceRequirements &requirements);

[[nodiscard]] terreate::Result<PhysicalDeviceEvaluation>
evaluatePhysicalDevice(const PhysicalDeviceCapabilities &capabilities,
                       const PhysicalDeviceRequirements &requirements);

/// Select one candidate after pure evaluation.  A single match is selected;
/// zero matches and multiple matches are distinct errors.  Multiple matches
/// can only be resolved by an explicit UUID policy.
[[nodiscard]] terreate::Result<PhysicalDeviceCandidate>
selectPhysicalDevice(std::span<const PhysicalDeviceCandidate> candidates,
                     const PhysicalDeviceRequirements &requirements,
                     const PhysicalDeviceSelectionPolicy &policy = {});

/// Manual selection of one already chosen candidate.  This overload never
/// compares or ranks other candidates.
[[nodiscard]] terreate::Result<PhysicalDeviceCandidate>
selectPhysicalDevice(const PhysicalDeviceCandidate &candidate,
                     const PhysicalDeviceRequirements &requirements);

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::PhysicalDeviceError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_PHYSICAL_DEVICE_HPP

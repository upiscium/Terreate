#ifndef TERREATE_GRAPHICS_DEVICE_HPP
#define TERREATE_GRAPHICS_DEVICE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <terreate/core/result.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace terreate::graphics {

class Device;
class DevicePlan;
class Queue;
struct DevicePlanBuilder;

/// The Vulkan feature structure family represented by a feature decision.
enum class DeviceFeatureVersion : std::uint8_t {
  vulkan_1_0,
  vulkan_1_1,
  vulkan_1_2,
  vulkan_1_3,
};

enum class DeviceApiVersionProvenance : std::uint8_t {
  explicit_request,
  capability_snapshot,
};

/// Queue aliasing is a relation between one request and the request named by
/// `DeviceQueueRequest::alias_target`.  There is deliberately no implicit
/// "allow aliasing" switch: every alias is described by a target and one of
/// these policies.
enum class DeviceQueueAliasPolicy : std::uint8_t {
  forbid_alias,
  require_alias,
  prefer_alias_with_distinct_fallback,
  prefer_distinct_with_alias_fallback,
};

/// One logical queue demand.  A request describes exactly one logical queue;
/// there is no count field.  `caller_id` is the stable identity used by the
/// resolver and by the resulting caller-to-allocation relation.  Family choice
/// is restricted to `allowed_family_indices`; `preferred_family_indices` is an
/// ordered tie-breaker within that whitelist.
struct DeviceQueueRequest {
  std::string caller_id{};
  RequirementStrength strength = RequirementStrength::required;
  vk::QueueFlags required_flags{};
  std::vector<std::size_t> allowed_family_indices{};
  std::vector<std::size_t> preferred_family_indices{};
  float priority = 1.0F;

  DeviceQueueAliasPolicy alias_policy = DeviceQueueAliasPolicy::forbid_alias;
  std::string alias_target{};
};

/// The explicit logical-device request.  Feature structures are value-owned
/// request masks: VK_TRUE means requested and VK_FALSE means unspecified.
/// The resolver validates and snapshots all structures with a null pNext.
struct DeviceDescription {
  std::optional<std::uint32_t> api_version{};

  std::vector<std::string> required_extensions{};
  std::vector<std::string> optional_extensions{};

  vk::PhysicalDeviceFeatures required_features{};
  vk::PhysicalDeviceFeatures optional_features{};
  vk::PhysicalDeviceVulkan11Features required_features_11{};
  vk::PhysicalDeviceVulkan11Features optional_features_11{};
  vk::PhysicalDeviceVulkan12Features required_features_12{};
  vk::PhysicalDeviceVulkan12Features optional_features_12{};
  vk::PhysicalDeviceVulkan13Features required_features_13{};
  vk::PhysicalDeviceVulkan13Features optional_features_13{};

  std::vector<DeviceQueueRequest> queue_requests{};
};

enum class DeviceDecisionOutcome : std::uint8_t {
  accepted,
  declined,
};

enum class DeviceDecisionReason : std::uint8_t {
  requested,
  supported,
  unsupported,
  accepted_by_aliasing,
  preferred_family,
  fallback_family,
  insufficient_queue_count,
  ambiguous,
  conflicting_priority,
};

/// The extension decision retains every stage of the resolution evidence.
/// `requested` is explicit caller intent, `supported` is capability evidence,
/// and `effective` is the value selected for the native plan.
struct DeviceExtensionDecision {
  std::string name{};
  RequirementStrength strength = RequirementStrength::optional;
  bool requested = true;
  bool supported = false;
  bool effective = false;
  DeviceDecisionOutcome outcome = DeviceDecisionOutcome::declined;
  DeviceDecisionReason reason = DeviceDecisionReason::unsupported;
};

/// A feature decision identifies both its Vulkan structure and its member.
/// This avoids making a bare member name ambiguous across feature versions.
struct DeviceFeatureDecision {
  DeviceFeatureVersion version = DeviceFeatureVersion::vulkan_1_0;
  std::string structure{};
  std::string member{};
  RequirementStrength strength = RequirementStrength::optional;
  bool requested = true;
  bool supported = false;
  bool effective = false;
  DeviceDecisionOutcome outcome = DeviceDecisionOutcome::declined;
  DeviceDecisionReason reason = DeviceDecisionReason::unsupported;
};

enum class DeviceQueueProvenance : std::uint8_t {
  explicit_required,
  explicit_optional,
  preferred_family,
  fallback_family,
  alias,
};

struct DeviceQueueDecision {
  std::string caller_id{};
  RequirementStrength strength = RequirementStrength::optional;
  bool requested = true;
  bool supported = false;
  bool effective = false;
  DeviceDecisionOutcome outcome = DeviceDecisionOutcome::declined;
  DeviceDecisionReason reason = DeviceDecisionReason::unsupported;
  DeviceQueueProvenance provenance = DeviceQueueProvenance::explicit_optional;
  std::optional<std::size_t> family_index{};
  std::optional<std::size_t> queue_index{};
  std::optional<std::size_t> alias_group{};
  std::optional<std::string> alias_target{};
};

/// One unique native queue allocation.  Native creation contains one queue
/// priority for each value in this collection.
struct DeviceQueueAllocation {
  std::size_t family_index = 0;
  std::size_t queue_index = 0;
  float priority = 1.0F;
  std::size_t alias_group = 0;
};

/// The effective relation for one logical request.  Every accepted request
/// points at exactly one native allocation; aliases point at the same
/// allocation index as their target.
struct DeviceQueueAssignment {
  std::string caller_id{};
  RequirementStrength strength = RequirementStrength::optional;
  DeviceQueueProvenance provenance = DeviceQueueProvenance::explicit_optional;
  std::size_t allocation_index = 0;
  std::size_t family_index = 0;
  std::size_t queue_index = 0;
  std::optional<std::size_t> alias_group{};
  std::optional<std::string> alias_target{};
};

struct DeviceQueueAliasGroup {
  std::size_t index = 0;
  std::size_t allocation_index = 0;
  std::size_t family_index = 0;
  std::size_t queue_index = 0;
  std::vector<std::string> caller_ids{};
};

/// A successful, immutable, deterministic pre-native logical-device plan.
/// The request and capability snapshots are owned values.  Vulkan feature
/// structures in the request and effective plan are pNext-free snapshots.
/// Native creation consumes this plan mechanically and never resolves again.
class DevicePlan final {
public:
  DevicePlan(const DevicePlan &) = default;
  DevicePlan &operator=(const DevicePlan &) = default;
  DevicePlan(DevicePlan &&) noexcept = default;
  DevicePlan &operator=(DevicePlan &&) noexcept = default;
  ~DevicePlan() = default;

  [[nodiscard]] const DeviceDescription &requested() const noexcept { return data_.requested; }
  [[nodiscard]] const PhysicalDevice &physicalDevice() const noexcept {
    return data_.physical_device;
  }
  [[nodiscard]] const PhysicalDeviceCapabilities &capabilities() const noexcept {
    return data_.capabilities;
  }
  [[nodiscard]] std::size_t enumerationIndex() const noexcept { return data_.enumeration_index; }
  [[nodiscard]] bool hasPhysicalDevice() const noexcept { return data_.physical_device.valid(); }

  [[nodiscard]] std::uint32_t effectiveApiVersion() const noexcept {
    return data_.effective_api_version;
  }
  [[nodiscard]] DeviceApiVersionProvenance apiVersionProvenance() const noexcept {
    return data_.api_version_provenance;
  }
  [[nodiscard]] const std::vector<std::string> &enabledExtensions() const noexcept {
    return data_.enabled_extensions;
  }

  [[nodiscard]] const vk::PhysicalDeviceFeatures &enabledFeatures() const noexcept {
    return data_.enabled_features_10;
  }
  [[nodiscard]] const vk::PhysicalDeviceFeatures &enabledFeatures10() const noexcept {
    return data_.enabled_features_10;
  }
  [[nodiscard]] const vk::PhysicalDeviceVulkan11Features &enabledFeatures11() const noexcept {
    return data_.enabled_features_11;
  }
  [[nodiscard]] const vk::PhysicalDeviceVulkan12Features &enabledFeatures12() const noexcept {
    return data_.enabled_features_12;
  }
  [[nodiscard]] const vk::PhysicalDeviceVulkan13Features &enabledFeatures13() const noexcept {
    return data_.enabled_features_13;
  }

  [[nodiscard]] const std::vector<DeviceFeatureDecision> &featureDecisions() const noexcept {
    return data_.feature_decisions;
  }
  [[nodiscard]] const std::vector<DeviceExtensionDecision> &extensionDecisions() const noexcept {
    return data_.extension_decisions;
  }
  [[nodiscard]] const std::vector<DeviceQueueDecision> &queueDecisions() const noexcept {
    return data_.queue_decisions;
  }
  [[nodiscard]] const std::vector<DeviceQueueAllocation> &queueAllocations() const noexcept {
    return data_.queue_allocations;
  }
  [[nodiscard]] const std::vector<DeviceQueueAssignment> &queueAssignments() const noexcept {
    return data_.queue_assignments;
  }
  [[nodiscard]] const std::vector<DeviceQueueAliasGroup> &queueAliasGroups() const noexcept {
    return data_.queue_alias_groups;
  }
  [[nodiscard]] const std::vector<float> &queuePriorities() const noexcept {
    return data_.queue_priorities;
  }

private:
  struct Data {
    DeviceDescription requested{};
    PhysicalDevice physical_device{};
    PhysicalDeviceCapabilities capabilities{};
    std::size_t enumeration_index = 0;
    std::uint32_t effective_api_version = VK_API_VERSION_1_0;
    DeviceApiVersionProvenance api_version_provenance =
        DeviceApiVersionProvenance::capability_snapshot;
    std::vector<std::string> enabled_extensions{};
    vk::PhysicalDeviceFeatures enabled_features_10{};
    vk::PhysicalDeviceVulkan11Features enabled_features_11{};
    vk::PhysicalDeviceVulkan12Features enabled_features_12{};
    vk::PhysicalDeviceVulkan13Features enabled_features_13{};
    std::vector<DeviceFeatureDecision> feature_decisions{};
    std::vector<DeviceExtensionDecision> extension_decisions{};
    std::vector<DeviceQueueDecision> queue_decisions{};
    std::vector<DeviceQueueAllocation> queue_allocations{};
    std::vector<DeviceQueueAssignment> queue_assignments{};
    std::vector<DeviceQueueAliasGroup> queue_alias_groups{};
    std::vector<float> queue_priorities{};
  };

  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  DevicePlan(DeviceDescription requested, PhysicalDevice physical_device,
             PhysicalDeviceCapabilities capabilities, std::size_t enumeration_index,
             std::uint32_t effective_api_version, std::vector<std::string> enabled_extensions,
             DeviceApiVersionProvenance api_version_provenance,
             vk::PhysicalDeviceFeatures enabled_features_10,
             vk::PhysicalDeviceVulkan11Features enabled_features_11,
             vk::PhysicalDeviceVulkan12Features enabled_features_12,
             vk::PhysicalDeviceVulkan13Features enabled_features_13,
             std::vector<DeviceFeatureDecision> feature_decisions,
             std::vector<DeviceExtensionDecision> extension_decisions,
             std::vector<DeviceQueueDecision> queue_decisions,
             std::vector<DeviceQueueAllocation> queue_allocations,
             std::vector<DeviceQueueAssignment> queue_assignments,
             std::vector<DeviceQueueAliasGroup> queue_alias_groups,
             std::vector<float> queue_priorities)
      : data_{} {
    data_.requested = std::move(requested);
    data_.physical_device = physical_device;
    data_.capabilities = std::move(capabilities);
    data_.enumeration_index = enumeration_index;
    data_.effective_api_version = effective_api_version;
    data_.api_version_provenance = api_version_provenance;
    data_.enabled_extensions = std::move(enabled_extensions);
    data_.enabled_features_10 = enabled_features_10;
    data_.enabled_features_11 = enabled_features_11;
    data_.enabled_features_12 = enabled_features_12;
    data_.enabled_features_13 = enabled_features_13;
    data_.feature_decisions = std::move(feature_decisions);
    data_.extension_decisions = std::move(extension_decisions);
    data_.queue_decisions = std::move(queue_decisions);
    data_.queue_allocations = std::move(queue_allocations);
    data_.queue_assignments = std::move(queue_assignments);
    data_.queue_alias_groups = std::move(queue_alias_groups);
    data_.queue_priorities = std::move(queue_priorities);
  }
  // NOLINTEND(bugprone-easily-swappable-parameters)

  Data data_{};

  friend auto resolveDevice(const PhysicalDeviceCandidate &, const DeviceDescription &)
      -> terreate::Result<DevicePlan>;
  friend auto resolveDevice(const PhysicalDeviceCapabilities &, const DeviceDescription &)
      -> terreate::Result<DevicePlan>;
  friend struct DevicePlanBuilder;
};

using DevicePlanResult = terreate::Result<DevicePlan>;

enum class DeviceError : std::uint8_t {
  invalid_description = 1,
  contradictory_requirements = 2,
  unsupported_api_version = 3,
  missing_required_extension = 4,
  unsupported_required_feature = 5,
  invalid_feature_chain = 6,
  invalid_queue_request = 7,
  insufficient_queue_count = 8,
  ambiguous_queue = 9,
  invalid_instance = 10,
  invalid_physical_device = 11,
  parent_mismatch = 12,
  invalid_plan = 13,
};

[[nodiscard]] const std::error_category &device_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(DeviceError error) noexcept;

/// Resolve using only the candidate's owned capability snapshot.  No native
/// calls or mutable global state are consulted.
[[nodiscard]] DevicePlanResult resolveDevice(const PhysicalDeviceCandidate &candidate,
                                             const DeviceDescription &description);

[[nodiscard]] DevicePlanResult resolveDevice(const PhysicalDeviceCapabilities &capabilities,
                                             const DeviceDescription &description);

/// A move-only exclusive owner of one Vulkan logical device.  Device does not
/// expose waitIdle.  The Instance supplied to createDevice and the parent
/// Instance of the plan's selected PhysicalDevice are borrowed parents: both
/// must remain alive until this Device is destroyed, and the application must
/// destroy Device before that Instance.  Device never retains shared or
/// exclusive ownership of either parent.  Queue values are non-owning views
/// and stale views after Device destruction are invalid by contract rather than
/// liveness-checked.
class Device final {
public:
  Device() = delete;
  Device(const Device &) = delete;
  Device &operator=(const Device &) = delete;
  Device(Device &&other) noexcept;
  Device &operator=(Device &&other) noexcept;
  ~Device();

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] vk::Device nativeHandle() const noexcept;
  [[nodiscard]] const DevicePlan *plan() const noexcept;
  [[nodiscard]] bool correlatedWith(const Instance &instance) const noexcept;

  /// Return a borrowed queue view by its unique caller ID.  Unknown IDs
  /// return an invalid view; no other request is guessed.
  [[nodiscard]] Queue queue(std::string_view caller_id) const noexcept;
  [[nodiscard]] std::span<const Queue> queues() const noexcept;

private:
  struct Impl;
  explicit Device(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> implementation_{};

  friend class Queue;
  friend auto createDevice(const Instance &, const DevicePlan &) -> terreate::Result<Device>;
};

/// A plain, copyable, non-owning Vulkan queue view.  It carries only the
/// native handle, its planned family/index, and a private Device correlation
/// pointer.  It does not own, retain, or tombstone the Device; use after the
/// parent Device lifetime is simply invalid by contract.
class Queue final {
public:
  Queue() noexcept = default;
  Queue(const Queue &) noexcept = default;
  Queue &operator=(const Queue &) noexcept = default;
  Queue(Queue &&) noexcept = default;
  Queue &operator=(Queue &&) noexcept = default;
  ~Queue() = default;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] vk::Queue nativeHandle() const noexcept;
  [[nodiscard]] std::size_t familyIndex() const noexcept { return family_index_; }
  [[nodiscard]] std::size_t queueIndex() const noexcept { return queue_index_; }
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;

private:
  Queue(vk::Queue native_handle, const Device::Impl *parent_identity, std::size_t family_index,
        std::size_t queue_index) noexcept;

  vk::Queue native_handle_{};
  const Device::Impl *parent_identity_ = nullptr;
  std::size_t family_index_ = 0;
  std::size_t queue_index_ = 0;

  friend class Device;
  friend struct Device::Impl;
  friend auto createDevice(const Instance &, const DevicePlan &) -> terreate::Result<Device>;
};

/// Apply a previously resolved plan by creating an owning Vulkan logical
/// device.  `instance` and the selected PhysicalDevice's parent Instance are
/// borrowed only; they must outlive the returned Device.  The caller must
/// destroy Device before its Instance and must retain no expectation that this
/// API creates shared ownership or keeps a parent alive.  A plan with no
/// effective queue allocation is rejected before native creation because
/// Vulkan requires queueCreateInfoCount to be non-zero.
[[nodiscard]] auto createDevice(const Instance &instance, const DevicePlan &plan)
    -> terreate::Result<Device>;

/// Equivalent argument order for createDevice(const Instance &, const DevicePlan &).
/// The same borrowed-parent and destruction-order contract applies.
[[nodiscard]]
auto createDevice(const DevicePlan &plan, const Instance &instance) -> terreate::Result<Device>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::DeviceError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_DEVICE_HPP

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
#include <variant>
#include <vector>

#include <terreate/core/result.hpp>
#include <terreate/graphics/instance.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace terreate::graphics {

class Instance;
struct PhysicalDeviceInventory;
struct PhysicalDeviceCapabilities;
struct PhysicalDeviceCandidate;
struct PhysicalDeviceRequirements;
struct PhysicalDeviceSelectionPolicy;
struct PhysicalDeviceSelection;

/// The UUID is a value-owned copy of VkPhysicalDeviceIDProperties::deviceUUID.
/// It is deliberately an array rather than a string: Vulkan UUIDs are binary
/// values and may contain zero bytes.
using PhysicalDeviceUuid = std::array<std::uint8_t, VK_UUID_SIZE>;

/// A borrowed PhysicalDevice handle.  Physical devices are owned by Vulkan's
/// parent Instance, not by this value.  Ownership never transfers to callers;
/// callers must never destroy a physical device through nativeHandle().  A
/// returned native handle is invalid after the parent Instance is destroyed.
/// A source view follows its parent implementation through an Instance move;
/// a view belonging to a destination implementation displaced by move
/// assignment fails closed and must be reacquired from the current owner.
///
/// The representation is deliberately a small borrowed value: the native
/// handle and a private, non-owning per-device token owned by the parent
/// Instance implementation.  That token records the exact handle, parent
/// identity, and pNext-free capability snapshot, so replacing only the copied
/// handle or public capabilities fails closed.  Only the production query path
/// can create a valid value.  Copies and moves copy or transfer borrowed
/// metadata; they never acquire native ownership, allocate, or retain ownership
/// of the parent Instance.
class PhysicalDevice final {
public:
  PhysicalDevice() noexcept = default;
  PhysicalDevice(const PhysicalDevice &) noexcept = default;
  PhysicalDevice &operator=(const PhysicalDevice &) noexcept = default;
  PhysicalDevice(PhysicalDevice &&) noexcept = default;
  PhysicalDevice &operator=(PhysicalDevice &&) noexcept = default;
  ~PhysicalDevice() = default;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] bool valid() const noexcept;

  /// Return a borrowed native handle.  Ownership never transfers to the
  /// caller, and callers must never destroy the Vulkan physical device through
  /// this handle.  The returned handle is empty when this view is invalid and
  /// is invalid after the parent Instance is destroyed or its implementation
  /// is displaced; reacquire it from the current owner.
  [[nodiscard]] vk::PhysicalDevice nativeHandle() const noexcept;

  /// Compare the private per-device token against the current Instance
  /// implementation.  A source view follows its moved implementation, while
  /// a view belonging to a displaced destination implementation fails closed.
  [[nodiscard]] bool correlatedWith(const Instance &instance) const noexcept;

private:
  struct QueryAuthority;

  [[nodiscard]] bool
  capabilitiesMatch(const PhysicalDeviceCapabilities &capabilities) const noexcept;

  PhysicalDevice(vk::PhysicalDevice native_handle,
                 const Instance::PhysicalDeviceToken *device_token,
                 const QueryAuthority *query_authority) noexcept;

  [[nodiscard]] static const QueryAuthority *query_authority() noexcept;

  vk::PhysicalDevice native_handle_{};
  const Instance::PhysicalDeviceToken *device_token_ = nullptr;

  friend auto queryPhysicalDevices(const Instance &instance)
      -> terreate::Result<PhysicalDeviceInventory>;
  friend auto selectPhysicalDevice(const PhysicalDeviceCandidate &candidate,
                                   const PhysicalDeviceRequirements &requirements)
      -> terreate::Result<PhysicalDeviceSelection>;
  friend auto selectPhysicalDevice(std::span<const PhysicalDeviceCandidate> candidates,
                                   const PhysicalDeviceRequirements &requirements,
                                   const PhysicalDeviceSelectionPolicy &policy)
      -> terreate::Result<PhysicalDeviceSelection>;
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
  /// VK_EXT_memory_budget was not reported or the memory-properties2 query
  /// was not available for the parent instance API.
  std::optional<vk::PhysicalDeviceMemoryBudgetPropertiesEXT> memory_budget{};

  /// Opaque query provenance.  Native query results carry the address of the
  /// parent-owned per-device token; synthetic snapshots intentionally leave it
  /// null.  Pure evaluation ignores this marker, while selection uses it to
  /// prevent a value copied from another view or parent from being substituted
  /// before evaluation.
  const void *query_identity = nullptr;
};

/// A query result item: the borrowed native view is correlated with the
/// eagerly-owned capability snapshot and carries the original enumeration
/// position only as provenance.  Selection never uses that position as a
/// hidden score or fallback policy.
struct PhysicalDeviceCandidate {
  PhysicalDevice device{};
  PhysicalDeviceCapabilities capabilities{};
  std::size_t enumeration_index = 0;
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
};

enum class PhysicalDeviceRequirementOutcome : std::uint8_t {
  accepted,
  declined,
};

enum class PhysicalDeviceRequirementReason : std::uint8_t {
  requested,
  supported,
  unsupported,
};

/// Stable semantic kinds keep requirement decisions machine-readable instead
/// of encoding their identity in display strings.
enum class PhysicalDeviceRequirementKind : std::uint8_t {
  api_version,
  uuid,
  type,
  extension,
  queue,
};

/// The compact, value-owned identity retained with an evaluation.  A missing
/// UUID means ID properties were not available; an all-zero UUID is therefore
/// never used as an implicit identity observation.
struct PhysicalDeviceCandidateIdentity {
  std::optional<PhysicalDeviceUuid> uuid{};
  std::uint32_t vendor_id = 0;
  std::uint32_t device_id = 0;
  vk::PhysicalDeviceType type = vk::PhysicalDeviceType::eOther;
  std::string device_name{};
};

struct PhysicalDeviceApiEvidence {
  std::uint32_t required_api_version = VK_API_VERSION_1_0;
  std::uint32_t available_api_version = VK_API_VERSION_1_0;
};

struct PhysicalDeviceUuidEvidence {
  PhysicalDeviceUuid required_uuid{};
  std::optional<PhysicalDeviceUuid> available_uuid{};
};

struct PhysicalDeviceTypeEvidence {
  vk::PhysicalDeviceType required_type = vk::PhysicalDeviceType::eOther;
  vk::PhysicalDeviceType available_type = vk::PhysicalDeviceType::eOther;
};

struct PhysicalDeviceExtensionEvidence {
  std::string extension{};
  bool available = false;
};

struct PhysicalDeviceQueueEvidence {
  PhysicalDeviceQueueRequirement requirement{};
  std::vector<std::size_t> matching_queue_indices{};
};

using PhysicalDeviceRequirementEvidence =
    std::variant<PhysicalDeviceApiEvidence, PhysicalDeviceUuidEvidence, PhysicalDeviceTypeEvidence,
                 PhysicalDeviceExtensionEvidence, PhysicalDeviceQueueEvidence>;

struct PhysicalDeviceRequirementDecision {
  PhysicalDeviceRequirementKind kind = PhysicalDeviceRequirementKind::api_version;
  RequirementStrength strength = RequirementStrength::optional;
  PhysicalDeviceRequirementOutcome outcome = PhysicalDeviceRequirementOutcome::accepted;
  PhysicalDeviceRequirementReason reason = PhysicalDeviceRequirementReason::supported;
  PhysicalDeviceRequirementEvidence evidence{};
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
};

struct PhysicalDeviceEvaluation {
  PhysicalDeviceCandidateIdentity candidate_identity{};
  bool matches = false;
  std::vector<PhysicalDeviceRequirementDecision> decisions{};
};

/// Selection policy is deliberately explicit.  With no UUID policy, more than
/// one matching candidate is an ambiguity.  A UUID order is a caller-owned
/// preference list copied/observed for this call; enumeration order is never a
/// fallback.  `explicit_uuid` is the manual-selection form.
struct PhysicalDeviceSelectionPolicy {
  std::optional<PhysicalDeviceUuid> explicit_uuid{};
  std::vector<PhysicalDeviceUuid> uuid_order{};
};

enum class PhysicalDeviceSelectionReason : std::uint8_t {
  /// The caller supplied one already chosen candidate directly.
  explicit_candidate,
  /// The complete inventory contained exactly one matching candidate.
  sole_match,
  explicit_uuid,
  uuid_order,
};

struct PhysicalDeviceSelection {
  PhysicalDeviceCandidate candidate{};
  PhysicalDeviceSelectionReason reason = PhysicalDeviceSelectionReason::sole_match;
  std::optional<std::size_t> policy_index{};
};

enum class PhysicalDeviceError : std::uint8_t {
  invalid_instance = 1,
  invalid_requirements = 2,
  no_match = 3,
  ambiguous_match = 4,
  invalid_view = 5,
};

[[nodiscard]] const std::error_category &physical_device_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(PhysicalDeviceError error) noexcept;

/// Enumerate physical devices and eagerly copy every supported observation.
/// A successful zero-device enumeration returns an empty inventory; it is not
/// converted into an error.  vk::SystemError codes from enumeration or any
/// per-device native query are preserved in the returned terreate::Error.
[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
queryPhysicalDevices(const Instance &instance);

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
[[nodiscard]] terreate::Result<PhysicalDeviceSelection>
selectPhysicalDevice(std::span<const PhysicalDeviceCandidate> candidates,
                     const PhysicalDeviceRequirements &requirements,
                     const PhysicalDeviceSelectionPolicy &policy = {});

/// Manual selection of one already chosen candidate.  This overload never
/// compares or ranks other candidates.
[[nodiscard]] terreate::Result<PhysicalDeviceSelection>
selectPhysicalDevice(const PhysicalDeviceCandidate &candidate,
                     const PhysicalDeviceRequirements &requirements);

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::PhysicalDeviceError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_PHYSICAL_DEVICE_HPP

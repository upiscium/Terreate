#ifndef TERREATE_GRAPHICS_INSTANCE_HPP
#define TERREATE_GRAPHICS_INSTANCE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <terreate/core/diagnostics.hpp>
#include <terreate/core/result.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace terreate::graphics {

class Instance;
class PhysicalDevice;
struct PhysicalDeviceInventory;

[[nodiscard]] terreate::Result<PhysicalDeviceInventory>
queryPhysicalDevices(const Instance &instance);

/// Vulkan instance API versions are represented using Vulkan's packed version
/// value.  A missing version in InstanceDescription is deliberately different
/// from an explicit value: it selects Vulkan 1.3 through the documented
/// default policy and is reported as such by InstancePlan.
inline constexpr std::uint32_t default_instance_api_version = VK_API_VERSION_1_3;

enum class RequirementStrength : std::uint8_t {
  required,
  optional,
};

enum class DebugUtilsMode : std::uint8_t {
  disabled,
  optional,
  required,
};

/// The source of the API version in the effective plan.
enum class InstanceApiVersionProvenance : std::uint8_t {
  explicit_request,
  default_policy,
};

enum class InstanceDecisionOutcome : std::uint8_t {
  accepted,
  declined,
};

enum class InstanceDecisionReason : std::uint8_t {
  requested,
  supported,
  unsupported,
  derived_prerequisite,
};

/// One observable extension or layer resolution decision.
struct InstanceDecision {
  std::string name{};
  RequirementStrength strength = RequirementStrength::optional;
  InstanceDecisionOutcome outcome = InstanceDecisionOutcome::accepted;
  InstanceDecisionReason reason = InstanceDecisionReason::requested;
  bool derived = false;
};

/// The request owned by the application.  Required and optional lists are
/// intentionally separate so the requested strength is explicit at the API
/// boundary.  `resolveInstance` only observes a description for the duration
/// of its call and copies its values into a successful InstancePlan; it does
/// not retain references to this object.
struct InstanceDescription {
  std::optional<std::uint32_t> api_version{};

  std::vector<std::string> required_extensions{};
  std::vector<std::string> optional_extensions{};
  std::vector<std::string> required_layers{};
  std::vector<std::string> optional_layers{};

  DebugUtilsMode debug_utils = DebugUtilsMode::disabled;
  std::string application_name{};
  std::uint32_t application_version = 0;
  std::string engine_name{};
  std::uint32_t engine_version = 0;
};

/// A successful, value-owned observation of the Vulkan instance environment.
/// It is intentionally usable as a synthetic snapshot in resolver tests; a
/// snapshot is evidence and never causes every observed name to be enabled.
/// `resolveInstance` copies the snapshot into a successful InstancePlan, so
/// the caller may release or reuse the input after the call returns.
struct InstanceCapabilities {
  std::uint32_t loader_api_version = VK_API_VERSION_1_0;
  std::vector<std::string> available_extensions{};
  std::vector<std::string> available_layers{};
  /// Vulkan exposes no portable loader/backend identity query.  A caller may
  /// attach identity obtained from an out-of-band source; native queries leave
  /// this value empty rather than inventing an identity.
  std::optional<std::string> loader_identity{};
};

/// The deterministic pre-native result of resolving a description against a
/// successful capability snapshot.  Effective enabled collections and
/// resolution decisions are canonicalised in lexicographic order and contain
/// no duplicates; the requested and capability snapshots preserve the exact
/// input values, including their order and duplicates.  Only resolveInstance
/// can construct a plan.  The plan owns all snapshots, enabled collections,
/// and decisions.  Its observers return borrowed read-only views, so a
/// successful resolution cannot be forged or changed before native apply;
/// observer references must not outlive the plan they view.
///
/// InstancePlan is a copyable value.  Its move operations use the ordinary
/// value semantics of its owned data; borrowed observers must not be retained
/// across a move of the plan.
class InstancePlan {
public:
  InstancePlan(const InstancePlan &) = default;
  InstancePlan &operator=(const InstancePlan &) = default;
  InstancePlan(InstancePlan &&) noexcept = default;
  InstancePlan &operator=(InstancePlan &&) noexcept = default;
  ~InstancePlan() = default;

  /// Return a borrowed, read-only view of the original request snapshot.
  /// The reference is valid while this plan remains alive and is invalidated
  /// by moving from or assigning to the plan.
  [[nodiscard]] const InstanceDescription &requested() const noexcept { return data_.requested; }
  /// Return a borrowed, read-only view of the capability snapshot used for
  /// resolution.  The reference follows the same lifetime rules as requested().
  [[nodiscard]] const InstanceCapabilities &capabilities() const noexcept {
    return data_.capabilities;
  }
  [[nodiscard]] std::uint32_t effectiveApiVersion() const noexcept {
    return data_.effective_api_version;
  }
  [[nodiscard]] InstanceApiVersionProvenance apiVersionProvenance() const noexcept {
    return data_.api_version_provenance;
  }
  /// Return a borrowed, read-only view of the canonical enabled extension
  /// collection.  No observer allocation or copy is performed.
  [[nodiscard]] const std::vector<std::string> &enabledExtensions() const noexcept {
    return data_.enabled_extensions;
  }
  /// Return a borrowed, read-only view of the canonical enabled layer
  /// collection.
  [[nodiscard]] const std::vector<std::string> &enabledLayers() const noexcept {
    return data_.enabled_layers;
  }
  /// Return a borrowed, read-only view of the extension resolution decisions.
  [[nodiscard]] const std::vector<InstanceDecision> &extensionDecisions() const noexcept {
    return data_.extension_decisions;
  }
  /// Return a borrowed, read-only view of the layer resolution decisions.
  [[nodiscard]] const std::vector<InstanceDecision> &layerDecisions() const noexcept {
    return data_.layer_decisions;
  }
  [[nodiscard]] bool debugUtilsEnabled() const noexcept { return data_.debug_utils_enabled; }
  [[nodiscard]] bool debugUtilsDerived() const noexcept { return data_.debug_utils_derived; }

private:
  struct Data {
    Data(InstanceDescription requested, InstanceCapabilities capabilities,
         std::uint32_t effective_api_version, InstanceApiVersionProvenance api_version_provenance,
         std::vector<std::string> enabled_extensions, std::vector<std::string> enabled_layers,
         std::vector<InstanceDecision> extension_decisions,
         std::vector<InstanceDecision> layer_decisions, bool debug_utils_enabled,
         bool debug_utils_derived)
        : requested(std::move(requested)), capabilities(std::move(capabilities)),
          effective_api_version(effective_api_version),
          api_version_provenance(api_version_provenance),
          enabled_extensions(std::move(enabled_extensions)),
          enabled_layers(std::move(enabled_layers)),
          extension_decisions(std::move(extension_decisions)),
          layer_decisions(std::move(layer_decisions)), debug_utils_enabled(debug_utils_enabled),
          debug_utils_derived(debug_utils_derived) {}

    InstanceDescription requested{};
    InstanceCapabilities capabilities{};
    std::uint32_t effective_api_version = default_instance_api_version;
    InstanceApiVersionProvenance api_version_provenance =
        InstanceApiVersionProvenance::default_policy;
    std::vector<std::string> enabled_extensions{};
    std::vector<std::string> enabled_layers{};
    std::vector<InstanceDecision> extension_decisions{};
    std::vector<InstanceDecision> layer_decisions{};
    bool debug_utils_enabled = false;
    bool debug_utils_derived = false;
  };

  InstancePlan(InstanceDescription requested, InstanceCapabilities capabilities,
               std::uint32_t effective_api_version,
               InstanceApiVersionProvenance api_version_provenance,
               std::vector<std::string> enabled_extensions, std::vector<std::string> enabled_layers,
               std::vector<InstanceDecision> extension_decisions,
               std::vector<InstanceDecision> layer_decisions, bool debug_utils_enabled,
               bool debug_utils_derived)
      : data_(std::move(requested), std::move(capabilities), effective_api_version,
              api_version_provenance, std::move(enabled_extensions), std::move(enabled_layers),
              std::move(extension_decisions), std::move(layer_decisions), debug_utils_enabled,
              debug_utils_derived) {}

  Data data_;

  friend auto resolveInstance(const InstanceDescription &, const InstanceCapabilities &)
      -> terreate::Result<InstancePlan>;
};

/// Stable semantic errors produced before a native Vulkan error is available.
enum class InstanceError : std::uint8_t {
  invalid_description = 1,
  contradictory_requirements = 2,
  unsupported_api_version = 3,
  missing_required_extension = 4,
  missing_required_layer = 5,
  loader_unavailable = 6,
};

[[nodiscard]] const std::error_category &instance_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(InstanceError error) noexcept;

/// A move-only owner of a Vulkan instance, its loader context, its optional
/// Debug Utils messenger, and the effective plan retained for that instance.
/// Instance has no public default constructor for an empty state and is
/// produced only by a successful createInstance call.  Move construction and
/// move assignment transfer the native ownership; a moved-from Instance has no
/// handle or plan.  Borrowed values belonging to the source implementation
/// follow that implementation to its new owner.  A destination implementation
/// displaced by move assignment invalidates its existing borrowed values.
class Instance {
public:
  Instance() = delete;
  Instance(const Instance &) = delete;
  Instance &operator=(const Instance &) = delete;

  Instance(Instance &&other) noexcept;
  Instance &operator=(Instance &&other) noexcept;
  ~Instance();

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] bool valid() const noexcept;

  /// Return a borrowed copy of the native handle.  Ownership never transfers
  /// to the caller, and callers must never destroy the Vulkan instance through
  /// this handle.  The returned handle is invalid after this Instance is
  /// destroyed or participates in a move; reacquire it from the current
  /// owning Instance instead.  This API relies on the caller to follow those
  /// rules; the handle type cannot enforce them.
  [[nodiscard]] vk::Instance nativeHandle() const noexcept;

  /// Return a pointer to the effective configuration used for native creation.
  /// The plan is owned by this Instance and is not the caller's plan passed to
  /// createInstance.  The pointer is borrowed, is nullptr for a moved-from
  /// Instance, and must not be retained across destruction or a move of this
  /// Instance.
  [[nodiscard]] const InstancePlan *plan() const noexcept;

private:
  struct LifetimeToken;
  struct PhysicalDeviceToken;
  struct Impl;

  explicit Instance(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> implementation_{};
  std::unique_ptr<LifetimeToken> retired_lifetime_tokens_{};
  std::unique_ptr<PhysicalDeviceToken> retired_physical_device_tokens_{};

  friend auto createInstance(const InstancePlan &plan, terreate::DiagnosticSinkView sink)
      -> terreate::Result<Instance>;
  friend class PhysicalDevice;
  friend auto queryPhysicalDevices(const Instance &instance)
      -> terreate::Result<PhysicalDeviceInventory>;
};

/// Query the loader's instance API version, instance extensions, and layers.
/// A Vulkan-Hpp vk::SystemError is returned with its original std::error_code;
/// expected loader construction unavailability uses InstanceError::loader_unavailable;
/// no failed query is converted into an empty capability snapshot.  The
/// successful result owns its returned strings and may be retained by the
/// caller independently of the loader context.  Vulkan has no portable loader
/// identity query, so loader_identity is left unavailable.
[[nodiscard]] terreate::Result<InstanceCapabilities> queryInstanceCapabilities();

/// Resolve explicit instance intent against a successful capability snapshot.
/// The input values are observed only during this call, never normalised in
/// place, and copied into the returned plan.  The plan is complete before
/// native creation is attempted; neither input needs to remain alive after the
/// call returns.
[[nodiscard]] terreate::Result<InstancePlan>
resolveInstance(const InstanceDescription &description, const InstanceCapabilities &capabilities);

/// Apply a previously resolved plan by creating an owning Vulkan instance (and
/// an optional Debug Utils messenger).  A native failure does not mutate the
/// supplied plan; it remains the successful pre-native result held by the
/// caller.  The plan is borrowed only for the duration of this call; a
/// successful Instance retains its own copy.  The sink view is also copied into
/// callback state, but its application-owned target is not; that target must
/// outlive the returned Instance and all callbacks delivered through it.
[[nodiscard]]
auto createInstance(const InstancePlan &plan, terreate::DiagnosticSinkView sink = {})
    -> terreate::Result<Instance>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::InstanceError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_INSTANCE_HPP

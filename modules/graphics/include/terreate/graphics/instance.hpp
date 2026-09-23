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
/// boundary.
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
struct InstanceCapabilities {
  std::uint32_t loader_api_version = VK_API_VERSION_1_0;
  std::vector<std::string> available_extensions{};
  std::vector<std::string> available_layers{};
};

/// The deterministic pre-native result of resolving a description against a
/// successful capability snapshot.  Effective enabled collections and
/// resolution decisions are canonicalised in lexicographic order and contain
/// no duplicates; requested and capability snapshots preserve the exact input
/// values, including their order and duplicates.
struct InstancePlan {
  // These fields remain public so callers can inspect every requested input,
  // capability observation, and resolution decision.  Native apply validates
  // them against the private resolver snapshot before using any field.
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

  InstancePlan() = default;
  InstancePlan(const InstancePlan &other);
  InstancePlan &operator=(const InstancePlan &other);
  InstancePlan(InstancePlan &&other) noexcept = default;
  InstancePlan &operator=(InstancePlan &&other) noexcept = default;
  ~InstancePlan() = default;

  [[nodiscard]] bool matchesCanonical() const;

private:
  // A plan returned by resolveInstance keeps an immutable copy of all of its
  // public values.  This is provenance for native apply, not Vulkan/resource
  // ownership.  Copies retain the provenance, while any public mutation is
  // rejected instead of becoming a new native configuration.
  std::unique_ptr<const InstancePlan> canonical_{};

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

/// A move-only owning Vulkan instance.  The native handle returned by
/// nativeHandle() is an explicitly borrowed Vulkan-Hpp handle.  It is valid
/// only while this Instance remains alive and must never be destroyed by the
/// caller.
class Instance {
public:
  Instance() noexcept;
  Instance(const Instance &) = delete;
  Instance &operator=(const Instance &) = delete;

  Instance(Instance &&other) noexcept;
  Instance &operator=(Instance &&other) noexcept;
  ~Instance();

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] bool valid() const noexcept;

  /// Return the borrowed native handle.  This does not transfer ownership or
  /// permit destruction through the returned value.
  [[nodiscard]] vk::Instance nativeHandle() const noexcept;

  /// The effective configuration used for native creation.  A moved-from or
  /// default Instance returns an empty plan rather than exposing native state.
  [[nodiscard]] const InstancePlan &plan() const noexcept;

private:
  struct Impl;

  explicit Instance(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_{};

  friend terreate::Result<Instance> createInstance(const InstancePlan &plan,
                                                   terreate::DiagnosticSinkView sink);
};

/// Query the loader's instance API version, instance extensions, and layers.
/// A Vulkan-Hpp vk::SystemError is returned with its original std::error_code;
/// expected loader construction unavailability uses InstanceError::loader_unavailable;
/// no failed query is converted into an empty capability snapshot.
[[nodiscard]] terreate::Result<InstanceCapabilities> queryInstanceCapabilities();

/// Resolve explicit instance intent against a successful capability snapshot.
/// The input values are observed, never normalised in place, and the returned
/// plan is complete before native creation is attempted.
[[nodiscard]] terreate::Result<InstancePlan>
resolveInstance(const InstanceDescription &description, const InstanceCapabilities &capabilities);

/// Apply a previously resolved plan by creating an owning Vulkan instance (and
/// an optional Debug Utils messenger).  A native failure does not mutate the
/// supplied plan; it remains the successful pre-native result held by the
/// caller.  The sink is borrowed and copied into callback state for the
/// lifetime of the returned Instance.
[[nodiscard]] auto createInstance(const InstancePlan &plan, terreate::DiagnosticSinkView sink = {})
    -> terreate::Result<Instance>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::InstanceError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_INSTANCE_HPP

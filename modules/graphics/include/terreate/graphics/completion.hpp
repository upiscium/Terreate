#ifndef TERREATE_GRAPHICS_COMPLETION_HPP
#define TERREATE_GRAPHICS_COMPLETION_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <variant>

#include <terreate/graphics/device.hpp>

namespace terreate::graphics {

/// Caller-issued identity, not a native handle. The borrowed domain must outlive
/// every copy; the caller must never reuse a nonzero serial within that domain.
/// This distinguishes fence reuse and equal timeline values on different work.
struct CompletionIdentity {
  const void *domain = nullptr;
  std::uint64_t serial = 0;

  [[nodiscard]] constexpr bool valid() const noexcept { return domain != nullptr && serial != 0; }
  bool operator==(const CompletionIdentity &) const = default;
};

struct FenceCompletionCondition {
  vk::Fence fence{};
};

/// The caller declares that semaphore is a timeline semaphore, not binary.
/// Zero is a legal threshold, but is never by itself proof of submitted work.
struct TimelineCompletionCondition {
  vk::Semaphore semaphore{};
  std::uint64_t signal_value = 0;
};

using CompletionCondition =
    std::variant<std::monostate, FenceCompletionCondition, TimelineCompletionCondition>;

enum class CompletionOutcome : std::uint8_t {
  unavailable,
  pending,
  completed,
  submit_failed,
  device_lost,
  native_error,
};

/// Invalid observations are not pending work and never mean completed work.
/// moved_from/stale_parent are reserved for explicit future operation results;
/// this value layer has neither a liveness registry nor automatic detection.
enum class CompletionError : std::uint8_t {
  empty,
  invalid_identity,
  invalid_device,
  invalid_queue,
  parent_mismatch,
  invalid_condition,
  invalid_outcome,
  moved_from,
  stale_parent,
};

struct CompletionDescription {
  CompletionIdentity requested{};
  CompletionIdentity effective{};
  const Device *device = nullptr;
  Queue queue{};
  CompletionCondition condition{};
};

/// Allocation-free borrowed observation, NOT a submitted-work credential.
/// requested/effective retain caller intent and the identity actually chosen;
/// equality of identities is not required and no identity is generated here.
///
/// Device (at the same address), its Instance parents, the identity domains, and
/// the native synchronization object must outlive all uses of every copy. Moving
/// or destroying the Device invalidates this association; recreate the value
/// against the live Device after a move. Stale parents must not be dereferenced.
/// The native object must not be reset/reused for different work while observed.
/// Copy and move both copy the borrow, leaving the source unchanged; destruction
/// does nothing to any parent or handle. No ownership or lifetime is extended.
///
/// All outcomes are explicit caller reports, not queried or verified facts.
/// Handle possession, structural validity, and even a caller-reported completed
/// outcome do not establish submission provenance (#258). This token claims no
/// resource coverage. Release safety requires the caller-declared conjunction
/// of EVERY known in-flight use across all queues; collection/retirement belongs
/// to #259. No native operations or feature enablement occur here.
class Completion final {
public:
  Completion() noexcept = default;

  /// Only structural validation is performed. native_result, when present, must
  /// agree with the explicit outcome. Omission means no native result reported.
  [[nodiscard]] static std::expected<Completion, CompletionError>
  observeNative(const CompletionDescription &description, CompletionOutcome outcome,
                std::optional<vk::Result> native_result = std::nullopt) noexcept {
    if (!description.requested.valid() || !description.effective.valid()) {
      return std::unexpected(CompletionError::invalid_identity);
    }
    if (description.device == nullptr || !description.device->valid()) {
      return std::unexpected(CompletionError::invalid_device);
    }
    if (!description.queue.valid()) {
      return std::unexpected(CompletionError::invalid_queue);
    }
    if (!description.queue.correlatedWith(*description.device)) {
      return std::unexpected(CompletionError::parent_mismatch);
    }
    const auto *fence = std::get_if<FenceCompletionCondition>(&description.condition);
    const auto *timeline = std::get_if<TimelineCompletionCondition>(&description.condition);
    if ((fence == nullptr || !fence->fence) && (timeline == nullptr || !timeline->semaphore)) {
      return std::unexpected(CompletionError::invalid_condition);
    }
    bool consistent = false;
    switch (outcome) {
    case CompletionOutcome::unavailable:
      consistent = !native_result;
      break;
    case CompletionOutcome::pending:
      consistent = !native_result || *native_result == vk::Result::eNotReady ||
                   *native_result == vk::Result::eTimeout;
      break;
    case CompletionOutcome::completed:
      consistent = !native_result || *native_result == vk::Result::eSuccess;
      break;
    case CompletionOutcome::submit_failed:
    case CompletionOutcome::native_error:
      consistent = !native_result || (static_cast<int>(*native_result) < 0 &&
                                      *native_result != vk::Result::eErrorDeviceLost);
      break;
    case CompletionOutcome::device_lost:
      consistent = !native_result || *native_result == vk::Result::eErrorDeviceLost;
      break;
    }
    if (!consistent) {
      return std::unexpected(CompletionError::invalid_outcome);
    }
    Completion observation;
    observation.description_ = description;
    observation.outcome_ = outcome;
    observation.native_result_ = native_result;
    observation.empty_ = false;
    return observation;
  }

  /// Structural presence only, never a liveness or completion check.
  [[nodiscard]] bool empty() const noexcept { return empty_; }
  [[nodiscard]] const CompletionDescription &description() const noexcept { return description_; }
  [[nodiscard]] std::expected<CompletionOutcome, CompletionError> outcome() const noexcept {
    if (empty_) {
      return std::unexpected(CompletionError::empty);
    }
    return outcome_;
  }
  [[nodiscard]] std::optional<vk::Result> nativeResult() const noexcept { return native_result_; }

private:
  CompletionDescription description_{};
  CompletionOutcome outcome_ = CompletionOutcome::unavailable;
  std::optional<vk::Result> native_result_{};
  bool empty_ = true;
};

} // namespace terreate::graphics

#endif // TERREATE_GRAPHICS_COMPLETION_HPP

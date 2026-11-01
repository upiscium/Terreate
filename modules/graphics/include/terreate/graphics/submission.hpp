#ifndef TERREATE_GRAPHICS_SUBMISSION_HPP
#define TERREATE_GRAPHICS_SUBMISSION_HPP

#include <optional>
#include <span>
#include <system_error>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/completion.hpp>

namespace terreate::graphics {

enum class SubmissionError : std::uint8_t {
  invalid_device = 1,
  invalid_queue,
  parent_mismatch,
  synchronization2_not_enabled,
  timeline_not_enabled,
  invalid_buffer,
  family_mismatch,
  protected_allocation,
  invalid_semaphore,
  invalid_native_structure,
  invalid_completion,
  invalid_batch,
};
[[nodiscard]] const std::error_category &submission_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SubmissionError error) noexcept;

enum class SubmissionSemaphoreKind : std::uint8_t { binary, timeline };

/// Caller declares the native semaphore type and selects every stage/value.
/// pNext must be null and deviceIndex zero; binary values must be zero.
struct SubmissionSemaphore {
  vk::SemaphoreSubmitInfo native{};
  SubmissionSemaphoreKind kind = SubmissionSemaphoreKind::binary;
};

/// One explicit batch, no ownership. All arrays need only survive submit().
/// The completion Device/Queue must match the selected submission Device/Queue.
/// Requested and effective completion identities must each be valid, but may
/// differ; the library never invents or rewrites these caller-issued values.
/// Fence completion must name fence; timeline completion must name an explicit
/// timeline signal with the identical value and ALL_COMMANDS stage.
struct SubmissionDescription {
  std::span<const CommandBuffer *const> command_buffers{};
  std::span<const SubmissionSemaphore> waits{};
  std::span<const SubmissionSemaphore> signals{};
  vk::Fence fence{};
  CompletionDescription completion{};
};

enum class SubmissionCompletionState : std::uint8_t { pending, completed, timeout };
struct SubmissionObservation {
  SubmissionCompletionState state = SubmissionCompletionState::pending;
  vk::Result native_result = vk::Result::eSuccess;
  /// Present only for a successful timeline poll.
  std::optional<std::uint64_t> counter_value{};
};

/// Borrowed evidence of native submission SUCCESS, never a completion claim.
/// Device at the same address, its parents, identity domains and native sync
/// object must outlive every use. No reset/reuse while this receipt is queried.
/// Copies extend no lifetime. No pending/reuse or resource tracking is done.
class SubmissionReceipt final {
public:
  [[nodiscard]] const CompletionDescription &completion() const noexcept { return completion_; }
  /// Materialize the #257 borrowed Completion token with outcome 'unavailable':
  /// the native submit succeeded, but no GPU completion has been queried yet.
  /// This does not poll, wait, or independently authenticate a copied token.
  [[nodiscard]] std::expected<Completion, CompletionError> initialCompletion() const noexcept {
    return Completion::observeNative(completion_, CompletionOutcome::unavailable);
  }
  [[nodiscard]] terreate::Result<SubmissionObservation> poll() const;
  /// Exactly one native wait, timeout in nanoseconds (UINT64_MAX is infinite).
  [[nodiscard]] terreate::Result<SubmissionObservation> wait(std::uint64_t timeout_ns) const;

private:
  explicit SubmissionReceipt(CompletionDescription completion) : completion_(completion) {}
  CompletionDescription completion_;
  friend auto submit(const Device &, Queue, const SubmissionDescription &)
      -> terreate::Result<SubmissionReceipt>;
};

/// Validate then issue exactly one vkQueueSubmit2 with submitCount == 1.
/// Caller externally synchronizes Queue, pool/buffers, fence and semaphore use.
/// Native escape hatches remain available; no waits, resets or retries occur.
[[nodiscard]] auto submit(const Device &device, Queue queue,
                          const SubmissionDescription &description)
    -> terreate::Result<SubmissionReceipt>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::SubmissionError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SUBMISSION_HPP

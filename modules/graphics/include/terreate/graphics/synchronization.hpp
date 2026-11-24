#ifndef TERREATE_GRAPHICS_SYNCHRONIZATION_HPP
#define TERREATE_GRAPHICS_SYNCHRONIZATION_HPP

#include <cstdint>
#include <memory>
#include <system_error>

#include <terreate/core/result.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/submission.hpp>

namespace terreate::graphics {

enum class SynchronizationError : std::uint8_t {
  invalid_device = 1,
  /// The Fence or Semaphore is moved-from.
  invalid_object,
  /// A timeline Semaphore needs the Vulkan 1.2 `timelineSemaphore` feature
  /// enabled on the Device; it is never replaced by a binary one.
  timeline_not_enabled,
  invalid_description,
  /// A timeline operation on a binary Semaphore.
  wrong_kind,
  /// A timeline function could not be loaded from the Device.
  function_unavailable,
  allocation_failed,
};

[[nodiscard]] const std::error_category &synchronization_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SynchronizationError error) noexcept;

/// The result of a wait: the condition held, or the timeout elapsed first. A
/// native failure is an error, never one of these.
enum class WaitStatus : std::uint8_t { satisfied, timeout };

enum class FenceStatus : std::uint8_t { signaled, unsignaled };

struct FenceDescription {
  /// Create the Fence already signaled.
  bool signaled = false;
};

/// A move-only owner of one VkFence. The Device is borrowed, not retained: it
/// must outlive the Fence, like every Graphics owner, and the application
/// destroys the Fence only when no GPU operation uses it (nothing is tracked,
/// destruction never waits). All operations are exactly one native call, with
/// no hidden waiting, retry or scheduling; host access to one Fence is
/// externally synchronized by the application.
class Fence final {
public:
  Fence() = delete;
  Fence(const Fence &) = delete;
  Fence &operator=(const Fence &) = delete;
  Fence(Fence &&other) noexcept;
  /// Destroys the Fence this object owned before taking over `other`'s.
  Fence &operator=(Fence &&other) noexcept;
  ~Fence();

  [[nodiscard]] bool valid() const noexcept { return implementation_ != nullptr; }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  /// The borrowed native handle (null when moved from); never destroy it. It is
  /// what `SubmissionDescription::fence`, `AcquireDescription::fence` and
  /// `PresentDescription::fence` take.
  [[nodiscard]] vk::Fence nativeHandle() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  /// The requested description; null when moved from. Effective equals requested.
  [[nodiscard]] const FenceDescription *configuration() const noexcept;

  /// One non-blocking vkGetFenceStatus.
  [[nodiscard]] auto status() const -> terreate::Result<FenceStatus>;
  /// One vkWaitForFences on this Fence; `timeout_ns` 0 polls and UINT64_MAX
  /// waits without limit. There is no default.
  [[nodiscard]] auto wait(std::uint64_t timeout_ns) const -> terreate::Result<WaitStatus>;
  /// One vkResetFences. The Fence must have no pending operation (the caller
  /// knows that from its own synchronization).
  [[nodiscard]] auto reset() const -> terreate::Result<void>;

  struct Impl;

private:
  explicit Fence(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_{};

  friend auto createFence(const Device &, const FenceDescription &) -> terreate::Result<Fence>;
};

[[nodiscard]] auto createFence(const Device &device, const FenceDescription &description)
    -> terreate::Result<Fence>;

/// What a host wait on a timeline Semaphore waits for, and for how long (named so
/// the two numbers cannot be swapped by mistake).
struct TimelineWait {
  /// The counter value to reach.
  std::uint64_t value = 0;
  /// 0 polls and UINT64_MAX waits without limit. There is no default wait.
  std::uint64_t timeout_ns = 0;
};

struct SemaphoreDescription {
  SubmissionSemaphoreKind kind = SubmissionSemaphoreKind::binary;
  /// The initial counter of a timeline Semaphore; must be 0 for a binary one.
  std::uint64_t initial_value = 0;
};

/// A move-only owner of one VkSemaphore, binary or timeline, with the same
/// lifetime and synchronization contract as `Fence`. A binary Semaphore is
/// used through its native handle (submit, acquire and present take it); the
/// timeline operations below are single native calls.
class Semaphore final {
public:
  Semaphore() = delete;
  Semaphore(const Semaphore &) = delete;
  Semaphore &operator=(const Semaphore &) = delete;
  Semaphore(Semaphore &&other) noexcept;
  Semaphore &operator=(Semaphore &&other) noexcept;
  ~Semaphore();

  [[nodiscard]] bool valid() const noexcept { return implementation_ != nullptr; }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::Semaphore nativeHandle() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  /// The kind this Semaphore was created with. Binary for a moved-from one.
  [[nodiscard]] SubmissionSemaphoreKind kind() const noexcept;
  [[nodiscard]] const SemaphoreDescription *configuration() const noexcept;

  /// One vkGetSemaphoreCounterValue. wrong_kind on a binary Semaphore.
  [[nodiscard]] auto counterValue() const -> terreate::Result<std::uint64_t>;
  /// One vkWaitSemaphores for the counter to reach `request.value`, within
  /// `request.timeout_ns`. wrong_kind on a binary Semaphore.
  [[nodiscard]] auto wait(const TimelineWait &request) const -> terreate::Result<WaitStatus>;
  /// One vkSignalSemaphore from the host. `value` must exceed the current
  /// counter and any pending signal (Vulkan's rule, not checked here).
  [[nodiscard]] auto signal(std::uint64_t value) const -> terreate::Result<void>;

  struct Impl;

private:
  explicit Semaphore(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_{};

  friend auto createSemaphore(const Device &, const SemaphoreDescription &)
      -> terreate::Result<Semaphore>;
};

[[nodiscard]] auto createSemaphore(const Device &device, const SemaphoreDescription &description)
    -> terreate::Result<Semaphore>;

/// Build the `SubmissionSemaphore` for a submission wait or signal with every
/// field explicit: the stage mask is the caller's, and `value` is the timeline
/// value (0 for a binary Semaphore, which `submit` requires). These only build
/// a value; nothing is called and nothing is inferred.
[[nodiscard]] auto submissionWait(const Semaphore &semaphore, vk::PipelineStageFlags2 stage_mask,
                                  std::uint64_t value = 0) noexcept -> SubmissionSemaphore;
[[nodiscard]] auto submissionSignal(const Semaphore &semaphore, vk::PipelineStageFlags2 stage_mask,
                                    std::uint64_t value = 0) noexcept -> SubmissionSemaphore;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::SynchronizationError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SYNCHRONIZATION_HPP

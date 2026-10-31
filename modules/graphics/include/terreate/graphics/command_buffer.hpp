#ifndef TERREATE_GRAPHICS_COMMAND_BUFFER_HPP
#define TERREATE_GRAPHICS_COMMAND_BUFFER_HPP

#include <cstdint>
#include <system_error>
#include <terreate/core/result.hpp>
#include <vulkan/vulkan.hpp>

namespace terreate::graphics {

class CommandPool;

enum class CommandError : std::uint8_t {
  invalid_parent = 1,
  invalid_queue_family,
  unsupported_flags,
  invalid_pool,
  invalid_buffer,
  invalid_transition,
  reset_not_enabled,
  invalid_native_handle,
};

[[nodiscard]] const std::error_category &command_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(CommandError error) noexcept;

/// Wrapper-observed state only: pending GPU execution is never inferred.
enum class CommandBufferState : std::uint8_t { initial, recording, executable, invalid };

/// Move-only view of a primary allocation owned exclusively by CommandPool.
/// Destruction and move assignment abandon the view, NOT the allocation; all
/// allocations (including abandoned ones) are retained until pool destruction.
/// The pool implementation and its borrowed Device must outlive every view and
/// native use. Pool moves preserve source views; replacing a destination pool
/// destroys its allocations and invalidates all destination views. Buffer moves
/// empty the source; replacing a destination buffer only abandons its old view.
/// There is no liveness check after parent destruction/displacement.
///
/// No thread affinity is imposed. The caller must externally synchronize all
/// operations on a pool and its buffers, including observations, moves and
/// destruction, and ensure GPU completion before reset or pool destruction.
/// No submission, completion tracking, implicit wait or recording helpers exist.
/// Native handles are borrowed: never free them or destroy their pool. Native
/// begin/end/reset or pool reset bypass tracking and invalidate the wrapper's
/// state contract; do not mix those operations with wrapper state operations.
/// Native recording commands between wrapper begin/end are permitted, subject
/// to Vulkan validity. Secondary allocation, inheritance and render-pass
/// continuation are outside this API; begin rejects continuation flags.
class CommandBuffer final {
public:
  CommandBuffer() = delete;
  CommandBuffer(const CommandBuffer &) = delete;
  CommandBuffer &operator=(const CommandBuffer &) = delete;
  CommandBuffer(CommandBuffer &&other) noexcept;
  CommandBuffer &operator=(CommandBuffer &&other) noexcept;
  ~CommandBuffer() = default;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::CommandBuffer nativeHandle() const noexcept;
  [[nodiscard]] CommandBufferState state() const noexcept;

  /// Only initial -> recording; no implicit reset of executable buffers.
  /// Accepts OneTimeSubmit or SimultaneousUse (or neither), not both.
  [[nodiscard]] terreate::Result<void> begin(vk::CommandBufferUsageFlags flags = {});
  /// Only recording -> executable. Native failure marks the buffer invalid.
  [[nodiscard]] terreate::Result<void> end();
  /// Initial/executable/invalid -> initial; recording is rejected. Requires
  /// the pool's ResetCommandBuffer flag. Only ReleaseResources is accepted.
  /// GPU completion is a caller precondition, not represented by state().
  [[nodiscard]] terreate::Result<void> reset(vk::CommandBufferResetFlags flags = {});

private:
  struct Record {
    vk::CommandBuffer handle{};
    vk::CommandPoolCreateFlags pool_flags{};
    CommandBufferState state = CommandBufferState::initial;
  };
  explicit CommandBuffer(Record *record) noexcept : record_(record) {}
  Record *record_ = nullptr;
  friend class CommandPool;
};

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::CommandError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_COMMAND_BUFFER_HPP

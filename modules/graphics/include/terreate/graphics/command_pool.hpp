#ifndef TERREATE_GRAPHICS_COMMAND_POOL_HPP
#define TERREATE_GRAPHICS_COMMAND_POOL_HPP

#include <cstdint>
#include <limits>
#include <memory>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/device.hpp>

namespace terreate::graphics {

struct CommandPoolDescription {
  /// No queue role or default family is inferred. Must name an allocation in
  /// Device::plan()->queueAllocations(), not merely a physical-device family.
  std::uint32_t queue_family_index = std::numeric_limits<std::uint32_t>::max();
  /// Transient, ResetCommandBuffer and Protected are supported. Protected
  /// requires already-enabled Vulkan 1.1 protectedMemory and a protected family.
  /// Unknown bits are rejected; flags are never silently enabled or stripped.
  vk::CommandPoolCreateFlags flags{};
};

/// Move-only exclusive owner of a native pool and ALL its buffer allocations.
/// The underlying logical Device is borrowed, not the Device wrapper address:
/// moving Device preserves this borrow, but destruction or displacement of its
/// implementation invalidates it. Destroy pools before Device and buffer views
/// before their pool. Moving a pool transfers its stable implementation and
/// preserves source views; move assignment first destroys destination resources.
/// No shared ownership, global owner registry or dangling-view detection exists.
///
/// Caller-managed external host synchronization covers the pool and every
/// allocated buffer; there is no thread affinity or internal locking. Caller
/// must ensure GPU completion before resetting/destroying/displacing a pool.
/// Destruction never waits. Native handles are borrowed, never ownership grants.
/// Native allocate/free/reset/destroy operations on this pool are unsupported:
/// they bypass the ownership/state contract. Native recording via buffer handles
/// is supported only as described by CommandBuffer.
class CommandPool final {
public:
  CommandPool() = delete;
  CommandPool(const CommandPool &) = delete;
  CommandPool &operator=(const CommandPool &) = delete;
  CommandPool(CommandPool &&) noexcept;
  CommandPool &operator=(CommandPool &&) noexcept;
  ~CommandPool();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::CommandPool nativeHandle() const noexcept;
  /// Null for moved-from owners. Effective values equal requested values.
  [[nodiscard]] const CommandPoolDescription *requested() const noexcept;
  [[nodiscard]] const CommandPoolDescription *effective() const noexcept;
  [[nodiscard]] terreate::Result<CommandBuffer> allocatePrimary();
  /// Reset the ENTIRE pool, including recording and abandoned buffers, to
  /// initial. Does not require ResetCommandBuffer. Only ReleaseResources is
  /// accepted. Native failure marks every tracked buffer invalid. All GPU use
  /// must have completed; no completion is checked or implied by this method.
  [[nodiscard]] terreate::Result<void> reset(vk::CommandPoolResetFlags flags = {});

private:
  struct Impl;
  explicit CommandPool(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createCommandPool(const Device &, const CommandPoolDescription &)
      -> terreate::Result<CommandPool>;
};

[[nodiscard]] auto createCommandPool(const Device &device,
                                     const CommandPoolDescription &description)
    -> terreate::Result<CommandPool>;

} // namespace terreate::graphics

#endif // TERREATE_GRAPHICS_COMMAND_POOL_HPP

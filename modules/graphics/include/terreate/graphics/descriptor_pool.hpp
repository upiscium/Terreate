#ifndef TERREATE_GRAPHICS_DESCRIPTOR_POOL_HPP
#define TERREATE_GRAPHICS_DESCRIPTOR_POOL_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <system_error>
#include <vector>

#include <terreate/graphics/descriptor_set.hpp>
#include <terreate/graphics/descriptor_set_layout.hpp>

namespace terreate::graphics {

struct DescriptorPoolSizeDescription {
  vk::DescriptorType type = static_cast<vk::DescriptorType>(VK_DESCRIPTOR_TYPE_MAX_ENUM);
  std::uint32_t descriptor_count = 0;
  bool operator==(const DescriptorPoolSizeDescription &) const = default;
};

/// Explicit capacities, not inferred from layouts. Duplicate types are legal
/// and preserved in order; Vulkan aggregates their capacities. Empty sizes are
/// valid for empty layouts. Only zero flags or eFreeDescriptorSet are supported.
struct DescriptorPoolDescription {
  std::uint32_t max_sets = 0;
  vk::DescriptorPoolCreateFlags flags{};
  std::vector<DescriptorPoolSizeDescription> sizes{};
  bool operator==(const DescriptorPoolDescription &) const = default;
};

struct DescriptorPoolConfiguration {
  DescriptorPoolDescription requested{};
  DescriptorPoolDescription effective{};
};

enum class DescriptorPoolState : std::uint8_t { ready, degraded, invalid_moved_from };
enum class DescriptorPoolError : std::uint8_t {
  invalid_device = 1,
  invalid_description,
  unsupported_configuration,
  invalid_pool,
  degraded_pool,
  invalid_layout,
  invalid_set,
  invalid_count,
  free_not_supported,
  native_null_handle,
  allocation_failed,
};

[[nodiscard]] const std::error_category &descriptor_pool_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(DescriptorPoolError error) noexcept;
[[nodiscard]] auto validateDescriptorPoolDescription(const DescriptorPoolDescription &description)
    -> terreate::Result<void>;

/// Exclusive RAII owner of one pool, borrowing its native Device (not wrapper
/// address). The Device must outlive destruction and move-assignment displacement.
/// Caller owns GPU completion before free/reset/destruction/displacement and all
/// Vulkan host external synchronization. No waits, submissions or hidden resets.
/// Borrowed native pool getters do NOT authorize direct Allocate/Free/Reset/
/// Destroy on this pool: bypassing wrapper state is unsupported.
class DescriptorPool final {
public:
  DescriptorPool() = delete;
  DescriptorPool(const DescriptorPool &) = delete;
  DescriptorPool &operator=(const DescriptorPool &) = delete;
  DescriptorPool(DescriptorPool &&) noexcept;
  DescriptorPool &operator=(DescriptorPool &&) noexcept;
  ~DescriptorPool();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] DescriptorPoolState state() const noexcept;
  [[nodiscard]] vk::DescriptorPool nativeHandle() const noexcept;
  [[nodiscard]] VkDescriptorPool nativeDescriptorPool() const noexcept;
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  [[nodiscard]] VkDevice nativeVkDevice() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  /// Borrowed snapshot, null after move; reacquire after ownership moves.
  [[nodiscard]] const DescriptorPoolConfiguration *configuration() const noexcept;

  /// Nonempty ordered layouts on this native Device. Non-null pointers must
  /// refer to live objects through this call only; no layout pointer is retained.
  /// Native allocation failure is atomic and leaves ready state unchanged.
  /// Success with any null output degrades the pool without hidden cleanup.
  [[nodiscard]] auto allocate(std::span<const DescriptorSetLayout *const> layouts)
      -> terreate::Result<std::vector<DescriptorSet>>;
  [[nodiscard]] auto allocate(const DescriptorSetLayout &layout) -> terreate::Result<DescriptorSet>;
  /// Requires FREE bit, nonempty distinct live cached handles from this pool.
  /// Caller guarantees actual liveness. Success clears passed views only;
  /// uncertain native failure preserves them and degrades the pool.
  [[nodiscard]] auto free(std::span<DescriptorSet> sets) -> terreate::Result<void>;
  [[nodiscard]] auto free(DescriptorSet &set) -> terreate::Result<void>;
  /// Explicit recovery from degraded state; flags always zero. Success restores
  /// ready and invalidates ALL native sets, but cannot mutate outstanding views:
  /// caller MUST discard those views. Failure degrades, with no retry.
  [[nodiscard]] auto reset() -> terreate::Result<void>;

private:
  struct Impl;
  explicit DescriptorPool(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createDescriptorPool(const Device &, const DescriptorPoolDescription &)
      -> terreate::Result<DescriptorPool>;
};

[[nodiscard]] auto createDescriptorPool(const Device &device,
                                        const DescriptorPoolDescription &description)
    -> terreate::Result<DescriptorPool>;
} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::DescriptorPoolError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_DESCRIPTOR_POOL_HPP

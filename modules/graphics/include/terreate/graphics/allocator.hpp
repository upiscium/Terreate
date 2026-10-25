#ifndef TERREATE_GRAPHICS_ALLOCATOR_HPP
#define TERREATE_GRAPHICS_ALLOCATOR_HPP

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <terreate/graphics/device.hpp>
#include <vk_mem_alloc.h>

namespace terreate::graphics {

/// Missing api_version selects Vulkan 1.0, not the highest supported version.
/// Explicit versions must be Vulkan 1.0--1.3 (patch zero), supported by both
/// parents. Flags are never added implicitly. Unknown flags and flags whose
/// feature prerequisites cannot be evidenced by DevicePlan are rejected.
struct AllocatorDescription {
  VmaAllocatorCreateFlags flags = 0;
  std::optional<std::uint32_t> api_version{};
};

inline constexpr std::uint32_t default_allocator_api_version = VK_API_VERSION_1_0;

struct AllocatorConfiguration {
  AllocatorDescription requested{};
  VmaAllocatorCreateFlags flags = 0;
  std::uint32_t api_version = default_allocator_api_version;
};

enum class AllocatorError : std::uint8_t {
  invalid_parent = 1,
  parent_mismatch,
  unsupported_api_version,
  incompatible_flags,
  invalid_allocator,
};

[[nodiscard]] const std::error_category &allocator_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(AllocatorError error) noexcept;

/// Native VMA budget values, indexed by physical heap, with an explicit count.
/// Without EXT_memory_budget these are VMA estimates, not driver budgets.
struct AllocatorBudgets {
  std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> heaps{};
  std::uint32_t heap_count = 0;
  bool memory_budget_enabled = false;
};

/// Exclusive owner of exactly one VmaAllocator; parents are only borrowed.
/// Application destruction order: Buffer/Image (and all VMA allocations/pools),
/// then Allocator, then Device, then Instance. PhysicalDevice is borrowed from
/// Instance. Complete GPU work before destroying resources; no waitIdle occurs.
/// Move assignment destroys the displaced allocator under the same rules.
/// Native handles must not be destroyed by callers. Synchronization remains
/// application-owned; EXTERNALLY_SYNCHRONIZED also applies to these queries.
class Allocator final {
public:
  Allocator() = delete;
  Allocator(const Allocator &) = delete;
  Allocator &operator=(const Allocator &) = delete;
  Allocator(Allocator &&) noexcept;
  Allocator &operator=(Allocator &&) noexcept;
  ~Allocator();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] VmaAllocator nativeHandle() const noexcept;
  /// Null for moved-from values. Borrowed observers expire on destruction or move.
  [[nodiscard]] const AllocatorConfiguration *configuration() const noexcept;
  [[nodiscard]] VmaAllocatorCreateFlags effectiveFlags() const noexcept;
  [[nodiscard]] const vk::PhysicalDeviceMemoryProperties *memoryProperties() const noexcept;
  [[nodiscard]] std::span<const vk::MemoryHeap> memoryHeaps() const noexcept;
  [[nodiscard]] std::span<const vk::MemoryType> memoryTypes() const noexcept;
  /// Value snapshots; invalid/moved-from allocators return structured errors.
  [[nodiscard]] terreate::Result<AllocatorBudgets> budgets() const;
  [[nodiscard]] terreate::Result<VmaTotalStatistics> statistics() const;

private:
  struct Impl;
  explicit Allocator(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createAllocator(const Instance &, const PhysicalDeviceCandidate &, const Device &,
                              const AllocatorDescription &) -> terreate::Result<Allocator>;
};

/// Candidate need only survive this call; its native parent must outlive the
/// allocator. No parent ownership is retained and no extensions/features enabled.
[[nodiscard]] auto createAllocator(const Instance &instance,
                                   const PhysicalDeviceCandidate &candidate, const Device &device,
                                   const AllocatorDescription &description = {})
    -> terreate::Result<Allocator>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::AllocatorError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_ALLOCATOR_HPP

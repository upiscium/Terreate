#ifndef TERREATE_GRAPHICS_DESCRIPTOR_SET_HPP
#define TERREATE_GRAPHICS_DESCRIPTOR_SET_HPP

#include <terreate/graphics/device.hpp>

namespace terreate::graphics {
class DescriptorPool;

/// Move-only NON-owning view. valid() tests only the cached non-null set handle:
/// it cannot detect pool reset/destruction, stale handles or handle reuse. The
/// caller must discard stale views and synchronize all host/GPU use explicitly.
/// Destruction and move-assignment never free a native set. No liveness registry.
class DescriptorSet final {
public:
  DescriptorSet() noexcept = default;
  DescriptorSet(const DescriptorSet &) = delete;
  DescriptorSet &operator=(const DescriptorSet &) = delete;
  DescriptorSet(DescriptorSet &&) noexcept;
  DescriptorSet &operator=(DescriptorSet &&) noexcept;
  ~DescriptorSet() = default;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::DescriptorSet nativeHandle() const noexcept;
  [[nodiscard]] VkDescriptorSet nativeDescriptorSet() const noexcept;
  [[nodiscard]] vk::DescriptorPool nativePool() const noexcept;
  [[nodiscard]] VkDescriptorPool nativeDescriptorPool() const noexcept;
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  [[nodiscard]] VkDevice nativeVkDevice() const noexcept;
  /// Allocation-time identity only, NOT proof that the layout still exists.
  [[nodiscard]] vk::DescriptorSetLayout nativeLayout() const noexcept;
  [[nodiscard]] VkDescriptorSetLayout nativeDescriptorSetLayout() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  [[nodiscard]] bool correlatedWith(const DescriptorPool &pool) const noexcept;

private:
  DescriptorSet(VkDescriptorSet, VkDescriptorPool, VkDevice, VkDescriptorSetLayout) noexcept;
  void invalidate() noexcept;
  VkDescriptorSet handle_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
  friend class DescriptorPool;
};
} // namespace terreate::graphics

#endif // TERREATE_GRAPHICS_DESCRIPTOR_SET_HPP

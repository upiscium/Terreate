#ifndef TERREATE_GRAPHICS_DESCRIPTOR_SET_LAYOUT_HPP
#define TERREATE_GRAPHICS_DESCRIPTOR_SET_LAYOUT_HPP

#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

#include <terreate/graphics/device.hpp>

namespace terreate::graphics {

/// Explicit binding metadata only, not descriptors or borrowed resource pointers.
/// A default binding is deliberately invalid: supply type, positive count and
/// a nonempty subset of the six core shader stages. No immutable samplers.
struct DescriptorSetLayoutBindingDescription {
  std::uint32_t binding = 0;
  vk::DescriptorType descriptor_type = static_cast<vk::DescriptorType>(VK_DESCRIPTOR_TYPE_MAX_ENUM);
  std::uint32_t descriptor_count = 0;
  vk::ShaderStageFlags stage_flags{};

  bool operator==(const DescriptorSetLayoutBindingDescription &) const = default;
};

/// Owns the caller's ordered bindings. Empty is a valid, explicit zero-binding
/// layout. Binding numbers need not be ordered or contiguous; no normalization,
/// shader reflection or binding semantics are inferred.
struct DescriptorSetLayoutDescription {
  std::vector<DescriptorSetLayoutBindingDescription> bindings{};
};

struct DescriptorSetLayoutConfiguration {
  DescriptorSetLayoutDescription requested{};
  DescriptorSetLayoutDescription effective{};
  /// Pointer-free capability evidence from the creating Device's retained plan.
  /// Only the core descriptor-set and per-stage limits are used in Phase A0.
  vk::PhysicalDeviceLimits limits{};
};

enum class DescriptorSetLayoutError : std::uint8_t {
  invalid_device = 1,
  invalid_description,
  unsupported_configuration,
  native_null_handle,
  allocation_failed,
};

[[nodiscard]] const std::error_category &descriptor_set_layout_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(DescriptorSetLayoutError error) noexcept;

/// Pure CPU validation, with no Device, Vulkan calls or feature enabling.
/// Supports uniform/storage buffers (including dynamic), samplers, sampled/
/// storage images and combined image samplers only. Checks this single layout's
/// contribution to core set and per-stage budgets, not aggregate pipeline layout
/// compatibility or implementation-specific native allocation support.
[[nodiscard]] auto
validateDescriptorSetLayoutDescription(const DescriptorSetLayoutDescription &description,
                                       const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void>;

/// Exclusive owner of one VkDescriptorSetLayout, borrowing its native VkDevice,
/// not the Device wrapper's address. The parent must outlive this owner and any
/// move-assignment displacement. Native handles are borrowed, never destroy them.
/// Host synchronization, dependent native lifetimes and completion of GPU use
/// are application-owned. No hidden waits, allocation of sets or shared ownership.
class DescriptorSetLayout final {
public:
  DescriptorSetLayout() = delete;
  DescriptorSetLayout(const DescriptorSetLayout &) = delete;
  DescriptorSetLayout &operator=(const DescriptorSetLayout &) = delete;
  DescriptorSetLayout(DescriptorSetLayout &&) noexcept;
  DescriptorSetLayout &operator=(DescriptorSetLayout &&) noexcept;
  ~DescriptorSetLayout();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::DescriptorSetLayout nativeHandle() const noexcept;
  [[nodiscard]] VkDescriptorSetLayout nativeDescriptorSetLayout() const noexcept;
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  [[nodiscard]] VkDevice nativeVkDevice() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  /// Borrowed owned snapshot; null after move. Reacquire after ownership moves.
  [[nodiscard]] const DescriptorSetLayoutConfiguration *configuration() const noexcept;

private:
  struct Impl;
  explicit DescriptorSetLayout(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createDescriptorSetLayout(Device &, const DescriptorSetLayoutDescription &)
      -> terreate::Result<DescriptorSetLayout>;
};

/// Revalidates against DevicePlan capabilities and copies requested/effective
/// descriptions identically. No input pointers survive the call. Native VkResult
/// failures are preserved; success with a null handle is rejected.
[[nodiscard]] auto createDescriptorSetLayout(Device &device,
                                             const DescriptorSetLayoutDescription &description)
    -> terreate::Result<DescriptorSetLayout>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::DescriptorSetLayoutError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_DESCRIPTOR_SET_LAYOUT_HPP

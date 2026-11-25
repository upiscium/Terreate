#ifndef TERREATE_GRAPHICS_FORMAT_PROPERTIES_HPP
#define TERREATE_GRAPHICS_FORMAT_PROPERTIES_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <system_error>

#include <terreate/core/result.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

namespace terreate::graphics {

enum class FormatPropertiesError : std::uint8_t {
  /// The Instance is not live.
  invalid_instance = 1,
  /// The candidate was not enumerated from this Instance.
  device_mismatch,
  /// Neither the Vulkan 1.1 nor the Vulkan 1.0 format query could be loaded.
  function_unavailable,
};

[[nodiscard]] const std::error_category &format_properties_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(FormatPropertiesError error) noexcept;

/// What a physical device supports for one format, copied when it was asked. The features are the
/// 64-bit `vk::FormatFeatureFlags2`; where the Instance or the device is below Vulkan 1.3 the
/// 32-bit flags are widened (their bits keep their values), so the same code runs. An unsupported
/// format has empty flags: that is an answer, not an error.
struct FormatProperties {
  vk::Format format = vk::Format::eUndefined;
  vk::FormatFeatureFlags2 linear_tiling{};
  vk::FormatFeatureFlags2 optimal_tiling{};
  vk::FormatFeatureFlags2 buffer{};

  /// True when every bit of `required` is among the features for `tiling` (linear or optimal;
  /// any other tiling has no features here). An empty `required` is always satisfied.
  [[nodiscard]] bool supports(vk::ImageTiling tiling,
                              vk::FormatFeatureFlags2 required) const noexcept {
    if (tiling == vk::ImageTiling::eLinear)
      return (linear_tiling & required) == required;
    if (tiling == vk::ImageTiling::eOptimal)
      return (optimal_tiling & required) == required;
    return false;
  }
  /// True when every bit of `required` is among the buffer features.
  [[nodiscard]] bool supportsBuffer(vk::FormatFeatureFlags2 required) const noexcept {
    return (buffer & required) == required;
  }
};

/// One native query for `format` on `candidate`. The format may be any `vk::Format` value: one the
/// device does not support, or no table knows, gives empty flags. The candidate must have been
/// enumerated from `instance`.
[[nodiscard]] auto queryFormatProperties(const Instance &instance,
                                         const PhysicalDeviceCandidate &candidate,
                                         vk::Format format) -> terreate::Result<FormatProperties>;

/// The application's own preference order, tried in that order: the first format of `preferences`
/// whose features for `tiling` include `required`, or nothing when none does. One query per tried
/// format. The library chooses no format: the list and the required features are the caller's.
[[nodiscard]] auto firstSupportedFormat(const Instance &instance,
                                        const PhysicalDeviceCandidate &candidate,
                                        vk::ImageTiling tiling, vk::FormatFeatureFlags2 required,
                                        std::span<const vk::Format> preferences)
    -> terreate::Result<std::optional<vk::Format>>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::FormatPropertiesError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_FORMAT_PROPERTIES_HPP

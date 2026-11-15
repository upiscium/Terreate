#ifndef TERREATE_GRAPHICS_SPECIALIZATION_HPP
#define TERREATE_GRAPHICS_SPECIALIZATION_HPP

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>

#include <terreate/core/result.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace terreate::graphics {

enum class SpecializationError : std::uint8_t {
  entry_out_of_range = 1,
  duplicate_constant_id,
  empty_entry,
  data_without_entries,
  too_large,
};

[[nodiscard]] const std::error_category &specialization_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SpecializationError error) noexcept;

/// Explicit specialization constants for one shader stage. Owns its map
/// entries and data; pipeline creation copies it into the configuration and
/// points VkSpecializationInfo into that copy. Empty (no entries and no data)
/// means no pSpecializationInfo. Constant IDs and types are never inferred
/// from SPIR-V.
struct SpecializationDescription {
  std::vector<vk::SpecializationMapEntry> map_entries{};
  std::vector<std::byte> data{};

  [[nodiscard]] bool empty() const noexcept { return map_entries.empty() && data.empty(); }
};

/// Pure CPU validation before any native call:
/// - each entry has a nonzero size (zero cannot match any constant,
///   VUID-VkSpecializationMapEntry-constantID-00776) and lies inside the data
///   (VUID-VkSpecializationInfo-offset-00773, -pMapEntries-00774);
/// - constant IDs are unique (VUID-VkSpecializationInfo-constantID-04911);
/// - data without entries is rejected as a caller mistake;
/// - the entry count fits uint32_t.
/// Whether each size matches the shader's constant type (00776) needs the
/// SPIR-V and remains the caller's responsibility.
[[nodiscard]] auto validateSpecializationDescription(const SpecializationDescription &description)
    -> terreate::Result<void>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::SpecializationError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SPECIALIZATION_HPP

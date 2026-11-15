#ifndef TERREATE_GRAPHICS_SRC_SPECIALIZATION_NATIVE_HPP
#define TERREATE_GRAPHICS_SRC_SPECIALIZATION_NATIVE_HPP

#include <cstdint>
#include <vector>

#include <terreate/graphics/specialization.hpp>

namespace terreate::graphics::detail {

/// Native C map entries copied field by field, so no vk:: wrapper storage is
/// accessed through a C struct pointer. May throw std::bad_alloc.
inline std::vector<VkSpecializationMapEntry>
nativeSpecializationEntries(const SpecializationDescription &description) {
  std::vector<VkSpecializationMapEntry> entries;
  entries.reserve(description.map_entries.size());
  for (const auto &entry : description.map_entries)
    entries.push_back(VkSpecializationMapEntry{entry.constantID, entry.offset, entry.size});
  return entries;
}

/// View over `entries` and the description's data, both of which must stay
/// alive and unmodified through the native create call.
inline VkSpecializationInfo
nativeSpecializationInfo(const std::vector<VkSpecializationMapEntry> &entries,
                         const SpecializationDescription &description) noexcept {
  const auto &data = description.data;
  return VkSpecializationInfo{
      .mapEntryCount = static_cast<std::uint32_t>(entries.size()),
      .pMapEntries = entries.empty() ? nullptr : entries.data(),
      .dataSize = data.size(),
      .pData = data.empty() ? nullptr : data.data(),
  };
}

} // namespace terreate::graphics::detail

#endif // TERREATE_GRAPHICS_SRC_SPECIALIZATION_NATIVE_HPP

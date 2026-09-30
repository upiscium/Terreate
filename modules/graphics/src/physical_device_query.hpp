#ifndef TERREATE_GRAPHICS_PHYSICAL_DEVICE_QUERY_HPP
#define TERREATE_GRAPHICS_PHYSICAL_DEVICE_QUERY_HPP

#include "instance_query.hpp"

#include <algorithm>
#include <utility>

#include <terreate/graphics/physical_device.hpp>

namespace terreate::graphics::detail {

/// The pNext members used by the native observation path are valid only when
/// both the parent Instance/device API versions and any enabling extension
/// permit them.  Keeping this calculation separate makes the lower-version
/// boundary explicit and directly testable without manufacturing native
/// Vulkan handles.
struct PhysicalDeviceQueryAvailability {
  bool properties2 = false;
  bool id_properties = false;
  bool driver_properties = false;
  bool features_11 = false;
  bool features_12 = false;
  bool features_13 = false;
  bool memory_properties2 = false;
};

struct PhysicalDeviceQueryApiVersions {
  std::uint32_t effective_instance_api = VK_API_VERSION_1_0;
  std::uint32_t physical_device_api = VK_API_VERSION_1_0;
};

[[nodiscard]] inline PhysicalDeviceQueryAvailability
physical_device_query_availability(PhysicalDeviceQueryApiVersions api_versions,
                                   bool properties2_extension, bool external_memory_extension,
                                   bool driver_properties_extension) noexcept {
  const bool instance_supports_11 = api_versions.effective_instance_api >= VK_API_VERSION_1_1;
  const bool instance_supports_12 = api_versions.effective_instance_api >= VK_API_VERSION_1_2;
  const bool instance_supports_13 = api_versions.effective_instance_api >= VK_API_VERSION_1_3;
  const bool device_supports_11 = api_versions.physical_device_api >= VK_API_VERSION_1_1;
  const bool device_supports_12 = api_versions.physical_device_api >= VK_API_VERSION_1_2;
  const bool device_supports_13 = api_versions.physical_device_api >= VK_API_VERSION_1_3;

  PhysicalDeviceQueryAvailability availability;
  const bool properties2_available = instance_supports_11 || properties2_extension;
  const bool core11_available = instance_supports_11 && device_supports_11;
  const bool core12_available = instance_supports_12 && device_supports_12;
  const bool core13_available = instance_supports_13 && device_supports_13;
  availability.properties2 = properties2_available;
  // VK_KHR_external_memory_capabilities is an instance-level compatibility
  // path for ID properties only when the parent API is pre-1.1.  Once the
  // parent is 1.1 or newer, the core query path is the sole source of IDs.
  const bool pre_11_id_fallback = !instance_supports_11 && external_memory_extension;
  availability.id_properties = availability.properties2 && (core11_available || pre_11_id_fallback);
  // VK_KHR_driver_properties is a device extension.  It is valid with
  // Properties2 independently of the instance's enabled extension list.
  availability.driver_properties =
      availability.properties2 && (core12_available || driver_properties_extension);
  availability.features_11 = availability.properties2 && core11_available;
  availability.features_12 = availability.properties2 && core12_available;
  availability.features_13 = availability.properties2 && core13_available;
  availability.memory_properties2 = availability.properties2;
  return availability;
}

/// The private test adapter receives only the native-query inputs.  It has no
/// construction capability; production physical-device views are created by
/// the source-local query bridge with the implementation-owned authority token.
struct PhysicalDeviceQueryInput {
  const Instance &instance;
  const vk::raii::Instance *native_instance = nullptr;
  const InstancePlan &plan;
};

using PhysicalDeviceQueryAdapter =
    terreate::Result<PhysicalDeviceInventory> (*)(const PhysicalDeviceQueryInput &);

[[nodiscard]] inline terreate::Result<PhysicalDeviceInventory>
query_physical_devices_from_adapter(PhysicalDeviceQueryAdapter query_adapter,
                                    const PhysicalDeviceQueryInput &input) {
  try {
    return query_adapter(input);
  } catch (const vk::SystemError &error) {
    return std::unexpected(
        terreate::Error{error.code(), "query Vulkan physical devices", error.what()});
  }
}

/// Canonicalise copied extension observations by name.  When a synthetic or
/// non-conforming source repeats a name, retain the highest advertised version
/// so the value result is deterministic as well as sorted and deduplicated.
[[nodiscard]] inline std::vector<PhysicalDeviceExtensionProperty>
canonicalize_physical_device_extensions(
    std::vector<PhysicalDeviceExtensionProperty> extension_properties) {
  std::sort(extension_properties.begin(), extension_properties.end(),
            [](const auto &left, const auto &right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.spec_version > right.spec_version;
            });
  extension_properties.erase(
      std::unique(extension_properties.begin(), extension_properties.end(),
                  [](const auto &left, const auto &right) { return left.name == right.name; }),
      extension_properties.end());
  return extension_properties;
}

} // namespace terreate::graphics::detail

#endif // TERREATE_GRAPHICS_PHYSICAL_DEVICE_QUERY_HPP

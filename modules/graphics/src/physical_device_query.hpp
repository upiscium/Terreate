#ifndef TERREATE_GRAPHICS_PHYSICAL_DEVICE_QUERY_HPP
#define TERREATE_GRAPHICS_PHYSICAL_DEVICE_QUERY_HPP

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

[[nodiscard]] PhysicalDeviceQueryAvailability
physical_device_query_availability(PhysicalDeviceQueryApiVersions api_versions,
                                   bool properties2_extension, bool external_memory_extension,
                                   bool driver_properties_extension) noexcept;

/// Native physical-device observation is kept behind a narrow function-pointer
/// seam.  Production uses the Vulkan-Hpp adapter; tests can return value-owned
/// inventories or throw a synthetic vk::SystemError without manufacturing a
/// logical device or exposing a public configuration hook.
using PhysicalDeviceQueryAdapter = terreate::Result<PhysicalDeviceInventory> (*)(
    const Instance &, const vk::raii::Instance *, const InstancePlan &);

[[nodiscard]] terreate::Result<PhysicalDeviceInventory> query_physical_devices_from_adapter(
    PhysicalDeviceQueryAdapter query_adapter, const Instance &instance,
    const vk::raii::Instance *native_instance, const InstancePlan &plan);

/// Canonicalise copied extension observations by name.  When a synthetic or
/// non-conforming source repeats a name, retain the highest advertised version
/// so the value result is deterministic as well as sorted and deduplicated.
[[nodiscard]] std::vector<PhysicalDeviceExtensionProperty> canonicalize_physical_device_extensions(
    std::vector<PhysicalDeviceExtensionProperty> extension_properties);

} // namespace terreate::graphics::detail

#endif // TERREATE_GRAPHICS_PHYSICAL_DEVICE_QUERY_HPP

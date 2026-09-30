#ifndef TERREATE_GRAPHICS_PHYSICAL_DEVICE_QUERY_HPP
#define TERREATE_GRAPHICS_PHYSICAL_DEVICE_QUERY_HPP

#include "instance_query.hpp"

#include <algorithm>
#include <utility>

#include <terreate/graphics/physical_device.hpp>

namespace terreate::graphics::detail {

/// Properties2-family commands have two legal dispatch routes.  Vulkan 1.1
/// promotes the commands to the core Instance API; Vulkan 1.0 can use the
/// extension commands when VK_KHR_get_physical_device_properties2 was enabled.
/// Keeping the route separate from the availability booleans prevents a
/// Vulkan 1.0 extension-backed query from accidentally entering the core Hpp
/// wrapper.
enum class PhysicalDeviceQueryRoute : std::uint8_t {
  unavailable,
  core,
  khr,
};

/// The pNext members used by the native observation path are valid only when
/// both the parent Instance/device API versions and any enabling extension
/// permit them.  Keeping this calculation separate makes the lower-version
/// boundary explicit and directly testable without manufacturing native
/// Vulkan handles.
struct PhysicalDeviceQueryAvailability {
  PhysicalDeviceQueryRoute properties2_route = PhysicalDeviceQueryRoute::unavailable;
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

[[nodiscard]] constexpr inline PhysicalDeviceQueryRoute
physical_device_query_route(PhysicalDeviceQueryApiVersions api_versions,
                            bool properties2_extension) noexcept {
  if (api_versions.effective_instance_api >= VK_API_VERSION_1_1) {
    return PhysicalDeviceQueryRoute::core;
  }
  return properties2_extension ? PhysicalDeviceQueryRoute::khr
                               : PhysicalDeviceQueryRoute::unavailable;
}

[[nodiscard]] constexpr inline PhysicalDeviceQueryAvailability
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
  availability.properties2_route = physical_device_query_route(api_versions, properties2_extension);
  const bool properties2_available =
      availability.properties2_route != PhysicalDeviceQueryRoute::unavailable;
  const bool core11_available = instance_supports_11 && device_supports_11;
  const bool core12_available = instance_supports_12 && device_supports_12;
  const bool core13_available = instance_supports_13 && device_supports_13;
  availability.properties2 = properties2_available;
  // ID properties are queryable only through an available Properties2 path.
  // VK_KHR_external_memory_capabilities authorizes the extension path even
  // when the parent Instance is already 1.1 but the device advertises only
  // Vulkan 1.0.
  availability.id_properties =
      properties2_available && (core11_available || external_memory_extension);
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

[[nodiscard]] constexpr bool id_properties_boundary_is_supported() noexcept {
  const auto vulkan10_without_properties2 = physical_device_query_availability(
      {VK_API_VERSION_1_0, VK_API_VERSION_1_0}, false, true, false);
  const auto vulkan11_instance_with_vulkan10_device = physical_device_query_availability(
      {VK_API_VERSION_1_1, VK_API_VERSION_1_0}, false, true, false);
  return !vulkan10_without_properties2.id_properties &&
         vulkan11_instance_with_vulkan10_device.id_properties;
}

static_assert(id_properties_boundary_is_supported());

/// Invoke exactly the dispatch route selected by the availability query.  The
/// unavailable route deliberately invokes neither callback; the helper is a
/// small private seam so route selection can be regression-tested without
/// manufacturing a native Vulkan physical-device handle.
template <typename CoreQuery, typename KhrQuery>
inline void dispatch_physical_device_query(PhysicalDeviceQueryRoute route, CoreQuery &&core_query,
                                           KhrQuery &&khr_query) {
  switch (route) {
  case PhysicalDeviceQueryRoute::core:
    std::forward<CoreQuery>(core_query)();
    return;
  case PhysicalDeviceQueryRoute::khr:
    std::forward<KhrQuery>(khr_query)();
    return;
  case PhysicalDeviceQueryRoute::unavailable:
    return;
  }
}

/// The private test adapter receives only the native-query inputs.  It has no
/// construction capability; production physical-device views are created by
/// the source-local query bridge with a typed, non-owning `Instance::Impl`
/// parent identity.
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

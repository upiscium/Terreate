#ifndef TERREATE_GRAPHICS_INSTANCE_QUERY_HPP
#define TERREATE_GRAPHICS_INSTANCE_QUERY_HPP

#include <cstdint>
#include <memory>

#include <terreate/graphics/instance.hpp>

namespace terreate::graphics::detail {

using InstanceContextFactory = std::unique_ptr<vk::raii::Context> (*)();
using InstanceCapabilityAdapter =
    terreate::Result<InstanceCapabilities> (*)(const vk::raii::Context *);
using InstanceVersionFunction = PFN_vkEnumerateInstanceVersion;

/// Query a loader API version without entering Vulkan-Hpp's asserting
/// Context::enumerateInstanceVersion wrapper when the native entry point is
/// absent.  A Vulkan 1.0 loader has no version-query function and therefore
/// returns the specification-defined Vulkan 1.0 baseline.
[[nodiscard]] std::uint32_t
query_instance_api_version(InstanceVersionFunction enumerate_instance_version);

/// Invoke one capability adapter behind the narrow vk::SystemError boundary.
/// This separate seam lets tests exercise native query failure without
/// requiring a host Vulkan loader.
[[nodiscard]] terreate::Result<InstanceCapabilities>
query_instance_capabilities_from_adapter(InstanceCapabilityAdapter capability_adapter,
                                         const vk::raii::Context *context);

/// Run the capability query behind the narrow native-loader error boundary.
/// The function-pointer seam keeps loader construction and native query
/// failures deterministic in graphics tests without exposing a production
/// configuration hook.
[[nodiscard]] terreate::Result<InstanceCapabilities>
query_instance_capabilities(InstanceContextFactory context_factory,
                            InstanceCapabilityAdapter capability_adapter);

/// Construct the context for native instance apply behind the loader-only
/// exception boundary.  This private function-pointer seam keeps apply-time
/// loader failures deterministic without exposing a production configuration
/// hook; exceptions from any later apply operation remain outside this
/// boundary.
[[nodiscard]] terreate::Result<std::unique_ptr<vk::raii::Context>>
create_instance_context(InstanceContextFactory context_factory);

} // namespace terreate::graphics::detail

#endif // TERREATE_GRAPHICS_INSTANCE_QUERY_HPP

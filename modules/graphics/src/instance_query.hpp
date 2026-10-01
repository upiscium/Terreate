#ifndef TERREATE_GRAPHICS_INSTANCE_QUERY_HPP
#define TERREATE_GRAPHICS_INSTANCE_QUERY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stdexcept>

#include "graphics_diagnostics.hpp"

#include <terreate/graphics/instance.hpp>
#include <vulkan/vulkan_core.h>

namespace terreate::graphics {

namespace detail {

using InstanceContextFactory = std::unique_ptr<vk::raii::Context> (*)();
using InstanceCapabilityAdapter =
    terreate::Result<InstanceCapabilities> (*)(const vk::raii::Context *);
using InstanceVersionFunction = PFN_vkEnumerateInstanceVersion;

// These types belong to the #268 Vulkan callback adapter.  Keep raw-mask
// conversion state out of the Vulkan-neutral diagnostics bridge; only the
// resulting semantic categories cross that boundary.
struct NativeDiagnosticCategoryMapping {
  std::array<NativeDiagnosticCategory, native_category_order.size()> categories{};
  std::size_t count = 0;

  [[nodiscard]] std::span<const NativeDiagnosticCategory> view() const noexcept {
    return {categories.data(), count};
  }
};

enum class NativeDiagnosticMessageTypeError : std::uint8_t {
  unsupported,
};

using NativeDiagnosticCategoryMappingResult =
    std::expected<NativeDiagnosticCategoryMapping, NativeDiagnosticMessageTypeError>;

/// Adapt the raw Vulkan Debug Utils message-type mask into the semantic
/// category values consumed by the Vulkan-neutral diagnostics bridge.  Zero
/// masks and bits absent from this header's supported raw surface are
/// rejected instead of being truncated or guessed as GENERAL.
[[nodiscard]] inline NativeDiagnosticCategoryMappingResult
map_vulkan_message_types(VkDebugUtilsMessageTypeFlagsEXT message_types) noexcept {
  constexpr VkDebugUtilsMessageTypeFlagsEXT general = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
  constexpr VkDebugUtilsMessageTypeFlagsEXT validation =
      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
  constexpr VkDebugUtilsMessageTypeFlagsEXT performance =
      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

  constexpr VkDebugUtilsMessageTypeFlagsEXT known_message_type_mask =
#ifdef VK_EXT_device_address_binding
      general | validation | performance |
      VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
#else
      general | validation | performance;
#endif

  if (message_types == 0 || (message_types & ~known_message_type_mask) != 0) {
    return std::unexpected(NativeDiagnosticMessageTypeError::unsupported);
  }

  NativeDiagnosticCategoryMapping mapping{};
  if ((message_types & general) != 0) {
    mapping.categories[mapping.count++] = NativeDiagnosticCategory::general;
  }
  if ((message_types & validation) != 0) {
    mapping.categories[mapping.count++] = NativeDiagnosticCategory::validation;
  }
  if ((message_types & performance) != 0) {
    mapping.categories[mapping.count++] = NativeDiagnosticCategory::performance;
  }
#ifdef VK_EXT_device_address_binding
  if ((message_types & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) != 0) {
    mapping.categories[mapping.count++] = NativeDiagnosticCategory::device_address_binding;
  }
#endif
  return mapping;
}

/// Query a loader API version without entering Vulkan-Hpp's asserting
/// Context::enumerateInstanceVersion wrapper when the native entry point is
/// absent.  A Vulkan 1.0 loader has no version-query function and therefore
/// returns the specification-defined Vulkan 1.0 baseline.
[[nodiscard]] inline std::uint32_t
query_instance_api_version(InstanceVersionFunction enumerate_instance_version) {
  if (enumerate_instance_version == nullptr) {
    return VK_API_VERSION_1_0;
  }

  std::uint32_t api_version = VK_API_VERSION_1_0;
  const auto result = enumerate_instance_version(&api_version);
  if (result != VK_SUCCESS) {
    throw vk::SystemError{static_cast<vk::Result>(result)};
  }
  return api_version;
}

/// Invoke one capability adapter behind the narrow vk::SystemError boundary.
/// This separate seam lets tests exercise native query failure without
/// requiring a host Vulkan loader.
[[nodiscard]] inline terreate::Result<InstanceCapabilities>
query_instance_capabilities_from_adapter(InstanceCapabilityAdapter capability_adapter,
                                         const vk::raii::Context *context) {
  try {
    return capability_adapter(context);
  } catch (const vk::SystemError &error) {
    return std::unexpected(
        terreate::Error{error.code(), "query Vulkan instance capabilities", error.what()});
  }
}

/// Run the capability query behind the narrow native-loader error boundary.
/// The function-pointer seam keeps loader construction and native query
/// failures deterministic in graphics tests without exposing a production
/// configuration hook.
[[nodiscard]] inline terreate::Result<InstanceCapabilities>
query_instance_capabilities(InstanceContextFactory context_factory,
                            InstanceCapabilityAdapter capability_adapter) {
  std::unique_ptr<vk::raii::Context> context;
  try {
    context = context_factory();
  } catch (const vk::SystemError &error) {
    return std::unexpected(
        terreate::Error{error.code(), "query Vulkan instance capabilities", error.what()});
  } catch (const std::runtime_error &error) {
    return std::unexpected(
        terreate::Error{terreate::graphics::make_error_code(InstanceError::loader_unavailable),
                        "query Vulkan instance capabilities", error.what()});
  }

  if (context == nullptr) {
    return std::unexpected(terreate::Error{
        terreate::graphics::make_error_code(InstanceError::loader_unavailable),
        "query Vulkan instance capabilities", "Vulkan loader context was not constructed"});
  }

  return query_instance_capabilities_from_adapter(capability_adapter, context.get());
}

/// Construct the context for native instance apply behind the loader-only
/// exception boundary.  This private function-pointer seam keeps apply-time
/// loader failures deterministic without exposing a production configuration
/// hook; exceptions from any later apply operation remain outside this
/// boundary.
[[nodiscard]] inline terreate::Result<std::unique_ptr<vk::raii::Context>>
create_instance_context(InstanceContextFactory context_factory) {
  try {
    auto context = context_factory();
    if (context == nullptr) {
      return std::unexpected(
          terreate::Error{terreate::graphics::make_error_code(InstanceError::loader_unavailable),
                          "create Vulkan instance", "Vulkan loader context was not constructed"});
    }
    return context;
  } catch (const vk::SystemError &error) {
    return std::unexpected(terreate::Error{error.code(), "create Vulkan instance", error.what()});
  } catch (const std::runtime_error &error) {
    return std::unexpected(
        terreate::Error{terreate::graphics::make_error_code(InstanceError::loader_unavailable),
                        "create Vulkan instance", error.what()});
  }
}

} // namespace detail

} // namespace terreate::graphics

#endif // TERREATE_GRAPHICS_INSTANCE_QUERY_HPP

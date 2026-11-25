#include <terreate/graphics/format_properties.hpp>

#include <new>
#include <string>

namespace terreate::graphics {
namespace {

class FormatPropertiesErrorCategory final : public std::error_category {
public:
  [[nodiscard]] const char *name() const noexcept override {
    return "terreate.graphics.format_properties";
  }
  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<FormatPropertiesError>(value)) {
    case FormatPropertiesError::invalid_instance:
      return "the Instance is not live";
    case FormatPropertiesError::device_mismatch:
      return "the physical device candidate does not belong to this Instance";
    case FormatPropertiesError::function_unavailable:
      return "the format properties query could not be loaded";
    }
    return "unknown format properties error";
  }
};

auto fail(FormatPropertiesError error, const char *operation, const char *detail)
    -> terreate::Error {
  try {
    return terreate::Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{make_error_code(error)};
  }
}

// The 32-bit feature bits and the 64-bit ones share their values for the first 31 bits.
vk::FormatFeatureFlags2 widen(VkFormatFeatureFlags flags) noexcept {
  return vk::FormatFeatureFlags2{static_cast<VkFormatFeatureFlags2>(flags)};
}

} // namespace

const std::error_category &format_properties_error_category() noexcept {
  static const FormatPropertiesErrorCategory category;
  return category;
}
std::error_code make_error_code(FormatPropertiesError error) noexcept {
  return {static_cast<int>(error), format_properties_error_category()};
}

auto queryFormatProperties(const Instance &instance, const PhysicalDeviceCandidate &candidate,
                           vk::Format format) -> terreate::Result<FormatProperties> {
  const vk::Instance native_instance = instance.nativeHandle();
  const InstancePlan *plan = instance.plan();
  if (!native_instance || plan == nullptr)
    return std::unexpected(fail(FormatPropertiesError::invalid_instance, "queryFormatProperties",
                                "the Instance is not live"));
  if (!candidate.device().correlatedWith(instance))
    return std::unexpected(fail(FormatPropertiesError::device_mismatch, "queryFormatProperties",
                                "the candidate belongs to another Instance"));

  const auto physical = static_cast<VkPhysicalDevice>(candidate.device().nativeHandle());
  const auto vk_format = static_cast<VkFormat>(format);
  FormatProperties result;
  result.format = format;
  try {
    vk::raii::Context context;
    const auto get_instance_proc = context.getDispatcher()->vkGetInstanceProcAddr;
    const auto raw_instance = static_cast<VkInstance>(native_instance);

    // Core 1.1 query, with FormatProperties3 (64-bit flags) chained when the Instance and the
    // device both have Vulkan 1.3.
    if (plan->effectiveApiVersion() >= VK_API_VERSION_1_1) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      const auto query2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
          get_instance_proc(raw_instance, "vkGetPhysicalDeviceFormatProperties2"));
      if (query2 != nullptr) {
        const bool has_properties3 = plan->effectiveApiVersion() >= VK_API_VERSION_1_3 &&
                                     candidate.capabilities().api_version >= VK_API_VERSION_1_3;
        VkFormatProperties3 properties3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 properties2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        if (has_properties3)
          properties2.pNext = &properties3;
        query2(physical, vk_format, &properties2);
        if (has_properties3) {
          result.linear_tiling = vk::FormatFeatureFlags2{properties3.linearTilingFeatures};
          result.optimal_tiling = vk::FormatFeatureFlags2{properties3.optimalTilingFeatures};
          result.buffer = vk::FormatFeatureFlags2{properties3.bufferFeatures};
        } else {
          result.linear_tiling = widen(properties2.formatProperties.linearTilingFeatures);
          result.optimal_tiling = widen(properties2.formatProperties.optimalTilingFeatures);
          result.buffer = widen(properties2.formatProperties.bufferFeatures);
        }
        return result;
      }
    }
    // The Vulkan 1.0 query.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        get_instance_proc(raw_instance, "vkGetPhysicalDeviceFormatProperties"));
    if (query == nullptr)
      return std::unexpected(fail(FormatPropertiesError::function_unavailable,
                                  "queryFormatProperties",
                                  "vkGetPhysicalDeviceFormatProperties could not be loaded"));
    VkFormatProperties properties{};
    query(physical, vk_format, &properties);
    result.linear_tiling = widen(properties.linearTilingFeatures);
    result.optimal_tiling = widen(properties.optimalTilingFeatures);
    result.buffer = widen(properties.bufferFeatures);
    return result;
  } catch (const vk::SystemError &error) {
    return std::unexpected(terreate::Error{error.code(), "queryFormatProperties", error.what()});
  }
}

auto firstSupportedFormat(const Instance &instance, const PhysicalDeviceCandidate &candidate,
                          vk::ImageTiling tiling, vk::FormatFeatureFlags2 required,
                          std::span<const vk::Format> preferences)
    -> terreate::Result<std::optional<vk::Format>> {
  for (const vk::Format format : preferences) {
    const auto properties = queryFormatProperties(instance, candidate, format);
    if (!properties)
      return std::unexpected(properties.error());
    if (properties->supports(tiling, required))
      return std::optional<vk::Format>{format};
  }
  return std::optional<vk::Format>{};
}

} // namespace terreate::graphics

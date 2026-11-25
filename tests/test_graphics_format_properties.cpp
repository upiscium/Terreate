// Format properties: supports / supportsBuffer require every stated bit, queryFormatProperties
// agrees with the native query and treats unsupported and unknown formats as empty answers,
// firstSupportedFormat walks the caller's list, and a candidate of another Instance is refused.
//
// `cpu` needs no device. `gpu` runs on headless Instances with a software or real device and
// requires the validation layer: without it the test fails, never passes. No linker or platform
// mechanism is used.
#include <terreate/graphics/format_properties.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace terreate::graphics;

const char *test_name = "graphics.format_properties";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

int blocked(const char *message) {
  std::fprintf(stderr, "%s: validation=BLOCKED: %s\n", test_name, message);
  std::fflush(stderr);
  return EXIT_FAILURE;
}

template <typename T>
bool hasError(const terreate::Result<T> &result, FormatPropertiesError error) {
  return !result && result.error().code() == make_error_code(error);
}

int cpuTest() {
  using Feature = vk::FormatFeatureFlagBits2;
  FormatProperties properties;
  properties.linear_tiling = Feature::eSampledImage;
  properties.optimal_tiling = Feature::eSampledImage | Feature::eDepthStencilAttachment;
  properties.buffer = Feature::eVertexBuffer;

  check(properties.supports(vk::ImageTiling::eOptimal, Feature::eDepthStencilAttachment),
        "a present bit is supported");
  check(!properties.supports(vk::ImageTiling::eLinear, Feature::eDepthStencilAttachment),
        "the linear features are separate from the optimal ones");
  check(properties.supports(vk::ImageTiling::eOptimal,
                            Feature::eSampledImage | Feature::eDepthStencilAttachment),
        "every required bit present is supported");
  check(!properties.supports(vk::ImageTiling::eOptimal,
                             Feature::eSampledImage | Feature::eColorAttachment),
        "a partial match is not supported");
  check(properties.supports(vk::ImageTiling::eOptimal, {}), "an empty requirement is satisfied");
  check(!properties.supports(vk::ImageTiling::eDrmFormatModifierEXT, {}) &&
            !properties.supports(vk::ImageTiling::eDrmFormatModifierEXT, Feature::eSampledImage),
        "a tiling other than linear and optimal has no features");
  check(properties.supportsBuffer(Feature::eVertexBuffer) &&
            !properties.supportsBuffer(Feature::eVertexBuffer | Feature::eUniformTexelBuffer),
        "the buffer features are all-bits too");
  if (failures == 0)
    std::puts("graphics.format_properties.cpu: cpu=PASS");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

bool hasName(const std::vector<std::string> &names, std::string_view wanted) {
  return std::ranges::any_of(names, [&](const auto &name) { return name == wanted; });
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        std::fprintf(stderr, "graphics.format_properties: validation message: %s\n",
                     event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

std::optional<Instance> makeInstance(const InstanceCapabilities &capabilities,
                                     ValidationRecorder &recorder, std::uint32_t api_version) {
  InstanceDescription description;
  description.api_version = api_version;
  description.application_name = "terreate-format-properties";
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, capabilities);
  if (!plan)
    return std::nullopt;
  auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return std::nullopt;
  return std::move(*instance);
}

// The native answer for one format, for comparison.
VkFormatProperties nativeProperties(const PhysicalDeviceCandidate &candidate, vk::Format format) {
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(
      static_cast<VkPhysicalDevice>(candidate.device().nativeHandle()),
      static_cast<VkFormat>(format), &properties);
  return properties;
}

int exercise(const Instance &instance, const Instance &other,
             const PhysicalDeviceCandidate &candidate) {
  using Feature = vk::FormatFeatureFlagBits2;

  // D16_UNORM as a depth attachment in optimal tiling is guaranteed by Vulkan.
  const auto depth = queryFormatProperties(instance, candidate, vk::Format::eD16Unorm);
  if (check(bool(depth), "queryFormatProperties answers for D16_UNORM")) {
    check(depth->format == vk::Format::eD16Unorm &&
              depth->supports(vk::ImageTiling::eOptimal, Feature::eDepthStencilAttachment),
          "D16_UNORM supports the depth-stencil attachment in optimal tiling");
  }

  // The answer agrees with the native query, for several formats and all three feature sets.
  for (const vk::Format format : {vk::Format::eD16Unorm, vk::Format::eR8G8B8A8Unorm,
                                  vk::Format::eR32G32B32Sfloat, vk::Format::eB8G8R8A8Srgb}) {
    const auto answer = queryFormatProperties(instance, candidate, format);
    const VkFormatProperties native = nativeProperties(candidate, format);
    // The 64-bit features are a superset of the 32-bit native ones (Vulkan 1.3 reports more
    // than the 1.0 query for the same format), so every native bit must be among them.
    const auto contains = [](vk::FormatFeatureFlags2 wide, VkFormatFeatureFlags narrow) {
      return (static_cast<VkFormatFeatureFlags2>(wide) & narrow) == narrow;
    };
    check(answer && contains(answer->linear_tiling, native.linearTilingFeatures) &&
              contains(answer->optimal_tiling, native.optimalTilingFeatures) &&
              contains(answer->buffer, native.bufferFeatures),
          "the features contain every native vkGetPhysicalDeviceFormatProperties bit");
  }
  const auto sampled = queryFormatProperties(instance, candidate, vk::Format::eR8G8B8A8Unorm);
  check(sampled && sampled->supports(vk::ImageTiling::eOptimal, Feature::eSampledImage),
        "R8G8B8A8_UNORM supports sampling in optimal tiling");

  // An undefined format gives empty flags, not an error.
  const auto undefined = queryFormatProperties(instance, candidate, vk::Format::eUndefined);
  check(undefined && !undefined->linear_tiling && !undefined->optimal_tiling && !undefined->buffer,
        "an undefined format has empty flags");
  // A real format through the buffer features and through linear tiling.
  const auto vertex = queryFormatProperties(instance, candidate, vk::Format::eR32G32B32A32Sfloat);
  check(vertex && vertex->supportsBuffer(Feature::eVertexBuffer),
        "R32G32B32A32_SFLOAT is a vertex buffer format (guaranteed by Vulkan)");
  const std::array<vk::Format, 2> linear_list{vk::Format::eUndefined, vk::Format::eR8G8B8A8Unorm};
  const auto linear =
      firstSupportedFormat(instance, candidate, vk::ImageTiling::eLinear, {}, linear_list);
  check(linear && linear->has_value() && **linear == vk::Format::eUndefined,
        "with no required feature the first entry is returned (linear tiling)");

  // A candidate of another Instance is refused.
  check(hasError(queryFormatProperties(other, candidate, vk::Format::eD16Unorm),
                 FormatPropertiesError::device_mismatch),
        "a candidate of another Instance is refused");

  // firstSupportedFormat: the first supported entry of the caller's list, or nothing.
  const std::array<vk::Format, 3> preferences{vk::Format::eUndefined, vk::Format::eD16Unorm,
                                              vk::Format::eR8G8B8A8Unorm};
  const auto first = firstSupportedFormat(instance, candidate, vk::ImageTiling::eOptimal,
                                          Feature::eDepthStencilAttachment, preferences);
  check(first && first->has_value() && **first == vk::Format::eD16Unorm,
        "firstSupportedFormat returns the first supported entry, in the caller's order");
  const std::array<vk::Format, 2> unsupported{vk::Format::eUndefined, vk::Format::eR8G8B8A8Unorm};
  const auto none = firstSupportedFormat(instance, candidate, vk::ImageTiling::eOptimal,
                                         Feature::eDepthStencilAttachment, unsupported);
  check(none && !none->has_value(), "a list without a supported format gives nothing");
  const auto empty = firstSupportedFormat(instance, candidate, vk::ImageTiling::eOptimal, {}, {});
  check(empty && !empty->has_value(), "an empty list gives nothing");
  check(hasError(firstSupportedFormat(other, candidate, vk::ImageTiling::eOptimal, {}, preferences),
                 FormatPropertiesError::device_mismatch),
        "firstSupportedFormat propagates the query's error");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int gpuTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  if (!hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation") ||
      !hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    return blocked("the validation layer and VK_EXT_debug_utils are required");
  ValidationRecorder recorder;
  auto instance = makeInstance(*capabilities, recorder, VK_API_VERSION_1_3);
  auto other = makeInstance(*capabilities, recorder, VK_API_VERSION_1_3);
  if (!instance || !other)
    return blocked("two Vulkan 1.3 Instances");
  auto inventory = queryPhysicalDevices(*instance);
  if (!inventory)
    return blocked("physical device enumeration");
  if (inventory->empty())
    return blocked("no physical device");
  int status = EXIT_SUCCESS;
  for (const auto &candidate : *inventory) {
    status = exercise(*instance, *other, candidate);
    if (status != EXIT_SUCCESS)
      return status;
  }

  // The widened 32-bit path: a Vulkan 1.1 Instance never chains VkFormatProperties3, so its answer
  // is exactly the native 32-bit one (no bit above 31). It is required when the loader has
  // Vulkan 1.1.
  if (capabilities->loader_api_version >= VK_API_VERSION_1_1) {
    auto legacy = makeInstance(*capabilities, recorder, VK_API_VERSION_1_1);
    if (!legacy)
      return blocked("a Vulkan 1.1 Instance (the loader supports Vulkan 1.1)");
    auto legacy_inventory = queryPhysicalDevices(*legacy);
    if (!legacy_inventory || legacy_inventory->empty())
      return blocked("physical devices of the Vulkan 1.1 Instance");
    for (const auto &candidate : *legacy_inventory) {
      for (const vk::Format format :
           {vk::Format::eD16Unorm, vk::Format::eR8G8B8A8Unorm, vk::Format::eR32G32B32A32Sfloat}) {
        const auto answer = queryFormatProperties(*legacy, candidate, format);
        const VkFormatProperties native = nativeProperties(candidate, format);
        check(answer &&
                  static_cast<VkFormatFeatureFlags2>(answer->linear_tiling) ==
                      native.linearTilingFeatures &&
                  static_cast<VkFormatFeatureFlags2>(answer->optimal_tiling) ==
                      native.optimalTilingFeatures &&
                  static_cast<VkFormatFeatureFlags2>(answer->buffer) == native.bufferFeatures,
              "a Vulkan 1.1 Instance answers with exactly the widened 32-bit native flags");
      }
    }
  }

  // A moved-from Instance is not live.
  auto third = makeInstance(*capabilities, recorder, VK_API_VERSION_1_3);
  if (!third)
    return blocked("a third Instance");
  const Instance taken = std::move(*third);
  check(hasError(queryFormatProperties(*third, (*inventory)[0], vk::Format::eD16Unorm),
                 FormatPropertiesError::invalid_instance),
        "a moved-from Instance is refused");
  check(taken.plan() != nullptr, "the Instance it was moved to is intact");
  check(recorder.warnings_or_errors.load() == 0, "the validation layer reports nothing");
  if (failures == 0)
    std::puts("graphics.format_properties: format_properties=PASS validation-layer=PASS");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv) {
  const std::string_view mode = argc > 1 ? argv[1] : "gpu";
  if (mode == "cpu")
    return cpuTest();
  if (mode == "gpu")
    return gpuTest();
  std::fprintf(stderr, "usage: terreate_graphics_format_properties_tests [cpu|gpu]\n");
  return EXIT_FAILURE;
}

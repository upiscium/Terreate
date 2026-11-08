#include <terreate/graphics/descriptor_set_layout.hpp>

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace terreate::graphics;

static_assert(!std::default_initializable<DescriptorSetLayout>);
static_assert(!std::copy_constructible<DescriptorSetLayout>);
static_assert(!std::is_copy_assignable_v<DescriptorSetLayout>);
static_assert(std::is_nothrow_move_constructible_v<DescriptorSetLayout>);
static_assert(std::is_nothrow_move_assignable_v<DescriptorSetLayout>);
static_assert(std::is_nothrow_destructible_v<DescriptorSetLayout>);
static_assert(std::is_aggregate_v<DescriptorSetLayoutBindingDescription>);
static_assert(std::is_trivially_copyable_v<DescriptorSetLayoutBindingDescription>);
static_assert(std::copy_constructible<DescriptorSetLayoutDescription>);

const char *test_name = "graphics.descriptor_set_layout.cpu";
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
  return EXIT_FAILURE;
}
template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code code) {
  return !result && result.error().code() == make_error_code(code);
}

constexpr auto compute = vk::ShaderStageFlagBits::eCompute;
constexpr std::array stages{
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eCompute,
};
constexpr std::array unsupported_types{
    vk::DescriptorType::eInputAttachment,
    vk::DescriptorType::eUniformTexelBuffer,
    vk::DescriptorType::eStorageTexelBuffer,
    vk::DescriptorType::eInlineUniformBlock,
    vk::DescriptorType::eAccelerationStructureKHR,
    vk::DescriptorType::eMutableEXT,
    static_cast<vk::DescriptorType>(VK_DESCRIPTOR_TYPE_MAX_ENUM),
    static_cast<vk::DescriptorType>(0x7ffffffe),
};

vk::PhysicalDeviceLimits limitsFixture(std::uint32_t limit = 64) {
  vk::PhysicalDeviceLimits limits;
  limits.maxDescriptorSetSamplers = limit;
  limits.maxDescriptorSetUniformBuffers = limit;
  limits.maxDescriptorSetUniformBuffersDynamic = limit;
  limits.maxDescriptorSetStorageBuffers = limit;
  limits.maxDescriptorSetStorageBuffersDynamic = limit;
  limits.maxDescriptorSetSampledImages = limit;
  limits.maxDescriptorSetStorageImages = limit;
  limits.maxPerStageDescriptorSamplers = limit;
  limits.maxPerStageDescriptorUniformBuffers = limit;
  limits.maxPerStageDescriptorStorageBuffers = limit;
  limits.maxPerStageDescriptorSampledImages = limit;
  limits.maxPerStageDescriptorStorageImages = limit;
  limits.maxPerStageResources = limit;
  return limits;
}

DescriptorSetLayoutDescription oneBinding(vk::DescriptorType type, std::uint32_t count,
                                          vk::ShaderStageFlags visibility = compute) {
  return {{{0, type, count, visibility}}};
}

auto invalidDescriptions() {
  const auto unknown = vk::ShaderStageFlags{static_cast<VkShaderStageFlags>(0x80000000U)};
  return std::array{
      DescriptorSetLayoutDescription{{DescriptorSetLayoutBindingDescription{}}},
      oneBinding(vk::DescriptorType::eUniformBuffer, 0),
      oneBinding(vk::DescriptorType::eUniformBuffer, 1, {}),
      oneBinding(vk::DescriptorType::eUniformBuffer, 1, unknown),
      oneBinding(vk::DescriptorType::eUniformBuffer, 1, unknown | compute),
      oneBinding(vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eAll),
      DescriptorSetLayoutDescription{
          {{4, vk::DescriptorType::eUniformBuffer, 1, compute},
           {2, vk::DescriptorType::eSampler, 1, compute},
           {4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eFragment}}},
  };
}

enum class Injection : std::uint8_t { none, failure, null_success };
struct NativeTracker {
  struct Record {
    VkDescriptorSetLayout handle = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
  };
  std::array<Record, 16> live{};
  std::size_t creates = 0;
  std::size_t native_handles = 0;
  std::size_t destroys = 0;
  VkDescriptorSetLayout last_destroyed = VK_NULL_HANDLE;
  Injection injection = Injection::none;
  bool forbid_create = true;

  void insert(Record record) {
    ++native_handles;
    for (auto &slot : live) {
      if (slot.handle == VK_NULL_HANDLE) {
        slot = record;
        return;
      }
    }
    check(false, "native handle tracker capacity exhausted");
  }
  VkDevice remove(VkDescriptorSetLayout handle, VkDevice device) {
    ++destroys;
    last_destroyed = handle;
    for (auto &slot : live) {
      if (slot.handle == handle) {
        const auto parent = slot.device;
        check(parent == device, "native destruction uses the creating Device");
        slot = {};
        return parent;
      }
    }
    check(false, "double destruction or untracked handle");
    return VK_NULL_HANDLE;
  }
  bool empty() const {
    for (const auto &slot : live)
      if (slot.handle != VK_NULL_HANDLE)
        return false;
    return true;
  }
} layouts;

struct NativeObservation {
  VkDevice device = VK_NULL_HANDLE;
  bool exact_header = false;
  bool pointer_matches_count = false;
  bool no_immutable_samplers = true;
  bool independent_input_storage = false;
  std::uint32_t binding_count = 0;
  std::array<DescriptorSetLayoutBindingDescription, 16> bindings{};
} observed;

// Test-only hook: free the caller's vector DURING the synchronous native call,
// before reading native bindings. The adapter must already own its snapshots
// and temporary native array. Never retain or dereference a native input pointer
// after vkCreateDescriptorSetLayout returns.
DescriptorSetLayoutDescription *input_to_release = nullptr;

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity == terreate::DiagnosticSeverity::warning ||
        event.severity == terreate::DiagnosticSeverity::error) {
      for (const auto &category : event.categories) {
        if (category == "VALIDATION") {
          warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
    }
  }
};
} // namespace

// Native C wrappers, never replacement Terreate symbols. Failure and null-success
// injections do not call the driver and cannot hide a real native allocation.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vkCreateDescriptorSetLayout(VkDevice,
                                                       const VkDescriptorSetLayoutCreateInfo *,
                                                       const VkAllocationCallbacks *,
                                                       VkDescriptorSetLayout *);
extern "C" void __real_vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout,
                                                    const VkAllocationCallbacks *);

extern "C" VkResult __wrap_vkCreateDescriptorSetLayout(VkDevice device,
                                                       const VkDescriptorSetLayoutCreateInfo *info,
                                                       const VkAllocationCallbacks *alloc,
                                                       VkDescriptorSetLayout *result) {
  ++layouts.creates;
  if (result)
    *result = VK_NULL_HANDLE;
  if (!check(!layouts.forbid_create, "invalid or CPU-only request reached native creation") ||
      !check(info && result, "native creation arguments present"))
    return VK_ERROR_INITIALIZATION_FAILED;
  observed = {};
  observed.device = device;
  observed.exact_header = info->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO &&
                          !info->pNext && info->flags == 0 && !alloc;
  observed.binding_count = info->bindingCount;
  observed.pointer_matches_count = (info->bindingCount == 0) == (info->pBindings == nullptr);
  if (!check(observed.exact_header && observed.pointer_matches_count &&
                 info->bindingCount <= observed.bindings.size(),
             "exact native header, count and binding pointer required"))
    return VK_ERROR_INITIALIZATION_FAILED;
  if (input_to_release) {
    auto *input = std::exchange(input_to_release, nullptr);
    observed.independent_input_storage = static_cast<const void *>(info->pBindings) !=
                                         static_cast<const void *>(input->bindings.data());
    if (!check(observed.independent_input_storage,
               "native binding array cannot alias caller storage"))
      return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<DescriptorSetLayoutBindingDescription>{}.swap(input->bindings);
  }
  for (std::size_t index = 0; index < info->bindingCount; ++index) {
    const auto &binding = info->pBindings[index];
    observed.no_immutable_samplers &= binding.pImmutableSamplers == nullptr;
    observed.bindings[index] = {binding.binding,
                                static_cast<vk::DescriptorType>(binding.descriptorType),
                                binding.descriptorCount, vk::ShaderStageFlags{binding.stageFlags}};
  }
  if (!check(observed.no_immutable_samplers, "native immutable sampler pointers are always null"))
    return VK_ERROR_INITIALIZATION_FAILED;
  const auto injection = std::exchange(layouts.injection, Injection::none);
  if (injection != Injection::none)
    return injection == Injection::null_success ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
  const auto status = __real_vkCreateDescriptorSetLayout(device, info, alloc, result);
  if (status == VK_SUCCESS && *result != VK_NULL_HANDLE)
    layouts.insert({*result, device});
  return status;
}

extern "C" void __wrap_vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout,
                                                    const VkAllocationCallbacks *alloc) {
  if (layout == VK_NULL_HANDLE)
    return;
  check(!alloc, "native destruction uses the same null allocator");
  const auto parent = layouts.remove(layout, device);
  // Avoid damaging driver state if a broken owner tries to destroy twice.
  if (parent != VK_NULL_HANDLE)
    __real_vkDestroyDescriptorSetLayout(parent, layout, alloc);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
void cpuTypeLimits() {
  struct TypeLimits {
    vk::DescriptorType type;
    std::uint32_t vk::PhysicalDeviceLimits::*set;
    std::uint32_t vk::PhysicalDeviceLimits::*stage;
  };
  const std::array cases{
      TypeLimits{vk::DescriptorType::eUniformBuffer,
                 &vk::PhysicalDeviceLimits::maxDescriptorSetUniformBuffers,
                 &vk::PhysicalDeviceLimits::maxPerStageDescriptorUniformBuffers},
      TypeLimits{vk::DescriptorType::eStorageBuffer,
                 &vk::PhysicalDeviceLimits::maxDescriptorSetStorageBuffers,
                 &vk::PhysicalDeviceLimits::maxPerStageDescriptorStorageBuffers},
      TypeLimits{vk::DescriptorType::eSampler, &vk::PhysicalDeviceLimits::maxDescriptorSetSamplers,
                 &vk::PhysicalDeviceLimits::maxPerStageDescriptorSamplers},
      TypeLimits{vk::DescriptorType::eSampledImage,
                 &vk::PhysicalDeviceLimits::maxDescriptorSetSampledImages,
                 &vk::PhysicalDeviceLimits::maxPerStageDescriptorSampledImages},
      TypeLimits{vk::DescriptorType::eStorageImage,
                 &vk::PhysicalDeviceLimits::maxDescriptorSetStorageImages,
                 &vk::PhysicalDeviceLimits::maxPerStageDescriptorStorageImages},
      TypeLimits{vk::DescriptorType::eCombinedImageSampler,
                 &vk::PhysicalDeviceLimits::maxDescriptorSetSampledImages,
                 &vk::PhysicalDeviceLimits::maxPerStageDescriptorSampledImages},
      TypeLimits{vk::DescriptorType::eUniformBufferDynamic,
                 &vk::PhysicalDeviceLimits::maxDescriptorSetUniformBuffersDynamic,
                 &vk::PhysicalDeviceLimits::maxPerStageDescriptorUniformBuffers},
      TypeLimits{vk::DescriptorType::eStorageBufferDynamic,
                 &vk::PhysicalDeviceLimits::maxDescriptorSetStorageBuffersDynamic,
                 &vk::PhysicalDeviceLimits::maxPerStageDescriptorStorageBuffers},
  };
  for (const auto &entry : cases) {
    const auto description =
        oneBinding(entry.type, 2, vk::ShaderStageFlagBits::eAllGraphics | compute);
    auto limits = limitsFixture(2);
    check(validateDescriptorSetLayoutDescription(description, limits).has_value(),
          "each supported type accepts exact limits and all six stages without multiplying set "
          "usage");
    limits.*entry.set = 1;
    check(hasError(validateDescriptorSetLayoutDescription(description, limits),
                   DescriptorSetLayoutError::unsupported_configuration),
          "each supported type enforces its own set limit");
    limits = limitsFixture(2);
    limits.*entry.stage = 1;
    check(hasError(validateDescriptorSetLayoutDescription(description, limits),
                   DescriptorSetLayoutError::unsupported_configuration),
          "each supported type enforces its own per-stage limit");
    check(hasError(validateDescriptorSetLayoutDescription(
                       oneBinding(entry.type, std::numeric_limits<std::uint32_t>::max()),
                       limitsFixture()),
                   DescriptorSetLayoutError::unsupported_configuration),
          "huge descriptor array rejected for every supported type");
  }
}

void cpuMixedLimits() {
  auto limits = limitsFixture(2);
  const auto combined = oneBinding(vk::DescriptorType::eCombinedImageSampler, 2);
  check(validateDescriptorSetLayoutDescription(combined, limits).has_value(),
        "combined sampler counts twice by category but once against maxPerStageResources");
  for (const auto member : {&vk::PhysicalDeviceLimits::maxDescriptorSetSamplers,
                            &vk::PhysicalDeviceLimits::maxDescriptorSetSampledImages,
                            &vk::PhysicalDeviceLimits::maxPerStageDescriptorSamplers,
                            &vk::PhysicalDeviceLimits::maxPerStageDescriptorSampledImages,
                            &vk::PhysicalDeviceLimits::maxPerStageResources}) {
    auto restricted = limits;
    restricted.*member = 1;
    check(hasError(validateDescriptorSetLayoutDescription(combined, restricted),
                   DescriptorSetLayoutError::unsupported_configuration),
          "combined image sampler consumes every applicable category and resource limit");
  }
  limits.maxPerStageResources = 0;
  check(validateDescriptorSetLayoutDescription(oneBinding(vk::DescriptorType::eSampler, 2), limits)
            .has_value(),
        "standalone sampler is excluded from maxPerStageResources");

  for (const bool uniform : {true, false}) {
    const auto base =
        uniform ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eStorageBuffer;
    const auto dynamic = uniform ? vk::DescriptorType::eUniformBufferDynamic
                                 : vk::DescriptorType::eStorageBufferDynamic;
    const auto set_member = uniform ? &vk::PhysicalDeviceLimits::maxDescriptorSetUniformBuffers
                                    : &vk::PhysicalDeviceLimits::maxDescriptorSetStorageBuffers;
    const auto dynamic_member =
        uniform ? &vk::PhysicalDeviceLimits::maxDescriptorSetUniformBuffersDynamic
                : &vk::PhysicalDeviceLimits::maxDescriptorSetStorageBuffersDynamic;
    const auto stage_member = uniform
                                  ? &vk::PhysicalDeviceLimits::maxPerStageDescriptorUniformBuffers
                                  : &vk::PhysicalDeviceLimits::maxPerStageDescriptorStorageBuffers;
    limits = limitsFixture(3);
    limits.*set_member = 2;
    DescriptorSetLayoutDescription mixed{{{8, base, 1, compute}, {3, dynamic, 2, compute}}};
    check(hasError(validateDescriptorSetLayoutDescription(mixed, limits),
                   DescriptorSetLayoutError::unsupported_configuration),
          "dynamic and nondynamic buffers jointly consume the maxDescriptorSet buffer budget");
    limits.*set_member = 3;
    check(validateDescriptorSetLayoutDescription(mixed, limits).has_value(),
          "combined regular/dynamic buffer count fits the aggregate set limit");
    limits.*stage_member = 2;
    check(hasError(validateDescriptorSetLayoutDescription(mixed, limits),
                   DescriptorSetLayoutError::unsupported_configuration),
          "dynamic and nondynamic buffers share a stage category");
    mixed.bindings[0].stage_flags = vk::ShaderStageFlagBits::eVertex;
    check(validateDescriptorSetLayoutDescription(mixed, limits).has_value(),
          "disjoint stages have independent counters");
    limits.*set_member = 1;
    check(hasError(validateDescriptorSetLayoutDescription(oneBinding(dynamic, 2), limits),
                   DescriptorSetLayoutError::unsupported_configuration),
          "dynamic buffers also consume aggregate maxDescriptorSet buffer limits");
    limits.*set_member = 3;
    limits.*dynamic_member = 1;
    check(hasError(validateDescriptorSetLayoutDescription(oneBinding(dynamic, 2), limits),
                   DescriptorSetLayoutError::unsupported_configuration),
          "dynamic descriptors additionally consume their dynamic-specific set limits");
  }

  DescriptorSetLayoutDescription mixed{
      {{10, vk::DescriptorType::eUniformBuffer, 1, compute},
       {2, vk::DescriptorType::eStorageBuffer, 1, compute},
       {9, vk::DescriptorType::eSampledImage, 1, compute},
       {3, vk::DescriptorType::eStorageImage, 1, compute},
       {8, vk::DescriptorType::eCombinedImageSampler, 1, compute},
       {4, vk::DescriptorType::eSampler, 5, compute},
       {7, vk::DescriptorType::eUniformBufferDynamic, 1, compute},
       {5, vk::DescriptorType::eStorageBufferDynamic, 1, compute}}};
  limits = limitsFixture();
  limits.maxPerStageResources = 7;
  check(validateDescriptorSetLayoutDescription(mixed, limits).has_value(),
        "mixed resource budget excludes five samplers and counts combined image only once");
  limits.maxPerStageResources = 6;
  check(hasError(validateDescriptorSetLayoutDescription(mixed, limits),
                 DescriptorSetLayoutError::unsupported_configuration),
        "mixed resource types still consume one shared per-stage resource budget");
  limits = limitsFixture();
  limits.maxPerStageDescriptorSamplers = 5;
  check(hasError(validateDescriptorSetLayoutDescription(mixed, limits),
                 DescriptorSetLayoutError::unsupported_configuration),
        "standalone and combined samplers accumulate together");
  limits = limitsFixture();
  limits.maxDescriptorSetSampledImages = 1;
  check(hasError(validateDescriptorSetLayoutDescription(mixed, limits),
                 DescriptorSetLayoutError::unsupported_configuration),
        "sampled and combined images accumulate together");

  for (const auto stage : stages) {
    auto description = oneBinding(vk::DescriptorType::eStorageImage, 2,
                                  vk::ShaderStageFlagBits::eAllGraphics | compute);
    description.bindings.push_back({7, vk::DescriptorType::eStorageImage, 1, stage});
    limits = limitsFixture();
    limits.maxPerStageDescriptorStorageImages = 2;
    check(hasError(validateDescriptorSetLayoutDescription(description, limits),
                   DescriptorSetLayoutError::unsupported_configuration),
          "overlapping multibit visibility is counted independently for every core stage");
    limits = limitsFixture();
    limits.maxPerStageResources = 2;
    check(hasError(validateDescriptorSetLayoutDescription(description, limits),
                   DescriptorSetLayoutError::unsupported_configuration),
          "resource budget is enforced for every core stage, not only compute");
  }

  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  limits = limitsFixture(maximum);
  auto overflow = oneBinding(vk::DescriptorType::eSampledImage, maximum);
  check(validateDescriptorSetLayoutDescription(overflow, limits).has_value(),
        "UINT32_MAX exact count does not overflow or trigger an arbitrary smaller cap");
  overflow.bindings.push_back(
      {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eVertex});
  check(hasError(validateDescriptorSetLayoutDescription(overflow, limits),
                 DescriptorSetLayoutError::unsupported_configuration),
        "set sum cannot wrap at UINT32_MAX even with disjoint stages");
  overflow.bindings[1] = {1, vk::DescriptorType::eStorageImage, 1, compute};
  check(hasError(validateDescriptorSetLayoutDescription(overflow, limits),
                 DescriptorSetLayoutError::unsupported_configuration),
        "mixed per-stage resource sum cannot wrap at UINT32_MAX");
  overflow.bindings[1].stage_flags = vk::ShaderStageFlagBits::eVertex;
  check(validateDescriptorSetLayoutDescription(overflow, limits).has_value(),
        "no artificial global resource sum rejects disjoint-stage, distinct-type budgets");
}

bool cpuTest() {
  check(validateDescriptorSetLayoutDescription({}, {}).has_value(),
        "explicit empty description is valid even with all limits zero");
  for (const auto &description : invalidDescriptions())
    check(hasError(validateDescriptorSetLayoutDescription(description, limitsFixture()),
                   DescriptorSetLayoutError::invalid_description),
          "default binding, zero count, zero/unknown/all stages and duplicates rejected");
  for (const auto type : unsupported_types)
    check(hasError(validateDescriptorSetLayoutDescription(oneBinding(type, 1), limitsFixture()),
                   DescriptorSetLayoutError::unsupported_configuration),
          "input attachments, texel buffers, extension and unknown enum types fail closed");
  auto sparse = oneBinding(vk::DescriptorType::eUniformBuffer, 1);
  sparse.bindings[0].binding = std::numeric_limits<std::uint32_t>::max();
  sparse.bindings.push_back({0, vk::DescriptorType::eSampler, 1, compute});
  const auto original = sparse;
  check(validateDescriptorSetLayoutDescription(sparse, limitsFixture()).has_value() &&
            sparse.bindings == original.bindings,
        "binding numbers are not descriptor counts; sparse unordered bindings remain unchanged");
  cpuTypeLimits();
  cpuMixedLimits();
  const std::error_code error = DescriptorSetLayoutError::invalid_description;
  check(error.value() != 0 && error.category() == descriptor_set_layout_error_category() &&
            std::string_view{error.category().name()} == "terreate.graphics.descriptor_set_layout",
        "public structured error code and category are inspectable");
  check(layouts.creates == 0 && layouts.destroys == 0 && layouts.native_handles == 0,
        "CPU validation makes no native create or destroy calls");
  if (failures == 0)
    std::puts("graphics.descriptor_set_layout.cpu: cpu=PASS");
  return failures == 0;
}

bool layoutTest(Device &device, Device &moved_from, Device &other) {
  const auto creates_before = layouts.creates;
  const auto handles_before = layouts.native_handles;
  const auto destroys_before = layouts.destroys;
  layouts.forbid_create = true;
  check(
      hasError(createDescriptorSetLayout(moved_from, {}), DescriptorSetLayoutError::invalid_device),
      "moved-from Device rejected before native creation");
  for (const auto &description : invalidDescriptions())
    check(hasError(createDescriptorSetLayout(device, description),
                   DescriptorSetLayoutError::invalid_description),
          "factory repeats CPU structural validation");
  for (const auto type : unsupported_types)
    check(hasError(createDescriptorSetLayout(device, oneBinding(type, 1)),
                   DescriptorSetLayoutError::unsupported_configuration),
          "factory rejects unsupported types before native creation");
  auto excessive =
      oneBinding(vk::DescriptorType::eSampler, std::numeric_limits<std::uint32_t>::max());
  excessive.bindings.push_back({1, vk::DescriptorType::eSampler, 1, compute});
  check(hasError(createDescriptorSetLayout(device, excessive),
                 DescriptorSetLayoutError::unsupported_configuration) &&
            layouts.creates == creates_before,
        "actual retained DevicePlan limits reject excess before native creation");
  layouts.forbid_create = false;

  layouts.injection = Injection::failure;
  const auto failed = createDescriptorSetLayout(device, {});
  check(hasError(failed, vk::Result::eErrorOutOfHostMemory) &&
            failed.error().context() == "vkCreateDescriptorSetLayout" &&
            layouts.creates == creates_before + 1 && layouts.native_handles == handles_before &&
            layouts.destroys == destroys_before && layouts.injection == Injection::none,
        "native failure preserves VkResult and operation without an owner or driver allocation");
  layouts.injection = Injection::null_success;
  check(hasError(createDescriptorSetLayout(device, {}),
                 DescriptorSetLayoutError::native_null_handle) &&
            layouts.creates == creates_before + 2 && layouts.native_handles == handles_before &&
            layouts.destroys == destroys_before && layouts.injection == Injection::none,
        "success with null handle is rejected without creating a hidden real allocation");

  {
    DescriptorSetLayoutDescription description{
        {{11, vk::DescriptorType::eUniformBuffer, 2,
          vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
         {2, vk::DescriptorType::eCombinedImageSampler, 3, vk::ShaderStageFlagBits::eFragment},
         {9, vk::DescriptorType::eStorageBuffer, 1, compute},
         {0, vk::DescriptorType::eSampler, 2, vk::ShaderStageFlagBits::eFragment},
         {7, vk::DescriptorType::eSampledImage, 2, vk::ShaderStageFlagBits::eFragment},
         {5, vk::DescriptorType::eStorageImage, 1, compute},
         {3, vk::DescriptorType::eUniformBufferDynamic, 1, vk::ShaderStageFlagBits::eVertex},
         {1, vk::DescriptorType::eStorageBufferDynamic, 1, compute}}};
    const auto requested = description;
    input_to_release = &description;
    auto first = createDescriptorSetLayout(device, description);
    const bool released_input = input_to_release == nullptr && description.bindings.empty();
    input_to_release = nullptr;
    if (!check(first.has_value(), "real Vulkan layout creation with all eight supported types"))
      return false;
    const auto first_native = first->nativeDescriptorSetLayout();
    check(released_input && observed.independent_input_storage && observed.exact_header &&
              observed.pointer_matches_count && observed.no_immutable_samplers &&
              observed.binding_count == requested.bindings.size() &&
              observed.device == static_cast<VkDevice>(device.nativeHandle()),
          "exact native create info and owned pointer lifetime survive caller storage release");
    for (std::size_t index = 0; index < requested.bindings.size(); ++index)
      check(observed.bindings[index] == requested.bindings[index],
            "native binding numbers, descriptor counts, types, stages and original ordering exact");
    check(first->valid() && static_cast<bool>(*first) &&
              first->nativeHandle() == vk::DescriptorSetLayout{first_native} &&
              first->nativeDevice() == device.nativeHandle() &&
              first->nativeVkDevice() == static_cast<VkDevice>(device.nativeHandle()) &&
              first->correlatedWith(device) && !first->correlatedWith(other) &&
              !first->correlatedWith(moved_from),
          "borrowed native handles and correlation distinguish separately created Devices");
    check(first->configuration() &&
              first->configuration()->requested.bindings == requested.bindings &&
              first->configuration()->effective.bindings == requested.bindings &&
              first->configuration()->limits == device.plan()->capabilities().properties.limits,
          "requested/effective and actual limits are independent owned snapshots");

    // A different native Device also proves move-assignment uses the displaced
    // owner's parent, not the incoming parent's handle, for destruction.
    auto second = createDescriptorSetLayout(other, {});
    if (!check(second.has_value(), "real zero-binding layout on a separate logical Device"))
      return false;
    const auto second_native = second->nativeDescriptorSetLayout();
    check(observed.exact_header && observed.pointer_matches_count && observed.binding_count == 0 &&
              second->configuration() && second->configuration()->requested.bindings.empty() &&
              second->configuration()->effective.bindings.empty() &&
              second->correlatedWith(other) && !second->correlatedWith(device),
          "empty layout has zero count, null binding array and exact empty configuration");
    // Intentionally inspect the documented moved-from observers.
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    DescriptorSetLayout moved = std::move(*first);
    check(!first->valid() && !static_cast<bool>(*first) &&
              first->nativeDescriptorSetLayout() == VK_NULL_HANDLE &&
              first->nativeHandle() == vk::DescriptorSetLayout{} &&
              first->nativeDevice() == vk::Device{} && first->nativeVkDevice() == VK_NULL_HANDLE &&
              !first->configuration() && !first->correlatedWith(device) &&
              moved.nativeDescriptorSetLayout() == first_native &&
              layouts.destroys == destroys_before,
          "move construction transfers sole ownership and nulls every source observer");
    *second = std::move(moved);
    check(!moved.valid() && !static_cast<bool>(moved) &&
              moved.nativeDescriptorSetLayout() == VK_NULL_HANDLE &&
              moved.nativeHandle() == vk::DescriptorSetLayout{} &&
              moved.nativeDevice() == vk::Device{} && moved.nativeVkDevice() == VK_NULL_HANDLE &&
              !moved.configuration() && !moved.correlatedWith(device) &&
              second->nativeDescriptorSetLayout() == first_native &&
              second->correlatedWith(device) && !second->correlatedWith(other) &&
              layouts.destroys == destroys_before + 1 && layouts.last_destroyed == second_native,
          "cross-Device move assignment destroys displaced ownership exactly once");
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    check(second->configuration() &&
              second->configuration()->effective.bindings == requested.bindings,
          "configuration follows moved ownership");
  }
  check(layouts.creates == creates_before + 4 && layouts.native_handles == handles_before + 2 &&
            layouts.destroys == destroys_before + 2 && layouts.empty(),
        "both real layouts destroyed exactly once with no submission or waits");
  return failures == 0;
}

void verifyAndCleanNativeObjects() {
  // All child owners have left scope. Even a broken implementation must not
  // leave a leaked child alive when the test's real Devices are destroyed.
  check(layouts.empty() && layouts.native_handles == layouts.destroys,
        "native handle creation and destruction balance exactly");
  for (const auto &record : layouts.live)
    if (record.handle != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(record.device, record.handle, nullptr);
}

int exerciseDevices(Instance &instance) {
  const auto devices = queryPhysicalDevices(instance);
  if (!devices || devices->empty())
    return blocked("no Vulkan PhysicalDevice");
  for (const auto &candidate : *devices) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t index = 0; index < families.size(); ++index) {
      if (!families[index].queueCount)
        continue;
      const auto flags = families[index].queueFlags;
      vk::QueueFlags required;
      if (flags & vk::QueueFlagBits::eTransfer)
        required = vk::QueueFlagBits::eTransfer;
      else if (flags & vk::QueueFlagBits::eCompute)
        required = vk::QueueFlagBits::eCompute;
      else if (flags & vk::QueueFlagBits::eGraphics)
        required = vk::QueueFlagBits::eGraphics;
      else
        continue;
      DeviceDescription request;
      DeviceQueueRequest queue;
      queue.caller_id = "descriptor-set-layout-test";
      queue.required_flags = required;
      queue.allowed_family_indices = {index};
      request.queue_requests.push_back(queue);
      const auto plan = resolveDevice(candidate, request);
      if (!plan)
        continue;
      auto device = createDevice(instance, *plan);
      auto other = createDevice(instance, *plan);
      if (!device || !other)
        return blocked("two native logical Devices required for parent-correlation tests");
      layouts.forbid_create = false;
      auto before_move = createDescriptorSetLayout(*device, {});
      if (!check(before_move.has_value(), "real layout created before parent Device move")) {
        verifyAndCleanNativeObjects();
        return EXIT_FAILURE;
      }
      // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
      Device moved_device = std::move(*device);
      {
        DescriptorSetLayout retained = std::move(*before_move);
        check(retained.correlatedWith(moved_device) && !retained.correlatedWith(*device) &&
                  retained.nativeDevice() == moved_device.nativeHandle(),
              "existing child borrows the native Device and follows wrapper moves");
      }
      const bool passed = layoutTest(moved_device, *device, *other);
      // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
      verifyAndCleanNativeObjects();
      return passed && failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  }
  return blocked("no queue family with a usable native Device");
}

bool validationProbe(Instance &instance, ValidationRecorder &recorder) {
  const auto raw = static_cast<VkInstance>(instance.nativeHandle());
  const auto send = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
      vkGetInstanceProcAddr(raw, "vkSubmitDebugUtilsMessageEXT"));
  if (!check(send != nullptr, "native validation callback entry point available"))
    return false;
  VkDebugUtilsMessengerCallbackDataEXT data{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
  data.pMessageIdName = "terreate-descriptor-set-layout-validation-probe";
  data.messageIdNumber = 266;
  data.pMessage = "synthetic validation warning to verify native sink";
  const auto prior = recorder.warnings_or_errors.load(std::memory_order_relaxed);
  send(raw, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
  if (!check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == prior + 1,
             "native Debug Utils warning reaches the application recorder"))
    return false;
  recorder.warnings_or_errors.fetch_sub(1, std::memory_order_relaxed);
  return true;
}

int nativeTest() {
  const auto caps = queryInstanceCapabilities();
  if (!caps)
    return blocked("Vulkan instance capabilities unavailable");
  bool has_validation = false;
  bool has_debug = false;
  for (const auto &layer : caps->available_layers)
    has_validation |= layer == "VK_LAYER_KHRONOS_validation";
  for (const auto &extension : caps->available_extensions)
    has_debug |= extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (!has_validation || !has_debug)
    return blocked("Validation Layer and Debug Utils are both required, never skipped");
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *caps);
  if (!plan)
    return blocked("Vulkan 1.3 Instance resolution");
  bool layer_enabled = false;
  for (const auto &layer : plan->enabledLayers())
    layer_enabled |= layer == "VK_LAYER_KHRONOS_validation";
  if (!layer_enabled || !plan->debugUtilsEnabled())
    return blocked("required Validation Layer / Debug Utils missing from effective configuration");
  ValidationRecorder recorder;
  {
    auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
    if (!instance)
      return blocked("headless Vulkan Instance creation");
    if (!validationProbe(*instance, recorder))
      return EXIT_FAILURE;
    const auto status = exerciseDevices(*instance);
    if (status != EXIT_SUCCESS)
      return status;
  }
  check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == 0,
        "zero real Validation Layer warnings/errors, including Device and Instance teardown");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.descriptor_set_layout: headless-native=PASS validation-layer=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify cpu or gpu mode");
  const std::string_view mode{argv[1]};
  if (mode == "cpu")
    return cpuTest() ? EXIT_SUCCESS : EXIT_FAILURE;
  if (mode == "gpu") {
    test_name = "graphics.descriptor_set_layout";
    return nativeTest();
  }
  return blocked("unknown test mode");
}

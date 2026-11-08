#include <terreate/graphics/compute_pipeline.hpp>

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
using namespace terreate::graphics;

// Same offline-compiled, validated 70-word SPIR-V 1.0 fixture as
// test_graphics_shader_module.cpp. No compiler/reflection at runtime.
// GLSL: #version 450
//       layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
//       void main() {}
constexpr std::array<std::uint32_t, 70> compute_words{
    0x07230203, 0x00010000, 0x0008000b, 0x0000000a, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00040047, 0x00000009, 0x0000000b, 0x00000019, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017,
    0x00000007, 0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x00000008, 0x00000001, 0x0006002c,
    0x00000007, 0x00000009, 0x00000008, 0x00000008, 0x00000008, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000100fd, 0x00010038,
};

static_assert(!std::default_initializable<PipelineLayout>);
static_assert(!std::copy_constructible<PipelineLayout>);
static_assert(!std::is_copy_assignable_v<PipelineLayout>);
static_assert(std::is_nothrow_move_constructible_v<PipelineLayout>);
static_assert(std::is_nothrow_move_assignable_v<PipelineLayout>);
static_assert(std::is_nothrow_destructible_v<PipelineLayout>);
static_assert(!std::default_initializable<ComputePipeline>);
static_assert(!std::copy_constructible<ComputePipeline>);
static_assert(!std::is_copy_assignable_v<ComputePipeline>);
static_assert(std::is_nothrow_move_constructible_v<ComputePipeline>);
static_assert(std::is_nothrow_move_assignable_v<ComputePipeline>);
static_assert(std::is_nothrow_destructible_v<ComputePipeline>);

const char *test_name = "graphics.pipeline.cpu";
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

enum class Injection : std::uint8_t { none, failure, null_success, partial_failure };

template <typename Handle> struct NativeTracker {
  struct Record {
    Handle handle = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
  };
  std::array<Record, 16> live{};
  std::size_t creates = 0;
  std::size_t native_handles = 0;
  std::size_t destroys = 0;
  Handle last_destroyed = VK_NULL_HANDLE;
  Injection injection = Injection::none;
  bool forbid_create = false;

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

  VkDevice remove(Handle handle, VkDevice device) {
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
    check(false, "double destruction or untracked native handle");
    return VK_NULL_HANDLE;
  }

  bool empty() const {
    for (const auto &slot : live)
      if (slot.handle != VK_NULL_HANDLE)
        return false;
    return true;
  }
};

NativeTracker<VkPipelineLayout> layouts;
NativeTracker<VkPipeline> pipelines;

struct LayoutObservation {
  VkDevice device = VK_NULL_HANDLE;
  bool exact_header = false;
  bool zero_descriptors = false;
  bool range_pointer_matches_count = false;
  std::uint32_t range_count = 0;
  std::array<VkPushConstantRange, max_pipeline_layout_push_constant_ranges> ranges{};
} observed_layout;

struct ComputeObservation {
  VkDevice device = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule module = VK_NULL_HANDLE;
  bool exact_header = false;
  bool compute_stage = false;
  bool entry_point_is_main = false;
} observed_compute;

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

// Native C wrappers, not substituted Terreate API symbols. Artificial null
// success never calls the driver, so no genuine native object is lost.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo *,
                                                  const VkAllocationCallbacks *,
                                                  VkPipelineLayout *);
extern "C" void __real_vkDestroyPipelineLayout(VkDevice, VkPipelineLayout,
                                               const VkAllocationCallbacks *);
extern "C" VkResult __real_vkCreateComputePipelines(VkDevice, VkPipelineCache, std::uint32_t,
                                                    const VkComputePipelineCreateInfo *,
                                                    const VkAllocationCallbacks *, VkPipeline *);
extern "C" void __real_vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *);

extern "C" VkResult __wrap_vkCreatePipelineLayout(VkDevice device,
                                                  const VkPipelineLayoutCreateInfo *info,
                                                  const VkAllocationCallbacks *alloc,
                                                  VkPipelineLayout *result) {
  ++layouts.creates;
  if (layouts.forbid_create) {
    check(false, "invalid layout request reached native creation");
    if (result)
      *result = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!check(info && result, "layout native create arguments are present"))
    return VK_ERROR_INITIALIZATION_FAILED;
  observed_layout = {};
  observed_layout.device = device;
  observed_layout.exact_header = info->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO &&
                                 !info->pNext && info->flags == 0 && !alloc;
  observed_layout.zero_descriptors = info->setLayoutCount == 0 && !info->pSetLayouts;
  observed_layout.range_count = info->pushConstantRangeCount;
  observed_layout.range_pointer_matches_count =
      (info->pushConstantRangeCount == 0) == (info->pPushConstantRanges == nullptr);
  if (info->pPushConstantRanges && info->pushConstantRangeCount <= observed_layout.ranges.size()) {
    for (std::size_t index = 0; index < info->pushConstantRangeCount; ++index)
      observed_layout.ranges[index] = info->pPushConstantRanges[index];
  }
  const auto injection = std::exchange(layouts.injection, Injection::none);
  if (injection != Injection::none) {
    *result = VK_NULL_HANDLE;
    return injection == Injection::null_success ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  const auto status = __real_vkCreatePipelineLayout(device, info, alloc, result);
  if (status == VK_SUCCESS && *result != VK_NULL_HANDLE)
    layouts.insert({*result, device, VK_NULL_HANDLE});
  return status;
}

extern "C" void __wrap_vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout layout,
                                               const VkAllocationCallbacks *alloc) {
  if (layout == VK_NULL_HANDLE)
    return;
  for (const auto &record : pipelines.live)
    check(record.handle == VK_NULL_HANDLE || record.layout != layout,
          "test retains layout until its pipelines are destroyed");
  const auto parent = layouts.remove(layout, device);
  // Keep a broken ownership implementation from double-destroying driver state.
  if (parent != VK_NULL_HANDLE)
    __real_vkDestroyPipelineLayout(parent, layout, alloc);
}

extern "C" VkResult __wrap_vkCreateComputePipelines(VkDevice device, VkPipelineCache cache,
                                                    std::uint32_t count,
                                                    const VkComputePipelineCreateInfo *info,
                                                    const VkAllocationCallbacks *alloc,
                                                    VkPipeline *result) {
  ++pipelines.creates;
  if (pipelines.forbid_create) {
    check(false, "invalid compute request reached native creation");
    if (result)
      *result = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!check(info && result && count == 1, "exactly one explicit compute create info required"))
    return VK_ERROR_INITIALIZATION_FAILED;
  observed_compute.device = device;
  observed_compute.layout = info->layout;
  observed_compute.module = info->stage.module;
  observed_compute.exact_header =
      cache == VK_NULL_HANDLE && !alloc &&
      info->sType == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO && !info->pNext &&
      info->flags == 0 && info->basePipelineHandle == VK_NULL_HANDLE &&
      info->basePipelineIndex == -1 &&
      info->stage.sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO &&
      !info->stage.pNext && info->stage.flags == 0 && !info->stage.pSpecializationInfo;
  observed_compute.compute_stage = info->stage.stage == VK_SHADER_STAGE_COMPUTE_BIT;
  observed_compute.entry_point_is_main =
      info->stage.pName && std::strcmp(info->stage.pName, "main") == 0;
  const auto injection = std::exchange(pipelines.injection, Injection::none);
  if (injection == Injection::failure || injection == Injection::null_success) {
    *result = VK_NULL_HANDLE;
    return injection == Injection::null_success ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  if (!check(observed_compute.exact_header && observed_compute.compute_stage &&
                 observed_compute.entry_point_is_main,
             "real compute creation receives explicit compute stage and exact pName main")) {
    *result = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const auto status = __real_vkCreateComputePipelines(device, cache, count, info, alloc, result);
  // Multi-create can return nonnull handles even on failure. Track all of them.
  if (*result != VK_NULL_HANDLE)
    pipelines.insert({*result, device, info->layout});
  if (injection == Injection::partial_failure && status == VK_SUCCESS) {
    check(*result != VK_NULL_HANDLE, "partial-failure injection owns a real native pipeline");
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  return status;
}

extern "C" void __wrap_vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
                                         const VkAllocationCallbacks *alloc) {
  if (pipeline == VK_NULL_HANDLE)
    return;
  const auto parent = pipelines.remove(pipeline, device);
  if (parent != VK_NULL_HANDLE)
    __real_vkDestroyPipeline(parent, pipeline, alloc);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
auto invalidLayouts() {
  constexpr auto compute = vk::ShaderStageFlagBits::eCompute;
  const auto unknown = vk::ShaderStageFlags{static_cast<VkShaderStageFlags>(0x80000000U)};
  return std::array{
      PipelineLayoutDescription{{vk::PushConstantRange{{}, 0, 4}}},
      PipelineLayoutDescription{{vk::PushConstantRange{unknown, 0, 4}}},
      PipelineLayoutDescription{{vk::PushConstantRange{vk::ShaderStageFlagBits::eAll, 0, 4}}},
      PipelineLayoutDescription{{vk::PushConstantRange{compute, 0, 0}}},
      PipelineLayoutDescription{{vk::PushConstantRange{compute, 2, 4}}},
      PipelineLayoutDescription{{vk::PushConstantRange{compute, 0, 6}}},
      PipelineLayoutDescription{
          {vk::PushConstantRange{compute, std::numeric_limits<std::uint32_t>::max() - 3, 8}}},
      PipelineLayoutDescription{
          {vk::PushConstantRange{compute, 0, 16}, vk::PushConstantRange{compute, 8, 16}}},
      PipelineLayoutDescription{
          {vk::PushConstantRange{compute, 0, 16}, vk::PushConstantRange{compute, 16, 16}}},
  };
}

auto invalidEntries() {
  return std::array{
      ComputePipelineDescription{},
      ComputePipelineDescription{std::string{"main\0suffix", 11}},
      ComputePipelineDescription{std::string(1, '\0')},
      ComputePipelineDescription{std::string(max_compute_entry_point_bytes + 1, 'x')},
  };
}

bool cpuTest() {
  bool passed = true;
  passed &= check(validatePipelineLayoutDescription({}, 0).has_value(),
                  "explicit empty layout requires no push constant capability");
  for (const auto &description : invalidLayouts())
    passed &= check(hasError(validatePipelineLayoutDescription(description, 128),
                             PipelineLayoutError::invalid_description),
                    "invalid range stage, alignment, size, overflow or repeated stage rejected");

  PipelineLayoutDescription description;
  description.push_constant_ranges.emplace_back(vk::ShaderStageFlagBits::eCompute, 124, 4);
  passed &= check(validatePipelineLayoutDescription(description, 128).has_value(),
                  "push constant upper endpoint may equal explicit limit");
  passed &= check(hasError(validatePipelineLayoutDescription(description, 127),
                           PipelineLayoutError::unsupported_configuration) &&
                      hasError(validatePipelineLayoutDescription(description, 0),
                               PipelineLayoutError::unsupported_configuration),
                  "supplied limit is enforced without subtraction underflow");
  description.push_constant_ranges = {{vk::ShaderStageFlagBits::eCompute, 0, 128}};
  passed &= check(validatePipelineLayoutDescription(description, 128).has_value(),
                  "a full-limit range is accepted");
  description.push_constant_ranges = {{vk::ShaderStageFlagBits::eCompute, 0, 16},
                                      {vk::ShaderStageFlagBits::eFragment, 8, 16}};
  passed &= check(validatePipelineLayoutDescription(description, 128).has_value(),
                  "overlapping bytes are valid with disjoint shader-stage masks");
  description.push_constant_ranges = {
      {vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eCompute, 0, 16},
      {vk::ShaderStageFlagBits::eCompute, 32, 4}};
  passed &= check(hasError(validatePipelineLayoutDescription(description, 128),
                           PipelineLayoutError::invalid_description),
                  "intersection of multibit stage masks is rejected even without byte overlap");
  description.push_constant_ranges = {{vk::ShaderStageFlagBits::eAllGraphics, 0, 16},
                                      {vk::ShaderStageFlagBits::eCompute, 0, 16}};
  passed &= check(validatePipelineLayoutDescription(description, 128).has_value(),
                  "explicit all-graphics core mask and separate compute mask are supported");
  description.push_constant_ranges = {{vk::ShaderStageFlagBits::eVertex, 0, 4},
                                      {vk::ShaderStageFlagBits::eTessellationControl, 0, 4},
                                      {vk::ShaderStageFlagBits::eTessellationEvaluation, 0, 4},
                                      {vk::ShaderStageFlagBits::eGeometry, 0, 4},
                                      {vk::ShaderStageFlagBits::eFragment, 0, 4},
                                      {vk::ShaderStageFlagBits::eCompute, 0, 4}};
  passed &= check(validatePipelineLayoutDescription(description, 128).has_value(),
                  "all six distinct core stages fit the bounded validation surface");
  description.push_constant_ranges.emplace_back(vk::ShaderStageFlagBits::eCompute, 4, 4);
  passed &= check(hasError(validatePipelineLayoutDescription(description, 128),
                           PipelineLayoutError::invalid_description),
                  "range count is bounded before native count conversion");

  for (const auto &entry : invalidEntries())
    passed &= check(hasError(validateComputePipelineDescription(entry),
                             ComputePipelineError::invalid_entry_point),
                    "empty, embedded-NUL and overlong entry points rejected without Device");
  passed &= check(
      validateComputePipelineDescription({"main"}).has_value() &&
          validateComputePipelineDescription({"caller_chosen_entry"}).has_value() &&
          validateComputePipelineDescription({std::string(max_compute_entry_point_bytes, 'x')})
              .has_value(),
      "entry point is explicit, not inferred or silently restricted to main");
  const std::error_code layout_error = PipelineLayoutError::invalid_description;
  const std::error_code pipeline_error = ComputePipelineError::invalid_entry_point;
  passed &= check(layout_error.value() != 0 && pipeline_error.value() != 0 &&
                      layout_error.category() != pipeline_error.category() &&
                      std::string_view{layout_error.category().name()} ==
                          "terreate.graphics.pipeline_layout" &&
                      std::string_view{pipeline_error.category().name()} ==
                          "terreate.graphics.compute_pipeline",
                  "public error categories support std::error_code conversion");
  passed &= check(layouts.creates == 0 && pipelines.creates == 0 && layouts.destroys == 0 &&
                      pipelines.destroys == 0,
                  "CPU validation never creates or destroys native Vulkan objects");
  if (passed && failures == 0)
    std::puts("graphics.pipeline.cpu: cpu=PASS");
  return passed && failures == 0;
}

bool layoutTest(const Device &device, const Device &moved_from, const Device &other) {
  bool passed = true;
  const auto creates_before = layouts.creates;
  const auto destroys_before = layouts.destroys;
  layouts.forbid_create = true;
  passed &=
      check(hasError(createPipelineLayout(moved_from, {}), PipelineLayoutError::invalid_device),
            "moved-from Device rejected before native layout creation");
  for (const auto &description : invalidLayouts())
    passed &= check(hasError(createPipelineLayout(device, description),
                             PipelineLayoutError::invalid_description),
                    "factory repeats CPU description rejection before vkCreatePipelineLayout");
  const auto limit = device.plan()->capabilities().properties.limits.maxPushConstantsSize;
  PipelineLayoutDescription too_large{
      {vk::PushConstantRange{vk::ShaderStageFlagBits::eCompute, limit & ~std::uint32_t{3}, 4}}};
  passed &= check(!createPipelineLayout(device, too_large) && layouts.creates == creates_before,
                  "actual DevicePlan push constant limit is enforced before native creation");
  layouts.forbid_create = false;

  layouts.injection = Injection::failure;
  const auto failed = createPipelineLayout(device, {});
  passed &= check(hasError(failed, vk::Result::eErrorOutOfHostMemory) &&
                      failed.error().context() == "vkCreatePipelineLayout" &&
                      layouts.creates == creates_before + 1 && layouts.destroys == destroys_before,
                  "layout native VkResult and operation propagate without an owner");
  layouts.injection = Injection::null_success;
  passed &=
      check(hasError(createPipelineLayout(device, {}), PipelineLayoutError::native_null_handle) &&
                layouts.creates == creates_before + 2 && layouts.destroys == destroys_before,
            "null layout on success is rejected without leaking a native handle");

  {
    PipelineLayoutDescription description;
    description.push_constant_ranges = {{vk::ShaderStageFlagBits::eCompute, 0, 16},
                                        {vk::ShaderStageFlagBits::eFragment, 8, 16}};
    const auto requested = description;
    auto first = createPipelineLayout(device, description);
    if (!check(first.has_value(), "real Vulkan push-constant PipelineLayout creation"))
      return false;
    const auto first_native = first->nativePipelineLayout();
    passed &= check(first->valid() && static_cast<bool>(*first) &&
                        first->nativeHandle() == vk::PipelineLayout{first_native} &&
                        first->nativeDevice() == device.nativeHandle() &&
                        first->nativeVkDevice() == static_cast<VkDevice>(device.nativeHandle()) &&
                        first->correlatedWith(device) && !first->correlatedWith(other) &&
                        !first->correlatedWith(moved_from),
                    "layout native access and same-Device correlation survive Device move");
    passed &= check(
        observed_layout.exact_header && observed_layout.zero_descriptors &&
            observed_layout.range_pointer_matches_count &&
            observed_layout.device == first->nativeVkDevice() && observed_layout.range_count == 2 &&
            observed_layout.ranges[0].stageFlags == VK_SHADER_STAGE_COMPUTE_BIT &&
            observed_layout.ranges[0].offset == 0 && observed_layout.ranges[0].size == 16 &&
            observed_layout.ranges[1].stageFlags == VK_SHADER_STAGE_FRAGMENT_BIT &&
            observed_layout.ranges[1].offset == 8 && observed_layout.ranges[1].size == 16,
        "native layout receives exact ordered ranges and zero descriptor layouts");
    description.push_constant_ranges.clear();
    passed &= check(first->configuration() &&
                        first->configuration()->requested.push_constant_ranges ==
                            requested.push_constant_ranges &&
                        first->configuration()->effective.push_constant_ranges ==
                            requested.push_constant_ranges &&
                        first->configuration()->max_push_constants_size == limit,
                    "layout owns requested/effective snapshots and real capability evidence");

    auto second = createPipelineLayout(device, {});
    if (!check(second.has_value(), "real empty PipelineLayout creation"))
      return false;
    const auto second_native = second->nativePipelineLayout();
    passed &= check(observed_layout.zero_descriptors && observed_layout.range_count == 0 &&
                        observed_layout.range_pointer_matches_count && second->configuration() &&
                        second->configuration()->effective.push_constant_ranges.empty(),
                    "empty descriptor-free layout uses null arrays and zero counts");
    // Intentionally inspect moved-from public state.
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    PipelineLayout moved = std::move(*first);
    passed &=
        check(!first->valid() && first->nativePipelineLayout() == VK_NULL_HANDLE &&
                  first->nativeHandle() == vk::PipelineLayout{} &&
                  first->nativeDevice() == vk::Device{} &&
                  first->nativeVkDevice() == VK_NULL_HANDLE && !first->configuration() &&
                  !first->correlatedWith(device) && moved.nativePipelineLayout() == first_native,
              "layout move construction nulls every source observer");
    *second = std::move(moved);
    passed &= check(!moved.valid() && moved.nativePipelineLayout() == VK_NULL_HANDLE &&
                        !moved.configuration() && second->nativePipelineLayout() == first_native &&
                        first_native != second_native && layouts.destroys == destroys_before + 1 &&
                        layouts.last_destroyed == second_native,
                    "layout move assignment destroys only the displaced handle exactly once");
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  }
  passed &= check(layouts.creates == creates_before + 4 &&
                      layouts.destroys == destroys_before + 2 && layouts.empty(),
                  "all real layouts destroyed exactly once without submission or waits");
  return passed && failures == 0;
}

bool computeTest(const Device &device, const Device &moved_from, const Device &other) {
  const auto binary = SpirvBinary::fromWords(compute_words);
  if (!check(binary.has_value(), "known-good precompiled compute fixture parses"))
    return false;
  auto shader_result = createShaderModule(device, *binary);
  auto layout_result = createPipelineLayout(
      device, {{vk::PushConstantRange{vk::ShaderStageFlagBits::eCompute, 0, 16}}});
  auto foreign_shader = createShaderModule(other, *binary);
  auto foreign_layout = createPipelineLayout(other, {});
  auto different_layout = createPipelineLayout(device, {});
  if (!check(shader_result && layout_result && foreign_shader && foreign_layout && different_layout,
             "real ShaderModules and PipelineLayouts created on two distinct Devices"))
    return false;
  // The result objects provide moved-from parents without manufacturing handles.
  std::optional<ShaderModule> shader{std::move(*shader_result)};
  PipelineLayout layout = std::move(*layout_result);
  ComputePipelineDescription description{"main"};
  bool passed = true;
  const auto creates_before = pipelines.creates;
  const auto destroys_before = pipelines.destroys;
  pipelines.forbid_create = true;
  // Intentionally pass the moved-from parent wrappers to rejection paths.
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  passed &= check(hasError(createComputePipeline(moved_from, *shader, layout, description),
                           ComputePipelineError::invalid_device) &&
                      hasError(createComputePipeline(device, *shader_result, layout, description),
                               ComputePipelineError::invalid_shader_module) &&
                      hasError(createComputePipeline(device, *shader, *layout_result, description),
                               ComputePipelineError::invalid_layout),
                  "moved-from Device, ShaderModule and layout rejected before native creation");
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  passed &= check(hasError(createComputePipeline(device, *foreign_shader, layout, description),
                           ComputePipelineError::parent_mismatch) &&
                      hasError(createComputePipeline(device, *shader, *foreign_layout, description),
                               ComputePipelineError::parent_mismatch) &&
                      hasError(createComputePipeline(other, *shader, layout, description),
                               ComputePipelineError::parent_mismatch),
                  "every parent must match the caller's explicit Device");
  for (const auto &entry : invalidEntries())
    passed &= check(hasError(createComputePipeline(device, *shader, layout, entry),
                             ComputePipelineError::invalid_entry_point),
                    "invalid entry point rejected before vkCreateComputePipelines");
  passed &= check(pipelines.creates == creates_before,
                  "all semantic rejection paths leave the native create counter unchanged");
  pipelines.forbid_create = false;

  pipelines.injection = Injection::failure;
  const auto failed = createComputePipeline(device, *shader, layout, description);
  passed &=
      check(hasError(failed, vk::Result::eErrorOutOfHostMemory) &&
                failed.error().context() == "vkCreateComputePipelines" &&
                pipelines.creates == creates_before + 1 && pipelines.destroys == destroys_before,
            "native compute failure preserves VkResult without a fake pipeline owner");
  pipelines.injection = Injection::null_success;
  passed &=
      check(hasError(createComputePipeline(device, *shader, layout, description),
                     ComputePipelineError::native_null_handle) &&
                pipelines.creates == creates_before + 2 && pipelines.destroys == destroys_before,
            "null pipeline on success is rejected without a real native allocation");
  pipelines.injection = Injection::partial_failure;
  passed &= check(hasError(createComputePipeline(device, *shader, layout, description),
                           vk::Result::eErrorOutOfHostMemory) &&
                      pipelines.creates == creates_before + 3 &&
                      pipelines.destroys == destroys_before + 1 && pipelines.empty(),
                  "nonnull pipeline output on failure is destroyed exactly once");

  {
    auto first = createComputePipeline(device, *shader, layout, description);
    const auto first_observation = observed_compute;
    auto second = createComputePipeline(device, *shader, *different_layout, description);
    if (!check(first && second, "real headless Vulkan compute pipeline creation"))
      return false;
    const auto first_native = first->nativePipeline();
    const auto second_native = second->nativePipeline();
    passed &= check(observed_compute.exact_header && observed_compute.compute_stage &&
                        observed_compute.entry_point_is_main &&
                        observed_compute.device == static_cast<VkDevice>(device.nativeHandle()) &&
                        observed_compute.module == shader->nativeShaderModule() &&
                        observed_compute.layout == different_layout->nativePipelineLayout() &&
                        first_observation.device == observed_compute.device &&
                        first_observation.module == observed_compute.module &&
                        first_observation.layout == layout.nativePipelineLayout(),
                    "native compute stage, main, module, layout and Device correlate exactly");
    passed &= check(second->correlatedWith(*different_layout) && !second->correlatedWith(layout),
                    "real compute creation also supports an entirely empty layout");
    passed &=
        check(first->valid() && static_cast<bool>(*first) &&
                  first->nativeHandle() == vk::Pipeline{first_native} &&
                  first->nativeDevice() == device.nativeHandle() &&
                  first->nativeVkDevice() == static_cast<VkDevice>(device.nativeHandle()) &&
                  first->nativeLayout() == layout.nativeHandle() && first->correlatedWith(device) &&
                  first->correlatedWith(layout) && !first->correlatedWith(other) &&
                  !first->correlatedWith(moved_from) && !first->correlatedWith(*foreign_layout) &&
                  !first->correlatedWith(*different_layout),
              "pipeline borrowed native handles and parent correlation are inspectable");
    description.entry_point = "caller changed its description";
    passed &=
        check(first->configuration() && first->configuration()->requested.entry_point == "main" &&
                  first->configuration()->effective.entry_point == "main" &&
                  first->configuration()->stage == vk::ShaderStageFlagBits::eCompute,
              "pipeline owns exact entry-point snapshots and explicit effective stage");
    shader.reset();
    passed &= check(first->valid() && second->valid(),
                    "ShaderModule may be destroyed after synchronous pipeline creation");
    // Intentionally inspect moved-from public state.
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    ComputePipeline moved = std::move(*first);
    passed &= check(!first->valid() && first->nativePipeline() == VK_NULL_HANDLE &&
                        first->nativeHandle() == vk::Pipeline{} &&
                        first->nativeVkDevice() == VK_NULL_HANDLE &&
                        first->nativeDevice() == vk::Device{} &&
                        first->nativeLayout() == vk::PipelineLayout{} && !first->configuration() &&
                        !first->correlatedWith(device) && !first->correlatedWith(layout) &&
                        moved.nativePipeline() == first_native,
                    "pipeline move construction nulls all source observers");
    *second = std::move(moved);
    passed &=
        check(!moved.valid() && moved.nativePipeline() == VK_NULL_HANDLE &&
                  !moved.configuration() && second->nativePipeline() == first_native &&
                  first_native != second_native && pipelines.destroys == destroys_before + 2 &&
                  pipelines.last_destroyed == second_native && second->correlatedWith(layout),
              "pipeline move assignment destroys only its displaced handle exactly once");
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  }
  passed &= check(pipelines.creates == creates_before + 5 &&
                      pipelines.destroys == destroys_before + 3 && pipelines.empty(),
                  "partial output and both successful pipelines cleaned up exactly once");
  return passed && failures == 0;
}

// All wrappers/RAII owners from the test have already left scope. If an assertion
// exposes a leak, record it and clean up before either Device dies. No GPU work
// has been submitted, so even the failure path has no in-flight teardown.
void verifyAndCleanNativeObjects() {
  check(pipelines.empty() && layouts.empty(), "no live native objects after owner destruction");
  check(pipelines.destroys == pipelines.native_handles &&
            layouts.destroys == layouts.native_handles,
        "native handle creation and destruction counts balance exactly");
  for (const auto &record : pipelines.live)
    if (record.handle != VK_NULL_HANDLE)
      vkDestroyPipeline(record.device, record.handle, nullptr);
  for (const auto &record : layouts.live)
    if (record.handle != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(record.device, record.handle, nullptr);
}

int exerciseDevices(Instance &instance, bool compute) {
  const auto devices = queryPhysicalDevices(instance);
  if (!devices || devices->empty())
    return blocked("no Vulkan PhysicalDevice");
  for (const auto &candidate : *devices) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t index = 0; index < families.size(); ++index) {
      if (!families[index].queueCount ||
          !(families[index].queueFlags & vk::QueueFlagBits::eCompute))
        continue;
      DeviceDescription request;
      DeviceQueueRequest queue_request;
      queue_request.caller_id = "pipeline-test";
      queue_request.required_flags = vk::QueueFlagBits::eCompute;
      queue_request.allowed_family_indices = {index};
      request.queue_requests.push_back(queue_request);
      const auto plan = resolveDevice(candidate, request);
      if (!plan)
        continue;
      auto device = createDevice(instance, *plan);
      auto other = createDevice(instance, *plan);
      if (!device || !other)
        return blocked("two native logical Devices required for parent-correlation tests");
      auto before_move = createPipelineLayout(*device, {});
      if (!check(before_move.has_value(), "layout created before Device move")) {
        verifyAndCleanNativeObjects();
        return EXIT_FAILURE;
      }
      // The moved-from Device remains an intentional invalid-parent test input.
      // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
      Device moved_device = std::move(*device);
      {
        // Transfer this child into an inner scope so it dies before moved_device.
        PipelineLayout retained = std::move(*before_move);
        check(retained.correlatedWith(moved_device) && !retained.correlatedWith(*device),
              "existing child follows native Device ownership through a wrapper move");
      }
      const bool passed = compute ? computeTest(moved_device, *device, *other)
                                  : layoutTest(moved_device, *device, *other);
      // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
      verifyAndCleanNativeObjects();
      return passed && failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  }
  return blocked("no compute-capable queue with a usable native Device");
}

bool validationProbe(Instance &instance, ValidationRecorder &recorder) {
  const auto raw = static_cast<VkInstance>(instance.nativeHandle());
  const auto send = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
      vkGetInstanceProcAddr(raw, "vkSubmitDebugUtilsMessageEXT"));
  if (!check(send != nullptr, "native validation callback entry point available"))
    return false;
  VkDebugUtilsMessengerCallbackDataEXT data{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
  data.pMessageIdName = "terreate-pipeline-validation-probe";
  data.messageIdNumber = 267;
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

int nativeTest(bool compute) {
  const auto caps = queryInstanceCapabilities();
  if (!caps)
    return blocked("Vulkan instance capabilities unavailable");
  bool has_validation = false;
  bool has_debug = false;
  for (const auto &layer : caps->available_layers)
    has_validation |= layer == "VK_LAYER_KHRONOS_validation";
  for (const auto &extension : caps->available_extensions)
    has_debug |= extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (has_validation && !has_debug)
    return blocked("Validation Layer available but Debug Utils unavailable");
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  if (has_debug)
    description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *caps);
  if (!plan)
    return blocked("Vulkan 1.3 Instance resolution");
  if (has_validation) {
    bool layer_enabled = false;
    for (const auto &layer : plan->enabledLayers())
      layer_enabled |= layer == "VK_LAYER_KHRONOS_validation";
    if (!layer_enabled || !plan->debugUtilsEnabled())
      return blocked("Validation Layer / Debug Utils not enabled in effective configuration");
  }
  ValidationRecorder recorder;
  {
    auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
    if (!instance)
      return blocked("headless Vulkan Instance creation");
    if (has_validation && !validationProbe(*instance, recorder))
      return EXIT_FAILURE;
    const auto status = exerciseDevices(*instance, compute);
    if (status != EXIT_SUCCESS)
      return status;
  }
  // Include Device and Instance teardown in the validation-cleanliness check.
  if (!check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == 0,
             "no real Vulkan Validation Layer warnings/errors, including teardown"))
    return EXIT_FAILURE;
  if (has_validation)
    std::printf("%s: validation-layer=PASS\n", test_name);
  else
    std::printf("%s: validation-layer=SKIP reason=layer-unavailable\n", test_name);
  if (failures != 0)
    return EXIT_FAILURE;
  std::printf("%s: headless-native=PASS\n", test_name);
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify cpu, layout or compute mode");
  const std::string_view mode{argv[1]};
  if (mode == "cpu")
    return cpuTest() ? EXIT_SUCCESS : EXIT_FAILURE;
  if (mode == "layout") {
    test_name = "graphics.pipeline_layout";
    return nativeTest(false);
  }
  if (mode == "compute") {
    test_name = "graphics.compute_pipeline";
    return nativeTest(true);
  }
  return blocked("unknown test mode");
}

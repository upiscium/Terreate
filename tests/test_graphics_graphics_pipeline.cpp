#include <terreate/graphics/compute_pipeline.hpp>
#include <terreate/graphics/graphics_pipeline.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
using namespace terreate::graphics;
using Error = GraphicsPipelineError;

static_assert(!std::is_default_constructible_v<GraphicsPipeline>);
static_assert(!std::is_copy_constructible_v<GraphicsPipeline>);
static_assert(!std::is_copy_assignable_v<GraphicsPipeline>);
static_assert(std::is_nothrow_move_constructible_v<GraphicsPipeline>);
static_assert(std::is_nothrow_move_assignable_v<GraphicsPipeline>);
static_assert(std::is_nothrow_destructible_v<GraphicsPipeline>);

// Offline SPIR-V 1.0 fixtures, with debug names omitted and IDs compacted.
// Equivalent GLSL sources (no descriptors, no inter-stage user interface):
// vertex.vert: #version 450
// layout(location=0) in vec4 position;
// void main() { gl_Position = position; }
// fragment.frag: #version 450
// layout(location=0) out vec4 color;
// void main() { color = vec4(1.0); }
// Compiler-command provenance/reproduction recipe:
// glslangValidator -V --target-env vulkan1.0 -S vert vertex.vert -o vertex.spv
// glslangValidator -V --target-env vulkan1.0 -S frag fragment.frag -o fragment.spv
// The compact words below encode those operations directly; no compiler runs in this test.
constexpr std::uint32_t vertex_words[]{
    0x07230203, 0x00010000, 0,          12,         0,          0x00020011, 1,          0x0003000e,
    0,          1,          0x0007000f, 0,          10,         0x6e69616d, 0,          7,
    8,          0x00040047, 7,          30,         0,          0x00040047, 8,          11,
    0,          0x00020013, 1,          0x00030021, 2,          1,          0x00030016, 3,
    32,         0x00040017, 4,          3,          4,          0x00040020, 5,          1,
    4,          0x00040020, 6,          3,          4,          0x0004003b, 5,          7,
    1,          0x0004003b, 6,          8,          3,          0x00050036, 1,          10,
    0,          2,          0x000200f8, 11,         0x0004003d, 4,          9,          7,
    0x0003003e, 8,          9,          0x000100fd, 0x00010038};
constexpr std::uint32_t fragment_words[]{
    0x07230203, 0x00010000, 0,          11,         0,          0x00020011, 1,          0x0003000e,
    0,          1,          0x0006000f, 4,          9,          0x6e69616d, 0,          6,
    0x00030010, 9,          7,          0x00040047, 6,          30,         0,          0x00020013,
    1,          0x00030021, 2,          1,          0x00030016, 3,          32,         0x00040017,
    4,          3,          4,          0x00040020, 5,          3,          4,          0x0004003b,
    5,          6,          3,          0x0004002b, 3,          7,          0x3f800000, 0x0007002c,
    4,          8,          7,          7,          7,          7,          0x00050036, 1,
    9,          0,          2,          0x000200f8, 10,         0x0003003e, 6,          8,
    0x000100fd, 0x00010038};

const char *test_name = "graphics.graphics_pipeline.cpu";
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
GraphicsPipelineDescription description() {
  GraphicsPipelineDescription value;
  value.vertex_entry_point = "main";
  value.fragment_entry_point = "main";
  value.vertex_bindings = {{0, 16, vk::VertexInputRate::eVertex}};
  value.vertex_attributes = {{0, 0, vk::Format::eR32G32B32A32Sfloat, 0}};
  value.color_attachment_formats = {vk::Format::eR8G8B8A8Unorm};
  value.color_blend_attachments.resize(1);
  value.color_blend_attachments[0].colorWriteMask =
      vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  return value;
}
auto invalidEntries() {
  return std::array{std::string{}, std::string{"main\0suffix", 11}, std::string(1, '\0'),
                    std::string(max_graphics_entry_point_bytes + 1, 'x')};
}
std::size_t creates = 0;
std::size_t destroys = 0;
std::size_t handles = 0;
struct Record {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
};
std::array<Record, 8> live{};
enum class Injection : std::uint8_t { none, failure, partial_failure, null_success };
Injection injection = Injection::none;
GraphicsPipelineDescription expected;
VkShaderModule expected_vertex = VK_NULL_HANDLE;
VkShaderModule expected_fragment = VK_NULL_HANDLE;
VkPipelineLayout expected_layout = VK_NULL_HANDLE;

int cpuTest() {
  const std::array codes{Error::invalid_device,
                         Error::invalid_shader_module,
                         Error::invalid_layout,
                         Error::parent_mismatch,
                         Error::invalid_entry_point,
                         Error::invalid_description,
                         Error::unsupported_configuration,
                         Error::feature_not_enabled,
                         Error::native_null_handle,
                         Error::allocation_failed};
  for (std::size_t i = 0; i < codes.size(); ++i) {
    const std::error_code code = codes[i];
    check(code.value() != 0 && code.category() == graphics_pipeline_error_category() &&
              code.category() != pipeline_layout_error_category() &&
              code.category() != compute_pipeline_error_category() &&
              std::string_view{code.category().name()} == "terreate.graphics.graphics_pipeline",
          "distinct graphics error category");
    for (std::size_t j = 0; j < i; ++j)
      check(code != make_error_code(codes[j]), "distinct error codes");
  }
  vk::PhysicalDeviceLimits limits;
  limits.maxVertexInputBindings = 2;
  limits.maxVertexInputAttributes = 2;
  limits.maxVertexInputBindingStride = 16;
  limits.maxVertexInputAttributeOffset = 16;
  limits.maxColorAttachments = 2;
  auto base = description();
  base.vertex_bindings = {{0, 16, vk::VertexInputRate::eVertex},
                          {1, 0, vk::VertexInputRate::eInstance}};
  base.vertex_attributes.push_back({1, 1, vk::Format::eR32Sfloat, 16});
  base.color_attachment_formats.push_back(vk::Format::eR8G8B8A8Unorm);
  base.color_blend_attachments.push_back(base.color_blend_attachments.front());
  check(validateGraphicsPipelineDescription(base, limits).has_value(), "exact synthetic endpoints");
  const auto reject = [&](auto mutate, Error error, const char *message) {
    auto value = base;
    mutate(value);
    check(hasError(validateGraphicsPipelineDescription(value, limits), error), message);
  };
  for (const auto &entry : invalidEntries()) {
    reject([&](auto &v) { v.vertex_entry_point = entry; }, Error::invalid_entry_point,
           "invalid vertex entry");
    reject([&](auto &v) { v.fragment_entry_point = entry; }, Error::invalid_entry_point,
           "invalid fragment entry");
  }
  for (const auto size : {std::size_t{1}, max_graphics_entry_point_bytes}) {
    auto value = base;
    value.vertex_entry_point = value.fragment_entry_point = std::string(size, 'x');
    check(validateGraphicsPipelineDescription(value, limits).has_value(), "entry length endpoints");
  }
  reject([](auto &v) { v.vertex_bindings[1].binding = 0; }, Error::invalid_description,
         "duplicate binding");
  reject([](auto &v) { v.vertex_bindings[1].binding = 2; }, Error::unsupported_configuration,
         "binding number limit");
  reject([](auto &v) { v.vertex_bindings[0].stride = 17; }, Error::unsupported_configuration,
         "stride limit");
  reject([](auto &v) { v.vertex_bindings.push_back(v.vertex_bindings[0]); },
         Error::unsupported_configuration, "binding count limit");
  reject([](auto &v) { v.vertex_attributes[1].location = 0; }, Error::invalid_description,
         "duplicate location");
  reject([](auto &v) { v.vertex_attributes[1].location = 2; }, Error::unsupported_configuration,
         "location limit");
  reject([](auto &v) { v.vertex_attributes[1].offset = 17; }, Error::unsupported_configuration,
         "offset limit");
  reject([](auto &v) { v.vertex_attributes.push_back(v.vertex_attributes[0]); },
         Error::unsupported_configuration, "attribute count limit");
  reject([](auto &v) { v.vertex_attributes[1].binding = 2; }, Error::invalid_description,
         "missing attribute binding");
  reject([](auto &v) { v.color_attachment_formats.push_back(vk::Format::eR8G8B8A8Unorm); },
         Error::unsupported_configuration, "color count limit");
  reject([](auto &v) { v.color_attachment_formats[0] = vk::Format::eUndefined; },
         Error::invalid_description, "undefined color");
  reject([](auto &v) { v.color_blend_attachments.pop_back(); }, Error::invalid_description,
         "blend count mismatch");
  reject([](auto &v) { v.depth_attachment_format = vk::Format::eS8Uint; },
         Error::invalid_description, "depth aspect required");
  reject([](auto &v) { v.stencil_attachment_format = vk::Format::eD32Sfloat; },
         Error::invalid_description, "stencil aspect required");
  reject([](auto &v) { v.depth_test_enable = true; }, Error::invalid_description,
         "depth test requires depth format");
  reject([](auto &v) { v.depth_write_enable = true; }, Error::invalid_description,
         "depth write requires depth format");
  reject(
      [](auto &v) {
        v.depth_attachment_format = vk::Format::eD32Sfloat;
        v.stencil_attachment_format = vk::Format::eS8Uint;
      },
      Error::invalid_description, "combined aspects must use the same format");
  for (const auto format : {vk::Format::eD24UnormS8Uint, vk::Format::eD32SfloatS8Uint}) {
    auto value = base;
    value.depth_attachment_format = value.stencil_attachment_format = format;
    value.depth_test_enable = value.depth_write_enable = true;
    check(validateGraphicsPipelineDescription(value, limits).has_value(), "combined depth/stencil");
  }
  auto empty = GraphicsPipelineDescription{"main", "main"};
  check(validateGraphicsPipelineDescription(empty, {}).has_value(), "zero limits and no inputs");
  check(hasError(validateGraphicsPipelineDescription(description(), {}),
                 Error::unsupported_configuration),
        "zero limits reject nonempty inputs");
  check(creates == 0 && destroys == 0, "CPU path makes no native calls");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.graphics_pipeline.cpu: cpu=PASS");
  return EXIT_SUCCESS;
}
} // namespace

// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, std::uint32_t,
                                                     const VkGraphicsPipelineCreateInfo *,
                                                     const VkAllocationCallbacks *, VkPipeline *);
extern "C" void __real_vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *);
extern "C" VkResult __wrap_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
                                                     std::uint32_t count,
                                                     const VkGraphicsPipelineCreateInfo *info,
                                                     const VkAllocationCallbacks *alloc,
                                                     VkPipeline *out) {
  ++creates;
  if (!check(info && out && count == 1, "one graphics pipeline per call"))
    return VK_ERROR_INITIALIZATION_FAILED;
  *out = VK_NULL_HANDLE;
  check(!cache && !alloc && !info->renderPass && info->subpass == 0 && info->flags == 0 &&
            !info->basePipelineHandle && info->basePipelineIndex == -1 &&
            info->layout == expected_layout,
        "explicit layout, null cache/renderPass, subpass zero and no derivatives");
  const auto *rendering = static_cast<const VkPipelineRenderingCreateInfo *>(info->pNext);
  if (check(rendering != nullptr, "dynamic rendering pNext")) {
    check(rendering->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO &&
              !rendering->pNext && rendering->viewMask == 0 &&
              rendering->colorAttachmentCount == expected.color_attachment_formats.size() &&
              rendering->depthAttachmentFormat ==
                  static_cast<VkFormat>(expected.depth_attachment_format) &&
              rendering->stencilAttachmentFormat ==
                  static_cast<VkFormat>(expected.stencil_attachment_format),
          "exact rendering formats and viewMask");
    if (check(rendering->pColorAttachmentFormats != nullptr, "color format array"))
      for (std::size_t i = 0;
           i < expected.color_attachment_formats.size() && i < rendering->colorAttachmentCount; ++i)
        check(rendering->pColorAttachmentFormats[i] ==
                  static_cast<VkFormat>(expected.color_attachment_formats[i]),
              "color format");
  }
  if (check(info->stageCount == 2 && info->pStages, "exactly vertex and fragment")) {
    for (std::size_t i = 0; i < 2; ++i) {
      const auto &stage = info->pStages[i];
      check(stage.stage == (i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT) &&
                stage.module == (i == 0 ? expected_vertex : expected_fragment) && stage.pName &&
                std::strcmp(stage.pName, i == 0 ? expected.vertex_entry_point.c_str()
                                                : expected.fragment_entry_point.c_str()) == 0 &&
                !stage.pSpecializationInfo && !stage.pNext,
            "stage, module and entry name");
    }
  }
  const auto *dynamic = info->pDynamicState;
  if (check(dynamic && dynamic->dynamicStateCount == 2 && dynamic->pDynamicStates,
            "exactly two dynamic states"))
    check(dynamic->pDynamicStates[0] == VK_DYNAMIC_STATE_VIEWPORT &&
              dynamic->pDynamicStates[1] == VK_DYNAMIC_STATE_SCISSOR,
          "viewport and scissor only");
  const auto *vertex = info->pVertexInputState;
  if (check(vertex && vertex->vertexBindingDescriptionCount == 1 &&
                vertex->vertexAttributeDescriptionCount == 1 &&
                vertex->pVertexBindingDescriptions && vertex->pVertexAttributeDescriptions,
            "vertex input counts")) {
    check(vk::VertexInputBindingDescription{vertex->pVertexBindingDescriptions[0]} ==
                  expected.vertex_bindings[0] &&
              vk::VertexInputAttributeDescription{vertex->pVertexAttributeDescriptions[0]} ==
                  expected.vertex_attributes[0],
          "exact vertex input");
  }
  const auto mode = std::exchange(injection, Injection::none);
  if (mode == Injection::failure || mode == Injection::null_success)
    return mode == Injection::failure ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
  const auto status = __real_vkCreateGraphicsPipelines(device, cache, count, info, alloc, out);
  if (*out) {
    ++handles;
    bool inserted = false;
    for (auto &record : live) {
      if (!record.pipeline) {
        record = {*out, device};
        inserted = true;
        break;
      }
    }
    check(inserted, "tracker capacity");
  }
  if (mode == Injection::partial_failure && status == VK_SUCCESS) {
    check(*out != VK_NULL_HANDLE, "partial failure uses real native handle");
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  return status;
}
extern "C" void __wrap_vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
                                         const VkAllocationCallbacks *alloc) {
  if (!pipeline)
    return;
  ++destroys;
  for (auto &record : live) {
    if (record.pipeline == pipeline) {
      check(record.device == device, "destruction uses creating Device");
      __real_vkDestroyPipeline(record.device, pipeline, alloc);
      record = {};
      return;
    }
  }
  check(false, "double destruction or untracked pipeline");
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
bool sameDescription(const GraphicsPipelineDescription &a, const GraphicsPipelineDescription &b) {
  return a.vertex_entry_point == b.vertex_entry_point &&
         a.fragment_entry_point == b.fragment_entry_point &&
         a.vertex_bindings == b.vertex_bindings && a.vertex_attributes == b.vertex_attributes &&
         a.topology == b.topology && a.primitive_restart == b.primitive_restart &&
         a.cull_mode == b.cull_mode && a.front_face == b.front_face &&
         a.depth_test_enable == b.depth_test_enable &&
         a.depth_write_enable == b.depth_write_enable && a.depth_compare_op == b.depth_compare_op &&
         a.color_attachment_formats == b.color_attachment_formats &&
         a.color_blend_attachments == b.color_blend_attachments &&
         a.depth_attachment_format == b.depth_attachment_format &&
         a.stencil_attachment_format == b.stencil_attachment_format;
}
// Inspects intentionally moved-from wrappers.
// NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
bool emptyPipeline(const GraphicsPipeline &value, const Device &device,
                   const PipelineLayout &layout) {
  return !value.valid() && !static_cast<bool>(value) && !value.nativePipeline() &&
         value.nativeHandle() == vk::Pipeline{} && !value.nativeVkDevice() &&
         value.nativeDevice() == vk::Device{} && value.nativeLayout() == vk::PipelineLayout{} &&
         !value.configuration() && !value.correlatedWith(device) && !value.correlatedWith(layout);
}
// NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
int pipelineTest(const Device &device, const Device &moved_from, const Device &other,
                 const Device &disabled) {
  const auto vertex_binary = SpirvBinary::fromWords(vertex_words);
  const auto fragment_binary = SpirvBinary::fromWords(fragment_words);
  if (!check(vertex_binary && fragment_binary, "embedded SPIR-V parses"))
    return EXIT_FAILURE;
  auto vertex_result = createShaderModule(device, *vertex_binary);
  auto fragment_result = createShaderModule(device, *fragment_binary);
  auto layout_result = createPipelineLayout(device, {});
  auto foreign_vertex = createShaderModule(other, *vertex_binary);
  auto foreign_fragment = createShaderModule(other, *fragment_binary);
  auto foreign_layout = createPipelineLayout(other, {});
  auto disabled_vertex = createShaderModule(disabled, *vertex_binary);
  auto disabled_fragment = createShaderModule(disabled, *fragment_binary);
  auto disabled_layout = createPipelineLayout(disabled, {});
  if (!vertex_result || !fragment_result || !layout_result || !foreign_vertex ||
      !foreign_fragment || !foreign_layout || !disabled_vertex || !disabled_fragment ||
      !disabled_layout)
    return blocked("native shader modules and descriptor-free layouts required");
  ShaderModule vertex = std::move(*vertex_result);
  ShaderModule fragment = std::move(*fragment_result);
  PipelineLayout layout = std::move(*layout_result);
  check(layout.configuration() && layout.configuration()->set_layout_handles.empty(),
        "descriptor-free layout");
  expected = description();
  expected_vertex = vertex.nativeShaderModule();
  expected_fragment = fragment.nativeShaderModule();
  expected_layout = layout.nativePipelineLayout();
  const auto before = creates;
  const auto reject = [&](const Device &d, const PipelineLayout &l, const ShaderModule &v,
                          const ShaderModule &f, Error error) {
    check(hasError(createGraphicsPipeline(d, l, v, f, expected), error), "invalid parent rejected");
  };
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  reject(moved_from, layout, vertex, fragment, Error::invalid_device);
  reject(device, *layout_result, vertex, fragment, Error::invalid_layout);
  reject(device, layout, *vertex_result, fragment, Error::invalid_shader_module);
  reject(device, layout, vertex, *fragment_result, Error::invalid_shader_module);
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  reject(device, *foreign_layout, vertex, fragment, Error::parent_mismatch);
  reject(device, layout, *foreign_vertex, fragment, Error::parent_mismatch);
  reject(device, layout, vertex, *foreign_fragment, Error::parent_mismatch);
  check(!disabled.plan()->enabledFeatures13().dynamicRendering, "feature explicitly disabled");
  reject(disabled, *disabled_layout, *disabled_vertex, *disabled_fragment,
         Error::feature_not_enabled);
  for (const auto &entry : invalidEntries()) {
    auto invalid = expected;
    invalid.vertex_entry_point = entry;
    check(hasError(createGraphicsPipeline(device, layout, vertex, fragment, invalid),
                   Error::invalid_entry_point),
          "factory rejects invalid vertex entry");
    invalid = expected;
    invalid.fragment_entry_point = entry;
    check(hasError(createGraphicsPipeline(device, layout, vertex, fragment, invalid),
                   Error::invalid_entry_point),
          "factory rejects invalid fragment entry");
  }
  check(creates == before, "rejections never reach Vulkan");
  const auto create = [&] {
    return createGraphicsPipeline(device, layout, vertex, fragment, expected);
  };
  injection = Injection::failure;
  const auto failed = create();
  check(hasError(failed, vk::Result::eErrorOutOfHostMemory) &&
            failed.error().context() == "vkCreateGraphicsPipelines",
        "native VkResult preserved");
  injection = Injection::null_success;
  check(hasError(create(), Error::native_null_handle), "null success rejected");
  const auto prior = destroys;
  injection = Injection::partial_failure;
  check(hasError(create(), vk::Result::eErrorOutOfHostMemory) && destroys == prior + 1,
        "partial failure destroys returned pipeline exactly once");
  {
    auto caller = expected;
    auto first = createGraphicsPipeline(device, layout, vertex, fragment, caller);
    auto second = create();
    if (!check(first && second, "real headless graphics pipelines"))
      return EXIT_FAILURE;
    const auto native = first->nativePipeline();
    check(first->valid() && static_cast<bool>(*first) && native &&
              first->nativeHandle() == vk::Pipeline{native} &&
              first->nativeDevice() == device.nativeHandle() &&
              first->nativeVkDevice() == static_cast<VkDevice>(device.nativeHandle()) &&
              first->nativeLayout() == layout.nativeHandle() && first->correlatedWith(device) &&
              first->correlatedWith(layout) && !first->correlatedWith(other) &&
              !first->correlatedWith(*foreign_layout) && !first->correlatedWith(moved_from),
          "live native observers and correlation");
    caller = {};
    const auto verifyConfig = [&](const GraphicsPipeline &pipeline) {
      const auto *config = pipeline.configuration();
      check(config && sameDescription(config->requested, expected) &&
                sameDescription(config->effective, expected) &&
                config->stages ==
                    (vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment) &&
                config->limits == device.plan()->capabilities().properties.limits,
            "owned requested/effective descriptions, exact stages and retained limits");
    };
    verifyConfig(*first);
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    GraphicsPipeline moved = std::move(*first);
    check(emptyPipeline(*first, device, layout) && moved.nativePipeline() == native,
          "move construction clears every source observer");
    verifyConfig(moved);
    const auto displaced = destroys;
    *second = std::move(moved);
    check(emptyPipeline(moved, device, layout) && second->nativePipeline() == native &&
              destroys == displaced + 1,
          "move assignment destroys displaced pipeline once");
    verifyConfig(*second);
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  }
  check(destroys == prior + 3 && handles == destroys, "all native handles destroyed exactly once");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity == terreate::DiagnosticSeverity::warning ||
        event.severity == terreate::DiagnosticSeverity::error)
      for (const auto &category : event.categories)
        if (category == "VALIDATION") {
          warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
          break;
        }
  }
};
int exerciseDevices(Instance &instance) {
  const auto devices = queryPhysicalDevices(instance);
  if (!devices || devices->empty())
    return blocked("no Vulkan PhysicalDevice");
  for (const auto &candidate : *devices) {
    DeviceDescription request;
    DeviceQueueRequest queue;
    queue.caller_id = "graphics-pipeline-test";
    queue.required_flags = vk::QueueFlagBits::eGraphics;
    request.queue_requests.push_back(queue);
    request.required_features_13.dynamicRendering = VK_TRUE;
    const auto plan = resolveDevice(candidate, request);
    if (!plan)
      continue;
    request.required_features_13.dynamicRendering = VK_FALSE;
    const auto disabled_plan = resolveDevice(candidate, request);
    if (!disabled_plan)
      continue;
    auto device = createDevice(instance, *plan);
    auto other = createDevice(instance, *plan);
    auto disabled = createDevice(instance, *disabled_plan);
    if (!device || !other || !disabled)
      return blocked("three logical Devices required");
    check(device->plan()->enabledFeatures13().dynamicRendering,
          "explicit dynamicRendering enabled");
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    Device moved = std::move(*device);
    const auto status = pipelineTest(moved, *device, *other, *disabled);
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    for (auto &record : live) {
      if (record.pipeline) {
        check(false, "leaked native pipeline");
        vkDestroyPipeline(record.device, record.pipeline, nullptr);
      }
    }
    return status;
  }
  return blocked("no graphics Device with dynamicRendering");
}
int nativeTest() {
  const auto caps = queryInstanceCapabilities();
  if (!caps)
    return blocked("Vulkan instance capabilities unavailable");
  bool validation = false;
  bool debug = false;
  for (const auto &layer : caps->available_layers)
    validation |= layer == "VK_LAYER_KHRONOS_validation";
  for (const auto &extension : caps->available_extensions)
    debug |= extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (!validation || !debug)
    return blocked("Validation Layer and Debug Utils required");
  InstanceDescription request;
  request.api_version = VK_API_VERSION_1_3;
  request.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  request.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(request, *caps);
  if (!plan)
    return blocked("Vulkan 1.3 Instance resolution");
  bool enabled = false;
  for (const auto &layer : plan->enabledLayers())
    enabled |= layer == "VK_LAYER_KHRONOS_validation";
  if (!enabled || !plan->debugUtilsEnabled())
    return blocked("validation not enabled");
  ValidationRecorder recorder;
  {
    auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
    if (!instance)
      return blocked("headless Instance creation");
    const auto raw = static_cast<VkInstance>(instance->nativeHandle());
    const auto send = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
        vkGetInstanceProcAddr(raw, "vkSubmitDebugUtilsMessageEXT"));
    if (!send)
      return blocked("validation callback probe unavailable");
    VkDebugUtilsMessengerCallbackDataEXT data{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    data.pMessageIdName = "graphics-pipeline-validation-probe";
    data.pMessage = "synthetic validation warning to verify native sink";
    send(raw, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
    if (!check(recorder.warnings_or_errors.load() == 1, "validation sink receives native probe"))
      return EXIT_FAILURE;
    recorder.warnings_or_errors.store(0);
    const auto status = exerciseDevices(*instance);
    if (status != EXIT_SUCCESS)
      return status;
  }
  check(recorder.warnings_or_errors.load() == 0,
        "no validation warnings/errors including teardown");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.graphics_pipeline: validation-layer=PASS");
  std::puts("graphics.graphics_pipeline: headless-native=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify cpu or gpu mode");
  const std::string_view mode{argv[1]};
  if (mode == "cpu")
    return cpuTest();
  if (mode == "gpu") {
    test_name = "graphics.graphics_pipeline";
    return nativeTest();
  }
  return blocked("unknown mode");
}

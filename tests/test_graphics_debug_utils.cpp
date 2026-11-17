// #340: explicit Debug Utils object naming and command/queue labels.
//
// The test wraps vkGetInstanceProcAddr (-Wl,--wrap) so the entry points that
// createDebugUtils loads become counting thunks that forward to the real
// Vulkan functions. Native Vulkan is otherwise used only for the caller-owned
// Fence (#258) and the validation-sink probe.
#include <terreate/graphics/debug_utils.hpp>

#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/descriptor_write.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/pipeline_commands.hpp>
#include <terreate/graphics/spirv_binary.hpp>
#include <terreate/graphics/submission.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace terreate::graphics;
using Error = DebugUtilsError;

// Offline-compiled and validated (spirv-val --target-env vulkan1.3) fixture;
// no compiler or reflection at runtime. 230 words, SHA-256 of the .spv:
// 91582f1f52b3f60f544f72ce548656c11919c78c119f25f33023bf6e21522b0b
//   glslangValidator -V -g0 --target-env vulkan1.3 -S comp -o fill.spv fill.comp
// GLSL: #version 450
//       layout(local_size_x = 64) in;
//       layout(set = 0, binding = 0) buffer Out { uint values[]; } out_buf;
//       layout(push_constant) uniform Push { uint base; uint scale; } pc;
//       void main() {
//         uint i = gl_GlobalInvocationID.x;
//         out_buf.values[i] = pc.base + i * pc.scale;
//       }
constexpr std::array<std::uint32_t, 230> fill_words{
    0x07230203, 0x00010600, 0x0008000b, 0x00000028, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000b, 0x00000013, 0x00000019,
    0x00060010, 0x00000004, 0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00040047, 0x0000000b,
    0x0000000b, 0x0000001c, 0x00040047, 0x00000010, 0x00000006, 0x00000004, 0x00030047, 0x00000011,
    0x00000002, 0x00050048, 0x00000011, 0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x00000013,
    0x00000021, 0x00000000, 0x00040047, 0x00000013, 0x00000022, 0x00000000, 0x00030047, 0x00000017,
    0x00000002, 0x00050048, 0x00000017, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000017,
    0x00000001, 0x00000023, 0x00000004, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040020, 0x00000007, 0x00000007, 0x00000006,
    0x00040017, 0x00000009, 0x00000006, 0x00000003, 0x00040020, 0x0000000a, 0x00000001, 0x00000009,
    0x0004003b, 0x0000000a, 0x0000000b, 0x00000001, 0x0004002b, 0x00000006, 0x0000000c, 0x00000000,
    0x00040020, 0x0000000d, 0x00000001, 0x00000006, 0x0003001d, 0x00000010, 0x00000006, 0x0003001e,
    0x00000011, 0x00000010, 0x00040020, 0x00000012, 0x0000000c, 0x00000011, 0x0004003b, 0x00000012,
    0x00000013, 0x0000000c, 0x00040015, 0x00000014, 0x00000020, 0x00000001, 0x0004002b, 0x00000014,
    0x00000015, 0x00000000, 0x0004001e, 0x00000017, 0x00000006, 0x00000006, 0x00040020, 0x00000018,
    0x00000009, 0x00000017, 0x0004003b, 0x00000018, 0x00000019, 0x00000009, 0x00040020, 0x0000001a,
    0x00000009, 0x00000006, 0x0004002b, 0x00000014, 0x0000001e, 0x00000001, 0x00040020, 0x00000023,
    0x0000000c, 0x00000006, 0x0004002b, 0x00000006, 0x00000025, 0x00000040, 0x0004002b, 0x00000006,
    0x00000026, 0x00000001, 0x0006002c, 0x00000009, 0x00000027, 0x00000025, 0x00000026, 0x00000026,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b,
    0x00000007, 0x00000008, 0x00000007, 0x00050041, 0x0000000d, 0x0000000e, 0x0000000b, 0x0000000c,
    0x0004003d, 0x00000006, 0x0000000f, 0x0000000e, 0x0003003e, 0x00000008, 0x0000000f, 0x0004003d,
    0x00000006, 0x00000016, 0x00000008, 0x00050041, 0x0000001a, 0x0000001b, 0x00000019, 0x00000015,
    0x0004003d, 0x00000006, 0x0000001c, 0x0000001b, 0x0004003d, 0x00000006, 0x0000001d, 0x00000008,
    0x00050041, 0x0000001a, 0x0000001f, 0x00000019, 0x0000001e, 0x0004003d, 0x00000006, 0x00000020,
    0x0000001f, 0x00050084, 0x00000006, 0x00000021, 0x0000001d, 0x00000020, 0x00050080, 0x00000006,
    0x00000022, 0x0000001c, 0x00000021, 0x00060041, 0x00000023, 0x00000024, 0x00000013, 0x00000015,
    0x00000016, 0x0003003e, 0x00000024, 0x00000022, 0x000100fd, 0x00010038,
};

constexpr std::uint32_t invocations = 64;
constexpr vk::DeviceSize payload_bytes = invocations * sizeof(std::uint32_t);

const char *test_name = "graphics.debug_utils.cpu";
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

template <typename T> bool hasError(const terreate::Result<T> &result, Error error) {
  return !result && result.error().code() == make_error_code(error);
}

// Observed native Debug Utils calls; every thunk forwards to the real function.
struct Recorded {
  std::size_t names = 0;
  VkObjectType object_type = VK_OBJECT_TYPE_UNKNOWN;
  std::uint64_t object_handle = 0;
  bool name_null = false;
  std::string name{};
  std::size_t cmd_begin = 0;
  std::size_t cmd_end = 0;
  std::size_t cmd_insert = 0;
  std::size_t queue_begin = 0;
  std::size_t queue_end = 0;
  std::size_t queue_insert = 0;
  std::string label{};
  std::array<float, 4> color{};
};
Recorded recorded;

std::size_t nativeCalls() {
  return recorded.names + recorded.cmd_begin + recorded.cmd_end + recorded.cmd_insert +
         recorded.queue_begin + recorded.queue_end + recorded.queue_insert;
}

PFN_vkSetDebugUtilsObjectNameEXT real_set_name = nullptr;
PFN_vkCmdBeginDebugUtilsLabelEXT real_cmd_begin = nullptr;
PFN_vkCmdEndDebugUtilsLabelEXT real_cmd_end = nullptr;
PFN_vkCmdInsertDebugUtilsLabelEXT real_cmd_insert = nullptr;
PFN_vkQueueBeginDebugUtilsLabelEXT real_queue_begin = nullptr;
PFN_vkQueueEndDebugUtilsLabelEXT real_queue_end = nullptr;
PFN_vkQueueInsertDebugUtilsLabelEXT real_queue_insert = nullptr;

void recordLabel(const VkDebugUtilsLabelEXT *label) {
  recorded.label = label->pLabelName;
  std::memcpy(recorded.color.data(), label->color, sizeof(label->color));
}

VKAPI_ATTR VkResult VKAPI_CALL countingSetName(VkDevice device,
                                               const VkDebugUtilsObjectNameInfoEXT *info) {
  ++recorded.names;
  recorded.object_type = info->objectType;
  recorded.object_handle = info->objectHandle;
  recorded.name_null = info->pObjectName == nullptr;
  recorded.name = info->pObjectName == nullptr ? std::string{} : std::string{info->pObjectName};
  return real_set_name(device, info);
}
VKAPI_ATTR void VKAPI_CALL countingCmdBegin(VkCommandBuffer command,
                                            const VkDebugUtilsLabelEXT *label) {
  ++recorded.cmd_begin;
  recordLabel(label);
  real_cmd_begin(command, label);
}
VKAPI_ATTR void VKAPI_CALL countingCmdEnd(VkCommandBuffer command) {
  ++recorded.cmd_end;
  real_cmd_end(command);
}
VKAPI_ATTR void VKAPI_CALL countingCmdInsert(VkCommandBuffer command,
                                             const VkDebugUtilsLabelEXT *label) {
  ++recorded.cmd_insert;
  recordLabel(label);
  real_cmd_insert(command, label);
}
VKAPI_ATTR void VKAPI_CALL countingQueueBegin(VkQueue queue, const VkDebugUtilsLabelEXT *label) {
  ++recorded.queue_begin;
  recordLabel(label);
  real_queue_begin(queue, label);
}
VKAPI_ATTR void VKAPI_CALL countingQueueEnd(VkQueue queue) {
  ++recorded.queue_end;
  real_queue_end(queue);
}
VKAPI_ATTR void VKAPI_CALL countingQueueInsert(VkQueue queue, const VkDebugUtilsLabelEXT *label) {
  ++recorded.queue_insert;
  recordLabel(label);
  real_queue_insert(queue, label);
}

template <typename Function>
PFN_vkVoidFunction thunk(PFN_vkVoidFunction real, Function &slot, Function counting) {
  if (real == nullptr)
    return nullptr;
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  slot = reinterpret_cast<Function>(real);
  return reinterpret_cast<PFN_vkVoidFunction>(counting);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

// Caller-owned sync primitive: #258 borrows the Fence, it never creates one.
struct NativeFence {
  VkDevice device = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  NativeFence() = default;
  NativeFence(const NativeFence &) = delete;
  NativeFence &operator=(const NativeFence &) = delete;
  ~NativeFence() {
    if (fence != VK_NULL_HANDLE)
      vkDestroyFence(device, fence, nullptr);
  }
};
} // namespace

// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" PFN_vkVoidFunction __real_vkGetInstanceProcAddr(VkInstance, const char *);

extern "C" PFN_vkVoidFunction __wrap_vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  const auto real = __real_vkGetInstanceProcAddr(instance, name);
  const std::string_view requested{name};
  if (requested == "vkSetDebugUtilsObjectNameEXT")
    return thunk(real, real_set_name, &countingSetName);
  if (requested == "vkCmdBeginDebugUtilsLabelEXT")
    return thunk(real, real_cmd_begin, &countingCmdBegin);
  if (requested == "vkCmdEndDebugUtilsLabelEXT")
    return thunk(real, real_cmd_end, &countingCmdEnd);
  if (requested == "vkCmdInsertDebugUtilsLabelEXT")
    return thunk(real, real_cmd_insert, &countingCmdInsert);
  if (requested == "vkQueueBeginDebugUtilsLabelEXT")
    return thunk(real, real_queue_begin, &countingQueueBegin);
  if (requested == "vkQueueEndDebugUtilsLabelEXT")
    return thunk(real, real_queue_end, &countingQueueEnd);
  if (requested == "vkQueueInsertDebugUtilsLabelEXT")
    return thunk(real, real_queue_insert, &countingQueueInsert);
  return real;
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
template <typename Handle> std::uint64_t handleValue(Handle handle) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<std::uint64_t>(handle);
}

bool named(VkObjectType type, std::uint64_t handle, std::string_view name) {
  return recorded.object_type == type && recorded.object_handle == handle && !recorded.name_null &&
         recorded.name == name;
}

int cpuTest() {
  static_assert(std::is_trivially_copyable_v<DebugUtils>);
  static_assert(std::is_nothrow_default_constructible_v<DebugUtils>);
  const DebugUtils disabled;
  check(!disabled.valid() && !disabled && disabled.nativeDevice() == VK_NULL_HANDLE &&
            disabled.nativeFunctions().set_object_name == nullptr,
        "a default DebugUtils is invalid and has no entry points");
  // Every call on a disabled value fails explicitly; nothing is a silent no-op.
  check(hasError(setObjectName(disabled, vk::ObjectType::eBuffer, 1, "name"), Error::not_enabled),
        "generic naming on a disabled value returns not_enabled");
  check(hasError(setObjectName(disabled, Queue{}, "name"), Error::not_enabled),
        "typed naming on a disabled value returns not_enabled before object checks");
  check(hasError(beginQueueDebugLabel(disabled, Queue{}, {"label"}), Error::not_enabled) &&
            hasError(insertQueueDebugLabel(disabled, Queue{}, {"label"}), Error::not_enabled) &&
            hasError(endQueueDebugLabel(disabled, Queue{}), Error::not_enabled),
        "queue labels on a disabled value return not_enabled");
  check(nativeCalls() == 0, "no native call without a DebugUtils value");
  check(std::string{debug_utils_error_category().name()} == "terreate.graphics.debug_utils" &&
            make_error_code(Error::extension_not_enabled).message() ==
                "VK_EXT_debug_utils is not enabled on the Instance",
        "error category name and message");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.debug_utils.cpu: cpu=PASS");
  return EXIT_SUCCESS;
}

int exercise(const Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             Device &other, Queue queue, const Instance &plain_instance, Device &plain_device) {
  // Creation: the extension and the parent are explicit preconditions.
  check(hasError(createDebugUtils(plain_instance, plain_device), Error::extension_not_enabled),
        "an Instance without VK_EXT_debug_utils yields extension_not_enabled");
  check(hasError(createDebugUtils(instance, plain_device), Error::parent_mismatch),
        "a Device of another Instance yields parent_mismatch");
  auto debug = createDebugUtils(instance, device);
  auto other_debug = createDebugUtils(instance, other);
  if (!debug || !other_debug)
    return blocked("createDebugUtils on Debug Utils Instance");
  const auto &functions = debug->nativeFunctions();
  check(debug->valid() && debug->nativeDevice() == static_cast<VkDevice>(device.nativeHandle()) &&
            debug->correlatedWith(device) && !debug->correlatedWith(other) &&
            debug->correlatedWith(queue) && !debug->correlatedWith(other.queue("debug-utils")) &&
            functions.set_object_name != nullptr && functions.cmd_begin_label != nullptr &&
            functions.cmd_end_label != nullptr && functions.cmd_insert_label != nullptr &&
            functions.queue_begin_label != nullptr && functions.queue_end_label != nullptr &&
            functions.queue_insert_label != nullptr,
        "a DebugUtils value exposes its Device, queues and all 7 entry points");
  check(nativeCalls() == 0, "creation makes no Debug Utils call");

  // Representative objects of every typed overload.
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("VMA allocator");
  BufferDescription buffer_description;
  buffer_description.size = payload_bytes;
  buffer_description.usage = vk::BufferUsageFlagBits::eStorageBuffer;
  auto storage = createBuffer(*allocator, buffer_description);
  ImageDescription image_description;
  image_description.format = vk::Format::eR8G8B8A8Unorm;
  image_description.extent = vk::Extent3D{4, 4, 1};
  image_description.usage = vk::ImageUsageFlagBits::eSampled;
  auto image = createImage(*allocator, image_description);
  if (!storage || !image)
    return blocked("Buffer and Image");
  ImageViewDescription view_description;
  view_description.format = vk::Format::eR8G8B8A8Unorm;
  view_description.subresource_range =
      vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
  auto view = createImageView(device, *image, view_description);
  auto sampler = createSampler(device);
  if (!view || !sampler)
    return blocked("ImageView and Sampler");
  const DescriptorSetLayoutDescription set_description{
      {{0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute}}};
  auto set_layout = createDescriptorSetLayout(device, set_description);
  auto pool = createDescriptorPool(device, {1, {}, {{vk::DescriptorType::eStorageBuffer, 1}}});
  if (!set_layout || !pool)
    return blocked("DescriptorSetLayout and DescriptorPool");
  auto set = pool->allocate(*set_layout);
  if (!set)
    return blocked("allocate DescriptorSet");
  const std::array<DescriptorWriteDescription, 1> writes{
      {{0, 0, vk::DescriptorType::eStorageBuffer,
        std::vector<DescriptorBufferWriteElement>{{&*storage, 0, payload_bytes}}}}};
  if (!updateDescriptorSet(device, *set_layout, *set, writes))
    return blocked("write storage descriptor");
  const std::array<const DescriptorSetLayout *, 1> set_layouts{&*set_layout};
  auto layout = createPipelineLayout(
      device, set_layouts,
      {{vk::PushConstantRange{vk::ShaderStageFlagBits::eCompute, 0, 2 * sizeof(std::uint32_t)}}});
  const auto binary = SpirvBinary::fromWords(fill_words);
  if (!layout || !binary)
    return blocked("PipelineLayout and SPIR-V fixture");
  auto shader = createShaderModule(device, *binary);
  if (!shader)
    return blocked("ShaderModule");
  auto pipeline =
      createComputePipeline(device, *shader, *layout, ComputePipelineDescription{"main"});
  if (!pipeline)
    return blocked("ComputePipeline");
  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  auto command_pool = createCommandPool(device, pool_description);
  auto other_pool = createCommandPool(other, pool_description);
  if (!command_pool || !other_pool)
    return blocked("CommandPools on both Devices");
  auto command = command_pool->allocatePrimary();
  auto other_command = other_pool->allocatePrimary();
  if (!command || !other_command || !other_command->begin())
    return blocked("primary CommandBuffers");

  // Each successful name is exactly one native call carrying the exact
  // object type, handle and name.
  std::size_t expected_names = 0;
  const auto name_case = [&](const char *what, auto &&call, VkObjectType type, std::uint64_t handle,
                             std::string_view name) {
    const auto result = call();
    ++expected_names;
    check(result.has_value() && recorded.names == expected_names && named(type, handle, name),
          what);
  };
  name_case(
      "Buffer name reaches vkSetDebugUtilsObjectNameEXT",
      [&] { return setObjectName(*debug, *storage, "storage-buffer"); }, VK_OBJECT_TYPE_BUFFER,
      handleValue(storage->nativeBuffer()), "storage-buffer");
  name_case(
      "Image name", [&] { return setObjectName(*debug, *image, "sampled-image"); },
      VK_OBJECT_TYPE_IMAGE, handleValue(static_cast<VkImage>(image->nativeHandle())),
      "sampled-image");
  name_case(
      "ImageView name", [&] { return setObjectName(*debug, *view, "sampled-view"); },
      VK_OBJECT_TYPE_IMAGE_VIEW, handleValue(view->nativeImageView()), "sampled-view");
  name_case(
      "Sampler name", [&] { return setObjectName(*debug, *sampler, "nearest-sampler"); },
      VK_OBJECT_TYPE_SAMPLER, handleValue(sampler->nativeSampler()), "nearest-sampler");
  name_case(
      "ShaderModule name", [&] { return setObjectName(*debug, *shader, "fill-shader"); },
      VK_OBJECT_TYPE_SHADER_MODULE, handleValue(shader->nativeShaderModule()), "fill-shader");
  name_case(
      "PipelineLayout name", [&] { return setObjectName(*debug, *layout, "fill-layout"); },
      VK_OBJECT_TYPE_PIPELINE_LAYOUT, handleValue(layout->nativePipelineLayout()), "fill-layout");
  name_case(
      "ComputePipeline name", [&] { return setObjectName(*debug, *pipeline, "fill-pipeline"); },
      VK_OBJECT_TYPE_PIPELINE, handleValue(pipeline->nativePipeline()), "fill-pipeline");
  name_case(
      "DescriptorSetLayout name",
      [&] { return setObjectName(*debug, *set_layout, "storage-set-layout"); },
      VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, handleValue(set_layout->nativeDescriptorSetLayout()),
      "storage-set-layout");
  name_case(
      "DescriptorPool name", [&] { return setObjectName(*debug, *pool, "storage-pool"); },
      VK_OBJECT_TYPE_DESCRIPTOR_POOL, handleValue(pool->nativeDescriptorPool()), "storage-pool");
  name_case(
      "CommandBuffer name", [&] { return setObjectName(*debug, *command, "fill-commands"); },
      VK_OBJECT_TYPE_COMMAND_BUFFER,
      handleValue(static_cast<VkCommandBuffer>(command->nativeHandle())), "fill-commands");
  name_case(
      "Queue name", [&] { return setObjectName(*debug, queue, "compute-queue"); },
      VK_OBJECT_TYPE_QUEUE, handleValue(static_cast<VkQueue>(queue.nativeHandle())),
      "compute-queue");
  name_case(
      "generic name of the native Device",
      [&] {
        return setObjectName(*debug, vk::ObjectType::eDevice,
                             handleValue(static_cast<VkDevice>(device.nativeHandle())),
                             "debug-device");
      },
      VK_OBJECT_TYPE_DEVICE, handleValue(static_cast<VkDevice>(device.nativeHandle())),
      "debug-device");
  const std::string long_name(300, 'n');
  name_case(
      "a name longer than the inline buffer is copied whole",
      [&] { return setObjectName(*debug, *storage, long_name); }, VK_OBJECT_TYPE_BUFFER,
      handleValue(storage->nativeBuffer()), long_name);
  // An empty name removes the name: pObjectName is NULL, as Vulkan allows.
  check(setObjectName(*debug, *storage, "").has_value() && recorded.names == ++expected_names &&
            recorded.name_null && recorded.object_handle == handleValue(storage->nativeBuffer()),
        "an empty name removes the object's name with a NULL pObjectName");

  // Rejections make no native call.
  const auto calls_before_rejections = nativeCalls();
  constexpr std::string_view embedded_nul{"bad\0name", 8};
  check(hasError(setObjectName(*debug, *storage, embedded_nul), Error::invalid_name),
        "a name with NUL is rejected");
  check(hasError(setObjectName(*debug, vk::ObjectType::eUnknown, 1, "x"), Error::invalid_object) &&
            hasError(setObjectName(*debug, vk::ObjectType::eBuffer, 0, "x"), Error::invalid_object),
        "the generic overload rejects eUnknown and a null handle");
  check(
      hasError(setObjectName(*other_debug, *storage, "x"), Error::parent_mismatch) &&
          hasError(setObjectName(*debug, *other_command, "x"), Error::parent_mismatch) &&
          hasError(setObjectName(*debug, other.queue("debug-utils"), "x"), Error::parent_mismatch),
      "objects of another Device are rejected");
  check(hasError(setObjectName(*debug, Queue{}, "x"), Error::invalid_object),
        "an invalid Queue cannot be named");
  check(hasError(recordBeginDebugLabel(*debug, *command, {"early"}), Error::not_recording),
        "a CommandBuffer that is not recording cannot take a label");
  check(hasError(recordInsertDebugLabel(*debug, *other_command, {"x"}), Error::parent_mismatch) &&
            hasError(recordEndDebugLabel(*debug, *other_command), Error::parent_mismatch),
        "a CommandBuffer of another Device cannot take a label");
  check(hasError(beginQueueDebugLabel(*debug, other.queue("debug-utils"), {"x"}),
                 Error::parent_mismatch) &&
            hasError(endQueueDebugLabel(*debug, Queue{}), Error::invalid_queue),
        "queue labels reject another Device's Queue and an invalid Queue");
  check(hasError(beginQueueDebugLabel(*debug, queue, {embedded_nul}), Error::invalid_name),
        "a label with NUL is rejected");
  check(nativeCalls() == calls_before_rejections, "every rejection made no native call");

  // A labeled compute dispatch inside a labeled queue region.
  if (!command->begin())
    return blocked("begin CommandBuffer");
  const DebugLabel region{"terreate-fill", {1.0F, 0.5F, 0.0F, 1.0F}};
  if (!check(recordBeginDebugLabel(*debug, *command, region).has_value(), "begin label"))
    return EXIT_FAILURE;
  check(recorded.cmd_begin == 1 && recorded.label == "terreate-fill" &&
            recorded.color == region.color,
        "one vkCmdBeginDebugUtilsLabelEXT with the name and color");
  const std::array<const DescriptorSet *, 1> bound{&*set};
  const std::array<std::uint32_t, 2> push_values{1000, 3};
  if (!recordBindPipeline(device, *command, *pipeline) ||
      !recordBindDescriptorSets(device, *command, vk::PipelineBindPoint::eCompute, *layout, 0,
                                bound, {}) ||
      !recordPushConstants(device, *command, *layout, vk::ShaderStageFlagBits::eCompute, 0,
                           std::as_bytes(std::span{push_values})))
    return blocked("record compute bind, descriptor set and push constants");
  check(recordInsertDebugLabel(*debug, *command, {"terreate-dispatch"}).has_value() &&
            recorded.cmd_insert == 1 && recorded.label == "terreate-dispatch" &&
            recorded.color == std::array<float, 4>{},
        "one vkCmdInsertDebugUtilsLabelEXT with no color");
  if (!recordDispatch(device, *command, 1, 1, 1))
    return blocked("record dispatch");
  check(recordEndDebugLabel(*debug, *command).has_value() && recorded.cmd_end == 1,
        "one vkCmdEndDebugUtilsLabelEXT");
  if (!command->end())
    return blocked("end CommandBuffer");
  check(hasError(recordInsertDebugLabel(*debug, *command, {"late"}), Error::not_recording) &&
            recorded.cmd_insert == 1,
        "an executable CommandBuffer cannot take a label");

  NativeFence fence;
  fence.device = static_cast<VkDevice>(device.nativeHandle());
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(fence.device, &fence_info, nullptr, &fence.fence) != VK_SUCCESS)
    return blocked("native Fence");
  const std::array<const CommandBuffer *, 1> commands{&*command};
  int identity_domain = 0;
  CompletionDescription completion;
  completion.device = &device;
  completion.queue = queue;
  completion.requested = {&identity_domain, 1};
  completion.effective = {&identity_domain, 1};
  completion.condition = FenceCompletionCondition{vk::Fence{fence.fence}};
  SubmissionDescription submission;
  submission.command_buffers = commands;
  submission.fence = vk::Fence{fence.fence};
  submission.completion = completion;
  if (!check(beginQueueDebugLabel(*debug, queue, {"terreate-submit", {0.0F, 1.0F, 0.0F, 1.0F}})
                 .has_value(),
             "begin queue label"))
    return EXIT_FAILURE;
  check(recorded.queue_begin == 1 && recorded.label == "terreate-submit",
        "one vkQueueBeginDebugUtilsLabelEXT with the name");
  auto receipt = submit(device, queue, submission);
  if (!receipt)
    std::_Exit(blocked("caller Queue submission"));
  check(insertQueueDebugLabel(*debug, queue, {"terreate-submitted"}).has_value() &&
            recorded.queue_insert == 1 && recorded.label == "terreate-submitted",
        "one vkQueueInsertDebugUtilsLabelEXT");
  check(endQueueDebugLabel(*debug, queue).has_value() && recorded.queue_end == 1,
        "one vkQueueEndDebugUtilsLabelEXT");
  const auto waited = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!waited || waited->state != SubmissionCompletionState::completed)
    std::_Exit(blocked("Fence did not prove GPU completion"));
  check(recorded.cmd_begin == 1 && recorded.cmd_insert == 1 && recorded.cmd_end == 1 &&
            recorded.queue_begin == 1 && recorded.queue_insert == 1 && recorded.queue_end == 1,
        "exactly one native call per successful label call");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

std::optional<std::size_t> computeFamily(const PhysicalDeviceCandidate &candidate) {
  const auto &families = candidate.capabilities().queue_families;
  for (std::size_t index = 0; index < families.size(); ++index)
    if (families[index].queueCount && (families[index].queueFlags & vk::QueueFlagBits::eCompute))
      return index;
  return std::nullopt;
}

DeviceDescription computeDeviceDescription(std::size_t family) {
  DeviceDescription description;
  description.required_features_13.synchronization2 = VK_TRUE;
  DeviceQueueRequest request;
  request.caller_id = "debug-utils";
  request.required_flags = vk::QueueFlagBits::eCompute;
  request.allowed_family_indices = {static_cast<std::uint32_t>(family)};
  description.queue_requests.push_back(request);
  return description;
}

int nativeTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  bool has_validation = false;
  bool has_debug_utils = false;
  for (const auto &layer : capabilities->available_layers)
    has_validation |= layer == "VK_LAYER_KHRONOS_validation";
  for (const auto &extension : capabilities->available_extensions)
    has_debug_utils |= extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (!has_debug_utils) {
    // The feature under test cannot exist here; never report PASS.
    std::puts("graphics.debug_utils: debug-utils=SKIP reason=VK_EXT_debug_utils unavailable");
    return EXIT_SUCCESS;
  }

  InstanceDescription instance_description;
  instance_description.api_version = VK_API_VERSION_1_3;
  instance_description.application_name = "terreate-debug-utils";
  instance_description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  instance_description.debug_utils = DebugUtilsMode::required;
  const auto instance_plan = resolveInstance(instance_description, *capabilities);
  if (!instance_plan || !instance_plan->debugUtilsEnabled())
    return blocked("Vulkan 1.3 Instance with Debug Utils");
  ValidationRecorder recorder;
  auto instance = createInstance(*instance_plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("headless Vulkan Instance");
  // The same request without Debug Utils: the disabled side of the contract.
  InstanceDescription plain_description;
  plain_description.api_version = VK_API_VERSION_1_3;
  plain_description.application_name = "terreate-debug-utils-plain";
  const auto plain_plan = resolveInstance(plain_description, *capabilities);
  if (!plain_plan || plain_plan->debugUtilsEnabled())
    return blocked("Vulkan 1.3 Instance without Debug Utils");
  auto plain_instance = createInstance(*plain_plan);
  if (!plain_instance)
    return blocked("plain Vulkan Instance");

  if (has_validation) {
    // Prove that validation messages reach the recorder, so a silent sink
    // cannot produce a false validation PASS.
    const auto native_instance = static_cast<VkInstance>(instance->nativeHandle());
    const auto emit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
        vkGetInstanceProcAddr(native_instance, "vkSubmitDebugUtilsMessageEXT"));
    if (!emit)
      return blocked("Debug Utils callback cannot be probed");
    VkDebugUtilsMessengerCallbackDataEXT probe{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    probe.pMessageIdName = "terreate-debug-utils-validation-probe";
    probe.messageIdNumber = 340;
    probe.pMessage = "test-only validation warning sent through Vulkan Debug Utils";
    emit(native_instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &probe);
    if (!check(recorder.warnings_or_errors.load() == 1, "validation sink receives native probe"))
      return EXIT_FAILURE;
    recorder.warnings_or_errors.store(0);
    std::puts("graphics.debug_utils: validation-callback-probe=PASS");
  }

  const auto candidates = queryPhysicalDevices(*instance);
  const auto plain_candidates = queryPhysicalDevices(*plain_instance);
  if (!candidates || candidates->empty() || !plain_candidates || plain_candidates->empty())
    return blocked("headless PhysicalDevice inventories");
  for (std::size_t index = 0; index < candidates->size(); ++index) {
    const auto &candidate = (*candidates)[index];
    const auto family = computeFamily(candidate);
    if (!family)
      continue;
    const auto plan = resolveDevice(candidate, computeDeviceDescription(*family));
    if (!plan)
      continue;
    // Any compute Device of the plain Instance serves the negative checks.
    std::optional<DevicePlan> plain_device_plan;
    for (const auto &plain_candidate : *plain_candidates) {
      const auto plain_family = computeFamily(plain_candidate);
      if (!plain_family)
        continue;
      auto resolved = resolveDevice(plain_candidate, computeDeviceDescription(*plain_family));
      if (resolved) {
        plain_device_plan = std::move(*resolved);
        break;
      }
    }
    if (!plain_device_plan)
      return blocked("compute Device on the plain Instance");
    int status = EXIT_FAILURE;
    {
      auto device = createDevice(*instance, *plan);
      auto other = createDevice(*instance, *plan);
      auto plain_device = createDevice(*plain_instance, *plain_device_plan);
      if (!device || !other || !plain_device)
        return blocked("compute logical Devices");
      status = exercise(*instance, candidate, *device, *other, device->queue("debug-utils"),
                        *plain_instance, *plain_device);
    }
    if (status != EXIT_SUCCESS)
      return status;
    if (has_validation) {
      if (!check(recorder.warnings_or_errors.load() == 0,
                 "no validation warnings/errors including teardown"))
        return EXIT_FAILURE;
      std::puts("graphics.debug_utils: validation-layer=PASS");
    } else {
      std::puts("graphics.debug_utils: validation-layer=SKIP "
                "reason=VK_LAYER_KHRONOS_validation unavailable");
    }
    std::puts("graphics.debug_utils: debug-utils=PASS");
    return EXIT_SUCCESS;
  }
  return blocked("no compute queue with synchronization2");
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify cpu or gpu mode");
  const std::string_view mode{argv[1]};
  if (mode == "cpu")
    return cpuTest();
  if (mode == "gpu") {
    test_name = "graphics.debug_utils";
    return nativeTest();
  }
  return blocked("unknown mode");
}

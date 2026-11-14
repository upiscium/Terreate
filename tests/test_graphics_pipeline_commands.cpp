#include <terreate/graphics/pipeline_commands.hpp>

#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/descriptor_pool.hpp>
#include <terreate/graphics/descriptor_write.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/readback_batch.hpp>
#include <terreate/graphics/shader_module.hpp>
#include <terreate/graphics/spirv_binary.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/transfer_commands.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using namespace terreate::graphics;
using Error = PipelineCommandError;

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
constexpr std::uint32_t push_base = 1000;
constexpr std::uint32_t push_scale = 3;
constexpr std::uint32_t sentinel = 0xDEADBEEFU;
constexpr vk::DeviceSize payload_bytes = invocations * sizeof(std::uint32_t);

const char *test_name = "graphics.pipeline_commands.cpu";
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

// Observed native recording calls; recording forwards to the real driver.
struct Recorded {
  std::size_t bind_pipeline = 0;
  VkPipelineBindPoint pipeline_bind_point = VK_PIPELINE_BIND_POINT_MAX_ENUM;
  VkPipeline pipeline = VK_NULL_HANDLE;
  std::size_t bind_sets = 0;
  VkPipelineBindPoint sets_bind_point = VK_PIPELINE_BIND_POINT_MAX_ENUM;
  VkPipelineLayout sets_layout = VK_NULL_HANDLE;
  std::uint32_t first_set = 0;
  std::vector<VkDescriptorSet> sets{};
  std::vector<std::uint32_t> dynamic_offsets{};
  std::size_t push = 0;
  VkPipelineLayout push_layout = VK_NULL_HANDLE;
  VkShaderStageFlags push_stages = 0;
  std::uint32_t push_offset = 0;
  std::vector<std::byte> push_bytes{};
  std::size_t dispatch = 0;
  std::array<std::uint32_t, 3> groups{};
};
Recorded recorded;

std::size_t nativeCalls() {
  return recorded.bind_pipeline + recorded.bind_sets + recorded.push + recorded.dispatch;
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
extern "C" void __real_vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
extern "C" void __real_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint,
                                               VkPipelineLayout, std::uint32_t, std::uint32_t,
                                               const VkDescriptorSet *, std::uint32_t,
                                               const std::uint32_t *);
extern "C" void __real_vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags,
                                          std::uint32_t, std::uint32_t, const void *);
extern "C" void __real_vkCmdDispatch(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t);

extern "C" void __wrap_vkCmdBindPipeline(VkCommandBuffer command, VkPipelineBindPoint bind_point,
                                         VkPipeline pipeline) {
  ++recorded.bind_pipeline;
  recorded.pipeline_bind_point = bind_point;
  recorded.pipeline = pipeline;
  __real_vkCmdBindPipeline(command, bind_point, pipeline);
}
extern "C" void __wrap_vkCmdBindDescriptorSets(VkCommandBuffer command,
                                               VkPipelineBindPoint bind_point,
                                               VkPipelineLayout layout, std::uint32_t first_set,
                                               std::uint32_t set_count, const VkDescriptorSet *sets,
                                               std::uint32_t offset_count,
                                               const std::uint32_t *offsets) {
  ++recorded.bind_sets;
  recorded.sets_bind_point = bind_point;
  recorded.sets_layout = layout;
  recorded.first_set = first_set;
  recorded.sets.assign(sets, sets + set_count);
  recorded.dynamic_offsets.assign(offsets, offsets + offset_count);
  __real_vkCmdBindDescriptorSets(command, bind_point, layout, first_set, set_count, sets,
                                 offset_count, offsets);
}
extern "C" void __wrap_vkCmdPushConstants(VkCommandBuffer command, VkPipelineLayout layout,
                                          VkShaderStageFlags stages, std::uint32_t offset,
                                          std::uint32_t size, const void *values) {
  ++recorded.push;
  recorded.push_layout = layout;
  recorded.push_stages = stages;
  recorded.push_offset = offset;
  const auto *bytes = static_cast<const std::byte *>(values);
  recorded.push_bytes.assign(bytes, bytes + size);
  __real_vkCmdPushConstants(command, layout, stages, offset, size, values);
}
extern "C" void __wrap_vkCmdDispatch(VkCommandBuffer command, std::uint32_t x, std::uint32_t y,
                                     std::uint32_t z) {
  ++recorded.dispatch;
  recorded.groups = {x, y, z};
  __real_vkCmdDispatch(command, x, y, z);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
DescriptorSetLayoutBindingDescription binding(std::uint32_t number, vk::DescriptorType type,
                                              std::uint32_t count) {
  return {number, type, count, vk::ShaderStageFlagBits::eCompute};
}

int cpuTest() {
  static_assert(std::is_error_code_enum_v<PipelineCommandError>);
  const std::error_code first = Error::invalid_device;
  const std::error_code last = Error::allocation_failed;
  check(first.category() == pipeline_command_error_category() && first != last &&
            std::string_view{pipeline_command_error_category().name()} ==
                "terreate.graphics.pipeline_commands" &&
            !first.message().empty() && !last.message().empty(),
        "typed error category and distinct codes");

  vk::PhysicalDeviceLimits limits{};
  limits.minUniformBufferOffsetAlignment = 64;
  limits.minStorageBufferOffsetAlignment = 16;
  limits.maxComputeWorkGroupCount = std::array<std::uint32_t, 3>{8, 4, 2};

  // Set 0 lists bindings out of order; Vulkan order is by binding number:
  // binding 1 (uniform dynamic x2), binding 5 (storage dynamic), then set 1.
  const std::array<DescriptorSetLayoutDescription, 2> sets{{
      {{binding(5, vk::DescriptorType::eStorageBufferDynamic, 1),
        binding(0, vk::DescriptorType::eUniformBuffer, 1),
        binding(1, vk::DescriptorType::eUniformBufferDynamic, 2)}},
      {{binding(0, vk::DescriptorType::eStorageBufferDynamic, 1)}},
  }};
  const std::array<std::uint32_t, 4> ordered{128, 64, 16, 32};
  check(validateDynamicOffsets(sets, ordered, limits).has_value(),
        "offsets in set/binding/element order with matching alignments");
  const std::array<std::uint32_t, 4> description_order{16, 128, 64, 32};
  check(hasError(validateDynamicOffsets(sets, description_order, limits),
                 Error::dynamic_offset_misaligned),
        "description order is not the Vulkan dynamic offset order");
  const std::array<std::uint32_t, 3> too_few{128, 64, 16};
  check(
      hasError(validateDynamicOffsets(sets, too_few, limits),
               Error::dynamic_offset_count_mismatch) &&
          hasError(validateDynamicOffsets(sets, {}, limits), Error::dynamic_offset_count_mismatch),
      "dynamic offset count must equal dynamic descriptor count");
  const std::array<std::uint32_t, 4> misaligned_storage{128, 64, 8, 32};
  check(hasError(validateDynamicOffsets(sets, misaligned_storage, limits),
                 Error::dynamic_offset_misaligned),
        "storage dynamic offset uses minStorageBufferOffsetAlignment");
  const std::array<DescriptorSetLayoutDescription, 1> static_only{
      {{{binding(0, vk::DescriptorType::eUniformBuffer, 3)}}}};
  check(validateDynamicOffsets(static_only, {}, limits).has_value(),
        "sets without dynamic descriptors take no offsets");

  PipelineLayoutDescription push{{
      vk::PushConstantRange{vk::ShaderStageFlagBits::eVertex, 0, 16},
      vk::PushConstantRange{vk::ShaderStageFlagBits::eFragment, 8, 16},
  }};
  constexpr auto vertex = vk::ShaderStageFlagBits::eVertex;
  constexpr auto fragment = vk::ShaderStageFlagBits::eFragment;
  check(validatePushConstants(push, vertex, 0, 8, 128).has_value() &&
            validatePushConstants(push, fragment, 16, 8, 128).has_value() &&
            validatePushConstants(push, vertex | fragment, 8, 8, 128).has_value(),
        "covered updates include every stage of each overlapping range");
  check(hasError(validatePushConstants(push, vertex, 8, 8, 128), Error::invalid_push_constants),
        "overlapping fragment range must be updated with its stage");
  check(hasError(validatePushConstants(push, fragment, 0, 8, 128), Error::invalid_push_constants),
        "every byte must be covered for each requested stage");
  check(
      hasError(validatePushConstants(push, vertex, 0, 0, 128), Error::invalid_push_constants) &&
          hasError(validatePushConstants(push, vertex, 2, 4, 128), Error::invalid_push_constants) &&
          hasError(validatePushConstants(push, vertex, 0, 6, 128), Error::invalid_push_constants) &&
          hasError(validatePushConstants(push, {}, 0, 4, 128), Error::invalid_push_constants) &&
          hasError(validatePushConstants(push, vertex, 0, 16, 12), Error::invalid_push_constants) &&
          hasError(validatePushConstants(push, vertex, 128, 4, 128), Error::invalid_push_constants),
      "empty, unaligned, stage-less and maxPushConstantsSize-exceeding updates rejected");

  check(validateDispatch(8, 4, 2, limits).has_value() &&
            validateDispatch(0, 0, 0, limits).has_value(),
        "dispatch at limits and zero-sized dispatch accepted");
  check(hasError(validateDispatch(9, 1, 1, limits), Error::invalid_dispatch) &&
            hasError(validateDispatch(1, 5, 1, limits), Error::invalid_dispatch) &&
            hasError(validateDispatch(1, 1, 3, limits), Error::invalid_dispatch),
        "each dimension bounded by maxComputeWorkGroupCount");

  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.pipeline_commands.cpu: cpu=PASS");
  return EXIT_SUCCESS;
}

int exercise(const Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             Device &other, Queue queue) {
  const auto &limits = device.plan()->capabilities().properties.limits;
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("VMA allocator");
  // Bind at a nonzero aligned dynamic offset to prove the offset reaches the GPU.
  // Vulkan caps minStorageBufferOffsetAlignment at 256; reject anything that
  // would not survive the uint32_t dynamic offset conversion.
  const auto alignment = std::max<vk::DeviceSize>(limits.minStorageBufferOffsetAlignment, 4);
  if (alignment > std::numeric_limits<std::uint32_t>::max() - payload_bytes)
    return blocked("minStorageBufferOffsetAlignment not representable as a dynamic offset");
  const auto dynamic_offset = static_cast<std::uint32_t>(alignment);
  BufferDescription buffer_description;
  buffer_description.size = dynamic_offset + payload_bytes;
  buffer_description.usage = vk::BufferUsageFlagBits::eStorageBuffer |
                             vk::BufferUsageFlagBits::eTransferSrc |
                             vk::BufferUsageFlagBits::eTransferDst;
  auto storage = createBuffer(*allocator, buffer_description);
  if (!storage)
    return blocked("STORAGE|TRANSFER_SRC|TRANSFER_DST Buffer");

  const DescriptorSetLayoutDescription set_description{
      {binding(0, vk::DescriptorType::eStorageBufferDynamic, 1)}};
  auto set_layout = createDescriptorSetLayout(device, set_description);
  auto twin_layout = createDescriptorSetLayout(device, set_description);
  if (!set_layout || !twin_layout)
    return blocked("dynamic storage DescriptorSetLayouts");
  auto pool =
      createDescriptorPool(device, {2, {}, {{vk::DescriptorType::eStorageBufferDynamic, 2}}});
  if (!pool)
    return blocked("DescriptorPool");
  auto set = pool->allocate(*set_layout);
  auto twin_set = pool->allocate(*twin_layout);
  if (!set || !twin_set)
    return blocked("allocate DescriptorSets");
  const std::array<DescriptorWriteDescription, 1> writes{
      {{0, 0, vk::DescriptorType::eStorageBufferDynamic,
        std::vector<DescriptorBufferWriteElement>{{&*storage, 0, payload_bytes}}}}};
  if (!updateDescriptorSet(device, *set_layout, *set, writes))
    return blocked("write dynamic storage descriptor");

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

  const std::array<const DescriptorSet *, 1> bound{&*set};
  const std::array<std::uint32_t, 1> offsets{dynamic_offset};
  const std::array<std::uint32_t, 2> push_values{push_base, push_scale};
  const auto push_bytes = std::as_bytes(std::span{push_values});
  constexpr auto compute = vk::PipelineBindPoint::eCompute;
  constexpr auto compute_stage = vk::ShaderStageFlagBits::eCompute;

  check(hasError(recordBindPipeline(device, *command, *pipeline), Error::not_recording) &&
            hasError(recordDispatch(device, *command, 1, 1, 1), Error::not_recording) &&
            nativeCalls() == 0,
        "initial CommandBuffer rejects recording without native calls");
  if (!command->begin())
    return blocked("begin primary CommandBuffer");

  const std::array<const DescriptorSet *, 1> twin_bound{&*twin_set};
  const std::array<const DescriptorSet *, 1> null_bound{nullptr};
  const std::array<const DescriptorSet *, 2> two_bound{&*set, &*set};
  const std::array<std::uint32_t, 1> misaligned{dynamic_offset + 1};
  check(
      hasError(recordBindPipeline(device, *other_command, *pipeline), Error::parent_mismatch) &&
          hasError(
              recordBindDescriptorSets(device, *command, compute, *layout, 0, twin_bound, offsets),
              Error::set_layout_mismatch) &&
          hasError(
              recordBindDescriptorSets(device, *command, compute, *layout, 0, null_bound, offsets),
              Error::invalid_descriptor_set) &&
          hasError(
              recordBindDescriptorSets(device, *command, compute, *layout, 0, two_bound, offsets),
              Error::invalid_set_range) &&
          hasError(recordBindDescriptorSets(device, *command, compute, *layout, 1, bound, offsets),
                   Error::invalid_set_range) &&
          hasError(recordBindDescriptorSets(device, *command, compute, *layout, 0, bound, {}),
                   Error::dynamic_offset_count_mismatch) &&
          hasError(recordBindDescriptorSets(device, *command, vk::PipelineBindPoint::eRayTracingKHR,
                                            *layout, 0, bound, offsets),
                   Error::invalid_bind_point) &&
          hasError(recordPushConstants(device, *command, *layout, vk::ShaderStageFlagBits::eVertex,
                                       0, push_bytes),
                   Error::invalid_push_constants) &&
          hasError(recordPushConstants(device, *command, *layout, compute_stage, 4, push_bytes),
                   Error::invalid_push_constants) &&
          nativeCalls() == 0,
      "Device, set layout identity, range, offset count, bind point and push range "
      "rejected before native calls");
  if (limits.maxComputeWorkGroupCount[0] < std::numeric_limits<std::uint32_t>::max())
    check(hasError(recordDispatch(device, *command, limits.maxComputeWorkGroupCount[0] + 1, 1, 1),
                   Error::invalid_dispatch) &&
              nativeCalls() == 0,
          "group count above maxComputeWorkGroupCount rejected before native calls");
  if (limits.minStorageBufferOffsetAlignment > 1)
    check(
        hasError(recordBindDescriptorSets(device, *command, compute, *layout, 0, bound, misaligned),
                 Error::dynamic_offset_misaligned) &&
            nativeCalls() == 0,
        "dynamic offset must honor minStorageBufferOffsetAlignment");
  if (!other_command->end())
    return blocked("end foreign CommandBuffer");

  // Native escape hatch: fill the whole Buffer with a sentinel first, so the
  // readback can prove the shader wrote only the dynamic-offset range.
  vkCmdFillBuffer(static_cast<VkCommandBuffer>(command->nativeHandle()), storage->nativeBuffer(), 0,
                  VK_WHOLE_SIZE, sentinel);
  const VkMemoryBarrier2 to_compute{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                    .pNext = nullptr,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
  if (!recordBarriers(device, *command, {.memory = std::span{&to_compute, 1}}))
    return blocked("explicit sentinel fill -> compute dependency");
  if (!check(nativeCalls() == 0, "sentinel fill and barrier are not pipeline commands"))
    return EXIT_FAILURE;

  if (!recordBindPipeline(device, *command, *pipeline) ||
      !recordBindDescriptorSets(device, *command, compute, *layout, 0, bound, offsets) ||
      !recordPushConstants(device, *command, *layout, compute_stage, 0, push_bytes) ||
      !recordDispatch(device, *command, 1, 1, 1))
    return blocked("record compute bind, descriptor set, push constants and dispatch");
  check(recorded.bind_pipeline == 1 &&
            recorded.pipeline_bind_point == VK_PIPELINE_BIND_POINT_COMPUTE &&
            recorded.pipeline == pipeline->nativePipeline(),
        "one vkCmdBindPipeline with the compute bind point and pipeline");
  check(recorded.bind_sets == 1 && recorded.sets_bind_point == VK_PIPELINE_BIND_POINT_COMPUTE &&
            recorded.sets_layout == layout->nativePipelineLayout() && recorded.first_set == 0 &&
            recorded.sets == std::vector<VkDescriptorSet>{set->nativeDescriptorSet()} &&
            recorded.dynamic_offsets == std::vector<std::uint32_t>{dynamic_offset},
        "one vkCmdBindDescriptorSets with exact layout, set and dynamic offset");
  check(recorded.push == 1 && recorded.push_layout == layout->nativePipelineLayout() &&
            recorded.push_stages == VK_SHADER_STAGE_COMPUTE_BIT && recorded.push_offset == 0 &&
            recorded.push_bytes.size() == push_bytes.size() &&
            std::memcmp(recorded.push_bytes.data(), push_bytes.data(), push_bytes.size()) == 0,
        "one vkCmdPushConstants with exact stages, offset and copied bytes");
  check(recorded.dispatch == 1 && recorded.groups == std::array<std::uint32_t, 3>{1, 1, 1},
        "one vkCmdDispatch with exact group counts");

  const std::array<BufferReadbackItem, 1> reads{
      {{&*storage, 0, vk::DeviceSize{dynamic_offset} + payload_bytes}}};
  auto readback = prepareReadbackBatch(*allocator, ReadbackBatchDescription{.buffers = reads});
  if (!readback)
    return blocked("prepare Readback");
  const VkMemoryBarrier2 to_transfer{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                     .pNext = nullptr,
                                     .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                     .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                     .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                     .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
  const VkMemoryBarrier2 to_host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                 .pNext = nullptr,
                                 .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                 .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                 .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
  if (!recordBarriers(device, *command, {.memory = std::span{&to_transfer, 1}}) ||
      !recordReadbackBatch(device, *command, *readback) ||
      !recordBarriers(device, *command, {.memory = std::span{&to_host, 1}}) || !command->end())
    return blocked("explicit barriers, readback and end");
  check(recorded.bind_pipeline == 1 && recorded.bind_sets == 1 && recorded.push == 1 &&
            recorded.dispatch == 1,
        "exactly one native call per successful recorder over the whole recording");

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
  // The application submits; recorders never submit or wait.
  auto receipt = submit(device, queue, submission);
  if (!receipt)
    std::_Exit(blocked("caller Queue submission"));
  const auto waited = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!waited || waited->state != SubmissionCompletionState::completed)
    std::_Exit(blocked("Fence did not prove GPU completion"));
  const std::span<const SubmissionReceipt> uses{&*receipt, 1};
  auto copied = tryCopyCompletedReadbackBatch(device, *readback, uses);
  if (!copied)
    return blocked("copy completed Readback");
  const auto data = copied->dataFor(0);
  const std::size_t prefix_words = dynamic_offset / sizeof(std::uint32_t);
  std::vector<std::uint32_t> words(prefix_words + invocations);
  if (!check(data.size() == words.size() * sizeof(std::uint32_t),
             "readback covers the prefix and every invocation"))
    return EXIT_FAILURE;
  std::memcpy(words.data(), data.data(), data.size());
  bool prefix_untouched = true;
  for (std::size_t index = 0; index < prefix_words; ++index)
    prefix_untouched &= words[index] == sentinel;
  check(prefix_untouched, "bytes before the dynamic offset keep the sentinel");
  bool matches = true;
  for (std::uint32_t index = 0; index < invocations; ++index)
    matches &= words[prefix_words + index] == push_base + index * push_scale;
  check(matches, "GPU wrote base + i * scale at the dynamic offset using pushed constants");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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
  if (!has_validation || !has_debug_utils)
    return blocked("VK_LAYER_KHRONOS_validation and Debug Utils required");

  InstanceDescription instance_description;
  instance_description.api_version = VK_API_VERSION_1_3;
  instance_description.application_name = "terreate-pipeline-commands";
  instance_description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  instance_description.debug_utils = DebugUtilsMode::required;
  const auto instance_plan = resolveInstance(instance_description, *capabilities);
  if (!instance_plan || !instance_plan->debugUtilsEnabled())
    return blocked("Vulkan 1.3 Instance with Debug Utils");
  ValidationRecorder recorder;
  auto instance = createInstance(*instance_plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("headless Vulkan Instance");

  // Prove that validation messages actually reach the recorder.
  const auto native_instance = static_cast<VkInstance>(instance->nativeHandle());
  const auto emit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
      vkGetInstanceProcAddr(native_instance, "vkSubmitDebugUtilsMessageEXT"));
  if (!emit)
    return blocked("Debug Utils callback cannot be probed");
  VkDebugUtilsMessengerCallbackDataEXT probe{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
  probe.pMessageIdName = "terreate-pipeline-commands-validation-probe";
  probe.pMessage = "test-only validation warning sent through Vulkan Debug Utils";
  emit(native_instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &probe);
  if (!check(recorder.warnings_or_errors.load() == 1, "validation sink receives native probe"))
    return EXIT_FAILURE;
  recorder.warnings_or_errors.store(0);

  const auto candidates = queryPhysicalDevices(*instance);
  if (!candidates || candidates->empty())
    return blocked("headless PhysicalDevice inventory");
  for (const auto &candidate : *candidates) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t index = 0; index < families.size(); ++index) {
      if (!families[index].queueCount ||
          !(families[index].queueFlags & vk::QueueFlagBits::eCompute))
        continue;
      DeviceDescription device_description;
      device_description.required_features_13.synchronization2 = VK_TRUE;
      DeviceQueueRequest request;
      request.caller_id = "pipeline-commands";
      request.required_flags = vk::QueueFlagBits::eCompute;
      request.allowed_family_indices = {static_cast<std::uint32_t>(index)};
      device_description.queue_requests.push_back(request);
      const auto plan = resolveDevice(candidate, device_description);
      if (!plan)
        continue;
      int status = EXIT_FAILURE;
      {
        auto device = createDevice(*instance, *plan);
        auto other = createDevice(*instance, *plan);
        if (!device || !other)
          return blocked("two compute logical Devices");
        status =
            exercise(*instance, candidate, *device, *other, device->queue("pipeline-commands"));
      }
      if (status != EXIT_SUCCESS)
        return status;
      if (!check(recorder.warnings_or_errors.load() == 0,
                 "no validation warnings/errors including teardown"))
        return EXIT_FAILURE;
      std::puts("graphics.pipeline_commands: validation-layer=PASS");
      std::puts("graphics.pipeline_commands: headless-dispatch=PASS");
      return EXIT_SUCCESS;
    }
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
    test_name = "graphics.pipeline_commands";
    return nativeTest();
  }
  return blocked("unknown mode");
}

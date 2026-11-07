#include <terreate/graphics/upload_batch.hpp>

#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/readback_batch.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/transfer_commands.hpp>

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

namespace {
using namespace terreate::graphics;

constexpr std::size_t payload_size = 256;
constexpr std::size_t overlap_offset = 17;
constexpr std::size_t overlap_size = 29;

int failures = 0;
std::size_t copy_calls = 0;
std::size_t barrier_calls = 0;
std::size_t submit_calls = 0;
std::size_t idle_calls = 0;
std::size_t map_calls = 0;
std::size_t flush_calls = 0;
std::size_t invalidate_calls = 0;
std::size_t destroy_buffer_calls = 0;
VkFence forced_pending_fence = VK_NULL_HANDLE;

struct ObservedCopy {
  VkBuffer src = VK_NULL_HANDLE;
  VkBuffer dst = VK_NULL_HANDLE;
  VkDeviceSize src_offset = 0;
  VkDeviceSize dst_offset = 0;
  VkDeviceSize size = 0;
};
std::array<ObservedCopy, 4> observed{};

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "graphics.buffer_roundtrip: assertion=FAIL: %s\n", message);
  }
  return condition;
}

int blocked(const char *message) {
  std::fprintf(stderr, "graphics.buffer_roundtrip: validation=BLOCKED: %s\n", message);
  std::fflush(stderr);
  return EXIT_FAILURE;
}

struct NativeFence {
  VkDevice device = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  ~NativeFence() {
    if (fence != VK_NULL_HANDLE)
      vkDestroyFence(device, fence, nullptr);
  }
};

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    // Graphics diagnostic translator publishes uppercase native category names.
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};
} // namespace

// GNU linker wrappers count actual native calls, not a synthetic copy simulation.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void __real_vkCmdCopyBuffer2(VkCommandBuffer, const VkCopyBufferInfo2 *);
extern "C" void __real_vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo *);
extern "C" VkResult __real_vkQueueSubmit2(VkQueue, std::uint32_t, const VkSubmitInfo2 *, VkFence);
extern "C" VkResult __real_vkGetFenceStatus(VkDevice, VkFence);
extern "C" VkResult __real_vkDeviceWaitIdle(VkDevice);
extern "C" VkResult __real_vmaMapMemory(VmaAllocator, VmaAllocation, void **);
extern "C" VkResult __real_vmaFlushAllocation(VmaAllocator, VmaAllocation, VkDeviceSize,
                                              VkDeviceSize);
extern "C" VkResult __real_vmaInvalidateAllocation(VmaAllocator, VmaAllocation, VkDeviceSize,
                                                   VkDeviceSize);
extern "C" void __real_vmaDestroyBuffer(VmaAllocator, VkBuffer, VmaAllocation);

extern "C" void __wrap_vkCmdCopyBuffer2(VkCommandBuffer command, const VkCopyBufferInfo2 *info) {
  if (copy_calls < observed.size() && info && info->regionCount == 1 && info->pRegions &&
      info->pRegions->sType == VK_STRUCTURE_TYPE_BUFFER_COPY_2) {
    const auto &region = *info->pRegions;
    observed[copy_calls] = ObservedCopy{info->srcBuffer, info->dstBuffer, region.srcOffset,
                                        region.dstOffset, region.size};
  }
  ++copy_calls;
  __real_vkCmdCopyBuffer2(command, info);
}
extern "C" void __wrap_vkCmdPipelineBarrier2(VkCommandBuffer command,
                                             const VkDependencyInfo *info) {
  ++barrier_calls;
  __real_vkCmdPipelineBarrier2(command, info);
}
extern "C" VkResult __wrap_vkQueueSubmit2(VkQueue queue, std::uint32_t count,
                                          const VkSubmitInfo2 *info, VkFence fence) {
  ++submit_calls;
  return __real_vkQueueSubmit2(queue, count, info, fence);
}
extern "C" VkResult __wrap_vkGetFenceStatus(VkDevice device, VkFence fence) {
  if (fence != VK_NULL_HANDLE && forced_pending_fence == fence)
    return VK_NOT_READY;
  return __real_vkGetFenceStatus(device, fence);
}
extern "C" VkResult __wrap_vkDeviceWaitIdle(VkDevice device) {
  ++idle_calls;
  return __real_vkDeviceWaitIdle(device);
}
extern "C" VkResult __wrap_vmaMapMemory(VmaAllocator allocator, VmaAllocation allocation,
                                        void **data) {
  ++map_calls;
  return __real_vmaMapMemory(allocator, allocation, data);
}
extern "C" VkResult __wrap_vmaFlushAllocation(VmaAllocator allocator, VmaAllocation allocation,
                                              VkDeviceSize offset, VkDeviceSize size) {
  ++flush_calls;
  return __real_vmaFlushAllocation(allocator, allocation, offset, size);
}
extern "C" VkResult __wrap_vmaInvalidateAllocation(VmaAllocator allocator, VmaAllocation allocation,
                                                   VkDeviceSize offset, VkDeviceSize size) {
  ++invalidate_calls;
  return __real_vmaInvalidateAllocation(allocator, allocation, offset, size);
}
extern "C" void __wrap_vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer,
                                        VmaAllocation allocation) {
  ++destroy_buffer_calls;
  __real_vmaDestroyBuffer(allocator, buffer, allocation);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
int exercise(Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             Queue queue) {
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("VMA allocator");

  BufferDescription buffer_description;
  buffer_description.size = payload_size;
  buffer_description.usage =
      vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc;
  auto gpu_buffer = createBuffer(*allocator, buffer_description);
  if (!gpu_buffer)
    return blocked("TRANSFER_SRC|TRANSFER_DST GPU Buffer");

  std::array<std::byte, payload_size> input{};
  for (std::size_t i = 0; i < input.size(); ++i)
    input[i] = static_cast<std::byte>((i * 73U + 0xB3U) & 0xFFU);
  input[0] = std::byte{0};
  input[1] = std::byte{0xFF};
  input[payload_size - 1] = std::byte{0xFF};
  const auto expected = input;

  const std::array<BufferUploadItem, 1> writes{{{&*gpu_buffer, 0, input}}};
  auto uploaded = prepareUploadBatch(*allocator, UploadBatchDescription{.buffers = writes});
  if (!uploaded)
    return blocked("prepare public #338 Buffer Upload");
  if (!check(uploaded->valid() && uploaded->stagingBuffer() && uploaded->stagingBuffer()->valid() &&
                 uploaded->copies().size() == 1 && uploaded->copies()[0].destination_offset == 0 &&
                 uploaded->copies()[0].size == payload_size,
             "Upload staging snapshots the declared 256-byte range"))
    return EXIT_FAILURE;

  // Prove input lifetime independence: prepare copied the original bytes.
  input.fill(std::byte{0x55});
  const std::array<BufferReadbackItem, 2> reads{{
      {&*gpu_buffer, 0, payload_size},
      {&*gpu_buffer, overlap_offset, overlap_size},
  }};
  auto readback = prepareReadbackBatch(*allocator, ReadbackBatchDescription{.buffers = reads});
  if (!readback)
    return blocked("prepare public #339 Buffer Readback");
  if (!check(readback->valid() && readback->stagingSize() == payload_size + overlap_size &&
                 readback->buffers().size() == reads.size() &&
                 readback->buffers()[0].staging_offset == 0 &&
                 readback->buffers()[1].staging_offset == payload_size &&
                 readback->buffers()[1].source_offset == overlap_offset,
             "Readback owns two contiguous destinations for full and overlapping source reads"))
    return EXIT_FAILURE;

  check(copy_calls == 0 && barrier_calls == 0 && submit_calls == 0 && idle_calls == 0,
        "prepare performs no native GPU recording or submission");

  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  auto pool = createCommandPool(device, pool_description);
  if (!pool)
    return blocked("caller-owned CommandPool");
  auto command = pool->allocatePrimary();
  if (!command || !command->begin())
    return blocked("begin caller-owned primary CommandBuffer");

  VkMemoryBarrier2 from_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  from_host.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  from_host.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
  from_host.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  from_host.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  if (!recordBarriers(device, *command, {.memory = std::span{&from_host, 1}}))
    return blocked("explicit upload staging HOST_WRITE -> TRANSFER_READ");

  if (!recordUploadBatch(device, *command, *uploaded))
    return blocked("record public #338 Buffer Upload");

  VkMemoryBarrier2 upload_to_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  upload_to_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  upload_to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  upload_to_read.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  upload_to_read.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  if (!recordBarriers(device, *command, {.memory = std::span{&upload_to_read, 1}}))
    return blocked("explicit GPU producer -> readback TRANSFER dependency");

  if (!recordReadbackBatch(device, *command, *readback))
    return blocked("record public #339 Buffer Readback");

  VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  if (!recordBarriers(device, *command, {.memory = std::span{&to_host, 1}}) || !command->end())
    return blocked("explicit readback staging TRANSFER_WRITE -> HOST_READ and end");

  if (!check(
          copy_calls == 3 && barrier_calls == 3 && submit_calls == 0 && idle_calls == 0,
          "exactly one Upload, two Readback Copy2 commands, three explicit barriers, no submit") ||
      !check(observed[0].src == uploaded->stagingBuffer()->nativeBuffer() &&
                 observed[0].dst == gpu_buffer->nativeBuffer() && observed[0].src_offset == 0 &&
                 observed[0].dst_offset == 0 && observed[0].size == payload_size &&
                 observed[1].src == gpu_buffer->nativeBuffer() && observed[1].src_offset == 0 &&
                 observed[1].dst_offset == 0 && observed[1].size == payload_size &&
                 observed[2].src == gpu_buffer->nativeBuffer() &&
                 observed[2].dst == observed[1].dst && observed[2].src_offset == overlap_offset &&
                 observed[2].dst_offset == payload_size && observed[2].size == overlap_size,
             "native Copy2 regions match both public helper descriptions"))
    return EXIT_FAILURE;

  NativeFence fence{static_cast<VkDevice>(device.nativeHandle())};
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(fence.device, &fence_info, nullptr, &fence.fence) != VK_SUCCESS)
    return blocked("native Fence creation");

  std::array<const CommandBuffer *, 1> commands{&*command};
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

  const auto maps_before = map_calls;
  const auto invalidates_before = invalidate_calls;
  auto no_receipt = tryCopyCompletedReadbackBatch(device, *readback, {});
  if (!check(!no_receipt &&
                 no_receipt.error().code() == make_error_code(ReadbackError::missing_completion) &&
                 map_calls == maps_before && invalidate_calls == invalidates_before,
             "Readback refuses host access without genuine completion evidence"))
    return EXIT_FAILURE;

  auto receipt = submit(device, queue, submission);
  if (!receipt) {
    // Native submission may have uncertain GPU outcome: bypass destructors for
    // staging, source Buffer, CommandPool and Fence in this test-only fatal path.
    std::_Exit(blocked("real Queue submission failed or completion evidence unavailable"));
  }
  const std::span<const SubmissionReceipt> uses{&*receipt, 1};

  forced_pending_fence = fence.fence;
  const auto pending = tryCopyCompletedReadbackBatch(device, *readback, uses);
  check(!pending && pending.error().code() == make_error_code(ReadbackError::completion_pending) &&
            map_calls == maps_before && invalidate_calls == invalidates_before &&
            uploaded->valid() && readback->valid(),
        "pending Fence cannot map, invalidate or expose CPU data");
  forced_pending_fence = VK_NULL_HANDLE;

  const auto waited = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!waited || waited->state != SubmissionCompletionState::completed) {
    // A failed/unknown Fence wait cannot authorize GPU resource teardown.
    std::_Exit(blocked("real Fence did not prove all GPU work complete"));
  }

  auto copied = tryCopyCompletedReadbackBatch(device, *readback, uses);
  if (!copied)
    return blocked("materialize completed public #339 CPU-owned result");
  check(copied->buffers().size() == 2 && copied->bytes().size() == payload_size + overlap_size &&
            copied->dataFor(0).size() == payload_size &&
            copied->dataFor(1).size() == overlap_size && copied->buffers()[0].source_offset == 0 &&
            copied->buffers()[1].source_offset == overlap_offset &&
            copied->buffers()[1].data_offset == payload_size,
        "CPU-owned readback metadata matches exact GPU-produced ranges");

  const auto data_full = copied->dataFor(0);
  const auto data_overlapping = copied->dataFor(1);
  check(data_full.size() == expected.size() &&
            std::memcmp(data_full.data(), expected.data(), expected.size()) == 0,
        "256-byte GPU round trip reproduces original bytes after caller input mutation");
  check(data_overlapping.size() == overlap_size &&
            std::memcmp(data_overlapping.data(), expected.data() + overlap_offset, overlap_size) ==
                0,
        "overlapping odd-offset 29-byte readback matches original payload");
  check(map_calls >= maps_before + 1 && flush_calls >= 1 &&
            invalidate_calls >= invalidates_before + 1 && idle_calls == 0 && submit_calls == 1,
        "explicit VMA flush/invalidate, one caller submit and no hidden waitIdle");

  const auto destroyed_before = destroy_buffer_calls;
  RetirementQueue retirement;
  if (!retirement.retire(*uploaded, uses) || !retirement.retire(*readback, uses)) {
    std::_Exit(blocked("retire both completed staging owners via genuine Receipt"));
  }
  check(retirement.pendingCount() == 2 && !uploaded->valid() && !readback->valid() &&
            destroy_buffer_calls == destroyed_before,
        "unique Upload and Readback staging ownership transferred to RetirementQueue");
  auto collected = retirement.collect();
  if (!collected || !retirement.requireEmpty())
    std::_Exit(blocked("collect both completed staging owners"));
  check(collected->collected == 2 && collected->pending == 0 &&
            destroy_buffer_calls == destroyed_before + 2,
        "GPU-complete staging buffers destroyed exactly once");
  check(std::memcmp(copied->dataFor(0).data(), expected.data(), expected.size()) == 0 &&
            std::memcmp(copied->dataFor(1).data(), expected.data() + overlap_offset,
                        overlap_size) == 0,
        "CPU-owned readback outlives Upload and Readback staging");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  bool has_validation = false;
  bool has_debug_utils = false;
  for (const auto &layer : capabilities->available_layers)
    if (layer == "VK_LAYER_KHRONOS_validation")
      has_validation = true;
  for (const auto &extension : capabilities->available_extensions)
    if (extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
      has_debug_utils = true;
  if (has_validation && !has_debug_utils)
    return blocked("Validation Layer available but Debug Utils observation unavailable");

  InstanceDescription instance_description;
  instance_description.api_version = VK_API_VERSION_1_3;
  instance_description.application_name = "terreate-buffer-roundtrip";
  instance_description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  if (has_debug_utils)
    instance_description.debug_utils = DebugUtilsMode::required;
  const auto instance_plan = resolveInstance(instance_description, *capabilities);
  if (!instance_plan)
    return blocked("Vulkan 1.3 Instance resolution");
  if (has_validation) {
    bool layer_enabled = false;
    for (const auto &layer : instance_plan->enabledLayers()) {
      if (layer == "VK_LAYER_KHRONOS_validation")
        layer_enabled = true;
    }
    if (!layer_enabled || !instance_plan->debugUtilsEnabled())
      return blocked("Validation Layer/Debug Utils available but not effectively enabled");
  }

  ValidationRecorder recorder;
  auto instance = createInstance(*instance_plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("headless Vulkan Instance creation");

  if (has_validation) {
    // Exercise the actual Debug Utils -> DiagnosticSinkView path. Without this
    // probe a misspelled category can silently produce a false validation PASS.
    const auto native_instance = static_cast<VkInstance>(instance->nativeHandle());
    const auto emit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
        vkGetInstanceProcAddr(native_instance, "vkSubmitDebugUtilsMessageEXT"));
    if (!emit)
      return blocked("Validation Layer enabled but Debug Utils callback cannot be probed");
    VkDebugUtilsMessengerCallbackDataEXT callback_data{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    callback_data.pMessageIdName = "terreate-buffer-roundtrip-validation-probe";
    callback_data.messageIdNumber = 282;
    callback_data.pMessage = "test-only validation warning sent through Vulkan Debug Utils";
    const auto before_probe = recorder.warnings_or_errors.load(std::memory_order_relaxed);
    emit(native_instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &callback_data);
    if (!check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == before_probe + 1,
               "Vulkan validation warning must reach the application diagnostic recorder"))
      return EXIT_FAILURE;
    // Remove only the synthetic probe. Retain preexisting real warnings/errors.
    recorder.warnings_or_errors.fetch_sub(1, std::memory_order_relaxed);
    std::puts("graphics.buffer_roundtrip: validation-callback-probe=PASS");
  }

  const auto candidates = queryPhysicalDevices(*instance);
  if (!candidates || candidates->empty())
    return blocked("headless PhysicalDevice inventory");

  for (const auto &candidate : *candidates) {
    for (std::size_t index = 0; index < candidate.capabilities().queue_families.size(); ++index) {
      const auto &family = candidate.capabilities().queue_families[index];
      if (!family.queueCount || !(family.queueFlags & vk::QueueFlagBits::eTransfer))
        continue;

      DeviceDescription device_description;
      device_description.required_features_13.synchronization2 = VK_TRUE;
      DeviceQueueRequest request;
      request.caller_id = "buffer-roundtrip";
      request.required_flags = vk::QueueFlagBits::eTransfer;
      request.allowed_family_indices = {static_cast<std::uint32_t>(index)};
      device_description.queue_requests.push_back(request);
      auto plan = resolveDevice(candidate, device_description);
      if (!plan)
        continue;

      int result = EXIT_FAILURE;
      {
        auto device = createDevice(*instance, *plan);
        if (!device)
          return blocked("create synchronization2-enabled logical Device");
        result = exercise(*instance, candidate, *device, device->queue("buffer-roundtrip"));
      }
      if (result != EXIT_SUCCESS)
        return result;
      if (has_validation) {
        if (recorder.warnings_or_errors.load(std::memory_order_relaxed) != 0)
          return blocked("Validation Layer warning/error observed in real GPU round trip");
        std::puts("graphics.buffer_roundtrip: validation-layer=PASS");
      } else {
        std::puts("graphics.buffer_roundtrip: validation-layer=SKIP "
                  "reason=VK_LAYER_KHRONOS_validation unavailable");
      }
      std::puts("graphics.buffer_roundtrip: headless-buffer-roundtrip=PASS");
      return EXIT_SUCCESS;
    }
  }
  return blocked("no Vulkan Transfer Queue with synchronization2 enabled");
}
} // namespace

int main() { return run(); }

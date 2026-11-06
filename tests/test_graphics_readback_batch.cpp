#include <terreate/graphics/readback_batch.hpp>

#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/transfer_commands.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>

namespace {
using namespace terreate::graphics;

int failures = 0;
std::size_t copy_calls = 0;
std::size_t submit_calls = 0;
std::size_t idle_calls = 0;
std::size_t map_calls = 0;
std::size_t invalidate_calls = 0;
std::size_t destroys = 0;
std::optional<VkResult> inject_map_error{};
std::optional<VkResult> inject_invalidate_error{};
std::optional<VkResult> inject_submit_error{};
std::optional<VkResult> inject_poll_error{};
VkFence fake_pending = VK_NULL_HANDLE;

struct Observed {
  VkBuffer src = VK_NULL_HANDLE;
  VkBuffer dst = VK_NULL_HANDLE;
  VkDeviceSize src_offset = 0;
  VkDeviceSize dst_offset = 0;
  VkDeviceSize size = 0;
};
std::array<Observed, 8> observed{};

static_assert(!std::default_initializable<PreparedReadbackBatch>);
static_assert(!std::copy_constructible<PreparedReadbackBatch>);
static_assert(!std::is_copy_assignable_v<PreparedReadbackBatch>);
static_assert(std::is_nothrow_move_constructible_v<PreparedReadbackBatch>);
static_assert(std::is_nothrow_move_assignable_v<PreparedReadbackBatch>);
static_assert(std::is_nothrow_destructible_v<PreparedReadbackBatch>);

bool check(bool value, const char *description) {
  if (!value) {
    ++failures;
    std::fprintf(stderr, "graphics.readback_batch: assertion=FAIL: %s\n", description);
  }
  return value;
}
int blocked(const char *message) {
  std::fprintf(stderr, "graphics.readback_batch: validation=BLOCKED: %s\n", message);
  return EXIT_FAILURE;
}
template <typename T> bool readbackError(const terreate::Result<T> &result, ReadbackError reason) {
  return !result && result.error().code() == make_error_code(reason);
}
template <typename T> bool nativeError(const terreate::Result<T> &result, VkResult code) {
  return !result && result.error().code() == vk::make_error_code(static_cast<vk::Result>(code));
}
struct NativeFence {
  VkDevice device = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  ~NativeFence() {
    if (fence != VK_NULL_HANDLE)
      vkDestroyFence(device, fence, nullptr);
  }
};
} // namespace

// GNU linker wrappers observe real native entry points; no fabricated PASS.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void __real_vkCmdCopyBuffer2(VkCommandBuffer, const VkCopyBufferInfo2 *);
extern "C" VkResult __real_vkQueueSubmit2(VkQueue, std::uint32_t, const VkSubmitInfo2 *, VkFence);
extern "C" VkResult __real_vkGetFenceStatus(VkDevice, VkFence);
extern "C" VkResult __real_vkDeviceWaitIdle(VkDevice);
extern "C" VkResult __real_vmaMapMemory(VmaAllocator, VmaAllocation, void **);
extern "C" VkResult __real_vmaInvalidateAllocation(VmaAllocator, VmaAllocation, VkDeviceSize,
                                                   VkDeviceSize);
extern "C" void __real_vmaDestroyBuffer(VmaAllocator, VkBuffer, VmaAllocation);

extern "C" void __wrap_vkCmdCopyBuffer2(VkCommandBuffer command, const VkCopyBufferInfo2 *info) {
  if (info && info->regionCount == 1 && info->pRegions && copy_calls < observed.size()) {
    const auto &r = *info->pRegions;
    observed[copy_calls] = {info->srcBuffer, info->dstBuffer, r.srcOffset, r.dstOffset, r.size};
  }
  ++copy_calls;
  __real_vkCmdCopyBuffer2(command, info);
}
extern "C" VkResult __wrap_vkQueueSubmit2(VkQueue queue, std::uint32_t count,
                                          const VkSubmitInfo2 *infos, VkFence fence) {
  ++submit_calls;
  if (inject_submit_error) {
    const auto result = *inject_submit_error;
    inject_submit_error.reset();
    return result;
  }
  return __real_vkQueueSubmit2(queue, count, infos, fence);
}
extern "C" VkResult __wrap_vkGetFenceStatus(VkDevice device, VkFence fence) {
  if (inject_poll_error) {
    const auto result = *inject_poll_error;
    inject_poll_error.reset();
    return result;
  }
  if (fake_pending == fence && fence != VK_NULL_HANDLE)
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
  if (inject_map_error) {
    const auto result = *inject_map_error;
    inject_map_error.reset();
    return result;
  }
  return __real_vmaMapMemory(allocator, allocation, data);
}
extern "C" VkResult __wrap_vmaInvalidateAllocation(VmaAllocator allocator, VmaAllocation allocation,
                                                   VkDeviceSize offset, VkDeviceSize size) {
  ++invalidate_calls;
  if (inject_invalidate_error) {
    const auto result = *inject_invalidate_error;
    inject_invalidate_error.reset();
    return result;
  }
  return __real_vmaInvalidateAllocation(allocator, allocation, offset, size);
}
extern "C" void __wrap_vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer,
                                        VmaAllocation allocation) {
  ++destroys;
  __real_vmaDestroyBuffer(allocator, buffer, allocation);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
int exercise(Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             Queue queue) {
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("VMA allocator");

  BufferDescription source_description;
  source_description.size = 96;
  source_description.usage = vk::BufferUsageFlagBits::eTransferSrc;
  source_description.allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  source_description.allocation.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
  auto source_one = createBuffer(*allocator, source_description);
  auto source_two = createBuffer(*allocator, source_description);
  if (!source_one || !source_two)
    return blocked("host-visible source buffers");
  std::array<std::byte, 96> first{};
  std::array<std::byte, 96> second{};
  for (std::size_t i = 0; i < first.size(); ++i) {
    first[i] = static_cast<std::byte>((i * 7U + 11U) & 0xffU);
    second[i] = static_cast<std::byte>((i * 3U + 47U) & 0xffU);
  }
  auto *one_memory = source_one->map().value_or(nullptr);
  auto *two_memory = source_two->map().value_or(nullptr);
  if (!one_memory || !two_memory)
    return blocked("map source buffers");
  std::memcpy(one_memory, first.data(), first.size());
  std::memcpy(two_memory, second.data(), second.size());
  if (!source_one->flush() || !source_two->flush() || !source_one->unmap() || !source_two->unmap())
    return blocked("flush and unmap source buffers");

  const std::array<BufferReadbackItem, 3> reads{
      {{&*source_one, 1, 19}, {&*source_one, 8, 7}, {&*source_two, 3, 5}}};
  const ReadbackBatchDescription description{.buffers = reads};
  check(readbackError(prepareReadbackBatch(*allocator, {}), ReadbackError::empty_batch),
        "empty batch rejected");
  auto bad = reads[0];
  bad.source = nullptr;
  check(readbackError(prepareReadbackBatch(*allocator, {.buffers = std::span{&bad, 1}}),
                      ReadbackError::invalid_source),
        "null source rejected");
  bad = reads[0];
  bad.size = 0;
  check(readbackError(prepareReadbackBatch(*allocator, {.buffers = std::span{&bad, 1}}),
                      ReadbackError::invalid_range),
        "zero byte range rejected");
  bad = reads[0];
  bad.source_offset = 96;
  check(readbackError(prepareReadbackBatch(*allocator, {.buffers = std::span{&bad, 1}}),
                      ReadbackError::invalid_range),
        "out-of-bounds source rejected");
  bad = reads[0];
  bad.size = std::numeric_limits<vk::DeviceSize>::max();
  check(readbackError(prepareReadbackBatch(*allocator, {.buffers = std::span{&bad, 1}}),
                      ReadbackError::invalid_range),
        "overflow source rejected");

  BufferDescription no_usage = source_description;
  no_usage.usage = vk::BufferUsageFlagBits::eTransferDst;
  auto other = createBuffer(*allocator, no_usage);
  if (!other)
    return blocked("negative-case source buffer");
  bad = reads[0];
  bad.source = &*other;
  check(readbackError(prepareReadbackBatch(*allocator, {.buffers = std::span{&bad, 1}}),
                      ReadbackError::missing_usage),
        "TRANSFER_SRC required");

  auto second_allocator = createAllocator(instance, candidate, device);
  if (!second_allocator)
    return blocked("second VMA allocator");
  auto unrelated = createBuffer(*second_allocator, source_description);
  if (!unrelated)
    return blocked("independent allocator source");
  bad = reads[0];
  bad.source = &*unrelated;
  check(readbackError(prepareReadbackBatch(*allocator, {.buffers = std::span{&bad, 1}}),
                      ReadbackError::allocator_mismatch),
        "allocator mismatch rejected");

  ReadbackBatchDescription unsupported = description;
  unsupported.staging_allocation.flags = 0;
  check(readbackError(prepareReadbackBatch(*allocator, unsupported),
                      ReadbackError::unsupported_host_access),
        "AUTO requires explicit host-access flags");

  const auto before_prepare_destroys = destroys;
  const auto before_prepare_submits = submit_calls;
  const auto before_prepare_maps = map_calls;
  auto prepared = prepareReadbackBatch(*allocator, description);
  if (!prepared)
    return blocked("prepare Buffer readback");
  auto allocation = prepared->stagingAllocationInfo();
  check(prepared->valid() && prepared->stagingSize() == 31 && prepared->buffers().size() == 3 &&
            prepared->buffers()[0].source_offset == 1 &&
            prepared->buffers()[0].staging_offset == 0 &&
            prepared->buffers()[1].staging_offset == 19 &&
            prepared->buffers()[2].staging_offset == 26 && prepared->stagingConfiguration() &&
            prepared->stagingConfiguration()->usage == vk::BufferUsageFlagBits::eTransferDst &&
            prepared->requestedStagingAllocation() && allocation &&
            (allocation->memory_properties & vk::MemoryPropertyFlagBits::eHostVisible),
        "packed byte-exact metadata and VMA allocation observed");
  check(submit_calls == before_prepare_submits && map_calls == before_prepare_maps &&
            destroys == before_prepare_destroys,
        "prepare does no submit or host read");

  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  pool_description.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  auto pool = createCommandPool(device, pool_description);
  if (!pool)
    return blocked("command pool");
  auto command = pool->allocatePrimary();
  if (!command)
    return blocked("command buffer");
  check(readbackError(recordReadbackBatch(device, *command, *prepared),
                      ReadbackError::not_recording) &&
            copy_calls == 0,
        "cannot record outside active command buffer");
  if (!command->begin())
    return blocked("begin command recording");

  Buffer moved_source = std::move(*source_two);
  check(readbackError(recordReadbackBatch(device, *command, *prepared),
                      ReadbackError::invalid_source) &&
            copy_calls == 0,
        "whole batch rejected before any native command on moved source");
  *source_two = std::move(moved_source);

  VkMemoryBarrier2 from_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  from_host.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  from_host.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
  from_host.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  from_host.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  if (!recordBarriers(device, *command, BarrierDescription{.memory = std::span{&from_host, 1}}))
    return blocked("explicit HOST_WRITE -> TRANSFER_READ barrier");
  if (!recordReadbackBatch(device, *command, *prepared))
    return blocked("record readback copies");
  check(copy_calls == 3 && submit_calls == before_prepare_submits &&
            observed[0].src == source_one->nativeBuffer() && observed[0].src_offset == 1 &&
            observed[0].dst_offset == 0 && observed[0].size == 19 && observed[1].src_offset == 8 &&
            observed[1].dst_offset == 19 && observed[1].size == 7 &&
            observed[2].src == source_two->nativeBuffer() && observed[2].src_offset == 3 &&
            observed[2].dst_offset == 26 && observed[2].size == 5,
        "native VkBufferCopy2 preserves packed regions");

  VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  if (!recordBarriers(device, *command, BarrierDescription{.memory = std::span{&to_host, 1}}) ||
      !command->end())
    return blocked("explicit TRANSFER_WRITE -> HOST_READ barrier and end");

  NativeFence fence{static_cast<VkDevice>(device.nativeHandle())};
  VkFenceCreateInfo fence_description{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(fence.device, &fence_description, nullptr, &fence.fence) != VK_SUCCESS)
    return blocked("native Fence");
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

  check(readbackError(tryCopyCompletedReadbackBatch(device, *prepared, {}),
                      ReadbackError::missing_completion) &&
            map_calls == before_prepare_maps,
        "no invented completion token or early map");
  inject_submit_error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  const auto failed_submit = submit(device, queue, submission);
  check(nativeError(failed_submit, VK_ERROR_OUT_OF_DEVICE_MEMORY) && prepared->valid(),
        "failed submit retains staging without a forged receipt");
  auto receipt = submit(device, queue, submission);
  if (!receipt)
    return blocked("explicit native submission");
  const std::span<const SubmissionReceipt> uses{&*receipt, 1};
  fake_pending = fence.fence;
  const auto before_poll_maps = map_calls;
  const auto before_poll_invalidate = invalidate_calls;
  check(readbackError(tryCopyCompletedReadbackBatch(device, *prepared, uses),
                      ReadbackError::completion_pending) &&
            map_calls == before_poll_maps && invalidate_calls == before_poll_invalidate,
        "pending completion never maps, invalidates or exposes data");
  inject_poll_error = VK_ERROR_DEVICE_LOST;
  check(nativeError(tryCopyCompletedReadbackBatch(device, *prepared, uses), VK_ERROR_DEVICE_LOST) &&
            prepared->valid() && map_calls == before_poll_maps,
        "native device-lost observation fails closed");
  fake_pending = VK_NULL_HANDLE;

  const auto waited = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!waited || waited->state != SubmissionCompletionState::completed)
    return blocked("real native Fence completion");
  inject_map_error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  check(nativeError(tryCopyCompletedReadbackBatch(device, *prepared, uses),
                    VK_ERROR_OUT_OF_DEVICE_MEMORY) &&
            prepared->valid(),
        "failed host map preserves staging");
  inject_invalidate_error = VK_ERROR_OUT_OF_HOST_MEMORY;
  check(nativeError(tryCopyCompletedReadbackBatch(device, *prepared, uses),
                    VK_ERROR_OUT_OF_HOST_MEMORY) &&
            prepared->valid(),
        "failed invalidate preserves staging and returns no result");

  auto copied = tryCopyCompletedReadbackBatch(device, *prepared, uses);
  if (!copied)
    return blocked("copy completed native readback to CPU");
  check(copied->buffers().size() == 3 && copied->bytes().size() == 31 &&
            copied->dataFor(0).size() == 19 && copied->dataFor(1).size() == 7 &&
            copied->dataFor(2).size() == 5 && copied->dataFor(3).empty() &&
            std::memcmp(copied->dataFor(0).data(), first.data() + 1, 19) == 0 &&
            std::memcmp(copied->dataFor(1).data(), first.data() + 8, 7) == 0 &&
            std::memcmp(copied->dataFor(2).data(), second.data() + 3, 5) == 0 &&
            copied->buffers()[2].source_offset == 3,
        "real GPU readback returns byte-exact, owned, non-4-byte CPU snapshots");
  check(prepared->valid() && destroys == before_prepare_destroys,
        "staging retained until CPU snapshot materialized");

  RetirementQueue retirement;
  if (!retirement.retire(*prepared, uses))
    return blocked("explicit retirement after CPU read");
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  check(!prepared->valid() && retirement.pendingCount() == 1,
        "retirement transfers exclusive staging owner");
  fake_pending = fence.fence;
  auto pending = retirement.collect();
  check(pending && pending->pending == 1 && pending->collected == 0 &&
            destroys == before_prepare_destroys,
        "retirement cannot prematurely destroy");
  fake_pending = VK_NULL_HANDLE;
  auto completed = retirement.collect();
  check(completed && completed->collected == 1 && completed->pending == 0 &&
            retirement.requireEmpty() && destroys == before_prepare_destroys + 1,
        "real completion retires staging exactly once");
  check(std::memcmp(copied->dataFor(0).data(), first.data() + 1, 19) == 0,
        "CPU snapshot survives staging destruction");
  check(idle_calls == 0 && submit_calls == before_prepare_submits + 2 && map_calls >= 4 &&
            invalidate_calls >= 2,
        "no hidden Queue Submit, waitIdle or premature host access");
  if (failures == 0)
    std::puts("graphics.readback_batch: headless-buffer-readback=PASS");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run() {
  const auto caps = queryInstanceCapabilities();
  if (!caps)
    return blocked("Vulkan Instance capabilities");
  InstanceDescription instance_description;
  instance_description.api_version = VK_API_VERSION_1_3;
  const auto instance_plan = resolveInstance(instance_description, *caps);
  if (!instance_plan)
    return blocked("Vulkan 1.3 Instance");
  auto instance = createInstance(*instance_plan);
  if (!instance)
    return blocked("native Vulkan Instance");
  auto candidates = queryPhysicalDevices(*instance);
  if (!candidates || candidates->empty())
    return blocked("physical devices");
  for (const auto &candidate : *candidates) {
    for (std::size_t i = 0; i < candidate.capabilities().queue_families.size(); ++i) {
      const auto &family = candidate.capabilities().queue_families[i];
      if (!family.queueCount || !(family.queueFlags & vk::QueueFlagBits::eTransfer))
        continue;
      DeviceDescription description;
      description.required_features_13.synchronization2 = VK_TRUE;
      DeviceQueueRequest request;
      request.caller_id = "buffer-readback";
      request.required_flags = vk::QueueFlagBits::eTransfer;
      request.allowed_family_indices = {i};
      description.queue_requests.push_back(request);
      auto plan = resolveDevice(candidate, description);
      if (!plan)
        continue;
      auto device = createDevice(*instance, *plan);
      if (!device)
        return blocked("Vulkan Device creation");
      return exercise(*instance, candidate, *device, device->queue("buffer-readback"));
    }
  }
  return blocked("no Vulkan Transfer Queue with synchronization2");
}
} // namespace
int main() { return run(); }

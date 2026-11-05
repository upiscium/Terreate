#include <terreate/graphics/upload_batch.hpp>

#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/submission.hpp>
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
std::size_t cmd_copy_calls = 0;
std::size_t submit_calls = 0;
std::size_t idle_calls = 0;
std::size_t staging_buffer_destroys = 0;
std::size_t map_calls = 0;
std::size_t flush_calls = 0;
std::optional<VkResult> force_map_failure{};
std::optional<VkResult> force_flush_failure{};
std::optional<VkResult> force_submit_failure{};
VkFence force_pending_fence = VK_NULL_HANDLE;

struct ObservedCopy {
  VkBuffer src = VK_NULL_HANDLE;
  VkBuffer dst = VK_NULL_HANDLE;
  VkDeviceSize src_offset = 0;
  VkDeviceSize dst_offset = 0;
  VkDeviceSize size = 0;
};
std::array<ObservedCopy, 12> observed{};

static_assert(!std::default_initializable<PreparedUploadBatch>);
static_assert(!std::copy_constructible<PreparedUploadBatch>);
static_assert(!std::is_copy_assignable_v<PreparedUploadBatch>);
static_assert(std::is_nothrow_move_constructible_v<PreparedUploadBatch>);
static_assert(std::is_nothrow_move_assignable_v<PreparedUploadBatch>);
static_assert(std::is_nothrow_destructible_v<PreparedUploadBatch>);

bool check(bool value, const char *message) {
  if (!value) {
    ++failures;
    std::fprintf(stderr, "graphics.upload_batch: assertion=FAIL: %s\n", message);
  }
  return value;
}
int blocked(const char *message) {
  std::fprintf(stderr, "graphics.upload_batch: validation=BLOCKED: %s\n", message);
  return EXIT_FAILURE;
}
template <typename T> bool hasUploadError(const terreate::Result<T> &result, UploadError error) {
  return !result && result.error().code() == make_error_code(error);
}
template <typename T> bool hasNativeError(const terreate::Result<T> &result, VkResult error) {
  return !result && result.error().code() == vk::make_error_code(static_cast<vk::Result>(error));
}
struct NativeFence {
  VkDevice device = VK_NULL_HANDLE;
  VkFence handle = VK_NULL_HANDLE;
  ~NativeFence() {
    if (handle != VK_NULL_HANDLE)
      vkDestroyFence(device, handle, nullptr);
  }
};
} // namespace

// The GNU linker wrappers observe actual Vulkan and VMA entry points.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void __real_vkCmdCopyBuffer2(VkCommandBuffer, const VkCopyBufferInfo2 *);
extern "C" VkResult __real_vkQueueSubmit2(VkQueue, std::uint32_t, const VkSubmitInfo2 *, VkFence);
extern "C" VkResult __real_vkGetFenceStatus(VkDevice, VkFence);
extern "C" VkResult __real_vkDeviceWaitIdle(VkDevice);
extern "C" VkResult __real_vmaMapMemory(VmaAllocator, VmaAllocation, void **);
extern "C" VkResult __real_vmaFlushAllocation(VmaAllocator, VmaAllocation, VkDeviceSize,
                                              VkDeviceSize);
extern "C" void __real_vmaDestroyBuffer(VmaAllocator, VkBuffer, VmaAllocation);

extern "C" void __wrap_vkCmdCopyBuffer2(VkCommandBuffer command, const VkCopyBufferInfo2 *info) {
  if (cmd_copy_calls < observed.size() && info && info->regionCount == 1 && info->pRegions &&
      info->pRegions->sType == VK_STRUCTURE_TYPE_BUFFER_COPY_2) {
    const auto &r = *info->pRegions;
    observed[cmd_copy_calls] =
        ObservedCopy{info->srcBuffer, info->dstBuffer, r.srcOffset, r.dstOffset, r.size};
  }
  ++cmd_copy_calls;
  __real_vkCmdCopyBuffer2(command, info);
}
extern "C" VkResult __wrap_vkQueueSubmit2(VkQueue queue, std::uint32_t count,
                                          const VkSubmitInfo2 *infos, VkFence fence) {
  ++submit_calls;
  if (force_submit_failure) {
    const auto result = *force_submit_failure;
    force_submit_failure.reset();
    return result;
  }
  return __real_vkQueueSubmit2(queue, count, infos, fence);
}
extern "C" VkResult __wrap_vkGetFenceStatus(VkDevice device, VkFence fence) {
  if (force_pending_fence != VK_NULL_HANDLE && fence == force_pending_fence)
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
  if (force_map_failure) {
    const auto result = *force_map_failure;
    force_map_failure.reset();
    return result;
  }
  return __real_vmaMapMemory(allocator, allocation, data);
}
extern "C" VkResult __wrap_vmaFlushAllocation(VmaAllocator allocator, VmaAllocation allocation,
                                              VkDeviceSize offset, VkDeviceSize size) {
  ++flush_calls;
  if (force_flush_failure) {
    const auto result = *force_flush_failure;
    force_flush_failure.reset();
    return result;
  }
  return __real_vmaFlushAllocation(allocator, allocation, offset, size);
}
extern "C" void __wrap_vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer,
                                        VmaAllocation allocation) {
  ++staging_buffer_destroys;
  __real_vmaDestroyBuffer(allocator, buffer, allocation);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
int exercise(Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             Queue queue) {
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("VMA Allocator");
  BufferDescription target_description;
  target_description.size = 128;
  target_description.usage =
      vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc;
  auto target_one = createBuffer(*allocator, target_description);
  auto target_two = createBuffer(*allocator, target_description);
  BufferDescription readback_description;
  readback_description.size = 128;
  readback_description.usage = vk::BufferUsageFlagBits::eTransferDst;
  readback_description.allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  readback_description.allocation.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
  auto readback = createBuffer(*allocator, readback_description);
  if (!target_one || !target_two || !readback)
    return blocked("target/readback VMA buffers");

  std::array<std::byte, 32> first{};
  std::array<std::byte, 16> second{};
  std::array<std::byte, 32> third{};
  for (std::size_t i = 0; i < first.size(); ++i)
    first[i] = static_cast<std::byte>((11U + i * 7U) & 0xffU);
  for (std::size_t i = 0; i < second.size(); ++i)
    second[i] = static_cast<std::byte>((31U + i * 5U) & 0xffU);
  for (std::size_t i = 0; i < third.size(); ++i)
    third[i] = static_cast<std::byte>((73U + i * 3U) & 0xffU);
  const auto expected_first = first;
  const auto expected_second = second;
  const auto expected_third = third;

  const std::array<BufferUploadItem, 3> writes{{
      {&*target_one, 0, first},
      {&*target_one, 64, second},
      {&*target_two, 0, third},
  }};
  UploadBatchDescription request;
  request.buffers = writes;

  // All deterministic validation failures must precede VMA staging allocation.
  const auto empty = prepareUploadBatch(*allocator, UploadBatchDescription{});
  check(hasUploadError(empty, UploadError::empty_batch), "empty upload batch must fail");
  auto bad = writes[0];
  bad.destination = nullptr;
  check(hasUploadError(
            prepareUploadBatch(*allocator, UploadBatchDescription{.buffers = std::span{&bad, 1}}),
            UploadError::invalid_destination),
        "null target rejected");
  bad = writes[0];
  bad.destination_offset = 1;
  check(hasUploadError(
            prepareUploadBatch(*allocator, UploadBatchDescription{.buffers = std::span{&bad, 1}}),
            UploadError::unaligned_copy),
        "unaligned destination rejected");
  bad = writes[0];
  bad.bytes = std::span{first.data(), std::size_t{3}};
  check(hasUploadError(
            prepareUploadBatch(*allocator, UploadBatchDescription{.buffers = std::span{&bad, 1}}),
            UploadError::unaligned_copy),
        "unaligned copy length rejected");
  bad = writes[0];
  bad.destination_offset = 120;
  check(hasUploadError(
            prepareUploadBatch(*allocator, UploadBatchDescription{.buffers = std::span{&bad, 1}}),
            UploadError::invalid_range),
        "range overflow rejected");
  std::array<BufferUploadItem, 2> overlapping{writes[0], writes[1]};
  overlapping[1].destination_offset = 16;
  check(
      hasUploadError(prepareUploadBatch(*allocator, UploadBatchDescription{.buffers = overlapping}),
                     UploadError::overlapping_writes),
      "overlapping writes to one target rejected");
  // Staging is explicitly host-writable; missing TRANSFER_DST must fail.
  BufferDescription wrong_usage;
  wrong_usage.size = 128;
  wrong_usage.usage = vk::BufferUsageFlagBits::eTransferSrc;
  auto not_dst = createBuffer(*allocator, wrong_usage);
  if (!not_dst)
    return blocked("create negative-case Buffer");
  bad = writes[0];
  bad.destination = &*not_dst;
  check(hasUploadError(
            prepareUploadBatch(*allocator, UploadBatchDescription{.buffers = std::span{&bad, 1}}),
            UploadError::missing_usage),
        "destination missing TRANSFER_DST rejected");

  const auto before_map_failure = staging_buffer_destroys;
  force_map_failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  auto map_failed = prepareUploadBatch(*allocator, request);
  check(hasNativeError(map_failed, VK_ERROR_OUT_OF_DEVICE_MEMORY) && !force_map_failure &&
            staging_buffer_destroys == before_map_failure + 1,
        "map native failure retains error and frees staged Buffer");
  const auto before_flush_failure = staging_buffer_destroys;
  force_flush_failure = VK_ERROR_OUT_OF_HOST_MEMORY;
  auto flush_failed = prepareUploadBatch(*allocator, request);
  check(hasNativeError(flush_failed, VK_ERROR_OUT_OF_HOST_MEMORY) && !force_flush_failure &&
            staging_buffer_destroys == before_flush_failure + 1,
        "flush native failure retains error and frees staged Buffer");

  auto extra_allocator = createAllocator(instance, candidate, device);
  if (!extra_allocator)
    return blocked("create independent Allocator for mismatch test");
  auto other_allocator_target = createBuffer(*extra_allocator, target_description);
  if (!other_allocator_target)
    return blocked("create mismatched Allocator target");
  bad = writes[0];
  bad.destination = &*other_allocator_target;
  check(hasUploadError(
            prepareUploadBatch(*allocator, UploadBatchDescription{.buffers = std::span{&bad, 1}}),
            UploadError::allocator_mismatch),
        "different Allocator target must be rejected by bounded convenience");

  const auto destroys_before_prepare = staging_buffer_destroys;
  const auto submits_before_prepare = submit_calls;
  auto prepared = prepareUploadBatch(*allocator, request);
  if (!prepared)
    return blocked("prepare real Buffer upload batch");
  check(prepared->valid() && prepared->stagingBuffer() && prepared->copies().size() == 3 &&
            prepared->stagingBuffer()->configuration() &&
            prepared->stagingBuffer()->configuration()->size == 80 &&
            prepared->requestedStagingAllocation() && prepared->copies()[0].staging_offset == 0 &&
            prepared->copies()[1].staging_offset == 32 &&
            prepared->copies()[2].staging_offset == 48 &&
            prepared->copies()[1].destination_offset == 64 && prepared->copies()[2].size == 32,
        "staging packing and requested/effective metadata");
  const auto actual_allocation = prepared->stagingBuffer()->allocationInfo();
  check(actual_allocation &&
            (actual_allocation->memory_properties & vk::MemoryPropertyFlagBits::eHostVisible),
        "actual VMA staging allocation must be host visible");
  check(staging_buffer_destroys == destroys_before_prepare &&
            submit_calls == submits_before_prepare,
        "prepare retains staging without native submission");

  // Source spans may be modified immediately after preparation; GPU must see
  // the snapshotted original bytes, not the mutated input arrays.
  first.fill(std::byte{0});
  second.fill(std::byte{0});
  third.fill(std::byte{0});

  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  pool_description.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  auto pool = createCommandPool(device, pool_description);
  if (!pool)
    return blocked("create CommandPool");
  auto command = pool->allocatePrimary();
  if (!command)
    return blocked("allocate CommandBuffer");
  check(
      hasUploadError(recordUploadBatch(device, *command, *prepared), UploadError::not_recording) &&
          cmd_copy_calls == 0,
      "no recording outside an active CommandBuffer");
  if (!command->begin())
    return blocked("begin recording");

  // Invalidation after prepare must reject the whole batch before any vkCmd.
  Buffer moved_target = std::move(*target_two);
  const auto wrong_record = recordUploadBatch(device, *command, *prepared);
  check(hasUploadError(wrong_record, UploadError::invalid_destination) && cmd_copy_calls == 0,
        "prevalidation prevents partial native copy recording");
  *target_two = std::move(moved_target);

  if (!recordUploadBatch(device, *command, *prepared))
    return blocked("record complete upload batch");
  check(cmd_copy_calls == 3 && submit_calls == submits_before_prepare,
        "record emits exactly three Copy2 calls but no submit");
  const auto *stage = prepared->stagingBuffer();
  check(observed[0].src == stage->nativeBuffer() && observed[0].dst == target_one->nativeBuffer() &&
            observed[0].src_offset == 0 && observed[0].dst_offset == 0 && observed[0].size == 32 &&
            observed[1].src == stage->nativeBuffer() && observed[1].src_offset == 32 &&
            observed[1].dst_offset == 64 && observed[1].size == 16 &&
            observed[2].dst == target_two->nativeBuffer() && observed[2].src_offset == 48 &&
            observed[2].size == 32,
        "native Copy2 arguments preserve explicit packing");

  VkMemoryBarrier2 upload_to_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  upload_to_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  upload_to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  upload_to_read.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  upload_to_read.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  BarrierDescription intermediate;
  intermediate.memory = std::span{&upload_to_read, 1};
  if (!recordBarriers(device, *command, intermediate))
    return blocked("record explicit transfer-read barrier");

  std::array<VkBufferCopy2, 2> readback_regions{
      VkBufferCopy2{VK_STRUCTURE_TYPE_BUFFER_COPY_2, nullptr, 0, 0, 32},
      VkBufferCopy2{VK_STRUCTURE_TYPE_BUFFER_COPY_2, nullptr, 64, 64, 16},
  };
  if (!recordCopyBuffer(device, *command, *target_one, *readback, readback_regions))
    return blocked("record readback of first target");
  const VkBufferCopy2 from_second{VK_STRUCTURE_TYPE_BUFFER_COPY_2, nullptr, 0, 96, 32};
  if (!recordCopyBuffer(device, *command, *target_two, *readback, std::span{&from_second, 1}))
    return blocked("record readback of second target");

  VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  BarrierDescription ending;
  ending.memory = std::span{&to_host, 1};
  if (!recordBarriers(device, *command, ending) || !command->end())
    return blocked("record host-read barrier and finish recording");

  NativeFence fence{static_cast<VkDevice>(device.nativeHandle())};
  VkFenceCreateInfo fence_create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(fence.device, &fence_create, nullptr, &fence.handle) != VK_SUCCESS)
    return blocked("create native Fence");
  std::array<const CommandBuffer *, 1> commands{&*command};
  int identity_domain = 0;
  CompletionDescription completion;
  completion.device = &device;
  completion.queue = queue;
  completion.requested = {&identity_domain, 1};
  completion.effective = {&identity_domain, 1};
  completion.condition = FenceCompletionCondition{vk::Fence{fence.handle}};
  SubmissionDescription submitted;
  submitted.command_buffers = commands;
  submitted.fence = vk::Fence{fence.handle};
  submitted.completion = completion;

  force_submit_failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  const auto failed_submit = submit(device, queue, submitted);
  check(hasNativeError(failed_submit, VK_ERROR_OUT_OF_DEVICE_MEMORY) && !force_submit_failure &&
            prepared->valid() && staging_buffer_destroys == destroys_before_prepare,
        "failed submit has no receipt and retains caller-owned staging");
  auto receipt = submit(device, queue, submitted);
  if (!receipt)
    return blocked("real native queue submission");
  check(submit_calls == submits_before_prepare + 2 && cmd_copy_calls == 5 && prepared->valid(),
        "only application invokes submit, source retains staging");

  RetirementQueue retirement;
  const std::span<const SubmissionReceipt> all_uses{&*receipt, 1};
  if (!retirement.retire(*prepared, all_uses))
    return blocked("retire staging after successful native submission");
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  check(!prepared->valid() && !prepared->stagingBuffer() && retirement.pendingCount() == 1 &&
            staging_buffer_destroys == destroys_before_prepare,
        "retirement transfers the unique staging owner without destruction");

  force_pending_fence = fence.handle;
  const auto pending = retirement.collect();
  check(pending && pending->pending == 1 && pending->collected == 0 &&
            staging_buffer_destroys == destroys_before_prepare,
        "native NOT_READY cannot destroy GPU staging");
  force_pending_fence = VK_NULL_HANDLE;

  const auto observed_completion = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!observed_completion || observed_completion->state != SubmissionCompletionState::completed)
    return blocked("real native GPU fence completion");
  const auto collected = retirement.collect();
  check(collected && collected->pending == 0 && collected->collected == 1 &&
            retirement.requireEmpty() && staging_buffer_destroys == destroys_before_prepare + 1,
        "completion collects staging exactly once");

  auto host_memory = readback->map();
  if (!host_memory)
    return blocked("map GPU readback Buffer");
  if (!readback->invalidate())
    return blocked("invalidate GPU readback Buffer");
  auto *bytes = static_cast<const std::byte *>(*host_memory);
  check(std::memcmp(bytes, expected_first.data(), expected_first.size()) == 0,
        "first GPU buffer upload preserved source snapshot");
  check(std::memcmp(bytes + 64, expected_second.data(), expected_second.size()) == 0,
        "second GPU buffer range upload preserved snapshot");
  check(std::memcmp(bytes + 96, expected_third.data(), expected_third.size()) == 0,
        "second destination Buffer upload preserved snapshot");
  if (!readback->unmap())
    return blocked("unmap readback Buffer");
  check(idle_calls == 0, "no hidden vkDeviceWaitIdle in prepare, record, submit or retire");
  check(map_calls >= 4 && flush_calls >= 2, "map/flush actually delegated to native VMA");

  if (failures == 0)
    std::puts("graphics.upload_batch: headless-buffer-upload=PASS");
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
    return blocked("Vulkan 1.3 Instance resolution");
  auto instance = createInstance(*instance_plan);
  if (!instance)
    return blocked("Vulkan Instance creation");
  const auto candidates = queryPhysicalDevices(*instance);
  if (!candidates || candidates->empty())
    return blocked("PhysicalDevice inventory");
  for (const auto &candidate : *candidates) {
    for (std::size_t i = 0; i < candidate.capabilities().queue_families.size(); ++i) {
      const auto &family = candidate.capabilities().queue_families[i];
      if (!family.queueCount || !(family.queueFlags & vk::QueueFlagBits::eTransfer))
        continue;
      DeviceDescription description;
      description.required_features_13.synchronization2 = VK_TRUE;
      DeviceQueueRequest request;
      request.caller_id = "buffer-upload";
      request.required_flags = vk::QueueFlagBits::eTransfer;
      request.allowed_family_indices = {i};
      description.queue_requests.push_back(request);
      auto plan = resolveDevice(candidate, description);
      if (!plan)
        continue;
      auto device = createDevice(*instance, *plan);
      if (!device)
        return blocked("native Device creation");
      return exercise(*instance, candidate, *device, device->queue("buffer-upload"));
    }
  }
  return blocked("no transfer/synchronization2-enabled Vulkan Device");
}
} // namespace

int main() { return run(); }

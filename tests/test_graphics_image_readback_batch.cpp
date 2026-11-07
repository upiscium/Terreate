#include <terreate/graphics/readback_batch.hpp>

#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/transfer_commands.hpp>
#include <terreate/graphics/upload_batch.hpp>

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
std::size_t upload_image_calls = 0;
std::size_t readback_image_calls = 0;
std::size_t readback_buffer_calls = 0;
std::size_t barrier_calls = 0;
std::size_t submit_calls = 0;
std::size_t idle_calls = 0;
std::size_t destroyed_buffers = 0;
std::size_t mapped_calls = 0;
std::size_t invalidated_calls = 0;
VkFence force_pending = VK_NULL_HANDLE;
std::optional<VkResult> force_map_error{};
std::optional<VkResult> force_invalidate_error{};
std::optional<VkResult> force_poll_error{};

struct ImageReadbackObserved {
  VkImage image = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
};
std::array<ImageReadbackObserved, 4> image_observed{};

struct BufferReadbackObserved {
  VkBuffer source = VK_NULL_HANDLE;
  VkBuffer target = VK_NULL_HANDLE;
  VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
};
std::array<BufferReadbackObserved, 4> buffer_observed{};

static_assert(!std::default_initializable<PreparedReadbackBatch>);
static_assert(!std::copy_constructible<PreparedReadbackBatch>);
static_assert(!std::is_copy_assignable_v<PreparedReadbackBatch>);
static_assert(std::is_nothrow_move_constructible_v<PreparedReadbackBatch>);
static_assert(std::is_nothrow_move_assignable_v<PreparedReadbackBatch>);
static_assert(std::is_nothrow_destructible_v<PreparedReadbackBatch>);
static_assert(std::is_aggregate_v<ImageReadbackItem>);
static_assert(
    std::is_same_v<decltype(std::declval<const CompletedReadbackBatch &>().imageDataFor(0)),
                   std::span<const std::byte>>);

bool check(bool test, const char *description) {
  if (!test) {
    ++failures;
    std::fprintf(stderr, "graphics.image_readback_batch: assertion=FAIL: %s\n", description);
  }
  return test;
}
int blocked(const char *reason) {
  std::fprintf(stderr, "graphics.image_readback_batch: validation=BLOCKED: %s\n", reason);
  return EXIT_FAILURE;
}
template <typename T> bool hasReadbackError(const terreate::Result<T> &value, ReadbackError error) {
  return !value && value.error().code() == make_error_code(error);
}
template <typename T> bool hasNativeError(const terreate::Result<T> &value, VkResult code) {
  return !value && value.error().code() == vk::make_error_code(static_cast<vk::Result>(code));
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

// GNU linker wrappers observe real native calls. Inject only explicit negative cases.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void __real_vkCmdCopyBufferToImage2(VkCommandBuffer, const VkCopyBufferToImageInfo2 *);
extern "C" void __real_vkCmdCopyImageToBuffer2(VkCommandBuffer, const VkCopyImageToBufferInfo2 *);
extern "C" void __real_vkCmdCopyBuffer2(VkCommandBuffer, const VkCopyBufferInfo2 *);
extern "C" void __real_vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo *);
extern "C" VkResult __real_vkQueueSubmit2(VkQueue, std::uint32_t, const VkSubmitInfo2 *, VkFence);
extern "C" VkResult __real_vkGetFenceStatus(VkDevice, VkFence);
extern "C" VkResult __real_vkDeviceWaitIdle(VkDevice);
extern "C" VkResult __real_vmaMapMemory(VmaAllocator, VmaAllocation, void **);
extern "C" VkResult __real_vmaInvalidateAllocation(VmaAllocator, VmaAllocation, VkDeviceSize,
                                                   VkDeviceSize);
extern "C" void __real_vmaDestroyBuffer(VmaAllocator, VkBuffer, VmaAllocation);

extern "C" void __wrap_vkCmdCopyBufferToImage2(VkCommandBuffer command,
                                               const VkCopyBufferToImageInfo2 *info) {
  ++upload_image_calls;
  __real_vkCmdCopyBufferToImage2(command, info);
}
extern "C" void __wrap_vkCmdCopyImageToBuffer2(VkCommandBuffer command,
                                               const VkCopyImageToBufferInfo2 *info) {
  if (readback_image_calls < image_observed.size() && info && info->regionCount == 1 &&
      info->pRegions) {
    image_observed[readback_image_calls] = ImageReadbackObserved{
        info->srcImage, info->dstBuffer, info->srcImageLayout, *info->pRegions};
  }
  ++readback_image_calls;
  __real_vkCmdCopyImageToBuffer2(command, info);
}
extern "C" void __wrap_vkCmdCopyBuffer2(VkCommandBuffer command, const VkCopyBufferInfo2 *info) {
  if (readback_buffer_calls < buffer_observed.size() && info && info->regionCount == 1 &&
      info->pRegions) {
    buffer_observed[readback_buffer_calls] =
        BufferReadbackObserved{info->srcBuffer, info->dstBuffer, *info->pRegions};
  }
  ++readback_buffer_calls;
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
  if (force_poll_error) {
    const auto result = *force_poll_error;
    force_poll_error.reset();
    return result;
  }
  if (fence != VK_NULL_HANDLE && fence == force_pending)
    return VK_NOT_READY;
  return __real_vkGetFenceStatus(device, fence);
}
extern "C" VkResult __wrap_vkDeviceWaitIdle(VkDevice device) {
  ++idle_calls;
  return __real_vkDeviceWaitIdle(device);
}
extern "C" VkResult __wrap_vmaMapMemory(VmaAllocator allocator, VmaAllocation allocation,
                                        void **mapped) {
  ++mapped_calls;
  if (force_map_error) {
    const auto result = *force_map_error;
    force_map_error.reset();
    return result;
  }
  return __real_vmaMapMemory(allocator, allocation, mapped);
}
extern "C" VkResult __wrap_vmaInvalidateAllocation(VmaAllocator allocator, VmaAllocation allocation,
                                                   VkDeviceSize offset, VkDeviceSize size) {
  ++invalidated_calls;
  if (force_invalidate_error) {
    const auto result = *force_invalidate_error;
    force_invalidate_error.reset();
    return result;
  }
  return __real_vmaInvalidateAllocation(allocator, allocation, offset, size);
}
extern "C" void __wrap_vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer,
                                        VmaAllocation allocation) {
  ++destroyed_buffers;
  __real_vmaDestroyBuffer(allocator, buffer, allocation);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
int exercise(Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             Queue queue) {
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("VMA Allocator");
  ImageDescription image_description;
  image_description.format = vk::Format::eR8G8B8A8Unorm;
  image_description.dimensions = vk::ImageType::e2D;
  image_description.extent = vk::Extent3D{4, 4, 1};
  image_description.usage =
      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
  auto image = createImage(*allocator, image_description);
  if (!image)
    return blocked("RGBA8 Image");

  BufferDescription source_description;
  source_description.size = 16;
  source_description.usage = vk::BufferUsageFlagBits::eTransferSrc;
  source_description.allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  source_description.allocation.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
  auto host_source = createBuffer(*allocator, source_description);
  if (!host_source)
    return blocked("host-visible Buffer source");

  std::array<std::byte, 64> expected_pixels{};
  std::array<std::byte, 16> expected_buffer{};
  for (std::size_t i = 0; i < expected_pixels.size(); ++i)
    expected_pixels[i] = static_cast<std::byte>((17U * i + 9U) & 255U);
  for (std::size_t i = 0; i < expected_buffer.size(); ++i)
    expected_buffer[i] = static_cast<std::byte>((5U * i + 31U) & 255U);

  auto host_mapped = host_source->map();
  if (!host_mapped)
    return blocked("map Buffer source");
  std::memcpy(*host_mapped, expected_buffer.data(), expected_buffer.size());
  if (!host_source->flush() || !host_source->unmap())
    return blocked("flush/unmap Buffer source");

  VkBufferImageCopy2 upload_region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
  upload_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  upload_region.imageExtent = {4, 4, 1};
  ImageUploadItem upload_image{&*image, vk::ImageLayout::eTransferDstOptimal, upload_region,
                               expected_pixels};
  UploadBatchDescription upload_description;
  upload_description.images = std::span{&upload_image, 1};
  auto upload = prepareUploadBatch(*allocator, upload_description);
  if (!upload)
    return blocked("prepare genuine Image Upload source");

  VkBufferImageCopy2 padded{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
  padded.bufferRowLength = 6;   // 6 texels -> 24 bytes per row.
  padded.bufferImageHeight = 4; // One layer/depth 1: no slice stride.
  padded.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  padded.imageExtent = {4, 4, 1};
  VkBufferImageCopy2 tight = padded;
  tight.bufferRowLength = 0;
  tight.bufferImageHeight = 0;

  std::array<ImageReadbackItem, 2> image_reads{{
      ImageReadbackItem{&*image, vk::ImageLayout::eTransferSrcOptimal, padded},
      ImageReadbackItem{&*image, vk::ImageLayout::eTransferSrcOptimal, tight},
  }};
  BufferReadbackItem buffer_read{&*host_source, 1, 5};
  ReadbackBatchDescription description;
  description.buffers = std::span{&buffer_read, 1};
  description.images = image_reads;

  auto bad = image_reads[0];
  bad.source = nullptr;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_source),
        "null Image rejected");
  bad = image_reads[0];
  bad.copy_layout = vk::ImageLayout::eUndefined;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::unsupported_layout),
        "invalid Image layout rejected");
  bad = image_reads[0];
  bad.region.bufferOffset = 4;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "request staging offset must be 0");
  bad = image_reads[0];
  bad.region.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "wrong native sType rejected");
  bad = image_reads[0];
  bad.region.pNext = &bad.region;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "pNext extension rejected");
  bad = image_reads[0];
  bad.region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "non-Color aspect rejected");
  bad = image_reads[0];
  bad.region.imageSubresource.mipLevel = 1;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "invalid mip level rejected");
  bad = image_reads[0];
  bad.region.imageSubresource.layerCount = 2;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "multiple Image layers rejected");
  bad = image_reads[0];
  bad.region.imageOffset.x = -1;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "negative Image offset rejected");
  bad = image_reads[0];
  bad.region.imageExtent.width = 5;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "Image extent outside mip rejected");
  bad = image_reads[0];
  bad.region.bufferRowLength = 3;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "row pitch smaller than width rejected");
  bad = image_reads[0];
  bad.region.bufferImageHeight = 3;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "buffer image height too small rejected");
  bad = image_reads[0];
  bad.region.bufferRowLength = std::numeric_limits<std::uint32_t>::max();
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::invalid_image_region),
        "oversized signed-31-bit row stride rejected");

  auto conflict = image_reads;
  conflict[1].copy_layout = vk::ImageLayout::eGeneral;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = conflict}),
                         ReadbackError::conflicting_image_layouts),
        "same Image mip/layer rejects conflicting layouts");

  ImageDescription wrong_usage_description = image_description;
  wrong_usage_description.usage = vk::ImageUsageFlagBits::eTransferDst;
  auto wrong_usage_image = createImage(*allocator, wrong_usage_description);
  if (!wrong_usage_image)
    return blocked("create missing TRANSFER_SRC Image fixture");
  bad = image_reads[0];
  bad.source = &*wrong_usage_image;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::missing_usage),
        "missing TRANSFER_SRC rejected");

  ImageDescription wrong_format_description = image_description;
  wrong_format_description.format = vk::Format::eR8Unorm;
  auto wrong_format_image = createImage(*allocator, wrong_format_description);
  if (!wrong_format_image)
    return blocked("create unsupported R8 Image fixture");
  bad = image_reads[0];
  bad.source = &*wrong_format_image;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::unsupported_image),
        "unsupported Image format rejected");

  auto extra_allocator = createAllocator(instance, candidate, device);
  if (!extra_allocator)
    return blocked("create independent VMA Allocator");
  auto foreign_image = createImage(*extra_allocator, image_description);
  if (!foreign_image)
    return blocked("create Image with independent VMA Allocator");
  bad = image_reads[0];
  bad.source = &*foreign_image;
  check(hasReadbackError(prepareReadbackBatch(*allocator, {.images = std::span{&bad, 1}}),
                         ReadbackError::allocator_mismatch),
        "unrelated Allocator rejected");

  ReadbackBatchDescription no_host = description;
  no_host.staging_allocation.flags = 0;
  check(hasReadbackError(prepareReadbackBatch(*allocator, no_host),
                         ReadbackError::unsupported_host_access),
        "AUTO staging without HOST_ACCESS rejected");

  {
    auto only_image =
        prepareReadbackBatch(*allocator, {.images = std::span{image_reads.data(), std::size_t{1}}});
    check(only_image && only_image->valid() && only_image->buffers().empty() &&
              only_image->images().size() == 1 && only_image->stagingSize() == 88,
          "Image-only batch is valid with exact touched footprint");
  }

  const auto destroys_before = destroyed_buffers;
  auto prepared = prepareReadbackBatch(*allocator, description);
  if (!prepared)
    return blocked("prepare mixed Buffer+Image Readback");
  const auto stage_info = prepared->stagingAllocationInfo();
  check(prepared->valid() && prepared->buffers().size() == 1 && prepared->images().size() == 2 &&
            prepared->stagingSize() == 160 && prepared->buffers()[0].staging_offset == 0 &&
            prepared->images()[0].region.bufferOffset == 8 &&
            prepared->images()[0].payload_bytes == 88 &&
            prepared->images()[1].region.bufferOffset == 96 &&
            prepared->images()[1].payload_bytes == 64 && prepared->requestedStagingAllocation() &&
            prepared->stagingConfiguration() && prepared->stagingConfiguration()->size == 160 &&
            stage_info &&
            (stage_info->memory_properties & vk::MemoryPropertyFlagBits::eHostVisible),
        "mixed staging packs odd Buffer read and two RGBA8 footprints correctly");
  check(submit_calls == 0 && readback_image_calls == 0 && readback_buffer_calls == 0,
        "prepare performs no GPU recording or hidden submit");

  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  pool_description.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  auto pool = createCommandPool(device, pool_description);
  if (!pool)
    return blocked("create native CommandPool");
  auto command = pool->allocatePrimary();
  if (!command)
    return blocked("allocate native CommandBuffer");
  check(hasReadbackError(recordReadbackBatch(device, *command, *prepared),
                         ReadbackError::not_recording),
        "caller controls recording lifecycle");
  if (!command->begin())
    return blocked("begin CommandBuffer");

  Image moved_image = std::move(*image);
  check(hasReadbackError(recordReadbackBatch(device, *command, *prepared),
                         ReadbackError::invalid_source) &&
            readback_image_calls == 0 && readback_buffer_calls == 0,
        "moved Image rejects whole mixed batch before native Buffer copy");
  *image = std::move(moved_image);

  VkImageMemoryBarrier2 initialize{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  initialize.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  initialize.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  initialize.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  initialize.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  initialize.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  initialize.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  initialize.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  initialize.image = image->nativeImage();
  initialize.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkMemoryBarrier2 from_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  from_host.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  from_host.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
  from_host.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  from_host.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  if (!recordBarriers(device, *command,
                      {.memory = std::span{&from_host, 1}, .images = std::span{&initialize, 1}}))
    return blocked("record caller-owned host-read dependency and initial Image layout");

  if (!recordUploadBatch(device, *command, *upload))
    return blocked("record real RGBA8 Image Upload");
  VkImageMemoryBarrier2 transition = initialize;
  transition.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  transition.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  transition.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  transition.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  transition.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  transition.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  if (!recordBarriers(device, *command, {.images = std::span{&transition, 1}}))
    return blocked("explicit Image transition to TRANSFER_SRC_OPTIMAL");

  if (!recordReadbackBatch(device, *command, *prepared))
    return blocked("record real mixed Buffer+Image Readback");
  check(upload_image_calls == 1 && readback_image_calls == 2 && readback_buffer_calls == 1 &&
            barrier_calls == 2 && submit_calls == 0,
        "upload and readback produce precise native Copy2 and explicit barriers");
  check(image_observed[0].image == image->nativeImage() &&
            image_observed[0].layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            image_observed[0].region.bufferOffset == 8 &&
            image_observed[0].region.bufferRowLength == 6 &&
            image_observed[0].region.bufferImageHeight == 4 &&
            image_observed[1].region.bufferOffset == 96 &&
            image_observed[1].region.bufferRowLength == 0 &&
            buffer_observed[0].source == host_source->nativeBuffer() &&
            buffer_observed[0].region.srcOffset == 1 && buffer_observed[0].region.dstOffset == 0 &&
            buffer_observed[0].region.size == 5,
        "Image and Buffer native region snapshots match effective staging positions");

  VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  if (!recordBarriers(device, *command, {.memory = std::span{&to_host, 1}}) || !command->end())
    return blocked("record explicit transfer-write-to-host barrier and end");

  NativeFence fence{static_cast<VkDevice>(device.nativeHandle())};
  VkFenceCreateInfo native_fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(fence.device, &native_fence, nullptr, &fence.fence) != VK_SUCCESS)
    return blocked("create native Fence");
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
  check(hasReadbackError(tryCopyCompletedReadbackBatch(device, *prepared, {}),
                         ReadbackError::missing_completion),
        "empty receipt set never exposes GPU data");
  auto receipt = submit(device, queue, submission);
  if (!receipt) {
    // Without a successful native receipt, the submit outcome may be
    // uncertain (in particular on Device Loss). Do not unwind GPU owners or
    // the Fence until their safety is known. In this test-only fatal path,
    // exit with BLOCKED/FAILURE without invoking Vulkan resource destructors.
    std::_Exit(blocked("real native Queue submission"));
  }
  const std::span<const SubmissionReceipt> uses{&*receipt, 1};

  // Keep both exclusive staging owners with the test until GPU completion
  // and CPU materialization. Early failure can then return BLOCKED cleanly
  // rather than unwinding a nonempty fail-fast RetirementQueue.
  const auto host_maps_before = mapped_calls;
  const auto invalidated_before = invalidated_calls;
  force_pending = fence.fence;
  check(hasReadbackError(tryCopyCompletedReadbackBatch(device, *prepared, uses),
                         ReadbackError::completion_pending) &&
            mapped_calls == host_maps_before && invalidated_calls == invalidated_before &&
            prepared->valid(),
        "pending native Fence refuses map/invalidate/CPU read");
  force_poll_error = VK_ERROR_DEVICE_LOST;
  check(hasNativeError(tryCopyCompletedReadbackBatch(device, *prepared, uses),
                       VK_ERROR_DEVICE_LOST) &&
            prepared->valid() && mapped_calls == host_maps_before,
        "native device-loss observation fails closed without changing ownership");
  force_pending = VK_NULL_HANDLE;

  const auto waited = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!waited || waited->state != SubmissionCompletionState::completed) {
    // Native submission succeeded but completion is unresolved. Returning
    // would unwind both staging owners and the Fence while GPU use might still
    // be in flight. This test-only fatal path must not run their destructors:
    // abandon the process with BLOCKED/FAILURE instead of invalid Vulkan teardown.
    std::_Exit(blocked("real GPU Fence completion"));
  }
  force_map_error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  check(hasNativeError(tryCopyCompletedReadbackBatch(device, *prepared, uses),
                       VK_ERROR_OUT_OF_DEVICE_MEMORY) &&
            prepared->valid(),
        "VMA map failure keeps exclusive staging owner");
  force_invalidate_error = VK_ERROR_OUT_OF_HOST_MEMORY;
  check(hasNativeError(tryCopyCompletedReadbackBatch(device, *prepared, uses),
                       VK_ERROR_OUT_OF_HOST_MEMORY) &&
            prepared->valid(),
        "VMA invalidate failure returns no partial Image result");

  auto copied = tryCopyCompletedReadbackBatch(device, *prepared, uses);
  if (!copied)
    return blocked("copy completed Image and Buffer into CPU-owned snapshot");
  const auto image_result = copied->imageDataFor(0);
  const auto tight_result = copied->imageDataFor(1);
  if (copied->buffers().size() != 1 || copied->images().size() != 2 ||
      copied->bytes().size() != 160 || copied->dataFor(0).size() != 5 ||
      image_result.size() != 88 || tight_result.size() != 64)
    return blocked("incomplete or incorrect CPU result size/metadata");
  check(copied->buffers().size() == 1 && copied->images().size() == 2 &&
            copied->bytes().size() == 160 && copied->dataFor(0).size() == 5 &&
            copied->imageDataFor(2).empty() &&
            copied->images()[0].format == vk::Format::eR8G8B8A8Unorm &&
            copied->images()[0].copy_layout == vk::ImageLayout::eTransferSrcOptimal &&
            copied->images()[0].region.bufferOffset == 8 && copied->images()[0].data_offset == 8 &&
            copied->images()[0].payload_bytes == 88 && copied->images()[0].row_pitch_bytes == 24 &&
            copied->images()[1].row_pitch_bytes == 16 && image_result.size() == 88 &&
            tight_result.size() == 64,
        "completed Image format, explicit layout, row pitch and region metadata");
  check(std::memcmp(copied->dataFor(0).data(), expected_buffer.data() + 1, 5) == 0 &&
            std::memcmp(tight_result.data(), expected_pixels.data(), expected_pixels.size()) == 0,
        "real mixed GPU readback returns exact Buffer and tightly packed Image data");
  for (std::size_t row = 0; row < 4; ++row) {
    check(std::memcmp(image_result.data() + row * 24, expected_pixels.data() + row * 16, 16) == 0,
          "padded Image row has correct active RGBA8 pixels");
    if (row < 3) {
      const std::array<std::byte, 8> zero{};
      check(std::memcmp(image_result.data() + row * 24 + 16, zero.data(), zero.size()) == 0,
            "padded Image row gap is deterministic zero, not uninitialized GPU memory");
    }
  }
  const std::array<std::byte, 3> zeros{};
  check(std::memcmp(copied->bytes().data() + 5, zeros.data(), zeros.size()) == 0,
        "mixed odd Buffer to aligned Image inter-entry gap is zero-filled");
  check(prepared->valid() && destroyed_buffers == destroys_before,
        "readback staging retained after native completion until CPU materialization");

  RetirementQueue retirement;
  if (!retirement.retire(*upload, uses))
    return blocked("retire Image Upload staging after GPU completion and CPU materialization");
  if (!retirement.retire(*prepared, uses)) {
    // GPU is already complete, so the first owner is safe to collect before
    // leaving this negative branch; never destroy a nonempty RetirementQueue.
    const auto cleanup = retirement.collect();
    if (!cleanup || !retirement.requireEmpty())
      return blocked("cleanup of completed upload retirement");
    return blocked("retire Image readback staging after CPU copy");
  }
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  check(!prepared->valid() && retirement.pendingCount() == 2,
        "retirement takes exclusive ownership of both GPU staging owners");
  force_pending = fence.fence;
  auto pending = retirement.collect();
  check(pending && pending->collected == 0 && pending->pending == 2 &&
            destroyed_buffers == destroys_before,
        "pending native completion never prematurely destroys readback staging");
  force_pending = VK_NULL_HANDLE;
  auto collected = retirement.collect();
  check(collected && collected->collected == 2 && collected->pending == 0 &&
            retirement.requireEmpty() && destroyed_buffers == destroys_before + 2,
        "completed native receipt retires Upload and Readback staging exactly once");
  check(std::memcmp(tight_result.data(), expected_pixels.data(), expected_pixels.size()) == 0 &&
            idle_calls == 0 && submit_calls == 1 && upload_image_calls == 1 &&
            readback_image_calls == 2 && readback_buffer_calls == 1 && barrier_calls == 3 &&
            invalidated_calls >= 2,
        "CPU Image data outlives staging; no hidden Queue Submit, waitIdle or barriers");
  if (failures == 0)
    std::puts("graphics.image_readback_batch: headless-rgba8-image-readback=PASS");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run() {
  const auto caps = queryInstanceCapabilities();
  if (!caps)
    return blocked("Vulkan Instance capabilities");
  InstanceDescription instance_description;
  instance_description.api_version = VK_API_VERSION_1_3;
  auto instance_plan = resolveInstance(instance_description, *caps);
  if (!instance_plan)
    return blocked("resolve Vulkan 1.3 Instance");
  auto instance = createInstance(*instance_plan);
  if (!instance)
    return blocked("create Vulkan Instance");
  auto candidates = queryPhysicalDevices(*instance);
  if (!candidates || candidates->empty())
    return blocked("PhysicalDevice enumeration");
  for (const auto &candidate : *candidates) {
    for (std::size_t i = 0; i < candidate.capabilities().queue_families.size(); ++i) {
      const auto &family = candidate.capabilities().queue_families[i];
      if (!family.queueCount || !(family.queueFlags & vk::QueueFlagBits::eTransfer))
        continue;
      // The Image Upload and both Readback regions copy the entire 4x4x1 mip.
      // This is permitted even for minImageTransferGranularity == {0,0,0}.
      DeviceDescription device_description;
      device_description.required_features_13.synchronization2 = VK_TRUE;
      DeviceQueueRequest queue_request;
      queue_request.caller_id = "image-readback";
      queue_request.required_flags = vk::QueueFlagBits::eTransfer;
      queue_request.allowed_family_indices = {i};
      device_description.queue_requests.push_back(queue_request);
      auto plan = resolveDevice(candidate, device_description);
      if (!plan)
        continue;
      auto device = createDevice(*instance, *plan);
      if (!device)
        return blocked("create Vulkan Device with Synchronization2 enabled");
      return exercise(*instance, candidate, *device, device->queue("image-readback"));
    }
  }
  return blocked("no Vulkan Device with Transfer Queue and Synchronization2 enabled");
}
} // namespace
int main() { return run(); }

#include <terreate/graphics/upload_batch.hpp>

#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/transfer_commands.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>

namespace {
using namespace terreate::graphics;

int failures = 0;
std::size_t upload_image_calls = 0;
std::size_t copy_buffer_calls = 0;
std::size_t image_readback_calls = 0;
std::size_t barrier_calls = 0;
std::size_t submit_calls = 0;
std::size_t idle_calls = 0;
std::size_t destroyed_buffers = 0;
VkFence pending_fence = VK_NULL_HANDLE;

struct ImageCopyObservation {
  VkBuffer src = VK_NULL_HANDLE;
  VkImage dst = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
};
std::array<ImageCopyObservation, 4> observed_images{};
struct BufferCopyObservation {
  VkBuffer src = VK_NULL_HANDLE;
  VkBuffer dst = VK_NULL_HANDLE;
  VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
};
std::array<BufferCopyObservation, 4> observed_buffers{};

bool check(bool expression, const char *description) {
  if (!expression) {
    ++failures;
    std::fprintf(stderr, "graphics.image_upload_batch: assertion=FAIL: %s\n", description);
  }
  return expression;
}
int blocked(const char *reason) {
  std::fprintf(stderr, "graphics.image_upload_batch: validation=BLOCKED: %s\n", reason);
  return EXIT_FAILURE;
}
template <class T> bool hasError(const terreate::Result<T> &result, UploadError error) {
  return !result && result.error().code() == make_error_code(error);
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

// Native wrappers are confined to this GNU toolchain test target.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void __real_vkCmdCopyBufferToImage2(VkCommandBuffer, const VkCopyBufferToImageInfo2 *);
extern "C" void __real_vkCmdCopyImageToBuffer2(VkCommandBuffer, const VkCopyImageToBufferInfo2 *);
extern "C" void __real_vkCmdCopyBuffer2(VkCommandBuffer, const VkCopyBufferInfo2 *);
extern "C" void __real_vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo *);
extern "C" VkResult __real_vkQueueSubmit2(VkQueue, std::uint32_t, const VkSubmitInfo2 *, VkFence);
extern "C" VkResult __real_vkGetFenceStatus(VkDevice, VkFence);
extern "C" VkResult __real_vkDeviceWaitIdle(VkDevice);
extern "C" void __real_vmaDestroyBuffer(VmaAllocator, VkBuffer, VmaAllocation);

extern "C" void __wrap_vkCmdCopyBufferToImage2(VkCommandBuffer command,
                                               const VkCopyBufferToImageInfo2 *info) {
  if (upload_image_calls < observed_images.size() && info && info->regionCount == 1 &&
      info->pRegions) {
    observed_images[upload_image_calls] = ImageCopyObservation{
        info->srcBuffer, info->dstImage, info->dstImageLayout, *info->pRegions};
  }
  ++upload_image_calls;
  __real_vkCmdCopyBufferToImage2(command, info);
}
extern "C" void __wrap_vkCmdCopyImageToBuffer2(VkCommandBuffer command,
                                               const VkCopyImageToBufferInfo2 *info) {
  ++image_readback_calls;
  __real_vkCmdCopyImageToBuffer2(command, info);
}
extern "C" void __wrap_vkCmdCopyBuffer2(VkCommandBuffer command, const VkCopyBufferInfo2 *info) {
  if (copy_buffer_calls < observed_buffers.size() && info && info->regionCount == 1 &&
      info->pRegions) {
    observed_buffers[copy_buffer_calls] =
        BufferCopyObservation{info->srcBuffer, info->dstBuffer, *info->pRegions};
  }
  ++copy_buffer_calls;
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
  if (pending_fence != VK_NULL_HANDLE && fence == pending_fence)
    return VK_NOT_READY;
  return __real_vkGetFenceStatus(device, fence);
}
extern "C" VkResult __wrap_vkDeviceWaitIdle(VkDevice device) {
  ++idle_calls;
  return __real_vkDeviceWaitIdle(device);
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
    return blocked("create VMA allocator");
  ImageDescription image_desc;
  image_desc.format = vk::Format::eR8G8B8A8Unorm;
  image_desc.dimensions = vk::ImageType::e2D;
  image_desc.extent = vk::Extent3D{4, 4, 1};
  image_desc.mip_levels = 2;
  image_desc.array_layers = 2;
  image_desc.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
  auto image = createImage(*allocator, image_desc);
  if (!image)
    return blocked("create RGBA8 2D array Image");

  BufferDescription gpu_desc;
  gpu_desc.size = 16;
  gpu_desc.usage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc;
  auto target = createBuffer(*allocator, gpu_desc);
  if (!target)
    return blocked("create GPU Buffer target");

  BufferDescription host_desc;
  host_desc.size = 64;
  host_desc.usage = vk::BufferUsageFlagBits::eTransferDst;
  host_desc.allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  host_desc.allocation.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
  auto image_readback = createBuffer(*allocator, host_desc);
  host_desc.size = 16;
  auto buffer_readback = createBuffer(*allocator, host_desc);
  if (!image_readback || !buffer_readback)
    return blocked("create host-visible readback buffers");

  std::array<std::byte, 64> expected_pixels{};
  std::array<std::byte, 40> top{};
  std::array<std::byte, 40> bottom{};
  std::array<std::byte, 16> bytes{};
  for (std::size_t i = 0; i < expected_pixels.size(); ++i)
    expected_pixels[i] = static_cast<std::byte>((i * 17U + 9U) & 255U);
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<std::byte>((i * 23U + 3U) & 255U);
  const auto expected_buffer = bytes;
  // bufferRowLength == 6 TEXELS, so each physical row is 24 bytes.
  // The second row's last valid pixel ends after 40 bytes total.
  std::memcpy(top.data(), expected_pixels.data(), 16);
  std::memcpy(top.data() + 24, expected_pixels.data() + 16, 16);
  std::memcpy(bottom.data(), expected_pixels.data() + 32, 16);
  std::memcpy(bottom.data() + 24, expected_pixels.data() + 48, 16);

  VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
  region.bufferRowLength = 6;
  region.bufferImageHeight = 2;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1};
  region.imageExtent = {4, 2, 1};
  VkBufferImageCopy2 lower = region;
  lower.imageOffset.y = 2;
  lower.bufferImageHeight = 0;

  std::array<ImageUploadItem, 2> image_writes{{
      ImageUploadItem{&*image, vk::ImageLayout::eTransferDstOptimal, region, top},
      ImageUploadItem{&*image, vk::ImageLayout::eTransferDstOptimal, lower, bottom},
  }};
  const std::array<BufferUploadItem, 1> buffer_writes{BufferUploadItem{&*target, 0, bytes}};
  UploadBatchDescription description;
  description.buffers = buffer_writes;
  description.images = image_writes;

  auto bad = image_writes[0];
  bad.region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  check(
      hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = std::span{&bad, 1}}),
               UploadError::invalid_image_region),
      "non-color aspect rejected before native commands");
  bad = image_writes[0];
  bad.copy_layout = vk::ImageLayout::eUndefined;
  check(
      hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = std::span{&bad, 1}}),
               UploadError::unsupported_layout),
      "explicit valid destination layout required");
  bad = image_writes[0];
  bad.region.bufferOffset = 4;
  check(
      hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = std::span{&bad, 1}}),
               UploadError::invalid_image_region),
      "caller cannot guess library-owned staging offset");
  bad = image_writes[0];
  bad.region.bufferRowLength = 3;
  check(
      hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = std::span{&bad, 1}}),
               UploadError::invalid_image_region),
      "row length smaller than Image width rejected");
  bad = image_writes[0];
  bad.region.bufferImageHeight = 1;
  check(
      hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = std::span{&bad, 1}}),
               UploadError::invalid_image_region),
      "slice height smaller than region rejected");
  bad = image_writes[0];
  bad.region.bufferRowLength = std::numeric_limits<std::uint32_t>::max();
  check(
      hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = std::span{&bad, 1}}),
               UploadError::invalid_image_region),
      "oversized Vulkan RGBA8 row stride rejected");
  bad = image_writes[0];
  bad.region.imageExtent.width = 5;
  check(
      hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = std::span{&bad, 1}}),
               UploadError::invalid_image_region),
      "Image extent beyond mip bounds rejected");
  bad = image_writes[0];
  bad.region.imageSubresource.mipLevel = 2;
  check(
      hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = std::span{&bad, 1}}),
               UploadError::invalid_image_region),
      "Image mip level out of bounds rejected");
  bad = image_writes[0];
  bad.region.imageSubresource.layerCount = 2;
  check(
      hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = std::span{&bad, 1}}),
               UploadError::invalid_image_region),
      "more than one Image layer per operation rejected");
  bad = image_writes[0];
  bad.bytes = std::span{top.data(), std::size_t{39}};
  check(
      hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = std::span{&bad, 1}}),
               UploadError::invalid_image_footprint),
      "row-padded accessible Image footprint must be exact");

  auto overlapping = image_writes;
  overlapping[1].region.imageOffset.y = 1;
  check(hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = overlapping}),
                 UploadError::overlapping_image_regions),
        "overlapping Image writes to same mip/layer rejected");
  auto conflicting = image_writes;
  conflicting[1].copy_layout = vk::ImageLayout::eGeneral;
  check(hasError(prepareUploadBatch(*allocator, UploadBatchDescription{.images = conflicting}),
                 UploadError::conflicting_image_layouts),
        "same Image mip/layer cannot use two different copy layouts");

  {
    auto only_image = prepareUploadBatch(
        *allocator,
        UploadBatchDescription{.images = std::span{image_writes.data(), std::size_t{1}}});
    check(only_image && only_image->valid() && only_image->copies().empty() &&
              only_image->images().size() == 1 && only_image->stagingBuffer() &&
              only_image->stagingBuffer()->configuration()->size == 40,
          "image-only batch is supported");
    // Unrecorded batch may be destroyed immediately; no GPU use exists.
  }

  const auto destroys_before = destroyed_buffers;
  auto prepared = prepareUploadBatch(*allocator, description);
  if (!prepared)
    return blocked("prepare mixed Buffer and Image Upload batch");
  check(prepared->valid() && prepared->copies().size() == 1 && prepared->images().size() == 2 &&
            prepared->stagingBuffer() && prepared->stagingBuffer()->configuration() &&
            prepared->stagingBuffer()->configuration()->size == 96 &&
            prepared->images()[0].region.bufferOffset == 16 &&
            prepared->images()[1].region.bufferOffset == 56 &&
            prepared->images()[0].region.bufferRowLength == 6 &&
            prepared->images()[1].region.imageSubresource.baseArrayLayer == 1 &&
            prepared->images()[0].payload_bytes == 40,
        "effective mixed upload region layout and byte footprint");
  const auto stage_allocation = prepared->stagingBuffer()->allocationInfo();
  check(stage_allocation &&
            (stage_allocation->memory_properties & vk::MemoryPropertyFlagBits::eHostVisible),
        "Staging allocation really host visible");
  top.fill(std::byte{0});
  bottom.fill(std::byte{0});
  bytes.fill(std::byte{0});

  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  pool_description.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  auto pool = createCommandPool(device, pool_description);
  if (!pool)
    return blocked("create CommandPool");
  auto command = pool->allocatePrimary();
  if (!command)
    return blocked("allocate CommandBuffer");
  check(hasError(recordUploadBatch(device, *command, *prepared), UploadError::not_recording) &&
            upload_image_calls == 0 && copy_buffer_calls == 0,
        "recording is explicitly controlled by caller");
  if (!command->begin())
    return blocked("begin command recording");

  Image moved = std::move(*image);
  check(
      hasError(recordUploadBatch(device, *command, *prepared), UploadError::invalid_destination) &&
          upload_image_calls == 0 && copy_buffer_calls == 0,
      "image move after prepare rejects whole batch before any VkCmd");
  *image = std::move(moved);

  VkImageMemoryBarrier2 init{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  init.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  init.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  init.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  init.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  init.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  init.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  init.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  init.image = image->nativeImage();
  init.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 1};
  if (!recordBarriers(device, *command, BarrierDescription{.images = std::span{&init, 1}}))
    return blocked("record caller-specified initial Image transition");
  check(barrier_calls == 1, "initial Image barrier was actually recorded");

  const auto *staging = prepared->stagingBuffer();
  if (!recordUploadBatch(device, *command, *prepared))
    return blocked("record mixed Buffer and Image uploads");
  check(upload_image_calls == 2 && copy_buffer_calls == 1 && barrier_calls == 1 &&
            submit_calls == 0,
        "exactly two native Image Copy2, one Buffer Copy2, no implicit barrier/submit");
  check(observed_images[0].src == staging->nativeBuffer() &&
            observed_images[0].dst == image->nativeImage() &&
            observed_images[0].layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
            observed_images[0].region.bufferOffset == 16 &&
            observed_images[0].region.bufferRowLength == 6 &&
            observed_images[0].region.bufferImageHeight == 2 &&
            observed_images[0].region.imageSubresource.baseArrayLayer == 1 &&
            observed_images[1].region.bufferOffset == 56 &&
            observed_images[1].region.imageOffset.y == 2 &&
            observed_buffers[0].src == staging->nativeBuffer() &&
            observed_buffers[0].dst == target->nativeBuffer() &&
            observed_buffers[0].region.srcOffset == 0 && observed_buffers[0].region.size == 16,
        "native Copy2 exactly reflects effective staging offsets and texel pitch");

  VkImageMemoryBarrier2 to_src = init;
  to_src.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_src.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_src.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkMemoryBarrier2 buffer_to_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  buffer_to_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  buffer_to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  buffer_to_read.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  buffer_to_read.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  if (!recordBarriers(device, *command,
                      BarrierDescription{.memory = std::span{&buffer_to_read, 1},
                                         .images = std::span{&to_src, 1}}))
    return blocked("record caller-specified transfer-read dependency");

  VkBufferImageCopy2 image_out{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
  image_out.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1};
  image_out.imageExtent = {4, 4, 1};
  if (!recordCopyImageToBuffer(device, *command, *image, vk::ImageLayout::eTransferSrcOptimal,
                               *image_readback, std::span{&image_out, 1}))
    return blocked("record Image readback in test fixture");
  const VkBufferCopy2 buffer_out{VK_STRUCTURE_TYPE_BUFFER_COPY_2, nullptr, 0, 0, 16};
  if (!recordCopyBuffer(device, *command, *target, *buffer_readback, std::span{&buffer_out, 1}))
    return blocked("record Buffer readback in test fixture");
  VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  if (!recordBarriers(device, *command, BarrierDescription{.memory = std::span{&to_host, 1}}))
    return blocked("record caller-specified host readback barrier");
  if (!command->end())
    return blocked("finish command recording");

  NativeFence fence{static_cast<VkDevice>(device.nativeHandle())};
  VkFenceCreateInfo fence_description{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(fence.device, &fence_description, nullptr, &fence.fence) != VK_SUCCESS)
    return blocked("create submission Fence");
  std::array<const CommandBuffer *, 1> commands{&*command};
  int identity_domain = 0;
  CompletionDescription completion;
  completion.device = &device;
  completion.queue = queue;
  completion.requested = {&identity_domain, 1};
  completion.effective = {&identity_domain, 1};
  completion.condition = FenceCompletionCondition{vk::Fence{fence.fence}};
  SubmissionDescription submit_description;
  submit_description.command_buffers = commands;
  submit_description.fence = vk::Fence{fence.fence};
  submit_description.completion = completion;
  check(submit_calls == 0, "no hidden submission before caller's submit");
  auto receipt = submit(device, queue, submit_description);
  if (!receipt)
    return blocked("real explicit Queue submission");
  check(submit_calls == 1 && prepared->valid(),
        "application submit returns a native-success receipt but retains staging");

  RetirementQueue retirement;
  const std::span<const SubmissionReceipt> uses{&*receipt, 1};
  if (!retirement.retire(*prepared, uses))
    return blocked("move staging owner to caller's RetirementQueue");
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  check(!prepared->valid() && retirement.pendingCount() == 1 &&
            destroyed_buffers == destroys_before,
        "staging owner transferred without early destruction");
  pending_fence = fence.fence;
  const auto pending = retirement.collect();
  check(pending && pending->collected == 0 && pending->pending == 1 &&
            destroyed_buffers == destroys_before,
        "pending GPU receipt cannot collect staging");
  pending_fence = VK_NULL_HANDLE;

  const auto waited = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!waited || waited->state != SubmissionCompletionState::completed)
    return blocked("native Fence completion");
  const auto collected = retirement.collect();
  check(collected && collected->pending == 0 && collected->collected == 1 &&
            retirement.requireEmpty() && destroyed_buffers == destroys_before + 1,
        "completed GPU receipt collects mixed staging exactly once");

  auto mapped_image = image_readback->map();
  auto mapped_buffer = buffer_readback->map();
  if (!mapped_image || !mapped_buffer)
    return blocked("map real GPU readback Buffer(s)");
  if (!image_readback->invalidate() || !buffer_readback->invalidate())
    return blocked("invalidate real GPU readback Buffer(s)");
  check(std::memcmp(*mapped_image, expected_pixels.data(), expected_pixels.size()) == 0,
        "two padded Image Upload regions round-trip pixel-perfect");
  check(std::memcmp(*mapped_buffer, expected_buffer.data(), expected_buffer.size()) == 0,
        "mixed Buffer Upload retains Phase A byte round-trip");
  if (!image_readback->unmap() || !buffer_readback->unmap())
    return blocked("unmap real GPU readback Buffer(s)");
  check(upload_image_calls == 2 && copy_buffer_calls == 2 && image_readback_calls == 1 &&
            barrier_calls == 3 && idle_calls == 0,
        "precise native call count; explicit barriers, no implicit device idle");

  if (failures == 0)
    std::puts("graphics.image_upload_batch: headless-rgba8-image=PASS");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
int run() {
  const auto caps = queryInstanceCapabilities();
  if (!caps)
    return blocked("query Vulkan Instance capabilities");
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
    return blocked("enumerate PhysicalDevice(s)");
  for (const auto &candidate : *candidates) {
    for (std::size_t i = 0; i < candidate.capabilities().queue_families.size(); ++i) {
      const auto &family = candidate.capabilities().queue_families[i];
      if (!family.queueCount || !(family.queueFlags & vk::QueueFlagBits::eTransfer))
        continue;
      // Both 4x2 transfers (at y=0 and y=2) must satisfy the selected
      // transfer family's minImageTransferGranularity, including offsets.
      const auto &granularity = family.minImageTransferGranularity;
      if (!granularity.width || !granularity.height || !granularity.depth ||
          (4U % granularity.width) != 0 || (2U % granularity.height) != 0 ||
          (1U % granularity.depth) != 0)
        continue;
      DeviceDescription device_description;
      device_description.required_features_13.synchronization2 = VK_TRUE;
      DeviceQueueRequest request;
      request.caller_id = "image-upload";
      request.required_flags = vk::QueueFlagBits::eTransfer;
      request.allowed_family_indices = {i};
      device_description.queue_requests.push_back(request);
      auto plan = resolveDevice(candidate, device_description);
      if (!plan)
        continue;
      auto device = createDevice(*instance, *plan);
      if (!device)
        return blocked("create Vulkan Device with explicit synchronization2");
      return exercise(*instance, candidate, *device, device->queue("image-upload"));
    }
  }
  return blocked("no transfer Queue with compatible Image granularity and synchronization2");
}
} // namespace

int main() { return run(); }

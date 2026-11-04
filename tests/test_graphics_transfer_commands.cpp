#include <terreate/graphics/transfer_commands.hpp>

#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/submission.hpp>

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
std::size_t barrier_calls = 0;
std::size_t buffer_calls = 0;
std::size_t image_calls = 0;
std::size_t to_image_calls = 0;
std::size_t from_image_calls = 0;
std::size_t implicit_idle_calls = 0;
bool check(bool pass, const char *label) {
  if (!pass) {
    ++failures;
    std::fprintf(stderr, "graphics.transfer_commands: assertion=FAIL %s\n", label);
  }
  return pass;
}
int blocked(const char *reason) {
  std::fprintf(stderr, "graphics.transfer_commands: environment=BLOCKED %s\n", reason);
  return EXIT_FAILURE;
}
bool transferError(const terreate::Result<void> &value, TransferCommandError error) {
  return !value && value.error().code() == make_error_code(error);
}
struct NativeFence {
  VkDevice device = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  ~NativeFence() {
    if (fence)
      vkDestroyFence(device, fence, nullptr);
  }
};
} // namespace

// This intercepts the actual linked Vulkan entrypoints, not the API wrapper.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void __real_vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo *);
extern "C" void __real_vkCmdCopyBuffer2(VkCommandBuffer, const VkCopyBufferInfo2 *);
extern "C" void __real_vkCmdCopyImage2(VkCommandBuffer, const VkCopyImageInfo2 *);
extern "C" void __real_vkCmdCopyBufferToImage2(VkCommandBuffer, const VkCopyBufferToImageInfo2 *);
extern "C" void __real_vkCmdCopyImageToBuffer2(VkCommandBuffer, const VkCopyImageToBufferInfo2 *);
extern "C" VkResult __real_vkDeviceWaitIdle(VkDevice);
extern "C" void __wrap_vkCmdPipelineBarrier2(VkCommandBuffer command,
                                             const VkDependencyInfo *info) {
  ++barrier_calls;
  __real_vkCmdPipelineBarrier2(command, info);
}
extern "C" void __wrap_vkCmdCopyBuffer2(VkCommandBuffer command, const VkCopyBufferInfo2 *info) {
  ++buffer_calls;
  __real_vkCmdCopyBuffer2(command, info);
}
extern "C" void __wrap_vkCmdCopyImage2(VkCommandBuffer command, const VkCopyImageInfo2 *info) {
  ++image_calls;
  __real_vkCmdCopyImage2(command, info);
}
extern "C" void __wrap_vkCmdCopyBufferToImage2(VkCommandBuffer command,
                                               const VkCopyBufferToImageInfo2 *info) {
  ++to_image_calls;
  __real_vkCmdCopyBufferToImage2(command, info);
}
extern "C" void __wrap_vkCmdCopyImageToBuffer2(VkCommandBuffer command,
                                               const VkCopyImageToBufferInfo2 *info) {
  ++from_image_calls;
  __real_vkCmdCopyImageToBuffer2(command, info);
}
extern "C" VkResult __wrap_vkDeviceWaitIdle(VkDevice device) {
  ++implicit_idle_calls;
  return __real_vkDeviceWaitIdle(device);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
int exercise(Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             Queue queue) {
  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  pool_description.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  auto pool = createCommandPool(device, pool_description);
  if (!pool)
    return blocked("create command pool");
  auto command = pool->allocatePrimary();
  if (!command)
    return blocked("allocate command buffer");
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("create VMA allocator");
  BufferDescription buffer_desc;
  buffer_desc.size = 64;
  buffer_desc.usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
  buffer_desc.allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  buffer_desc.allocation.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
  auto source = createBuffer(*allocator, buffer_desc);
  auto destination = createBuffer(*allocator, buffer_desc);
  auto image_readback = createBuffer(*allocator, buffer_desc);
  if (!source || !destination || !image_readback)
    return blocked("create host-visible VMA Buffers");
  ImageDescription image_desc;
  image_desc.format = vk::Format::eR8G8B8A8Unorm;
  image_desc.extent = vk::Extent3D{4, 4, 1};
  image_desc.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
  auto image_a = createImage(*allocator, image_desc);
  auto image_b = createImage(*allocator, image_desc);
  if (!image_a || !image_b)
    return blocked("create transfer Images");

  const auto no_barriers = recordBarriers(device, *command, {});
  check(transferError(no_barriers, TransferCommandError::not_recording),
        "barriers require recording state");
  const std::array<VkBufferCopy2, 1> region{
      {VkBufferCopy2{VK_STRUCTURE_TYPE_BUFFER_COPY_2, nullptr, 0, 0, 64}}};
  const auto not_recording = recordCopyBuffer(device, *command, *source, *destination, region);
  check(transferError(not_recording, TransferCommandError::not_recording),
        "copy requires recording state");
  check(buffer_calls == 0 && barrier_calls == 0, "no native calls before recording");

  if (!command->begin())
    return blocked("begin command buffer");
  const auto empty_barriers = recordBarriers(device, *command, {});
  check(transferError(empty_barriers, TransferCommandError::missing_regions),
        "empty barriers must be rejected");
  const auto zero_copy = recordCopyBuffer(device, *command, *source, *destination, {});
  check(transferError(zero_copy, TransferCommandError::missing_regions),
        "empty copy regions must be rejected");
  VkBufferCopy2 invalid_region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
  invalid_region.size = 65;
  const auto overflow_copy =
      recordCopyBuffer(device, *command, *source, *destination, std::span{&invalid_region, 1});
  check(transferError(overflow_copy, TransferCommandError::out_of_bounds),
        "invalid Buffer size must fail before native record");
  invalid_region.size = 0;
  const auto zero_region =
      recordCopyBuffer(device, *command, *source, *destination, std::span{&invalid_region, 1});
  check(transferError(zero_region, TransferCommandError::invalid_region),
        "zero copy size must fail before native record");
  invalid_region = region[0];
  invalid_region.pNext = &invalid_region;
  const auto bad_next =
      recordCopyBuffer(device, *command, *source, *destination, std::span{&invalid_region, 1});
  check(transferError(bad_next, TransferCommandError::invalid_region),
        "non-null pNext must fail before native record");
  check(buffer_calls == 0, "invalid copy did not reach Vulkan");

  std::array<std::byte, 64> pixels{};
  for (std::size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = static_cast<std::byte>((i * 19U + 7U) & 0xffU);
  auto mapped_source = source->map();
  if (!mapped_source)
    return blocked("map source Buffer");
  std::memcpy(*mapped_source, pixels.data(), pixels.size());
  if (!source->flush() || !source->unmap())
    return blocked("flush and unmap source");

  VkImageMemoryBarrier2 initial_a{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  initial_a.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  initial_a.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  initial_a.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  initial_a.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  initial_a.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  initial_a.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  initial_a.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  initial_a.image = image_a->nativeImage();
  initial_a.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier2 initial_b = initial_a;
  initial_b.image = image_b->nativeImage();
  const std::array<VkImageMemoryBarrier2, 2> image_initial{initial_a, initial_b};
  BarrierDescription begin_barriers;
  begin_barriers.images = image_initial;
  if (!recordBarriers(device, *command, begin_barriers))
    return blocked("record initial image layouts");

  VkBufferImageCopy2 image_region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
  image_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  image_region.imageExtent = {4, 4, 1};
  if (!recordCopyBufferToImage(device, *command, *source, *image_a,
                               vk::ImageLayout::eTransferDstOptimal, std::span{&image_region, 1}))
    return blocked("record Buffer to Image");
  const auto bad_region_saved = image_region;
  image_region.imageExtent.width = 0;
  check(transferError(recordCopyBufferToImage(device, *command, *source, *image_a,
                                              vk::ImageLayout::eTransferDstOptimal,
                                              std::span{&image_region, 1}),
                      TransferCommandError::invalid_region),
        "zero image extent must fail");
  image_region = bad_region_saved;

  VkImageMemoryBarrier2 a_to_src = initial_a;
  a_to_src.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  a_to_src.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  a_to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  a_to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  a_to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  if (!recordBarriers(device, *command, BarrierDescription{.images = std::span{&a_to_src, 1}}))
    return blocked("record image source transition");

  VkImageCopy2 image_copy{VK_STRUCTURE_TYPE_IMAGE_COPY_2};
  image_copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  image_copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  image_copy.extent = {4, 4, 1};
  if (!recordCopyImage(device, *command, *image_a, vk::ImageLayout::eTransferSrcOptimal, *image_b,
                       vk::ImageLayout::eTransferDstOptimal, std::span{&image_copy, 1}))
    return blocked("record Image to Image");

  VkImageMemoryBarrier2 b_to_src = a_to_src;
  b_to_src.image = image_b->nativeImage();
  if (!recordBarriers(device, *command, BarrierDescription{.images = std::span{&b_to_src, 1}}))
    return blocked("record image destination transition");
  if (!recordCopyImageToBuffer(device, *command, *image_b, vk::ImageLayout::eTransferSrcOptimal,
                               *image_readback, std::span{&image_region, 1}))
    return blocked("record Image to Buffer");
  if (!recordCopyBuffer(device, *command, *source, *destination, region))
    return blocked("record Buffer copy");

  VkMemoryBarrier2 host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  host_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  host_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  host_read.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  host_read.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  BarrierDescription host_barriers;
  host_barriers.memory = std::span{&host_read, 1};
  if (!recordBarriers(device, *command, host_barriers))
    return blocked("record host read barrier");
  if (!command->end())
    return blocked("end command buffer");

  const auto before_copy = buffer_calls;
  check(transferError(recordCopyBuffer(device, *command, *source, *destination, region),
                      TransferCommandError::not_recording) &&
            buffer_calls == before_copy,
        "command ends recording before subsequent calls");
  const auto native_device = static_cast<VkDevice>(device.nativeHandle());
  NativeFence fence{native_device};
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(native_device, &fence_info, nullptr, &fence.fence) != VK_SUCCESS)
    return blocked("create submission fence");
  std::array<const CommandBuffer *, 1> commands{&*command};
  int domain = 0;
  CompletionDescription completion;
  completion.device = &device;
  completion.queue = queue;
  completion.requested = {&domain, 1};
  completion.effective = {&domain, 1};
  completion.condition = FenceCompletionCondition{vk::Fence{fence.fence}};
  SubmissionDescription submission;
  submission.command_buffers = commands;
  submission.fence = vk::Fence{fence.fence};
  submission.completion = completion;
  auto receipt = submit(device, queue, submission);
  if (!receipt)
    return blocked("native submission");
  const auto outcome = receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!outcome || outcome->state != SubmissionCompletionState::completed)
    return blocked("native submission fence completion");

  auto mapped_destination = destination->map();
  auto mapped_readback = image_readback->map();
  if (!mapped_destination || !mapped_readback)
    return blocked("map destination buffers");
  if (!destination->invalidate() || !image_readback->invalidate())
    return blocked("invalidate host-visible destination buffers");
  check(std::memcmp(*mapped_destination, pixels.data(), pixels.size()) == 0,
        "native Buffer copy bytes match");
  check(std::memcmp(*mapped_readback, pixels.data(), pixels.size()) == 0,
        "native Buffer -> Image -> Image -> Buffer bytes match");
  if (!destination->unmap() || !image_readback->unmap())
    return blocked("unmap destination buffers");

  check(barrier_calls == 4 && buffer_calls == 1 && image_calls == 1 && to_image_calls == 1 &&
            from_image_calls == 1,
        "native Vulkan commands called exactly once per explicit description");
  check(implicit_idle_calls == 0, "record and submit have no hidden device idle");
  if (failures == 0)
    std::puts("graphics.transfer_commands: headless-transfer=PASS");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
int run() {
  auto caps = queryInstanceCapabilities();
  if (!caps)
    return blocked("instance capabilities");
  InstanceDescription instance_desc;
  instance_desc.api_version = VK_API_VERSION_1_3;
  auto instance_plan = resolveInstance(instance_desc, *caps);
  if (!instance_plan)
    return blocked("Vulkan 1.3 instance");
  auto instance = createInstance(*instance_plan);
  if (!instance)
    return blocked("instance creation");
  auto candidates = queryPhysicalDevices(*instance);
  if (!candidates)
    return blocked("physical device enumeration");
  for (const auto &candidate : *candidates) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t i = 0; i < families.size(); ++i) {
      if (families[i].queueCount == 0 || !(families[i].queueFlags & vk::QueueFlagBits::eTransfer))
        continue;
      DeviceDescription desc;
      desc.required_features_13.synchronization2 = VK_TRUE;
      DeviceQueueRequest request;
      request.caller_id = "transfer";
      request.required_flags = vk::QueueFlagBits::eTransfer;
      request.allowed_family_indices = {i};
      desc.queue_requests.push_back(request);
      auto plan = resolveDevice(candidate, desc);
      if (!plan)
        continue;
      auto device = createDevice(*instance, *plan);
      if (!device)
        return blocked("logical Device creation");
      return exercise(*instance, candidate, *device, device->queue("transfer"));
    }
  }
  return blocked("no transfer/synchronization2 Vulkan Device");
}
} // namespace

int main() { return run(); }

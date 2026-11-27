// The Image upload of prepareUploadBatch / recordUploadBatch accepts the
// uncompressed single-plane color formats R8, R8G8, R8G8B8A8 (UNORM and SRGB),
// R16, R16G16, R16G16B16A16 (UNORM) and R32, R32G32, R32G32B32A32 (SFLOAT), and
// still refuses everything else. This test checks, per format that the device
// supports for transfers: the texel-size footprint, the staging offsets (a
// multiple of the texel size and of 4), and a real round trip: upload two mips
// (one with a padded row), submit with a Fence, wait explicitly, copy the image
// back to host-visible memory and compare the bytes.
//
// A format the device cannot use is reported as SKIP for that format with the
// reason, never as PASS; if no format could be tested the whole test is SKIP.
// No linker or platform mechanism is used.
#include <terreate/graphics/upload_batch.hpp>

#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/format_properties.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/synchronization.hpp>
#include <terreate/graphics/transfer_commands.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace terreate::graphics;

const char *test_name = "graphics.image_upload_formats";
int failures = 0;
std::size_t tested_formats = 0;

bool check(bool expression, const std::string &description) {
  if (!expression) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, description.c_str());
  }
  return expression;
}
int blocked(const char *reason) {
  std::fprintf(stderr, "%s: validation=BLOCKED: %s\n", test_name, reason);
  return EXIT_FAILURE;
}
template <class T> bool hasError(const terreate::Result<T> &result, UploadError error) {
  return !result && result.error().code() == make_error_code(error);
}

struct FormatCase {
  vk::Format format;
  std::uint32_t texel;
};
constexpr std::array supported{
    FormatCase{vk::Format::eR8Unorm, 1},           FormatCase{vk::Format::eR8Srgb, 1},
    FormatCase{vk::Format::eR8G8Unorm, 2},         FormatCase{vk::Format::eR8G8Srgb, 2},
    FormatCase{vk::Format::eR8G8B8A8Unorm, 4},     FormatCase{vk::Format::eR8G8B8A8Srgb, 4},
    FormatCase{vk::Format::eR16Unorm, 2},          FormatCase{vk::Format::eR16G16Unorm, 4},
    FormatCase{vk::Format::eR16G16B16A16Unorm, 8}, FormatCase{vk::Format::eR32Sfloat, 4},
    FormatCase{vk::Format::eR32G32Sfloat, 8},      FormatCase{vk::Format::eR32G32B32A32Sfloat, 16},
};

constexpr std::uint64_t wait_ns = 10'000'000'000;
constexpr std::uint32_t row_texels = 7; // mip 0 is 5 wide: two texels of row padding
constexpr std::uint32_t base_width = 5;
constexpr std::uint32_t base_height = 3;

struct Gpu {
  Instance &instance;
  const PhysicalDeviceCandidate &candidate;
  Device &device;
  Allocator &allocator;
  Queue queue;
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<std::byte> pattern(std::size_t size, unsigned seed) {
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i)
    bytes[i] = static_cast<std::byte>((i * 29U + static_cast<std::size_t>(seed) * 53U + 1U) & 255U);
  return bytes;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
VkBufferImageCopy2 region(std::uint32_t mip, std::uint32_t width, std::uint32_t height,
                          std::uint32_t row_length) {
  VkBufferImageCopy2 copy{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
  copy.bufferRowLength = row_length;
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
  copy.imageExtent = {width, height, 1};
  return copy;
}

// The staging offset must be a multiple of the texel size and of 4.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool alignedTo(vk::DeviceSize offset, std::uint32_t texel) {
  const std::uint32_t alignment = texel > 4 ? texel : 4;
  return offset % alignment == 0;
}

bool roundTrip(Gpu &gpu, const FormatCase &c) {
  const std::string label = vk::to_string(c.format);
  const auto properties = queryFormatProperties(gpu.instance, gpu.candidate, c.format);
  const auto needed =
      vk::FormatFeatureFlagBits2::eTransferSrc | vk::FormatFeatureFlagBits2::eTransferDst;
  if (!properties || !properties->supports(vk::ImageTiling::eOptimal, needed)) {
    std::printf("%s: format=%s SKIP reason=no optimal-tiling transfer support\n", test_name,
                label.c_str());
    return true;
  }
  ImageDescription description;
  description.format = c.format;
  description.extent = vk::Extent3D{base_width, base_height, 1};
  description.mip_levels = 2;
  description.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
  auto image = createImage(gpu.allocator, description);
  // The device reported the needed features, so a creation failure is a failure, not a SKIP.
  if (!check(bool(image), label + ": the image is created although the features are supported"))
    return false;
  ++tested_formats;

  // Mip 0: 5x3 texels in rows of 7 texels (the last row ends after its 5th texel).
  // Mip 1: 2x1 texels, tight. Its staging offset follows an odd byte count for narrow texels.
  const std::size_t mip0_bytes =
      ((std::size_t{base_height} - 1) * row_texels + base_width) * std::size_t{c.texel};
  const std::size_t mip1_bytes = std::size_t{2} * c.texel;
  const auto mip0 = pattern(mip0_bytes, 1);
  const auto mip1 = pattern(mip1_bytes, 2);
  const std::array<ImageUploadItem, 2> writes{
      ImageUploadItem{&*image, vk::ImageLayout::eTransferDstOptimal,
                      region(0, base_width, base_height, row_texels), mip0},
      ImageUploadItem{&*image, vk::ImageLayout::eTransferDstOptimal, region(1, 2, 1, 0), mip1},
  };

  // A byte count that is not the exact footprint is refused.
  auto wrong = writes[0];
  wrong.bytes = std::span{mip0.data(), mip0.size() - 1};
  check(hasError(prepareUploadBatch(gpu.allocator,
                                    UploadBatchDescription{.images = std::span{&wrong, 1}}),
                 UploadError::invalid_image_footprint),
        label + ": a byte count that is not texels times the texel size is refused");
  // Zero extent is not a footprint.
  auto empty = writes[1];
  empty.region.imageExtent.width = 0;
  check(hasError(prepareUploadBatch(gpu.allocator,
                                    UploadBatchDescription{.images = std::span{&empty, 1}}),
                 UploadError::invalid_image_region),
        label + ": a zero extent is refused");

  auto prepared = prepareUploadBatch(gpu.allocator, UploadBatchDescription{.images = writes});
  if (!check(bool(prepared), label + ": prepareUploadBatch"))
    return false;
  const auto images = prepared->images();
  if (!check(images.size() == 2, label + ": two prepared copies"))
    return false;
  check(
      images[0].payload_bytes == mip0_bytes && images[1].payload_bytes == mip1_bytes &&
          images[0].region.bufferOffset == 0 && alignedTo(images[0].region.bufferOffset, c.texel) &&
          alignedTo(images[1].region.bufferOffset, c.texel) &&
          images[1].region.bufferOffset >= mip0_bytes &&
          images[1].region.bufferOffset < mip0_bytes + 16 &&
          prepared->stagingBuffer()->configuration()->size ==
              images[1].region.bufferOffset + mip1_bytes,
      label + ": the footprint is texels times the texel size and the staging offsets are aligned");

  BufferDescription host_desc;
  host_desc.size = 1024;
  host_desc.usage = vk::BufferUsageFlagBits::eTransferDst;
  host_desc.allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  host_desc.allocation.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
  auto host = createBuffer(gpu.allocator, host_desc);
  if (!check(bool(host), label + ": host-visible readback buffer"))
    return false;

  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(gpu.queue.familyIndex());
  auto pool = createCommandPool(gpu.device, pool_description);
  if (!check(bool(pool), label + ": CommandPool"))
    return false;
  auto command = pool->allocatePrimary();
  if (!check(command && command->begin(), label + ": begin CommandBuffer"))
    return false;

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  const auto transition = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                              VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                              VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image->nativeImage();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 1};
    return recordBarriers(gpu.device, *command,
                          BarrierDescription{.images = std::span{&barrier, 1}});
  };
  if (!check(bool(transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                             VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT)),
             label + ": undefined -> transfer dst") ||
      !check(bool(recordUploadBatch(gpu.device, *command, *prepared)),
             label + ": recordUploadBatch") ||
      !check(bool(transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_READ_BIT)),
             label + ": transfer dst -> transfer src"))
    return false;

  // Read each mip back tight: mip 0 at byte 0, mip 1 at byte 512.
  VkBufferImageCopy2 out0 = region(0, base_width, base_height, 0);
  VkBufferImageCopy2 out1 = region(1, 2, 1, 0);
  out1.bufferOffset = 512;
  if (!check(bool(recordCopyImageToBuffer(gpu.device, *command, *image,
                                          vk::ImageLayout::eTransferSrcOptimal, *host,
                                          std::span{&out0, 1})) &&
                 bool(recordCopyImageToBuffer(gpu.device, *command, *image,
                                              vk::ImageLayout::eTransferSrcOptimal, *host,
                                              std::span{&out1, 1})),
             label + ": readback copies"))
    return false;
  VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  if (!check(bool(recordBarriers(gpu.device, *command,
                                 BarrierDescription{.memory = std::span{&to_host, 1}})) &&
                 command->end(),
             label + ": host barrier and end"))
    return false;

  auto fence = createFence(gpu.device, {});
  if (!check(bool(fence), label + ": Fence"))
    return false;
  int domain = 0;
  const std::array<const CommandBuffer *, 1> commands{&*command};
  auto receipt =
      submitFenced(gpu.device, gpu.queue, *fence, {&domain, 1}, {.command_buffers = commands});
  if (!receipt) {
    // The outcome is unknown: do not run destructors for resources the GPU may use.
    std::fprintf(stderr, "%s: submission failed\n", test_name);
    std::_Exit(blocked("real Queue submission failed"));
  }
  const auto waited = fence->wait(wait_ns);
  if (!waited || *waited != WaitStatus::satisfied)
    std::_Exit(blocked("the Fence did not prove the GPU work complete"));
  const std::array<SubmissionReceipt, 1> uses{*receipt};
  RetirementQueue retirement;
  check(retirement.retire(*prepared, uses) && bool(retirement.collect()) &&
            retirement.requireEmpty(),
        label + ": the staging buffer is retired and collected");

  auto mapped = host->map();
  if (!check(bool(mapped) && bool(host->invalidate()), label + ": map and invalidate"))
    return false;
  const auto *bytes = static_cast<const std::byte *>(*mapped);
  // Mip 0 comes back tight; the padded source rows are compared row by row.
  bool mip0_equal = true;
  for (std::uint32_t row = 0; row < base_height; ++row)
    mip0_equal = mip0_equal &&
                 std::memcmp(bytes + static_cast<std::size_t>(row) * base_width * c.texel,
                             mip0.data() + static_cast<std::size_t>(row) * row_texels * c.texel,
                             static_cast<std::size_t>(base_width) * c.texel) == 0;
  check(mip0_equal, label + ": the padded mip 0 round-trips texel for texel");
  check(std::memcmp(bytes + 512, mip1.data(), mip1.size()) == 0,
        label + ": mip 1 round-trips texel for texel");
  check(bool(host->unmap()), label + ": unmap");
  std::printf("%s: format=%s PASS\n", test_name, label.c_str());
  return failures == 0;
}

// Formats outside the supported set are refused with unsupported_image, both at
// prepare time. Only formats the device can create as a transfer destination can
// be tried; the others are reported.
void refusals(Gpu &gpu) {
  constexpr std::array refused{
      vk::Format::eB8G8R8A8Unorm,          vk::Format::eR8G8B8Unorm,
      vk::Format::eR8G8B8A8Uint,           vk::Format::eR32G32B32A32Uint,
      vk::Format::eA2B10G10R10UnormPack32, vk::Format::eR16G16B16A16Sfloat,
      vk::Format::eBc1RgbaUnormBlock,      vk::Format::eD32Sfloat,
  };
  std::size_t tried = 0;
  const std::array<std::byte, 64> bytes{};
  for (const auto format : refused) {
    ImageDescription description;
    description.format = format;
    description.extent = vk::Extent3D{4, 4, 1};
    description.usage = vk::ImageUsageFlagBits::eTransferDst;
    auto image = createImage(gpu.allocator, description);
    if (!image)
      continue;
    ++tried;
    VkBufferImageCopy2 copy = region(0, 4, 4, 0);
    ImageUploadItem item{&*image, vk::ImageLayout::eTransferDstOptimal, copy, bytes};
    check(hasError(prepareUploadBatch(gpu.allocator,
                                      UploadBatchDescription{.images = std::span{&item, 1}}),
                   UploadError::unsupported_image),
          vk::to_string(format) + ": an unsupported format is unsupported_image");
  }
  check(tried != 0, "at least one unsupported format could be created and refused");
  std::printf("%s: refused-formats checked=%zu of %zu\n", test_name, tried, refused.size());

  // 3D and multisampled images stay unsupported.
  ImageDescription volume;
  volume.format = vk::Format::eR8G8B8A8Unorm;
  volume.dimensions = vk::ImageType::e3D;
  volume.extent = vk::Extent3D{4, 4, 4};
  volume.usage = vk::ImageUsageFlagBits::eTransferDst;
  if (auto image = createImage(gpu.allocator, volume)) {
    VkBufferImageCopy2 copy = region(0, 4, 4, 0);
    ImageUploadItem item{&*image, vk::ImageLayout::eTransferDstOptimal, copy, bytes};
    check(hasError(prepareUploadBatch(gpu.allocator,
                                      UploadBatchDescription{.images = std::span{&item, 1}}),
                   UploadError::unsupported_image),
          "a 3D image stays unsupported_image");
  }
}

int exercise(Gpu &gpu) {
  refusals(gpu);
  for (const auto &c : supported) {
    if (!roundTrip(gpu, c))
      return EXIT_FAILURE;
  }
  if (failures != 0)
    return EXIT_FAILURE;
  if (tested_formats == 0) {
    std::printf("%s: formats=SKIP reason=no supported format could be created on this device\n",
                test_name);
    return EXIT_SUCCESS;
  }
  std::printf("%s: formats=PASS tested=%zu of %zu\n", test_name, tested_formats, supported.size());
  return EXIT_SUCCESS;
}

int run() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("query Vulkan Instance capabilities");
  InstanceDescription instance_description;
  instance_description.api_version = VK_API_VERSION_1_3;
  const auto instance_plan = resolveInstance(instance_description, *capabilities);
  if (!instance_plan)
    return blocked("resolve Vulkan 1.3 Instance");
  auto instance = createInstance(*instance_plan);
  if (!instance)
    return blocked("create Vulkan Instance");
  auto candidates = queryPhysicalDevices(*instance);
  if (!candidates)
    return blocked("enumerate PhysicalDevice(s)");
  for (const auto &candidate : *candidates) {
    DeviceDescription request;
    DeviceQueueRequest queue_request;
    queue_request.caller_id = "main";
    queue_request.required_flags = vk::QueueFlagBits::eGraphics;
    request.queue_requests = {queue_request};
    request.required_features_13.synchronization2 = VK_TRUE;
    const auto plan = resolveDevice(candidate, request);
    if (!plan)
      continue;
    auto device = createDevice(*instance, *plan);
    if (!device)
      return blocked("create Device with synchronization2");
    const Queue queue = device->queue("main");
    if (!queue.valid())
      return blocked("graphics Queue");
    auto allocator = createAllocator(*instance, candidate, *device);
    if (!allocator)
      return blocked("create VMA Allocator");
    Gpu gpu{*instance, candidate, *device, *allocator, queue};
    return exercise(gpu);
  }
  std::printf("%s: formats=SKIP reason=no device with a graphics queue and synchronization2\n",
              test_name);
  return EXIT_SUCCESS;
}
} // namespace

int main() { return run(); }

// #279 part 2: an ImageData reaches the GPU in three explicit steps (plan, create
// image, upload items), and the application does the rest: it transitions the
// layouts with imageBarrier, prepares and records the upload batch, submits with
// a fence, waits for the fence, copies the image into a buffer, reads that buffer
// back through a ReadbackBatch and retires the staging buffers. For every Vulkan
// format the Integration maps to and the device supports, every (mip, layer)
// read back must equal the ImageData's pixels, and the validation layer must stay
// silent.
//
// GPU test on a headless Instance (no window system). A format the device cannot
// create or use for transfer and sampling is reported as SKIP for that format
// with the reason, never as PASS; if no format could be tested the whole test
// reports SKIP. The validation layer must be available: without it the test
// fails. No linker or platform mechanism is used.
#include <terreate/integration/asset_graphics/image_upload.hpp>

#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/barriers.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/format_properties.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/readback_batch.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/synchronization.hpp>
#include <terreate/graphics/transfer_commands.hpp>
#include <terreate/graphics/upload_batch.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace asset = terreate::asset;
namespace graphics = terreate::graphics;
namespace ag = terreate::integration::asset_graphics;

const char *test_name = "integration.asset_graphics.image";
int failures = 0;
std::size_t tested_formats = 0;

bool check(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message.c_str());
  }
  return condition;
}

int blocked(const char *message) {
  std::fprintf(stderr, "%s: validation=BLOCKED: %s\n", test_name, message);
  std::fflush(stderr);
  return EXIT_FAILURE;
}

int skip(const char *reason) {
  std::printf("%s: image=SKIP reason=%s\n", test_name, reason);
  return EXIT_SUCCESS;
}

bool hasName(const std::vector<std::string> &names, std::string_view wanted) {
  return std::ranges::any_of(names, [&](const auto &name) { return name == wanted; });
}

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
  return !result && result.error().code() == make_error_code(error);
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        std::fprintf(stderr, "integration.asset_graphics.image: validation message: %s\n",
                     event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

constexpr std::uint64_t wait_ns = 10'000'000'000;

// The application's side of the upload: everything the Integration leaves to it.
struct Gpu {
  graphics::Instance &instance;
  const graphics::PhysicalDeviceCandidate &candidate;
  graphics::Device &device;
  graphics::Allocator &allocator;
  graphics::Queue queue;
};

struct Case {
  asset::PixelFormat format;
  asset::ColorInterpretation interpretation;
  vk::Format expected;
};

constexpr std::array cases{
    Case{asset::PixelFormat::r8, asset::ColorInterpretation::linear, vk::Format::eR8Unorm},
    Case{asset::PixelFormat::r8, asset::ColorInterpretation::srgb, vk::Format::eR8Srgb},
    Case{asset::PixelFormat::rg8, asset::ColorInterpretation::linear, vk::Format::eR8G8Unorm},
    Case{asset::PixelFormat::rg8, asset::ColorInterpretation::srgb, vk::Format::eR8G8Srgb},
    Case{asset::PixelFormat::rgba8, asset::ColorInterpretation::linear, vk::Format::eR8G8B8A8Unorm},
    Case{asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, vk::Format::eR8G8B8A8Srgb},
    Case{asset::PixelFormat::r16, asset::ColorInterpretation::linear, vk::Format::eR16Unorm},
    Case{asset::PixelFormat::rg16, asset::ColorInterpretation::linear, vk::Format::eR16G16Unorm},
    Case{asset::PixelFormat::rgba16, asset::ColorInterpretation::linear,
         vk::Format::eR16G16B16A16Unorm},
    Case{asset::PixelFormat::r32f, asset::ColorInterpretation::linear, vk::Format::eR32Sfloat},
    Case{asset::PixelFormat::rg32f, asset::ColorInterpretation::linear, vk::Format::eR32G32Sfloat},
    Case{asset::PixelFormat::rgba32f, asset::ColorInterpretation::linear,
         vk::Format::eR32G32B32A32Sfloat},
};

// A 5x3 image with 2 mips and 2 layers. Layer 0 has one texel of padding per row, layer 1 is
// tight, so both pitch kinds reach the GPU. Floats are finite and the pattern is deterministic.
asset::ImageData makeImage(const Case &c) {
  constexpr std::uint32_t layers = 2;
  constexpr std::uint32_t mips = 2;
  asset::ImageData image;
  image.format = c.format;
  image.interpretation = c.interpretation;
  image.extent = {5, 3, 1};
  image.mip_levels = mips;
  image.array_layers = layers;
  const std::size_t texel = asset::byteSize(c.format);
  std::size_t offset = 0;
  for (std::uint32_t layer = 0; layer < layers; ++layer) {
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
      const auto extent = image.mipExtent(mip);
      const std::size_t pitch = (extent.width + (layer == 0 ? 1U : 0U)) * texel;
      image.subresources.push_back({mip, layer, offset, pitch, pitch * extent.height});
      offset += pitch * extent.height;
    }
  }
  image.data.resize(offset);
  for (std::size_t i = 0; i < offset; ++i)
    image.data[i] = static_cast<std::byte>((i * 37U + 11U) & 0x3FU); // below 0x40: finite floats
  return image;
}

// Each subresource read back tight, at an offset that is a multiple of 16.
struct Readback {
  std::size_t offset = 0;
  std::size_t tight_bytes = 0;
};

bool roundTrip(Gpu &gpu, const Case &c, bool negative_checks) {
  const std::string label = vk::to_string(c.expected);
  const auto properties = graphics::queryFormatProperties(gpu.instance, gpu.candidate, c.expected);
  const auto needed = vk::FormatFeatureFlagBits2::eTransferSrc |
                      vk::FormatFeatureFlagBits2::eTransferDst |
                      vk::FormatFeatureFlagBits2::eSampledImage;
  if (!properties || !properties->supports(vk::ImageTiling::eOptimal, needed)) {
    std::printf("%s: format=%s SKIP reason=no optimal-tiling transfer and sampled support\n",
                test_name, label.c_str());
    return true;
  }

  const auto data = makeImage(c);
  ag::ImageUploadDescription upload_description;
  upload_description.extra_usage = vk::ImageUsageFlagBits::eTransferSrc;
  auto plan = ag::planImageUpload(data, upload_description);
  if (!check(bool(plan), label + ": planImageUpload"))
    return false;
  check(plan->format == c.expected && plan->image.format == c.expected,
        label + ": the plan maps to the expected format");

  auto image = ag::createImage(gpu.allocator, *plan);
  // The device reported the needed features, so a creation failure is a failure, not a SKIP.
  if (!check(bool(image), label + ": the image is created although the features are supported"))
    return false;
  ++tested_formats;
  const auto *configuration = image->configuration();
  check(configuration != nullptr && configuration->format == c.expected &&
            configuration->mip_levels == 2 && configuration->array_layers == 2 &&
            configuration->extent.width == 5 && configuration->extent.height == 3 &&
            (configuration->usage & vk::ImageUsageFlagBits::eTransferDst) &&
            (configuration->usage & vk::ImageUsageFlagBits::eSampled) &&
            (configuration->usage & vk::ImageUsageFlagBits::eTransferSrc),
        label + ": the created image is the planned one");

  auto items = ag::imageUploadItems(*plan, data, *image);
  if (!check(bool(items), label + ": imageUploadItems"))
    return false;
  bool borrows = items->size() == plan->subresources.size();
  for (std::size_t i = 0; borrows && i < items->size(); ++i) {
    const auto bytes = data.bytes(plan->subresources[i].mip, plan->subresources[i].layer);
    borrows = bytes && (*items)[i].destination == &*image &&
              (*items)[i].copy_layout == plan->copy_layout &&
              (*items)[i].bytes.data() == bytes->data() &&
              (*items)[i].bytes.size() == bytes->size();
  }
  check(borrows, label + ": every item borrows the image and the ImageData's bytes");

  if (negative_checks) {
    // A plan that disagrees with its own data, or buffers that are not the plan's.
    auto tampered = *plan;
    tampered.image.usage = vk::ImageUsageFlagBits::eTransferDst;
    check(hasError(ag::createImage(gpu.allocator, tampered), ag::ImageUploadError::plan_mismatch),
          "createImage refuses a plan without Sampled");
    tampered = *plan;
    tampered.subresources.pop_back();
    check(hasError(ag::createImage(gpu.allocator, tampered), ag::ImageUploadError::plan_mismatch),
          "createImage refuses a plan with a missing subresource");
    tampered = *plan;
    tampered.copy_layout = vk::ImageLayout::eGeneral;
    check(
        hasError(ag::imageUploadItems(tampered, data, *image), ag::ImageUploadError::plan_mismatch),
        "imageUploadItems refuses another copy layout");
    auto other = data;
    other.extent.width = 4;
    check(hasError(ag::imageUploadItems(*plan, other, *image), ag::ImageUploadError::plan_mismatch),
          "imageUploadItems refuses ImageData of another extent");
    other = data;
    other.subresources[1].offset += 1;
    check(hasError(ag::imageUploadItems(*plan, other, *image), ag::ImageUploadError::plan_mismatch),
          "imageUploadItems refuses a moved subresource");
    auto wrong_plan = *plan;
    wrong_plan.image.extent.width = 4;
    wrong_plan.subresources.clear();
    check(hasError(ag::imageUploadItems(wrong_plan, data, *image),
                   ag::ImageUploadError::plan_mismatch),
          "imageUploadItems refuses an inconsistent plan");
    auto smaller = *plan;
    smaller.image.mip_levels = 1;
    smaller.image.array_layers = 1;
    smaller.subresources.resize(1);
    check(
        hasError(ag::imageUploadItems(smaller, data, *image), ag::ImageUploadError::plan_mismatch),
        "imageUploadItems refuses an image that is not the one created from the plan");
  }

  auto uploaded = graphics::prepareUploadBatch(gpu.allocator,
                                               graphics::UploadBatchDescription{.images = *items});
  if (!check(bool(uploaded), label + ": prepareUploadBatch"))
    return false;

  // The device buffer the image is copied into, one tight slot per subresource.
  std::vector<Readback> slots;
  std::size_t total = 0;
  const std::size_t texel = asset::byteSize(c.format);
  for (const auto &entry : plan->subresources) {
    const std::size_t tight = static_cast<std::size_t>(entry.region.imageExtent.width) *
                              entry.region.imageExtent.height * texel;
    slots.push_back({total, tight});
    total += (tight + 15U) / 16U * 16U;
  }
  graphics::BufferDescription buffer_description;
  buffer_description.size = total;
  buffer_description.usage =
      vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc;
  auto buffer = graphics::createBuffer(gpu.allocator, buffer_description);
  if (!check(bool(buffer), label + ": readback source buffer"))
    return false;
  const std::array<graphics::BufferReadbackItem, 1> reads{
      graphics::BufferReadbackItem{&*buffer, 0, total}};
  auto readback = graphics::prepareReadbackBatch(
      gpu.allocator, graphics::ReadbackBatchDescription{.buffers = reads});
  if (!check(bool(readback), label + ": prepareReadbackBatch"))
    return false;

  graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(gpu.queue.familyIndex());
  auto pool = graphics::createCommandPool(gpu.device, pool_description);
  if (!check(bool(pool), label + ": CommandPool"))
    return false;
  auto command = pool->allocatePrimary();
  if (!check(command && command->begin(), label + ": begin CommandBuffer"))
    return false;

  // The application's own layout transitions, with the layouts the plan states.
  graphics::ImageBarrierDescription to_copy;
  to_copy.src_stage = vk::PipelineStageFlagBits2::eTopOfPipe;
  to_copy.dst_stage = vk::PipelineStageFlagBits2::eTransfer;
  to_copy.dst_access = vk::AccessFlagBits2::eTransferWrite;
  to_copy.old_layout = vk::ImageLayout::eUndefined;
  to_copy.new_layout = plan->copy_layout;
  to_copy.subresource_range = vk::ImageSubresourceRange{
      vk::ImageAspectFlagBits::eColor, 0, plan->image.mip_levels, 0, plan->image.array_layers};
  if (!check(bool(graphics::recordImageBarrier(gpu.device, *command, *image, to_copy)),
             label + ": undefined -> the plan's copy layout"))
    return false;
  if (!check(bool(graphics::recordUploadBatch(gpu.device, *command, *uploaded)),
             label + ": recordUploadBatch"))
    return false;
  graphics::ImageBarrierDescription to_read;
  to_read.src_stage = vk::PipelineStageFlagBits2::eTransfer;
  to_read.src_access = vk::AccessFlagBits2::eTransferWrite;
  to_read.dst_stage = vk::PipelineStageFlagBits2::eTransfer;
  to_read.dst_access = vk::AccessFlagBits2::eTransferRead;
  to_read.old_layout = plan->copy_layout;
  to_read.new_layout = vk::ImageLayout::eTransferSrcOptimal;
  to_read.subresource_range = to_copy.subresource_range;
  if (!check(bool(graphics::recordImageBarrier(gpu.device, *command, *image, to_read)),
             label + ": copy layout -> transfer src"))
    return false;

  for (std::size_t i = 0; i < slots.size(); ++i) {
    const auto &entry = plan->subresources[i];
    VkBufferImageCopy2 out{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    out.bufferOffset = slots[i].offset;
    out.imageSubresource = entry.region.imageSubresource;
    out.imageExtent = entry.region.imageExtent;
    if (!check(bool(graphics::recordCopyImageToBuffer(gpu.device, *command, *image,
                                                      vk::ImageLayout::eTransferSrcOptimal, *buffer,
                                                      std::span{&out, 1})),
               label + ": copy a subresource into the buffer"))
      return false;
  }
  VkMemoryBarrier2 to_readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  to_readback.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_readback.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_readback.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_readback.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  if (!check(bool(graphics::recordBarriers(gpu.device, *command,
                                           {.memory = std::span{&to_readback, 1}})),
             label + ": image copies -> readback barrier"))
    return false;
  if (!check(bool(graphics::recordReadbackBatch(gpu.device, *command, *readback)),
             label + ": recordReadbackBatch"))
    return false;
  VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  if (!check(graphics::recordBarriers(gpu.device, *command, {.memory = std::span{&to_host, 1}}) &&
                 command->end(),
             label + ": readback -> host barrier and end"))
    return false;

  auto fence = graphics::createFence(gpu.device, {});
  if (!check(bool(fence), label + ": Fence"))
    return false;
  int domain = 0;
  const std::array<const graphics::CommandBuffer *, 1> commands{&*command};
  auto receipt = graphics::submitFenced(gpu.device, gpu.queue, *fence, {&domain, 1},
                                        {.command_buffers = commands});
  if (!receipt) {
    // The outcome of a failed submission is unknown: do not run destructors that
    // would free resources the GPU may still use.
    std::fprintf(stderr, "%s: submission failed\n", test_name);
    std::_Exit(blocked("real Queue submission failed"));
  }
  const auto waited = fence->wait(wait_ns);
  if (!waited || *waited != graphics::WaitStatus::satisfied)
    std::_Exit(blocked("the Fence did not prove the GPU work complete"));
  const std::array<graphics::SubmissionReceipt, 1> uses{*receipt};
  auto copied = graphics::tryCopyCompletedReadbackBatch(gpu.device, *readback, uses);
  if (!check(bool(copied), label + ": the completed readback is copied"))
    return false;

  const auto back = copied->dataFor(0);
  bool equal = back.size() == total;
  for (std::size_t i = 0; equal && i < slots.size(); ++i) {
    const auto &entry = plan->subresources[i];
    const auto source = data.bytes(entry.mip, entry.layer);
    const std::size_t width_bytes =
        static_cast<std::size_t>(entry.region.imageExtent.width) * texel;
    for (std::uint32_t row = 0; equal && row < entry.region.imageExtent.height; ++row)
      equal =
          source &&
          std::memcmp(back.data() + slots[i].offset + static_cast<std::size_t>(row) * width_bytes,
                      source->data() + static_cast<std::size_t>(row) * entry.row_pitch,
                      width_bytes) == 0;
    if (!equal)
      std::fprintf(stderr, "%s: %s: mip %u layer %u differs\n", test_name, label.c_str(), entry.mip,
                   entry.layer);
  }
  check(equal, label + ": every (mip, layer) reads back equal to the ImageData");

  // The application retires the staging buffers with the real receipt.
  graphics::RetirementQueue retirement;
  const bool retired = retirement.retire(*uploaded, uses) && retirement.retire(*readback, uses);
  check(retired && retirement.pendingCount() == 2, label + ": both staging owners are retired");
  const auto collected = retirement.collect();
  check(collected && collected->collected == 2 && retirement.requireEmpty(),
        label + ": the staging buffers are collected once the Fence is signaled");
  if (failures == 0)
    std::printf("%s: format=%s PASS\n", test_name, label.c_str());
  return failures == 0;
}

int exercise(Gpu &gpu, ValidationRecorder &recorder) {
  bool first = true;
  for (const auto &c : cases) {
    if (!roundTrip(gpu, c, first))
      return EXIT_FAILURE;
    first = false;
  }
  check(recorder.warnings_or_errors.load() == 0, "the validation layer reports nothing");
  if (failures != 0)
    return EXIT_FAILURE;
  if (tested_formats == 0)
    return skip("no mapped format could be created and used on this device");
  std::printf("%s: image=PASS validation-layer=PASS tested=%zu of %zu\n", test_name, tested_formats,
              cases.size());
  return EXIT_SUCCESS;
}

int gpuTest() {
  const auto capabilities = graphics::queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  if (!hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation") ||
      !hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    return blocked("the validation layer and VK_EXT_debug_utils are required");
  graphics::InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-asset-graphics-image";
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = graphics::DebugUtilsMode::required;
  const auto plan = graphics::resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance");
  ValidationRecorder recorder;
  auto instance = graphics::createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("Vulkan Instance");
  auto inventory = graphics::queryPhysicalDevices(*instance);
  if (!inventory)
    return blocked("physical device enumeration");

  for (const auto &candidate : *inventory) {
    graphics::DeviceDescription request;
    graphics::DeviceQueueRequest queue_request;
    queue_request.caller_id = "main";
    queue_request.strength = graphics::RequirementStrength::required;
    queue_request.required_flags = vk::QueueFlagBits::eGraphics;
    request.queue_requests = {queue_request};
    request.required_features_13.synchronization2 = VK_TRUE;
    const auto device_plan = graphics::resolveDevice(candidate, request);
    if (!device_plan)
      continue;
    int status = EXIT_FAILURE;
    {
      auto device = graphics::createDevice(*instance, *device_plan);
      if (!device)
        return blocked("Device with synchronization2");
      const graphics::Queue queue = device->queue("main");
      if (!queue.valid())
        return blocked("graphics Queue");
      auto allocator = graphics::createAllocator(*instance, candidate, *device);
      if (!allocator)
        return blocked("Allocator");
      Gpu gpu{*instance, candidate, *device, *allocator, queue};
      status = exercise(gpu, recorder);
    }
    if (status != EXIT_SUCCESS)
      return status;
    check(recorder.warnings_or_errors.load() == 0,
          "the validation layer reports nothing after destruction");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  return skip("no device with a graphics queue and synchronization2");
}

} // namespace

int main() { return gpuTest(); }

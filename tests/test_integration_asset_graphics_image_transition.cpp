// #468: the explicit, opt-in layout transition of the image upload. The application plans the
// upload with LayoutTransition::shader_read_only, creates the image, records the upload batch and
// then recordImageUploadTransition into one command buffer, submits with a fence and waits for it
// explicitly. The layout the plan states is then proven on the GPU: the same command buffer moves
// the image from ShaderReadOnlyOptimal (the layout the plan states; the validation layer reports a
// layout that is anything else) to TransferSrcOptimal, copies every (mip, layer) out and the
// readback must equal the ImageData. The sampled-read access and the consumer stage are checked on
// the plan (CPU test), not by sampling on the GPU. The errors of recordImageUploadTransition are
// checked on the real image and a real command buffer.
//
// GPU test on a headless Instance (no window system). A format the device cannot create or use for
// transfer and sampling is reported as SKIP for that format with the reason, never as PASS; if no
// format could be tested the whole test reports SKIP. The validation layer must be available:
// without it the test fails. No linker or platform mechanism is used.
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

const char *test_name = "integration.asset_graphics.image_transition";
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
  std::printf("%s: transition=SKIP reason=%s\n", test_name, reason);
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
        std::fprintf(stderr,
                     "integration.asset_graphics.image_transition: validation message: %s\n",
                     event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

constexpr std::uint64_t wait_ns = 10'000'000'000;

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
  vk::PipelineStageFlags2 consumer_stages;
};

constexpr std::array cases{
    // The default consumer: a fragment shader.
    Case{asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, vk::Format::eR8G8B8A8Srgb,
         vk::PipelineStageFlagBits2::eFragmentShader},
    Case{asset::PixelFormat::r8, asset::ColorInterpretation::linear, vk::Format::eR8Unorm,
         vk::PipelineStageFlagBits2::eFragmentShader},
    // The application names other consumer stages.
    Case{asset::PixelFormat::rgba32f, asset::ColorInterpretation::linear,
         vk::Format::eR32G32B32A32Sfloat,
         vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eComputeShader},
};

// A 5x3 image with 2 mips and 2 layers. Layer 0 has one texel of padding per row, layer 1 is
// tight. Floats are finite and the pattern is deterministic.
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
    image.data[i] = static_cast<std::byte>((i * 37U + 11U) & 0x3FU);
  return image;
}

graphics::ImageBarrierDescription copyBarrier(const ag::ImageUploadPlan &plan) {
  graphics::ImageBarrierDescription to_copy;
  to_copy.src_stage = vk::PipelineStageFlagBits2::eTopOfPipe;
  to_copy.dst_stage = vk::PipelineStageFlagBits2::eTransfer;
  to_copy.dst_access = vk::AccessFlagBits2::eTransferWrite;
  to_copy.old_layout = vk::ImageLayout::eUndefined;
  to_copy.new_layout = plan.copy_layout;
  to_copy.subresource_range = vk::ImageSubresourceRange{
      vk::ImageAspectFlagBits::eColor, 0, plan.image.mip_levels, 0, plan.image.array_layers};
  return to_copy;
}

// The errors of recordImageUploadTransition, on the real image and command buffers.
void errorChecks(Gpu &gpu, const std::string &label, const ag::ImageUploadPlan &plan,
                 const graphics::Image &image, const graphics::CommandBuffer &recording,
                 const graphics::CommandBuffer &not_recording, const asset::ImageData &data) {
  auto none_plan = terreate::unwrap(ag::planImageUpload(data));
  check(hasError(ag::recordImageUploadTransition(gpu.device, recording, none_plan, image),
                 ag::ImageUploadError::no_transition),
        label + ": a plan without a transition is no_transition");

  const auto mismatch = [&](auto edit, const char *what) {
    auto copy = plan;
    edit(copy);
    check(hasError(ag::recordImageUploadTransition(gpu.device, recording, copy, image),
                   ag::ImageUploadError::plan_mismatch),
          label + ": " + what);
  };
  mismatch([](ag::ImageUploadPlan &p) { p.final_layout = vk::ImageLayout::eGeneral; },
           "a wrong final layout is plan_mismatch");
  mismatch([](ag::ImageUploadPlan &p) { p.transition_barriers.clear(); },
           "a missing barrier is plan_mismatch");
  mismatch([](ag::ImageUploadPlan &p) { p.transition_barriers.front().src_queue_family = 1; },
           "an ownership transfer in the barrier is plan_mismatch");
  mismatch(
      [](ag::ImageUploadPlan &p) {
        p.transition_barriers.front().subresource_range.levelCount = 1;
      },
      "a narrowed range is plan_mismatch");
  mismatch([](ag::ImageUploadPlan &p) { p.transition_barriers.front().dst_stage = {}; },
           "an empty destination stage is plan_mismatch");
  mismatch([](ag::ImageUploadPlan &p) { p.layout_transition = ag::LayoutTransition::none; },
           "barriers on a plan without a transition are plan_mismatch");

  // An image that is not the plan's image.
  asset::ImageData small;
  small.format = data.format;
  small.interpretation = data.interpretation;
  small.extent = {4, 4, 1};
  const auto layout = terreate::unwrap(asset::packedLayout(small.format, small.extent, 1, 1));
  small.data.assign(layout.byte_size, std::byte{0});
  small.subresources = layout.subresources;
  auto other = ag::createImage(gpu.allocator, terreate::unwrap(ag::planImageUpload(small)));
  if (check(bool(other), label + ": a second image"))
    check(hasError(ag::recordImageUploadTransition(gpu.device, recording, plan, *other),
                   ag::ImageUploadError::plan_mismatch),
          label + ": an image that is not the plan's image is plan_mismatch");

  // A command buffer that is not recording is the Graphics error, not an Integration one.
  const auto refused = ag::recordImageUploadTransition(gpu.device, not_recording, plan, image);
  check(!refused && refused.error().code().category() != ag::image_upload_error_category(),
        label + ": a command buffer that is not recording is refused by Graphics");
}

bool roundTrip(Gpu &gpu, const Case &c, bool error_checks) {
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
  upload_description.layout_transition = ag::LayoutTransition::shader_read_only;
  upload_description.consumer_stages = c.consumer_stages;
  auto plan = ag::planImageUpload(data, upload_description);
  if (!check(bool(plan), label + ": planImageUpload"))
    return false;
  if (!check(plan->transition_barriers.size() == 1 &&
                 plan->final_layout == vk::ImageLayout::eShaderReadOnlyOptimal &&
                 plan->transition_barriers.front().dst_stage == c.consumer_stages,
             label + ": the plan states the transition to ShaderReadOnlyOptimal"))
    return false;

  auto image = ag::createImage(gpu.allocator, *plan);
  if (!check(bool(image), label + ": the image is created although the features are supported"))
    return false;
  ++tested_formats;
  auto items = ag::imageUploadItems(*plan, data, *image);
  if (!check(bool(items), label + ": imageUploadItems"))
    return false;
  auto uploaded = graphics::prepareUploadBatch(gpu.allocator,
                                               graphics::UploadBatchDescription{.images = *items});
  if (!check(bool(uploaded), label + ": prepareUploadBatch"))
    return false;

  // The device buffer the image is copied into, one tight slot per subresource.
  std::vector<std::size_t> offsets;
  std::size_t total = 0;
  const std::size_t texel = asset::byteSize(c.format);
  for (const auto &entry : plan->subresources) {
    const std::size_t tight = static_cast<std::size_t>(entry.region.imageExtent.width) *
                              entry.region.imageExtent.height * texel;
    offsets.push_back(total);
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
  auto idle = pool->allocatePrimary();
  if (!check(command && idle && command->begin(), label + ": begin CommandBuffer"))
    return false;

  // The application's own transition into the copy layout, then the upload batch, then the
  // Integration's transition: the only barrier the Integration records.
  if (!check(bool(graphics::recordImageBarrier(gpu.device, *command, *image, copyBarrier(*plan))),
             label + ": undefined -> the plan's copy layout"))
    return false;
  if (!check(bool(graphics::recordUploadBatch(gpu.device, *command, *uploaded)),
             label + ": recordUploadBatch"))
    return false;
  if (error_checks)
    errorChecks(gpu, label, *plan, *image, *command, *idle, data);
  if (!check(bool(ag::recordImageUploadTransition(gpu.device, *command, *plan, *image)),
             label + ": recordImageUploadTransition"))
    return false;

  // The proof: leave the layout the plan states (an old layout that is not the image's actual
  // one is a validation error), then read every subresource out.
  graphics::ImageBarrierDescription to_read;
  to_read.src_stage = c.consumer_stages;
  to_read.dst_stage = vk::PipelineStageFlagBits2::eTransfer;
  to_read.dst_access = vk::AccessFlagBits2::eTransferRead;
  to_read.old_layout = plan->final_layout;
  to_read.new_layout = vk::ImageLayout::eTransferSrcOptimal;
  to_read.subresource_range = plan->transition_barriers.front().subresource_range;
  if (!check(bool(graphics::recordImageBarrier(gpu.device, *command, *image, to_read)),
             label + ": the plan's final layout -> transfer src"))
    return false;
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    const auto &entry = plan->subresources[i];
    VkBufferImageCopy2 out{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    out.bufferOffset = offsets[i];
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
  for (std::size_t i = 0; equal && i < offsets.size(); ++i) {
    const auto &entry = plan->subresources[i];
    const auto source = data.bytes(entry.mip, entry.layer);
    const std::size_t width_bytes =
        static_cast<std::size_t>(entry.region.imageExtent.width) * texel;
    for (std::uint32_t row = 0; equal && row < entry.region.imageExtent.height; ++row)
      equal = source &&
              std::memcmp(back.data() + offsets[i] + static_cast<std::size_t>(row) * width_bytes,
                          source->data() + static_cast<std::size_t>(row) * entry.row_pitch,
                          width_bytes) == 0;
    if (!equal)
      std::fprintf(stderr, "%s: %s: mip %u layer %u differs\n", test_name, label.c_str(), entry.mip,
                   entry.layer);
  }
  check(equal, label + ": every (mip, layer) reads back equal after the transition");

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
  std::printf("%s: transition=PASS validation-layer=PASS tested=%zu of %zu\n", test_name,
              tested_formats, cases.size());
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
  description.application_name = "terreate-asset-graphics-image-transition";
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

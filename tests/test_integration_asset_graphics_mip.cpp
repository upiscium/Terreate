// #467: explicit, opt-in CPU mip generation (MipGeneration::cpu_box) on a real device.
//
// The source is a single-level rgba8 sRGB image with straight alpha, 5x3 pixels and two layers
// (layer 0 has padded rows). The expected bytes are made without the Integration's help:
// `asset::generateMipChain` by hand, then `planImageUpload` of the result (the lower-level
// equivalent), tight bytes of every (mip, layer). Then every public path uploads with
// `cpu_box` and the image is read back level by level and compared byte for byte:
// (a) the explicit steps (plan with cpu_box, createImage, the application's own barrier, the
//     upload batch, recordImageUploadTransition, its own fence and retirement);
// (b) submitImageUpload and (c) uploadImageBlocking with mip_generation = cpu_box.
// The plan must show three levels (5x3, 2x1, 1x1) before anything is created.
//
// GPU test on a headless Instance (no window system). A format the device cannot create or
// use for transfer and sampling is reported as SKIP for that format with the reason, never
// as PASS; if no format could be tested the whole test reports SKIP. The validation layer
// must be available: without it the test fails, and it must stay silent. No linker or
// platform mechanism is used.
#include <terreate/integration/asset_graphics/image_upload.hpp>
#include <terreate/integration/asset_graphics/image_upload_convenience.hpp>

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

const char *test_name = "integration.asset_graphics.mip";
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
  std::printf("%s: mip=SKIP reason=%s\n", test_name, reason);
  return EXIT_SUCCESS;
}

// The generated image of a plan, or an empty image when the plan has none (the checks then fail).
const asset::ImageData &generatedOf(const ag::ImageUploadPlan &plan) {
  static const asset::ImageData none;
  return plan.generated ? *plan.generated : none;
}

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
  return !result && result.error().code() == make_error_code(error);
}

bool hasName(const std::vector<std::string> &names, std::string_view wanted) {
  return std::ranges::any_of(names, [&](const auto &name) { return name == wanted; });
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        std::fprintf(stderr, "%s: validation message: %s\n", test_name, event.message.c_str());
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
};

constexpr std::array cases{
    Case{asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, vk::Format::eR8G8B8A8Srgb},
};

// A 5x3 image with ONE mip and 2 layers. Layer 0 has one texel of padding per row, layer 1 is
// tight. The pattern is deterministic and has varied alpha.
asset::ImageData makeImage(const Case &c) {
  constexpr std::uint32_t layers = 2;
  asset::ImageData image;
  image.format = c.format;
  image.interpretation = c.interpretation;
  image.alpha = asset::AlphaMode::straight;
  image.extent = {5, 3, 1};
  image.mip_levels = 1;
  image.array_layers = layers;
  const std::size_t texel = asset::byteSize(c.format);
  std::size_t offset = 0;
  for (std::uint32_t layer = 0; layer < layers; ++layer) {
    const std::size_t pitch = (image.extent.width + (layer == 0 ? 1U : 0U)) * texel;
    image.subresources.push_back({0, layer, offset, pitch, pitch * image.extent.height});
    offset += pitch * image.extent.height;
  }
  image.data.resize(offset);
  for (std::size_t i = 0; i < offset; ++i)
    image.data[i] = static_cast<std::byte>((i * 37U + 11U) & 0xFFU);
  return image;
}

// The tight bytes of every (mip, layer) of the ImageData, in plan order.
std::vector<std::byte> tightBytes(const asset::ImageData &data, const ag::ImageUploadPlan &plan) {
  std::vector<std::byte> out;
  const std::size_t texel = asset::byteSize(data.format);
  for (const auto &entry : plan.subresources) {
    const auto source = data.bytes(entry.mip, entry.layer);
    const std::size_t width_bytes =
        static_cast<std::size_t>(entry.region.imageExtent.width) * texel;
    for (std::uint32_t row = 0; row < entry.region.imageExtent.height; ++row) {
      const auto *from = source->data() + static_cast<std::size_t>(row) * entry.row_pitch;
      out.insert(out.end(), from, from + width_bytes);
    }
  }
  return out;
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

// Waits on the fence or ends the test as BLOCKED.
void mustWait(const graphics::Fence &fence, const char *what) {
  const auto waited = fence.wait(wait_ns);
  if (!waited || *waited != graphics::WaitStatus::satisfied)
    std::_Exit(blocked(what));
}

// Reads every (mip, layer) of `image`, which is in `layout` and whose upload is complete, back
// with the explicit Graphics API, in a command buffer and fence of its own. The output is the
// tight rows of each subresource, like `tightBytes`. The readback buffer pads each subresource
// to 16 bytes, but the copies never write those padding bytes and a new buffer is not zeroed
// (a driver may hand out recycled memory), so the padding is dropped and never compared.
bool readBack(Gpu &gpu, const graphics::Image &image, const ag::ImageUploadPlan &plan,
              vk::ImageLayout layout, std::size_t texel_size, std::vector<std::byte> &out,
              const std::string &label) {
  std::vector<std::size_t> offsets;
  std::size_t total = 0;
  const std::size_t texel = texel_size;
  for (const auto &entry : plan.subresources) {
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
  if (!check(bool(pool), label + ": readback CommandPool"))
    return false;
  auto command = pool->allocatePrimary();
  if (!check(command && command->begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit),
             label + ": begin readback CommandBuffer"))
    return false;

  // Leave the layout the upload stated (an old layout that is not the image's actual one is a
  // validation error). The upload is complete (the fence was waited on), so nothing needs to
  // be made available here.
  graphics::ImageBarrierDescription to_read;
  to_read.src_stage = vk::PipelineStageFlagBits2::eAllCommands;
  to_read.dst_stage = vk::PipelineStageFlagBits2::eTransfer;
  to_read.dst_access = vk::AccessFlagBits2::eTransferRead;
  to_read.old_layout = layout;
  to_read.new_layout = vk::ImageLayout::eTransferSrcOptimal;
  to_read.subresource_range = vk::ImageSubresourceRange{
      vk::ImageAspectFlagBits::eColor, 0, plan.image.mip_levels, 0, plan.image.array_layers};
  if (!check(bool(graphics::recordImageBarrier(gpu.device, *command, image, to_read)),
             label + ": the stated layout -> transfer src"))
    return false;
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    const auto &entry = plan.subresources[i];
    VkBufferImageCopy2 copy{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    copy.bufferOffset = offsets[i];
    copy.imageSubresource = entry.region.imageSubresource;
    copy.imageExtent = entry.region.imageExtent;
    if (!check(bool(graphics::recordCopyImageToBuffer(gpu.device, *command, image,
                                                      vk::ImageLayout::eTransferSrcOptimal, *buffer,
                                                      std::span{&copy, 1})),
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
  if (!check(bool(fence), label + ": readback Fence"))
    return false;
  static constexpr int domain = 0;
  const std::array<const graphics::CommandBuffer *, 1> commands{&*command};
  auto receipt = graphics::submitFenced(gpu.device, gpu.queue, *fence, {&domain, 1},
                                        {.command_buffers = commands});
  if (!receipt)
    std::_Exit(blocked("readback submission failed"));
  mustWait(*fence, "the readback Fence did not prove the GPU work complete");
  const std::array<graphics::SubmissionReceipt, 1> uses{*receipt};
  auto copied = graphics::tryCopyCompletedReadbackBatch(gpu.device, *readback, uses);
  if (!check(bool(copied), label + ": the completed readback is copied"))
    return false;
  const auto back = copied->dataFor(0);
  if (!check(back.size() >= total, label + ": the readback holds every subresource"))
    return false;
  out.clear();
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    const auto &extent = plan.subresources[i].region.imageExtent;
    const std::size_t tight = static_cast<std::size_t>(extent.width) * extent.height * texel;
    out.insert(out.end(), back.begin() + static_cast<std::ptrdiff_t>(offsets[i]),
               back.begin() + static_cast<std::ptrdiff_t>(offsets[i] + tight));
  }
  graphics::RetirementQueue retirement;
  const bool retired = static_cast<bool>(retirement.retire(*readback, uses));
  const auto collected = retirement.collect();
  check(retired && collected && collected->collected == 1 && retirement.requireEmpty(),
        label + ": the readback staging is retired");
  return true;
}

// The explicit steps, written out as the documentation says, with the application's own
// command buffer, fence, wait and retirement.
bool explicitUpload(Gpu &gpu, const asset::ImageData &data, vk::ImageUsageFlags usage,
                    std::vector<std::byte> &out, const std::string &label) {
  ag::ImageUploadDescription description;
  description.extra_usage = usage;
  description.layout_transition = ag::LayoutTransition::shader_read_only;
  description.mip_generation = ag::MipGeneration::cpu_box;
  auto plan = ag::planImageUpload(data, description);
  if (!check(bool(plan), label + ": explicit plan"))
    return false;
  auto image = ag::createImage(gpu.allocator, *plan);
  if (!check(bool(image), label + ": explicit createImage"))
    return false;
  auto items = ag::imageUploadItems(*plan, data, *image);
  if (!check(bool(items), label + ": explicit items"))
    return false;

  // An inconsistent plan is refused on the real image (createImage as well).
  const auto mismatch = [&](auto edit, const char *what) {
    auto forged = *plan;
    edit(forged);
    check(hasError(ag::imageUploadItems(forged, data, *image), ag::ImageUploadError::plan_mismatch),
          label + ": " + what);
    check(hasError(ag::createImage(gpu.allocator, forged), ag::ImageUploadError::plan_mismatch),
          label + ": createImage: " + what);
  };
  mismatch([](ag::ImageUploadPlan &p) { p.generated.reset(); },
           "a cpu_box plan without its generated image is plan_mismatch");
  mismatch([](ag::ImageUploadPlan &p) { p.mip_generation = ag::MipGeneration::none; },
           "a none plan that owns a generated image is plan_mismatch");
  mismatch([](ag::ImageUploadPlan &p) { p.generated->mip_levels = 1; },
           "a generated image with other levels than the plan is plan_mismatch");
  // The source image must be the one the plan was made from: a chain is not.
  const auto chain = asset::generateMipChain(data);
  check(chain && hasError(ag::imageUploadItems(*plan, *chain, *image),
                          ag::ImageUploadError::plan_mismatch),
        label + ": a source that already has mips is plan_mismatch");
  auto staging = graphics::prepareUploadBatch(gpu.allocator,
                                              graphics::UploadBatchDescription{.images = *items});
  if (!check(bool(staging), label + ": explicit prepareUploadBatch"))
    return false;
  graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(gpu.queue.familyIndex());
  auto pool = graphics::createCommandPool(gpu.device, pool_description);
  if (!check(bool(pool), label + ": explicit CommandPool"))
    return false;
  auto command = pool->allocatePrimary();
  if (!check(command && command->begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit) &&
                 graphics::recordImageBarrier(gpu.device, *command, *image, copyBarrier(*plan)) &&
                 graphics::recordUploadBatch(gpu.device, *command, *staging) &&
                 ag::recordImageUploadTransition(gpu.device, *command, *plan, *image) &&
                 command->end(),
             label + ": explicit recording"))
    return false;
  auto fence = graphics::createFence(gpu.device, {});
  if (!check(bool(fence), label + ": explicit Fence"))
    return false;
  static constexpr int domain = 0;
  const std::array<const graphics::CommandBuffer *, 1> commands{&*command};
  auto receipt = graphics::submitFenced(gpu.device, gpu.queue, *fence, {&domain, 1},
                                        {.command_buffers = commands});
  if (!receipt)
    std::_Exit(blocked("explicit submission failed"));
  mustWait(*fence, "the explicit Fence did not prove the GPU work complete");
  const std::array<graphics::SubmissionReceipt, 1> uses{*receipt};
  graphics::RetirementQueue retirement;
  check(retirement.retire(*staging, uses) && retirement.collect() && retirement.requireEmpty(),
        label + ": explicit staging retired");
  return readBack(gpu, *image, *plan, plan->final_layout, asset::byteSize(data.format), out,
                  label + " explicit");
}

// (a) + (b): submitImageUpload, the test's own wait, retirement and read back.
bool submitPath(Gpu &gpu, const asset::ImageData &data, ag::LayoutTransition transition,
                vk::ImageUsageFlags usage, const std::vector<std::byte> &explicit_bytes,
                const std::string &label) {
  graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(gpu.queue.familyIndex());
  auto pool = graphics::createCommandPool(gpu.device, pool_description);
  if (!check(bool(pool), label + ": CommandPool"))
    return false;
  auto command = pool->allocatePrimary();
  if (!check(command && command->begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit),
             label + ": begin"))
    return false;
  auto fence = graphics::createFence(gpu.device, {});
  if (!check(bool(fence), label + ": Fence"))
    return false;
  ag::ImageUploadConvenienceDescription description;
  description.extra_usage = usage;
  description.layout_transition = transition;
  description.mip_generation = ag::MipGeneration::cpu_box;
  static constexpr int domain = 0;
  auto submission = ag::submitImageUpload(gpu.device, gpu.allocator, gpu.queue, *command, *fence,
                                          {&domain, 1}, data, description);
  if (!check(bool(submission), label + ": submitImageUpload")) {
    if (!submission)
      std::fprintf(stderr, "%s: %s\n", test_name, submission.error().code().message().c_str());
    return false;
  }
  check(submission->plan.layout_transition == transition,
        label + ": the plan records the transition");
  check(command->isExecutable(), label + ": the command buffer was ended");

  // The staging owner goes to the RetirementQueue with the receipt; the test itself waits on
  // the fence (submitImageUpload never does).
  const std::array<graphics::SubmissionReceipt, 1> uses{submission->receipt};
  graphics::RetirementQueue retirement;
  if (!check(retirement.retire(submission->staging, uses) && retirement.pendingCount() == 1,
             label + ": the staging owner is retired with the receipt")) {
    // The GPU still owns the resources: wait before they are destroyed.
    mustWait(*fence, "the Fence did not prove the GPU work complete");
    return false;
  }
  mustWait(*fence, "the Fence did not prove the GPU work complete");
  const auto collected = retirement.collect();
  check(collected && collected->collected == 1 && retirement.requireEmpty(),
        label + ": the staging is collected only once the Fence has signaled");

  std::vector<std::byte> bytes;
  if (!readBack(gpu, submission->image, submission->plan, submission->plan.final_layout,
                asset::byteSize(data.format), bytes, label))
    return false;
  check(bytes == tightBytes(generatedOf(submission->plan), submission->plan),
        label + ": the image holds the generated chain");
  check(bytes == explicit_bytes, label + ": same bytes as the explicit steps");
  return true;
}

// (c) uploadImageBlocking: the image is complete and in the final layout on return.
bool blockingPath(Gpu &gpu, const asset::ImageData &data, ag::LayoutTransition transition,
                  vk::ImageUsageFlags usage, const std::vector<std::byte> &explicit_bytes,
                  const std::string &label) {
  ag::ImageUploadConvenienceDescription description;
  description.extra_usage = usage;
  description.layout_transition = transition;
  description.mip_generation = ag::MipGeneration::cpu_box;
  auto result =
      ag::uploadImageBlocking(gpu.device, gpu.allocator, gpu.queue, data, wait_ns, description);
  if (!check(bool(result), label + ": uploadImageBlocking")) {
    if (!result)
      std::fprintf(stderr, "%s: %s\n", test_name, result.error().code().message().c_str());
    return false;
  }
  check(result->plan.layout_transition == transition, label + ": the plan records the transition");
  std::vector<std::byte> bytes;
  if (!readBack(gpu, result->image, result->plan, result->plan.final_layout,
                asset::byteSize(data.format), bytes, label))
    return false;
  check(bytes == tightBytes(generatedOf(result->plan), result->plan),
        label + ": the image holds the generated chain");
  check(bytes == explicit_bytes, label + ": same bytes as the explicit steps");
  return true;
}

bool roundTrip(Gpu &gpu, const Case &c) {
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
  ++tested_formats;
  const auto data = makeImage(c);
  const auto usage = vk::ImageUsageFlagBits::eTransferSrc;

  std::vector<std::byte> explicit_bytes;
  if (!explicitUpload(gpu, data, usage, explicit_bytes, label))
    return false;
  // The lower-level equivalent: generate the chain yourself, plan the result, take its bytes.
  auto chain = asset::generateMipChain(data);
  if (!check(bool(chain) && chain->mip_levels == 3, label + ": generateMipChain by hand"))
    return false;
  const auto reference = ag::planImageUpload(*chain);
  if (!check(bool(reference), label + ": the plan of the generated chain"))
    return false;
  const auto expected = tightBytes(*chain, *reference);
  if (!check(explicit_bytes == expected, label + ": the explicit cpu_box path holds the chain"))
    return false;
  // The plan of the choice shows the same levels and subresources as the hand-made one.
  ag::ImageUploadDescription named;
  named.mip_generation = ag::MipGeneration::cpu_box;
  const auto plan = ag::planImageUpload(data, named);
  if (!check(bool(plan) && plan->image.mip_levels == 3 && plan->subresources.size() == 6 &&
                 plan->generated && generatedOf(*plan).data == chain->data &&
                 plan->mip_generation == ag::MipGeneration::cpu_box &&
                 plan->image.extent.width == 5 && plan->image.extent.height == 3,
             label + ": the plan shows the generated levels"))
    return false;
  for (std::size_t i = 0; plan && i < plan->subresources.size(); ++i)
    check(plan->subresources[i].data_offset == reference->subresources[i].data_offset &&
              plan->subresources[i].byte_count == reference->subresources[i].byte_count,
          label + ": plan subresource matches the hand-made plan");

  if (!submitPath(gpu, data, ag::LayoutTransition::shader_read_only, usage, expected,
                  label + " submit"))
    return false;
  if (!submitPath(gpu, data, ag::LayoutTransition::none, usage, expected, label + " submit none"))
    return false;
  if (!blockingPath(gpu, data, ag::LayoutTransition::shader_read_only, usage, expected,
                    label + " blocking"))
    return false;
  if (!blockingPath(gpu, data, ag::LayoutTransition::none, usage, expected,
                    label + " blocking none"))
    return false;
  if (failures == 0)
    std::printf("%s: format=%s PASS\n", test_name, label.c_str());
  return failures == 0;
}

// (d) An invalid ImageData, or one that has mips already, is the Asset error, unchanged, and
// nothing is submitted.
void errorsPass(Gpu &gpu) {
  ag::ImageUploadConvenienceDescription named;
  named.mip_generation = ag::MipGeneration::cpu_box;
  auto broken = makeImage(cases.front());
  broken.extent = {0, 3, 1};
  auto result =
      ag::uploadImageBlocking(gpu.device, gpu.allocator, gpu.queue, broken, wait_ns, named);
  check(!result && result.error().code() == make_error_code(asset::ImageDataError::zero_extent),
        "uploadImageBlocking returns the asset error unchanged");
  auto rgb = makeImage(cases.front());
  rgb.format = asset::PixelFormat::rgb8;
  rgb.alpha = asset::AlphaMode::none;
  const auto refused =
      ag::uploadImageBlocking(gpu.device, gpu.allocator, gpu.queue, rgb, wait_ns, named);
  check(!refused &&
            refused.error().code() == make_error_code(ag::ImageUploadError::unsupported_format),
        "uploadImageBlocking returns the Integration error unchanged");
  // An image that already has mips is refused, not overwritten, with the Asset error.
  const auto flat = makeImage(cases.front());
  auto with_mips = asset::generateMipChain(flat);
  if (check(bool(with_mips), "a chain to refuse")) {
    const auto again =
        ag::uploadImageBlocking(gpu.device, gpu.allocator, gpu.queue, *with_mips, wait_ns, named);
    check(!again && again.error().code() == make_error_code(asset::MipChainError::already_has_mips),
          "uploadImageBlocking returns the MipChainError unchanged");
  }
}

int exercise(Gpu &gpu, ValidationRecorder &recorder) {
  errorsPass(gpu);
  for (const auto &c : cases)
    if (!roundTrip(gpu, c))
      return EXIT_FAILURE;
  check(recorder.warnings_or_errors.load() == 0, "the validation layer reports nothing");
  if (failures != 0)
    return EXIT_FAILURE;
  if (tested_formats == 0)
    return skip("no mapped format could be created and used on this device");
  std::printf("%s: mip=PASS validation-layer=PASS tested=%zu of %zu\n", test_name, tested_formats,
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
  description.application_name = "terreate-asset-graphics-mip";
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

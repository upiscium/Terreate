#include <terreate/integration/asset_graphics/image_upload_convenience.hpp>

#include <array>
#include <atomic>
#include <optional>
#include <utility>

#include <terreate/graphics/barriers.hpp>
#include <terreate/graphics/command_pool.hpp>

namespace terreate::integration::asset_graphics {
namespace {

// Everything the GPU may still be using after a failed or timed-out wait. It is
// declared in the order of dependence, so it would be destroyed in a valid order.
struct Helpers {
  graphics::Fence fence;
  graphics::CommandPool pool;
  std::optional<ImageUploadSubmission> submission;
  Helpers *next = nullptr;
};

// The head of the list of abandoned helpers. Nothing ever frees them: this is the
// deliberate, visible leak of a timed-out upload.
std::atomic<Helpers *> abandoned{nullptr};

void abandon(Helpers *helpers) noexcept {
  helpers->next = abandoned.load(std::memory_order_relaxed);
  while (!abandoned.compare_exchange_weak(helpers->next, helpers, std::memory_order_release,
                                          std::memory_order_relaxed)) {
  }
}

} // namespace

auto toImageUploadDescription(const ImageUploadConvenienceDescription &description)
    -> ImageUploadDescription {
  ImageUploadDescription result;
  result.extra_usage = description.extra_usage;
  result.allocation = description.allocation;
  result.layout_transition = description.layout_transition;
  result.consumer_stages = description.consumer_stages;
  return result;
}

auto planImageUploadWithDefaults(const asset::ImageData &image,
                                 const ImageUploadConvenienceDescription &description)
    -> terreate::Result<ImageUploadPlan> {
  return planImageUpload(image, toImageUploadDescription(description));
}

auto submitImageUpload(const graphics::Device &device, const graphics::Allocator &allocator,
                       graphics::Queue queue, graphics::CommandBuffer &command,
                       const graphics::Fence &fence, graphics::CompletionIdentity identity,
                       const asset::ImageData &image_data,
                       const ImageUploadConvenienceDescription &description)
    -> terreate::Result<ImageUploadSubmission> {
  auto plan = planImageUploadWithDefaults(image_data, description);
  if (!plan)
    return std::unexpected(plan.error());
  auto image = createImage(allocator, *plan);
  if (!image)
    return std::unexpected(image.error());
  auto items = imageUploadItems(*plan, image_data, *image);
  if (!items)
    return std::unexpected(items.error());
  auto staging =
      graphics::prepareUploadBatch(allocator, graphics::UploadBatchDescription{.images = *items});
  if (!staging)
    return std::unexpected(staging.error());

  // The image is created in eUndefined and the upload batch records no transition:
  // move every mip and layer into the copy layout first. The image has no earlier
  // use, so nothing needs to be made available.
  graphics::ImageBarrierDescription to_copy;
  to_copy.src_stage = vk::PipelineStageFlagBits2::eTopOfPipe;
  to_copy.dst_stage = vk::PipelineStageFlagBits2::eTransfer;
  to_copy.dst_access = vk::AccessFlagBits2::eTransferWrite;
  to_copy.old_layout = vk::ImageLayout::eUndefined;
  to_copy.new_layout = plan->copy_layout;
  to_copy.subresource_range = vk::ImageSubresourceRange{
      vk::ImageAspectFlagBits::eColor, 0, plan->image.mip_levels, 0, plan->image.array_layers};
  if (auto barrier = graphics::recordImageBarrier(device, command, *image, to_copy); !barrier)
    return std::unexpected(barrier.error());

  if (auto recorded = graphics::recordUploadBatch(device, command, *staging); !recorded)
    return std::unexpected(recorded.error());
  if (plan->layout_transition != LayoutTransition::none) {
    if (auto moved = recordImageUploadTransition(device, command, *plan, *image); !moved)
      return std::unexpected(moved.error());
  }
  if (auto ended = command.end(); !ended)
    return std::unexpected(ended.error());
  const std::array<const graphics::CommandBuffer *, 1> commands{&command};
  auto receipt =
      graphics::submitFenced(device, queue, fence, identity, {.command_buffers = commands});
  if (!receipt)
    return std::unexpected(receipt.error());
  return ImageUploadSubmission{std::move(*plan), std::move(*image), std::move(*staging), *receipt};
}

auto uploadImageBlocking(const graphics::Device &device, const graphics::Allocator &allocator,
                         graphics::Queue queue, const asset::ImageData &image_data,
                         std::uint64_t timeout_ns,
                         const ImageUploadConvenienceDescription &description)
    -> terreate::Result<ImageUploadResult> {
  auto fence = graphics::createFence(device, {});
  if (!fence)
    return std::unexpected(fence.error());
  graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  auto pool = graphics::createCommandPool(device, pool_description);
  if (!pool)
    return std::unexpected(pool.error());
  auto command = pool->allocatePrimary();
  if (!command)
    return std::unexpected(command.error());
  if (auto begun = command->begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit); !begun)
    return std::unexpected(begun.error());

  // The identity only has to be valid and unique for this private fence.
  static constexpr int domain = 0;
  auto submission = submitImageUpload(device, allocator, queue, *command, *fence, {&domain, 1},
                                      image_data, description);
  if (!submission)
    return std::unexpected(submission.error());

  auto waited = fence->wait(timeout_ns);
  if (!waited || *waited != graphics::WaitStatus::satisfied) {
    // The GPU may still read the staging memory and write the image, and the
    // command buffer and fence are still in use: destroying any of them would be
    // undefined behavior. They are deliberately leaked.
    abandon(new Helpers{std::move(*fence), std::move(*pool), std::move(*submission)});
    if (!waited)
      return std::unexpected(waited.error());
    return std::unexpected(terreate::Error{make_error_code(ImageUploadError::wait_timeout),
                                           "uploadImageBlocking",
                                           "the fence did not signal in time"});
  }
  // Complete: the staging, command buffer, pool and fence are destroyed here.
  return ImageUploadResult{std::move(submission->plan), std::move(submission->image)};
}

} // namespace terreate::integration::asset_graphics

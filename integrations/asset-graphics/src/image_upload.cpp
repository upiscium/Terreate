#include <terreate/integration/asset_graphics/image_upload.hpp>

#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace terreate::integration::asset_graphics {
namespace {

class ImageUploadErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override {
    return "terreate.integration.asset_graphics.image_upload";
  }
  std::string message(int value) const override {
    switch (static_cast<ImageUploadError>(value)) {
    case ImageUploadError::unsupported_format:
      return "the pixel format has no portable Vulkan format";
    case ImageUploadError::unsupported_image:
      return "only 2D images can be uploaded";
    case ImageUploadError::unaligned_row_pitch:
      return "a row pitch is not a multiple of the texel size";
    case ImageUploadError::size_overflow:
      return "a size does not fit its Vulkan field";
    case ImageUploadError::unsupported_option:
      return "a choice holds an undefined value or a required field is empty";
    case ImageUploadError::plan_mismatch:
      return "the plan, the image data or the image are not consistent with each other";
    case ImageUploadError::no_transition:
      return "the plan states no layout transition";
    case ImageUploadError::wait_timeout:
      return "the fence did not signal within the timeout";
    }
    return "unknown image upload error";
  }
};

// Error construction allocates; on exhaustion a code-only Error is returned,
// which does not allocate.
terreate::Error fail(ImageUploadError error, const char *operation, const std::string &detail) {
  try {
    return terreate::Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{make_error_code(error)};
  }
}

std::optional<vk::Format> toVulkan(asset::PixelFormat format,
                                   asset::ColorInterpretation interpretation) noexcept {
  const bool srgb = interpretation == asset::ColorInterpretation::srgb;
  switch (format) {
  case asset::PixelFormat::r8:
    return srgb ? vk::Format::eR8Srgb : vk::Format::eR8Unorm;
  case asset::PixelFormat::rg8:
    return srgb ? vk::Format::eR8G8Srgb : vk::Format::eR8G8Unorm;
  case asset::PixelFormat::rgba8:
    return srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
  case asset::PixelFormat::r16:
    return vk::Format::eR16Unorm;
  case asset::PixelFormat::rg16:
    return vk::Format::eR16G16Unorm;
  case asset::PixelFormat::rgba16:
    return vk::Format::eR16G16B16A16Unorm;
  case asset::PixelFormat::r32f:
    return vk::Format::eR32Sfloat;
  case asset::PixelFormat::rg32f:
    return vk::Format::eR32G32Sfloat;
  case asset::PixelFormat::rgba32f:
    return vk::Format::eR32G32B32A32Sfloat;
  case asset::PixelFormat::rgb8:
    break;
  }
  return std::nullopt;
}

constexpr std::uint64_t max31 = std::numeric_limits<std::int32_t>::max();

// The barrier `shader_read_only` implies for an image with these mips and layers.
graphics::ImageBarrierDescription shaderReadBarrier(const graphics::ImageDescription &image,
                                                    vk::PipelineStageFlags2 consumer_stages) {
  graphics::ImageBarrierDescription barrier;
  barrier.src_stage = vk::PipelineStageFlagBits2::eTransfer;
  barrier.src_access = vk::AccessFlagBits2::eTransferWrite;
  barrier.dst_stage = consumer_stages;
  barrier.dst_access = vk::AccessFlagBits2::eShaderSampledRead;
  barrier.old_layout = vk::ImageLayout::eTransferDstOptimal;
  barrier.new_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
  barrier.subresource_range = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0,
                                                        image.mip_levels, 0, image.array_layers};
  barrier.src_queue_family = VK_QUEUE_FAMILY_IGNORED;
  barrier.dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
  return barrier;
}

bool sameBarrier(const graphics::ImageBarrierDescription &a,
                 const graphics::ImageBarrierDescription &b) noexcept {
  const auto &ra = a.subresource_range;
  const auto &rb = b.subresource_range;
  return a.src_stage == b.src_stage && a.src_access == b.src_access && a.dst_stage == b.dst_stage &&
         a.dst_access == b.dst_access && a.old_layout == b.old_layout &&
         a.new_layout == b.new_layout && ra.aspectMask == rb.aspectMask &&
         ra.baseMipLevel == rb.baseMipLevel && ra.levelCount == rb.levelCount &&
         ra.baseArrayLayer == rb.baseArrayLayer && ra.layerCount == rb.layerCount &&
         a.src_queue_family == b.src_queue_family && a.dst_queue_family == b.dst_queue_family;
}

bool hasUsage(vk::ImageUsageFlags usage, vk::ImageUsageFlags needed) noexcept {
  return (usage & needed) == needed;
}

// A description of what is wrong with `plan`, or an empty string when it is
// consistent on its own: the image description, the required usage, the copy
// layout and one entry per (mip, layer) in layer-major order.
std::string planProblem(const ImageUploadPlan &plan) {
  const auto &image = plan.image;
  if (plan.format == vk::Format::eUndefined || image.format != plan.format)
    return "image.format differs from the plan's format";
  if (image.dimensions != vk::ImageType::e2D || image.extent.depth != 1 ||
      image.extent.width == 0 || image.extent.height == 0)
    return "the image is not a 2D image of non-zero extent";
  if (image.mip_levels == 0 || image.array_layers == 0)
    return "the image has no mip level or no layer";
  if (image.samples != vk::SampleCountFlagBits::e1)
    return "the image is multisampled";
  if (!hasUsage(image.usage,
                vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled))
    return "image.usage lacks eTransferDst or eSampled";
  if (plan.copy_layout != vk::ImageLayout::eTransferDstOptimal)
    return "copy_layout is not eTransferDstOptimal";
  if (plan.mip_generation != MipGeneration::none)
    return "a reserved choice holds an undefined value";
  switch (plan.layout_transition) {
  case LayoutTransition::none:
    if (plan.final_layout != plan.copy_layout || !plan.transition_barriers.empty())
      return "a plan without a transition has a final layout or barriers of a transition";
    break;
  case LayoutTransition::shader_read_only:
    if (plan.final_layout != vk::ImageLayout::eShaderReadOnlyOptimal ||
        plan.transition_barriers.size() != 1 || !plan.transition_barriers.front().dst_stage ||
        !sameBarrier(plan.transition_barriers.front(),
                     shaderReadBarrier(image, plan.transition_barriers.front().dst_stage)))
      return "the transition barrier is not the one shader_read_only implies";
    break;
  default:
    return "layout_transition holds an undefined value";
  }
  const std::uint64_t expected = static_cast<std::uint64_t>(image.mip_levels) * image.array_layers;
  if (plan.subresources.size() != expected)
    return "the plan does not have one subresource per mip and layer";
  for (std::size_t i = 0; i < plan.subresources.size(); ++i) {
    const auto &entry = plan.subresources[i];
    if (entry.layer != i / image.mip_levels || entry.mip != i % image.mip_levels)
      return "the subresources are not in layer-major order";
  }
  return {};
}

bool configurationMatches(const graphics::Image &destination, const ImageUploadPlan &plan) {
  const auto *configuration = destination.configuration();
  return destination.valid() && configuration != nullptr &&
         configuration->format == plan.image.format &&
         configuration->dimensions == vk::ImageType::e2D &&
         configuration->extent == plan.image.extent &&
         configuration->mip_levels == plan.image.mip_levels &&
         configuration->array_layers == plan.image.array_layers &&
         configuration->samples == vk::SampleCountFlagBits::e1 &&
         hasUsage(configuration->usage, vk::ImageUsageFlagBits::eTransferDst);
}

} // namespace

const std::error_category &image_upload_error_category() noexcept {
  static const ImageUploadErrorCategory category;
  return category;
}
std::error_code make_error_code(ImageUploadError error) noexcept {
  return {static_cast<int>(error), image_upload_error_category()};
}

auto planImageUpload(const asset::ImageData &image, const ImageUploadDescription &description)
    -> terreate::Result<ImageUploadPlan> {
  constexpr const char *operation = "planImageUpload";
  if (const auto valid = asset::validate(image); !valid)
    return std::unexpected(valid.error());
  if (description.mip_generation != MipGeneration::none)
    return std::unexpected(fail(ImageUploadError::unsupported_option, operation,
                                "only MipGeneration::none is defined"));
  if (description.layout_transition != LayoutTransition::none &&
      description.layout_transition != LayoutTransition::shader_read_only)
    return std::unexpected(fail(ImageUploadError::unsupported_option, operation,
                                "only LayoutTransition::none and shader_read_only are defined"));
  if (description.layout_transition == LayoutTransition::shader_read_only &&
      !description.consumer_stages)
    return std::unexpected(fail(ImageUploadError::unsupported_option, operation,
                                "shader_read_only needs at least one consumer stage"));
  const auto format = toVulkan(image.format, image.interpretation);
  if (!format)
    return std::unexpected(fail(ImageUploadError::unsupported_format, operation,
                                "rgb8 has no portable Vulkan format; repack it as rgba8"));
  if (image.extent.depth != 1)
    return std::unexpected(
        fail(ImageUploadError::unsupported_image, operation, "the image has a depth above 1"));

  const std::size_t texel = asset::byteSize(image.format);
  ImageUploadPlan plan;
  plan.format = *format;
  plan.image.format = *format;
  plan.image.extent = vk::Extent3D{image.extent.width, image.extent.height, 1};
  plan.image.dimensions = vk::ImageType::e2D;
  plan.image.mip_levels = image.mip_levels;
  plan.image.array_layers = image.array_layers;
  plan.image.samples = vk::SampleCountFlagBits::e1;
  plan.image.tiling = vk::ImageTiling::eOptimal;
  plan.image.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled |
                     description.extra_usage;
  plan.image.initial_layout = vk::ImageLayout::eUndefined;
  plan.image.allocation = description.allocation;

  plan.layout_transition = description.layout_transition;
  plan.final_layout = plan.copy_layout;
  if (description.layout_transition == LayoutTransition::shader_read_only) {
    plan.final_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    plan.transition_barriers.push_back(shaderReadBarrier(plan.image, description.consumer_stages));
  }

  plan.subresources.reserve(static_cast<std::size_t>(image.mip_levels) * image.array_layers);
  for (std::uint32_t layer = 0; layer < image.array_layers; ++layer) {
    for (std::uint32_t mip = 0; mip < image.mip_levels; ++mip) {
      const auto *entry = image.find(mip, layer);
      auto bytes = image.bytes(mip, layer);
      if (!bytes)
        return std::unexpected(bytes.error());
      if (entry == nullptr)
        return std::unexpected(
            fail(ImageUploadError::plan_mismatch, operation, "a subresource has no table entry"));
      const std::string where = "mip " + std::to_string(mip) + " layer " + std::to_string(layer);
      if (entry->row_pitch % texel != 0)
        return std::unexpected(fail(ImageUploadError::unaligned_row_pitch, operation,
                                    where + ": the row pitch is not a multiple of " +
                                        std::to_string(texel) + " bytes"));
      if (entry->row_pitch > max31)
        return std::unexpected(fail(ImageUploadError::size_overflow, operation,
                                    where + ": a row exceeds 2^31 - 1 bytes"));
      const auto extent = image.mipExtent(mip);
      ImageUploadSubresource subresource;
      subresource.mip = mip;
      subresource.layer = layer;
      subresource.data_offset = entry->offset;
      subresource.byte_count = bytes->size();
      subresource.row_pitch = entry->row_pitch;
      subresource.region.bufferOffset = 0;
      subresource.region.bufferRowLength = static_cast<std::uint32_t>(entry->row_pitch / texel);
      subresource.region.bufferImageHeight = 0;
      subresource.region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
      subresource.region.imageOffset = {0, 0, 0};
      subresource.region.imageExtent = {extent.width, extent.height, 1};
      plan.subresources.push_back(subresource);
    }
  }
  return plan;
}

auto createImage(const graphics::Allocator &allocator, const ImageUploadPlan &plan)
    -> terreate::Result<graphics::Image> {
  if (const auto problem = planProblem(plan); !problem.empty())
    return std::unexpected(fail(ImageUploadError::plan_mismatch, "createImage", problem));
  return graphics::createImage(allocator, plan.image);
}

auto imageUploadItems(const ImageUploadPlan &plan, const asset::ImageData &image,
                      const graphics::Image &destination)
    -> terreate::Result<std::vector<graphics::ImageUploadItem>> {
  constexpr const char *operation = "imageUploadItems";
  if (const auto problem = planProblem(plan); !problem.empty())
    return std::unexpected(fail(ImageUploadError::plan_mismatch, operation, problem));
  if (!configurationMatches(destination, plan))
    return std::unexpected(fail(ImageUploadError::plan_mismatch, operation,
                                "the image is invalid or differs from the plan's image"));
  const auto format = toVulkan(image.format, image.interpretation);
  if (!format || *format != plan.format || image.extent.depth != 1 ||
      image.extent.width != plan.image.extent.width ||
      image.extent.height != plan.image.extent.height ||
      image.mip_levels != plan.image.mip_levels || image.array_layers != plan.image.array_layers)
    return std::unexpected(fail(ImageUploadError::plan_mismatch, operation,
                                "the image data differs from the one the plan was made from"));

  std::vector<graphics::ImageUploadItem> items;
  items.reserve(plan.subresources.size());
  for (const auto &entry : plan.subresources) {
    const auto *found = image.find(entry.mip, entry.layer);
    auto bytes = image.bytes(entry.mip, entry.layer);
    if (found == nullptr || !bytes || found->offset != entry.data_offset ||
        found->row_pitch != entry.row_pitch || bytes->size() != entry.byte_count)
      return std::unexpected(fail(ImageUploadError::plan_mismatch, operation,
                                  "mip " + std::to_string(entry.mip) + " layer " +
                                      std::to_string(entry.layer) +
                                      " differs from the plan's subresource"));
    graphics::ImageUploadItem item;
    item.destination = &destination;
    item.copy_layout = plan.copy_layout;
    item.region = entry.region;
    item.bytes = *bytes;
    items.push_back(item);
  }
  return items;
}

auto recordImageUploadTransition(const graphics::Device &device,
                                 const graphics::CommandBuffer &command,
                                 const ImageUploadPlan &plan, const graphics::Image &image)
    -> terreate::Result<void> {
  constexpr const char *operation = "recordImageUploadTransition";
  if (const auto problem = planProblem(plan); !problem.empty())
    return std::unexpected(fail(ImageUploadError::plan_mismatch, operation, problem));
  if (plan.layout_transition == LayoutTransition::none)
    return std::unexpected(fail(ImageUploadError::no_transition, operation,
                                "the plan was made with LayoutTransition::none"));
  if (!configurationMatches(image, plan))
    return std::unexpected(fail(ImageUploadError::plan_mismatch, operation,
                                "the image is invalid or differs from the plan's image"));
  std::vector<VkImageMemoryBarrier2> barriers;
  barriers.reserve(plan.transition_barriers.size());
  for (const auto &description : plan.transition_barriers)
    barriers.push_back(graphics::imageBarrier(image, description));
  return graphics::recordBarriers(device, command, {.images = barriers});
}

} // namespace terreate::integration::asset_graphics

// #279 part 2: planImageUpload is CPU only. It builds the observable plan from an
// ImageData: the Vulkan format, the image description, the copy layout, one copy
// region per (mip, layer) with its row pitch, and the named errors. No Vulkan
// Instance or device is created; no OS- or linker-specific mechanism is used.
#include <terreate/integration/asset_graphics/image_upload.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
namespace asset = terreate::asset;
namespace graphics = terreate::graphics;
namespace ag = terreate::integration::asset_graphics;

const char *test_name = "integration.asset_graphics.image_plan";
int failures = 0;

bool check(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message.c_str());
  }
  return condition;
}

static_assert(std::is_error_code_enum_v<ag::ImageUploadError>);
static_assert(std::is_enum_v<ag::MipGeneration> && std::is_enum_v<ag::LayoutTransition>);
static_assert(static_cast<int>(ag::MipGeneration::none) == 0 &&
              static_cast<int>(ag::LayoutTransition::none) == 0);

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
  return !result && result.error().code() == make_error_code(error);
}

// An ImageData with the given shape, the rows of every subresource `pad` bytes
// longer than tight, filled with a deterministic pattern.
asset::ImageData makeImage(asset::PixelFormat format, asset::ColorInterpretation interpretation,
                           // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                           std::uint32_t width, std::uint32_t height, std::uint32_t mips = 1,
                           std::uint32_t layers = 1, std::size_t pad = 0) {
  asset::ImageData image;
  image.format = format;
  image.interpretation = interpretation;
  image.extent = {width, height, 1};
  image.mip_levels = mips;
  image.array_layers = layers;
  std::size_t offset = 0;
  const std::size_t texel = asset::byteSize(format);
  for (std::uint32_t layer = 0; layer < layers; ++layer) {
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
      const auto extent = asset::mipExtent(image.extent, mip);
      const std::size_t tight = extent.width * texel;
      const std::size_t pitch = tight + pad;
      image.subresources.push_back({mip, layer, offset, pitch, pitch * extent.height});
      offset += pitch * extent.height;
    }
  }
  image.data.resize(offset);
  for (std::size_t i = 0; i < offset; ++i)
    image.data[i] = static_cast<std::byte>((i * 31U + 7U) & 255U);
  return image;
}

void formatFamilies() {
  using asset::ColorInterpretation;
  using asset::PixelFormat;
  struct Row {
    PixelFormat format;
    ColorInterpretation interpretation;
    vk::Format expected;
    std::size_t texel;
  };
  const std::array rows{
      Row{PixelFormat::r8, ColorInterpretation::srgb, vk::Format::eR8Srgb, 1},
      Row{PixelFormat::r8, ColorInterpretation::linear, vk::Format::eR8Unorm, 1},
      Row{PixelFormat::rg8, ColorInterpretation::srgb, vk::Format::eR8G8Srgb, 2},
      Row{PixelFormat::rg8, ColorInterpretation::linear, vk::Format::eR8G8Unorm, 2},
      Row{PixelFormat::rg8, ColorInterpretation::normal_map, vk::Format::eR8G8Unorm, 2},
      Row{PixelFormat::rgba8, ColorInterpretation::srgb, vk::Format::eR8G8B8A8Srgb, 4},
      Row{PixelFormat::rgba8, ColorInterpretation::linear, vk::Format::eR8G8B8A8Unorm, 4},
      Row{PixelFormat::rgba8, ColorInterpretation::normal_map, vk::Format::eR8G8B8A8Unorm, 4},
      Row{PixelFormat::r16, ColorInterpretation::linear, vk::Format::eR16Unorm, 2},
      Row{PixelFormat::rg16, ColorInterpretation::linear, vk::Format::eR16G16Unorm, 4},
      Row{PixelFormat::rg16, ColorInterpretation::normal_map, vk::Format::eR16G16Unorm, 4},
      Row{PixelFormat::rgba16, ColorInterpretation::linear, vk::Format::eR16G16B16A16Unorm, 8},
      Row{PixelFormat::r32f, ColorInterpretation::linear, vk::Format::eR32Sfloat, 4},
      Row{PixelFormat::rg32f, ColorInterpretation::linear, vk::Format::eR32G32Sfloat, 8},
      Row{PixelFormat::rgba32f, ColorInterpretation::linear, vk::Format::eR32G32B32A32Sfloat, 16},
  };
  for (const auto &row : rows) {
    const auto image = makeImage(row.format, row.interpretation, 5, 3);
    const auto plan = ag::planImageUpload(image);
    const std::string label = vk::to_string(row.expected);
    if (!check(bool(plan), label + ": planned"))
      continue;
    check(plan->format == row.expected && plan->image.format == row.expected,
          label + ": the mapped Vulkan format");
    check(plan->subresources.size() == 1 &&
              plan->subresources[0].byte_count == (2 * 5 + 5) * row.texel &&
              plan->subresources[0].row_pitch == 5 * row.texel &&
              plan->subresources[0].region.bufferRowLength == 5,
          label + ": the byte count and the texel row length follow the texel size");
  }

  // rgb8 has no portable Vulkan format, whatever the interpretation.
  for (const auto interpretation : {ColorInterpretation::srgb, ColorInterpretation::linear}) {
    check(hasError(ag::planImageUpload(makeImage(PixelFormat::rgb8, interpretation, 4, 4)),
                   ag::ImageUploadError::unsupported_format),
          "rgb8 is unsupported_format");
  }
}

void descriptionFields() {
  const auto image = makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, 6, 4);
  const auto plan = terreate::unwrap(ag::planImageUpload(image));
  const auto &desc = plan.image;
  check(desc.extent.width == 6 && desc.extent.height == 4 && desc.extent.depth == 1 &&
            desc.dimensions == vk::ImageType::e2D && desc.mip_levels == 1 &&
            desc.array_layers == 1 && desc.samples == vk::SampleCountFlagBits::e1 &&
            desc.tiling == vk::ImageTiling::eOptimal &&
            desc.initial_layout == vk::ImageLayout::eUndefined,
        "the image description is the 2D, single-sample, optimal image of the data");
  check(desc.usage == (vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled),
        "the usage is TransferDst | Sampled");
  check(plan.copy_layout == vk::ImageLayout::eTransferDstOptimal,
        "the plan states the copy layout");
  check(plan.mip_generation == ag::MipGeneration::none &&
            plan.layout_transition == ag::LayoutTransition::none,
        "the reserved choices are none");

  ag::ImageUploadDescription description;
  description.extra_usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage;
  description.allocation.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  description.allocation.required_flags = vk::MemoryPropertyFlagBits::eDeviceLocal;
  const auto extra = terreate::unwrap(ag::planImageUpload(image, description));
  check(extra.image.usage ==
            (vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled |
             vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage),
        "the extra usage is added, never replacing the required one");
  check(extra.image.allocation.flags == VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT &&
            extra.image.allocation.required_flags == vk::MemoryPropertyFlagBits::eDeviceLocal,
        "the allocation is the application's");
}

void mipAndLayerTable() {
  const auto image =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::linear, 5, 3, 3, 2);
  const auto plan = terreate::unwrap(ag::planImageUpload(image));
  check(plan.image.mip_levels == 3 && plan.image.array_layers == 2, "mips and layers");
  if (!check(plan.subresources.size() == 6, "one entry per (mip, layer)"))
    return;
  for (std::size_t i = 0; i < plan.subresources.size(); ++i) {
    const auto &entry = plan.subresources[i];
    const std::uint32_t layer = static_cast<std::uint32_t>(i / 3);
    const std::uint32_t mip = static_cast<std::uint32_t>(i % 3);
    const auto extent = image.mipExtent(mip);
    const auto *table = image.find(mip, layer);
    const auto bytes = image.bytes(mip, layer);
    check(entry.mip == mip && entry.layer == layer, "layer-major order");
    check(table != nullptr && bytes && entry.data_offset == table->offset &&
              entry.byte_count == bytes->size() && entry.row_pitch == table->row_pitch,
          "the entry matches the ImageData table");
    const auto &region = entry.region;
    check(region.sType == VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 && region.pNext == nullptr &&
              region.bufferOffset == 0 && region.bufferImageHeight == 0 &&
              region.bufferRowLength == extent.width &&
              region.imageSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
              region.imageSubresource.mipLevel == mip &&
              region.imageSubresource.baseArrayLayer == layer &&
              region.imageSubresource.layerCount == 1 && region.imageOffset.x == 0 &&
              region.imageOffset.y == 0 && region.imageOffset.z == 0 &&
              region.imageExtent.width == extent.width &&
              region.imageExtent.height == extent.height && region.imageExtent.depth == 1,
          "the copy region covers the whole mip of the layer");
  }
  // Mip extents are halved down to 1x1: 5x3, 2x1, 1x1.
  check(plan.subresources[1].region.imageExtent.width == 2 &&
            plan.subresources[1].region.imageExtent.height == 1 &&
            plan.subresources[2].region.imageExtent.width == 1,
        "mip extents follow the Asset rule");

  // A table in another order than layer-major still yields the layer-major plan.
  auto shuffled = image;
  std::swap(shuffled.subresources.front(), shuffled.subresources.back());
  const auto reordered = terreate::unwrap(ag::planImageUpload(shuffled));
  check(reordered.subresources[0].mip == 0 && reordered.subresources[0].layer == 0 &&
            reordered.subresources[5].mip == 2 && reordered.subresources[5].layer == 1 &&
            reordered.subresources[0].data_offset == image.find(0, 0)->offset,
        "the plan is layer-major whatever the table order");
}

void paddedRows() {
  // Width 3 rgba8 has 12 tight bytes per row; with 4 bytes of padding the pitch is 16 bytes
  // = 4 texels, which is the row length in texels and the byte count excludes trailing padding.
  const auto image =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::linear, 3, 2, 1, 1, 4);
  const auto plan = terreate::unwrap(ag::planImageUpload(image));
  check(plan.subresources[0].row_pitch == 16 && plan.subresources[0].region.bufferRowLength == 4 &&
            plan.subresources[0].byte_count == 16 + 12,
        "a padded row pitch becomes an explicit texel row length");

  // A pitch that is not a multiple of the texel size cannot be a row length in texels.
  const auto odd =
      makeImage(asset::PixelFormat::r16, asset::ColorInterpretation::linear, 3, 2, 1, 1, 1);
  const auto refused = ag::planImageUpload(odd);
  check(hasError(refused, ag::ImageUploadError::unaligned_row_pitch) &&
            refused.error().detail().find("mip 0 layer 0") != std::string::npos,
        "a row pitch that is not a texel multiple is unaligned_row_pitch");
  // The same with a one-byte texel is fine.
  check(bool(ag::planImageUpload(
            makeImage(asset::PixelFormat::r8, asset::ColorInterpretation::linear, 3, 2, 1, 1, 1))),
        "any pitch is a texel multiple for a one-byte texel");
}

void errors() {
  // An invalid image: the ImageDataError comes back unchanged.
  auto zero = makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, 2, 2);
  zero.extent.width = 0;
  check(hasError(ag::planImageUpload(zero), asset::ImageDataError::zero_extent),
        "an invalid image returns its ImageDataError unchanged");
  auto truncated = makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, 2, 2);
  truncated.data.pop_back();
  check(hasError(ag::planImageUpload(truncated), asset::ImageDataError::data_out_of_bounds),
        "an image whose data is too short returns data_out_of_bounds");
  // The invalid image is reported before the format is looked at.
  auto rgb_zero = makeImage(asset::PixelFormat::rgb8, asset::ColorInterpretation::srgb, 2, 2);
  rgb_zero.mip_levels = 0;
  check(hasError(ag::planImageUpload(rgb_zero), asset::ImageDataError::invalid_mip_count),
        "validate comes before the format mapping");

  // A 3D image (depth above 1) is not uploaded.
  asset::ImageData volume;
  volume.format = asset::PixelFormat::rgba8;
  volume.interpretation = asset::ColorInterpretation::linear;
  volume.extent = {2, 2, 2};
  auto layout = terreate::unwrap(asset::packedLayout(volume.format, volume.extent, 1, 1));
  volume.data.assign(layout.byte_size, std::byte{0});
  volume.subresources = layout.subresources;
  check(hasError(ag::planImageUpload(volume), ag::ImageUploadError::unsupported_image),
        "a 3D image is unsupported_image");

  // A reserved choice with an undefined value is refused, not ignored.
  const auto image = makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, 2, 2);
  ag::ImageUploadDescription forged;
  // The reserved enums have one enumerator on purpose; forging another value is the point.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  forged.mip_generation = static_cast<ag::MipGeneration>(1);
  check(hasError(ag::planImageUpload(image, forged), ag::ImageUploadError::unsupported_option),
        "an undefined mip_generation value is unsupported_option");
  forged = {};
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  forged.layout_transition = static_cast<ag::LayoutTransition>(2);
  check(hasError(ag::planImageUpload(image, forged), ag::ImageUploadError::unsupported_option),
        "an undefined layout_transition value is unsupported_option");

  // The category is named and messages are defined for every value.
  check(std::string{ag::image_upload_error_category().name()} ==
            "terreate.integration.asset_graphics.image_upload",
        "the category name");
  for (int value = 1; value <= 7; ++value)
    check(!ag::image_upload_error_category().message(value).empty() &&
              ag::image_upload_error_category().message(value) != "unknown image upload error",
          "every ImageUploadError has a message");
  check(ag::make_error_code(ag::ImageUploadError::plan_mismatch).category() ==
            ag::image_upload_error_category(),
        "make_error_code uses the category");
}

// #468: the transition is opt-in, named by the target layout. The plan states the layout the
// image ends in and the barrier (layouts, stages, access, the whole subresource range).
void transitionPlan() {
  struct Shape {
    std::uint32_t mips;
    std::uint32_t layers;
  };
  const std::array shapes{Shape{1, 1}, Shape{3, 1}, Shape{1, 4}, Shape{3, 2}};
  for (const auto shape : shapes) {
    const auto image = makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, 8, 8,
                                 shape.mips, shape.layers);
    // Off by default: no final layout change, no barrier.
    const auto off = terreate::unwrap(ag::planImageUpload(image));
    check(off.layout_transition == ag::LayoutTransition::none &&
              off.final_layout == vk::ImageLayout::eTransferDstOptimal &&
              off.transition_barriers.empty(),
          "the default plan states no transition and stays in the copy layout");

    ag::ImageUploadDescription description;
    description.layout_transition = ag::LayoutTransition::shader_read_only;
    const auto plan = terreate::unwrap(ag::planImageUpload(image, description));
    check(plan.layout_transition == ag::LayoutTransition::shader_read_only &&
              plan.final_layout == vk::ImageLayout::eShaderReadOnlyOptimal &&
              plan.copy_layout == vk::ImageLayout::eTransferDstOptimal,
          "shader_read_only ends in ShaderReadOnlyOptimal after the TransferDst copy layout");
    if (!check(plan.transition_barriers.size() == 1, "one barrier covers the whole image"))
      continue;
    const auto &barrier = plan.transition_barriers.front();
    check(barrier.old_layout == vk::ImageLayout::eTransferDstOptimal &&
              barrier.new_layout == vk::ImageLayout::eShaderReadOnlyOptimal,
          "the barrier moves TransferDstOptimal to ShaderReadOnlyOptimal");
    check(barrier.src_stage == vk::PipelineStageFlagBits2::eTransfer &&
              barrier.src_access == vk::AccessFlagBits2::eTransferWrite,
          "the source scope is the transfer write of the upload");
    check(barrier.dst_stage == vk::PipelineStageFlagBits2::eFragmentShader &&
              barrier.dst_access == vk::AccessFlagBits2::eShaderSampledRead,
          "the default consumer is a fragment shader sampled read");
    const auto &range = barrier.subresource_range;
    check(range.aspectMask == vk::ImageAspectFlagBits::eColor && range.baseMipLevel == 0 &&
              range.levelCount == shape.mips && range.baseArrayLayer == 0 &&
              range.layerCount == shape.layers,
          "the range covers every uploaded mip and layer");
    check(barrier.src_queue_family == VK_QUEUE_FAMILY_IGNORED &&
              barrier.dst_queue_family == VK_QUEUE_FAMILY_IGNORED,
          "no queue family ownership transfer");
    check(plan.subresources.size() == static_cast<std::size_t>(shape.mips) * shape.layers &&
              plan.image.mip_levels == shape.mips && plan.image.array_layers == shape.layers,
          "the upload part of the plan is unchanged by the transition");
  }

  // The consumer stages are the application's and land in the destination stage.
  const auto image = makeImage(asset::PixelFormat::r8, asset::ColorInterpretation::linear, 4, 4);
  ag::ImageUploadDescription description;
  description.layout_transition = ag::LayoutTransition::shader_read_only;
  description.consumer_stages =
      vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eComputeShader;
  const auto plan = terreate::unwrap(ag::planImageUpload(image, description));
  check(plan.transition_barriers.size() == 1 && plan.transition_barriers.front().dst_stage ==
                                                    (vk::PipelineStageFlagBits2::eVertexShader |
                                                     vk::PipelineStageFlagBits2::eComputeShader),
        "consumer_stages is the destination stage");

  // Empty consumer stages are refused with a transition and ignored without one.
  description.consumer_stages = {};
  check(hasError(ag::planImageUpload(image, description), ag::ImageUploadError::unsupported_option),
        "a transition without consumer stages is unsupported_option");
  description.layout_transition = ag::LayoutTransition::none;
  check(bool(ag::planImageUpload(image, description)),
        "consumer_stages is ignored without a transition");
}

} // namespace

int main() {
  formatFamilies();
  descriptionFields();
  mipAndLayerTable();
  paddedRows();
  errors();
  transitionPlan();
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("integration.asset_graphics.image_plan: plan=PASS");
  return EXIT_SUCCESS;
}

#ifndef TERREATE_INTEGRATION_ASSET_GRAPHICS_IMAGE_UPLOAD_HPP
#define TERREATE_INTEGRATION_ASSET_GRAPHICS_IMAGE_UPLOAD_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <vector>

#include <terreate/asset/image_data.hpp>
#include <terreate/asset/mip_chain.hpp>
#include <terreate/core/result.hpp>
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/barriers.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/upload_batch.hpp>

/// Connects Asset and Graphics for images, through their public APIs only (#279,
/// part 2; part 1 is `mesh_upload.hpp`). Asset stays free of Vulkan and Graphics
/// stays free of Asset; neither depends on this Integration.
///
/// Getting an `asset::ImageData` onto the GPU takes three explicit steps, and
/// nothing in these steps records a command, submits work, waits, creates an
/// `ImageView` or a `Sampler`, generates mips (unless `MipGeneration::cpu_box` is
/// named, #467) or transitions a layout (unless `LayoutTransition` is named):
///
/// 1. `planImageUpload` describes the conversion on the CPU. The result,
///    `ImageUploadPlan`, is the observable effective state: the Vulkan format,
///    the image description, the copy layout and one copy region per
///    (mip, layer).
/// 2. `createImage` creates the `graphics::Image` the plan describes.
/// 3. `imageUploadItems` gives the `graphics::ImageUploadItem` values the
///    application hands to `graphics::prepareUploadBatch`.
///
/// The application keeps the timing, the queue, the barriers (the plan states
/// the layout the copy needs; record the transitions with
/// `graphics::imageBarrier`) and the retirement.
///
/// One optional fourth step is an explicit opt-in (#468): naming
/// `LayoutTransition::shader_read_only` in the description makes the plan state the
/// barrier that moves the image from the copy layout to the layout it is sampled
/// in, and `recordImageUploadTransition` records exactly that barrier into the
/// application's command buffer after the upload batch. It never submits, waits,
/// transfers queue family ownership or keeps state; see
/// `recordImageUploadTransition` for the synchronization scope and the equivalent
/// `graphics::imageBarrier` code.
namespace terreate::integration::asset_graphics {

/// How mips are produced. Nothing is generated unless the application names a value
/// other than `none` (#467); never a default, and never done by `createImage` or
/// `imageUploadItems`.
enum class MipGeneration : std::uint8_t {
  /// Upload the mip levels of the `ImageData` as they are.
  none,
  /// Generate the mip chain on the CPU with a 2x2 box filter, in `planImageUpload`,
  /// by calling `asset::generateMipChain(image, description.mip_options)`. The
  /// `ImageData` must have the base level only (`mip_levels == 1`; otherwise
  /// `asset::MipChainError::already_has_mips`). The plan owns the generated
  /// `ImageData` (`ImageUploadPlan::generated`) and describes its levels. Works for
  /// the formats the upload supports (every format except `rgb8`; the image must
  /// be 2D). The filter and its rounding are defined at `asset::generateMipChain`.
  /// A GPU blit value may be added later.
  cpu_box,
};

/// Whether the plan states a layout transition after the upload, and to which
/// layout. Opt-in by naming the target layout; never implied, and the default is
/// `none`.
enum class LayoutTransition : std::uint8_t {
  /// State no transition; the image stays in `ImageUploadPlan::copy_layout`
  /// (`eTransferDstOptimal`) and the application transitions it with
  /// `graphics::imageBarrier`.
  none,
  /// The plan states one barrier from `eTransferDstOptimal` to
  /// `eShaderReadOnlyOptimal` (see `ImageUploadPlan::transition_barriers`) that
  /// `recordImageUploadTransition` records.
  shader_read_only,
};

/// What to create and how. Plain data with documented defaults.
struct ImageUploadDescription {
  /// Usage added to the image. `eTransferDst` and `eSampled` are always set; add
  /// for example `eTransferSrc` to read it back.
  vk::ImageUsageFlags extra_usage{};
  /// Memory of the image. The default (VMA automatic selection of an optimally
  /// tiled image) is device local where the device has such memory.
  graphics::ImageAllocationDescription allocation{};
  /// How mips are produced (#467). `none` (the default) uploads the levels of the
  /// `ImageData` as they are; `cpu_box` generates the chain on the CPU in
  /// `planImageUpload` and uploads that.
  MipGeneration mip_generation = MipGeneration::none;
  /// Only with `MipGeneration::cpu_box`: the options handed to
  /// `asset::generateMipChain` (level count, normal renormalization). Ignored with
  /// `none`.
  asset::MipChainOptions mip_options{};
  /// The layout transition the plan states after the upload (#468). `none` (the
  /// default) states no transition; `shader_read_only` states a barrier to
  /// `eShaderReadOnlyOptimal`.
  LayoutTransition layout_transition = LayoutTransition::none;
  /// Only with `LayoutTransition::shader_read_only`: the pipeline stages that read
  /// the image after the transition. They become the destination stage of the
  /// barrier, so a read in a stage not named here is not synchronized with the
  /// upload. The default is the fragment shader; add for example
  /// `eVertexShader` or `eComputeShader` for other consumers. Empty is
  /// `unsupported_option` when a transition is requested. Ignored with `none`.
  vk::PipelineStageFlags2 consumer_stages = vk::PipelineStageFlagBits2::eFragmentShader;
};

/// One (mip, layer) of the image: where its bytes are in `ImageData::data` and
/// the native copy region that places them.
struct ImageUploadSubresource {
  /// The mip level.
  std::uint32_t mip = 0;
  /// The array layer.
  std::uint32_t layer = 0;
  /// Byte offset of the first pixel in `ImageData::data`.
  std::size_t data_offset = 0;
  /// Number of bytes uploaded: from the first pixel to the end of the last row of
  /// this mip (`(height - 1) * row_pitch + width * texel size`), without trailing
  /// padding. This is exactly `ImageData::bytes(mip, layer).size()`.
  std::size_t byte_count = 0;
  /// Byte distance between rows in the data (the `ImageData` row pitch, at least
  /// the tight row size; padding is allowed).
  std::size_t row_pitch = 0;
  /// The region handed to `graphics::prepareUploadBatch`: color aspect, this mip
  /// and layer (one layer), the mip extent, `bufferRowLength` = `row_pitch` in
  /// texels (an explicit texel pitch), `bufferImageHeight` 0. `bufferOffset` is 0
  /// because `prepareUploadBatch` chooses its own staging offset (see
  /// `graphics::PreparedUploadBatch::images()` for the effective one).
  VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
};

/// The result of `planImageUpload`: everything that will be created and
/// uploaded, as plain values the application can inspect, copy and edit before
/// the next step. No Vulkan object exists yet.
///
/// With `MipGeneration::none` the plan holds no pixels: the upload items borrow the
/// pixels from the `ImageData` the plan was made from (see `imageUploadItems`), so
/// the plan stays small and the pixels are copied only once, into staging. With
/// `MipGeneration::cpu_box` the plan owns the generated image (`generated`) and the
/// items borrow from that, so the plan must outlive `graphics::prepareUploadBatch`.
struct ImageUploadPlan {
  /// The Vulkan format (see `planImageUpload` for the mapping).
  vk::Format format = vk::Format::eUndefined;
  /// The image `createImage` will create: format, extent, `mip_levels`,
  /// `array_layers`, 2D, one sample, optimal tiling, usage (always
  /// `eTransferDst | eSampled` plus the extra usage), undefined initial layout
  /// and the allocation.
  graphics::ImageDescription image;
  /// The layout the copy requires: `eTransferDstOptimal`. The Integration
  /// records no transition. Before `graphics::recordUploadBatch` the application
  /// must move every subresource from `eUndefined` to this layout with
  /// `graphics::imageBarrier`, and after the upload to the layout it needs next.
  vk::ImageLayout copy_layout = vk::ImageLayout::eTransferDstOptimal;
  /// One entry per (mip, layer) of the uploaded image (the `ImageData` itself, or
  /// `generated` with `cpu_box`), layer-major: layer 0 with mips 0..n-1, then layer
  /// 1, and so on. Every subresource is uploaded as it is. `image.mip_levels` is
  /// the number of levels, so the application sees what will be uploaded.
  std::vector<ImageUploadSubresource> subresources;
  /// The choice the description named (#467); `none` by default.
  MipGeneration mip_generation = MipGeneration::none;
  /// Only with `MipGeneration::cpu_box`: the generated, validated, tightly packed
  /// chain (the levels listed in `subresources`); empty with `none`. It is the data
  /// the upload items borrow from. Plain data: the application may inspect it, but
  /// editing it (or `subresources`) so that they disagree is `plan_mismatch`.
  std::optional<asset::ImageData> generated;
  /// The transition the description named (#468); `none` by default.
  LayoutTransition layout_transition = LayoutTransition::none;
  /// The layout every subresource is in once the upload batch and, if requested,
  /// the transition have executed: `copy_layout` with `none`,
  /// `eShaderReadOnlyOptimal` with `shader_read_only`.
  vk::ImageLayout final_layout = vk::ImageLayout::eTransferDstOptimal;
  /// The barrier descriptions the transition implies, for `graphics::imageBarrier`
  /// (empty with `none`). With `shader_read_only` there is exactly one, covering
  /// every uploaded mip and layer (color aspect, mips 0..`mip_levels`-1, layers
  /// 0..`array_layers`-1):
  ///
  /// - `old_layout` `eTransferDstOptimal`, `new_layout` `eShaderReadOnlyOptimal`;
  /// - `src_stage` `eTransfer`, `src_access` `eTransferWrite`: the upload copies;
  /// - `dst_stage` the description's `consumer_stages`, `dst_access`
  ///   `eShaderSampledRead`;
  /// - both queue families `VK_QUEUE_FAMILY_IGNORED`: no ownership transfer.
  ///
  /// The scope is one queue and one submission order: the barrier makes the
  /// copies recorded before it available and visible to sampled reads in the
  /// consumer stages recorded after it, on the same queue. Another queue needs
  /// the application's own semaphore and ownership transfer. The values are plain
  /// data: the application may change `dst_stage` before
  /// `recordImageUploadTransition`; any other edit makes the plan inconsistent
  /// (`plan_mismatch`).
  std::vector<graphics::ImageBarrierDescription> transition_barriers;
};

/// The failures of this part of the Integration. An image that fails
/// `asset::validate` is not in this list: its `asset::ImageDataError` is returned
/// unchanged. Errors of `graphics::createImage` (`graphics::ImageError`, native
/// failures) are also returned unchanged, as are those of `graphics::recordBarriers`
/// from `recordImageUploadTransition` (for example a command buffer that is not
/// recording). Values start at 1; new values are appended.
enum class ImageUploadError : std::uint8_t {
  /// The pixel format has no portable Vulkan format (`rgb8`: three-component
  /// 8-bit formats are not required to support sampling or transfer).
  unsupported_format = 1,
  /// The image is not 2D (a depth above 1): the upload is 2D, one layer per
  /// copy.
  unsupported_image,
  /// A row pitch is not a multiple of the texel size, so it cannot be given to
  /// Vulkan as a row length in texels. Repack the image (`asset::packedLayout`).
  unaligned_row_pitch,
  /// A size does not fit its Vulkan field (a row longer than 2^31 - 1 bytes).
  size_overflow,
  /// A choice holds a value that is not defined (`mip_generation`: `none` and
  /// `cpu_box` are; `layout_transition`: `none` and `shader_read_only`).
  unsupported_option,
  /// A plan is not self-consistent, or the `ImageData` or the `Image` is not the
  /// one the plan was made for.
  plan_mismatch,
  /// `recordImageUploadTransition` was given a plan made without a transition
  /// (`LayoutTransition::none`): there is nothing to record.
  no_transition,
  /// `uploadImageBlocking` (image_upload_convenience.hpp): the fence did not signal
  /// within the timeout. The GPU may still use the resources, so they were
  /// deliberately not destroyed.
  wait_timeout,
};

/// The category of ImageUploadError codes (name
/// "terreate.integration.asset_graphics.image_upload").
[[nodiscard]] auto image_upload_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of an ImageUploadError.
[[nodiscard]] auto make_error_code(ImageUploadError error) noexcept -> std::error_code;

/// Step 1, CPU only: describes how `image` becomes a Vulkan image and its copy
/// regions. Nothing is created and no Vulkan function is called.
///
/// Checks run in this order: `asset::validate(image)` (its error is returned
/// unchanged), the choices (`unsupported_option`: an undefined `mip_generation` or
/// `layout_transition`, or empty `consumer_stages` with a transition), the format
/// (`unsupported_format`), the dimensionality (`unsupported_image`), then, with
/// `MipGeneration::cpu_box`, `asset::generateMipChain` (its `asset::MipChainError` and
/// `asset::ImageDataError` are returned unchanged), then each subresource of the image to upload
/// (`unaligned_row_pitch`, `size_overflow`). Nothing is repaired. With `cpu_box` the plan describes
/// the GENERATED levels (`image.mip_levels`, `subresources`) and owns the generated image in
/// `generated`; the `image` argument is only read during this call.
///
/// Format mapping, by pixel format and color interpretation:
///
/// - r8, rg8, rgba8: srgb gives R8_SRGB, R8G8_SRGB, R8G8B8A8_SRGB; linear and
///   normal_map give R8_UNORM, R8G8_UNORM, R8G8B8A8_UNORM.
/// - r16, rg16, rgba16: R16_UNORM, R16G16_UNORM, R16G16B16A16_UNORM.
/// - r32f, rg32f, rgba32f: R32_SFLOAT, R32G32_SFLOAT, R32G32B32A32_SFLOAT.
/// - rgb8: unsupported_format.
///
/// The alpha mode is not part of the plan: Vulkan has no such property, so the
/// shader and the blend state interpret it. Usage is always `eTransferDst |
/// eSampled` plus the application's extra usage. Whether the device supports the
/// format for these usages is not checked here (query it with
/// `graphics::queryFormatProperties`); `createImage` reports what the device
/// refuses. Allocation failure throws `std::bad_alloc`.
[[nodiscard]] auto planImageUpload(const asset::ImageData &image,
                                   const ImageUploadDescription &description = {})
    -> terreate::Result<ImageUploadPlan>;

/// Step 2: creates the image the plan describes with `graphics::createImage`,
/// exactly as `plan.image` says. No data is copied, no GPU work is recorded and
/// no `ImageView` or `Sampler` is created; the image is in `eUndefined`. A plan
/// that is not self-consistent gives `ImageUploadError::plan_mismatch`; creation
/// errors are returned unchanged.
[[nodiscard]] auto createImage(const graphics::Allocator &allocator, const ImageUploadPlan &plan)
    -> terreate::Result<graphics::Image>;

/// Step 3: the upload items for `graphics::prepareUploadBatch`, one per entry of
/// `plan.subresources`, each with `plan.copy_layout`.
///
/// The items borrow from the arguments: `bytes` points into the data to upload and
/// `destination` is `&destination`. That data is `image.data` for a plan made with
/// `MipGeneration::none`, and `plan.generated->data` with `cpu_box` (then `image`
/// must still be the source image the plan was made from: same format mapping,
/// extent and layers, one mip level; its bytes are not read). Keep `image` (with
/// `none`) or `plan` (with `cpu_box`) alive and unchanged until
/// `graphics::prepareUploadBatch` has returned (it copies the bytes into staging
/// and keeps no span), and keep `destination` alive and in place through every
/// GPU use. `ImageUploadError::plan_mismatch` when `image` is not the one the
/// plan was made from (format mapping, extent, mips, layers or any subresource
/// range differs), the plan is not self-consistent (for example `generated` is
/// missing with `cpu_box`) or `destination` is invalid or not the image created
/// from the plan.
[[nodiscard]] auto imageUploadItems(const ImageUploadPlan &plan, const asset::ImageData &image,
                                    const graphics::Image &destination)
    -> terreate::Result<std::vector<graphics::ImageUploadItem>>;

/// Optional step 4 (#468), recording only: records the barrier of
/// `plan.transition_barriers` for `image` with `graphics::recordBarriers` (one
/// `vkCmdPipelineBarrier2`), moving every subresource from the copy layout to
/// `plan.final_layout`. Record it after `graphics::recordUploadBatch` in the
/// same command buffer; the image must be in `plan.copy_layout` then.
///
/// It does not submit, wait, create or retire anything, does not transfer queue
/// family ownership and remembers nothing. The synchronization scope is the one
/// stated in `ImageUploadPlan::transition_barriers`: transfer writes to sampled
/// reads in `ImageUploadDescription::consumer_stages`, on this queue.
///
/// The equivalent lower-level code, with `plan` from `planImageUpload` with
/// `layout_transition = shader_read_only` and `range` covering every mip and layer:
///
/// ```
/// graphics::ImageBarrierDescription barrier;
/// barrier.src_stage = vk::PipelineStageFlagBits2::eTransfer;
/// barrier.src_access = vk::AccessFlagBits2::eTransferWrite;
/// barrier.dst_stage = vk::PipelineStageFlagBits2::eFragmentShader;
/// barrier.dst_access = vk::AccessFlagBits2::eShaderSampledRead;
/// barrier.old_layout = vk::ImageLayout::eTransferDstOptimal;
/// barrier.new_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
/// barrier.subresource_range = range;
/// graphics::recordImageBarrier(device, command, image, barrier);
/// ```
///
/// Errors: `ImageUploadError::no_transition` when the plan states none (nothing
/// is recorded); `ImageUploadError::plan_mismatch` when the plan is not
/// self-consistent (its barrier is not the one `shader_read_only` implies, other
/// than the destination stage) or `image` is invalid or not the image created from
/// the plan; the errors of `graphics::recordBarriers` otherwise (for example a
/// command buffer that is not recording, or a device without synchronization2).
[[nodiscard]] auto
recordImageUploadTransition(const graphics::Device &device, const graphics::CommandBuffer &command,
                            const ImageUploadPlan &plan, const graphics::Image &image)
    -> terreate::Result<void>;

} // namespace terreate::integration::asset_graphics

namespace std {
/// Lets ImageUploadError convert to std::error_code.
template <>
struct is_error_code_enum<terreate::integration::asset_graphics::ImageUploadError> : true_type {};
} // namespace std

#endif // TERREATE_INTEGRATION_ASSET_GRAPHICS_IMAGE_UPLOAD_HPP

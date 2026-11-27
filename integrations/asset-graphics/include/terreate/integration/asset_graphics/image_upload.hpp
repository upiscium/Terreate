#ifndef TERREATE_INTEGRATION_ASSET_GRAPHICS_IMAGE_UPLOAD_HPP
#define TERREATE_INTEGRATION_ASSET_GRAPHICS_IMAGE_UPLOAD_HPP

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>

#include <terreate/asset/image_data.hpp>
#include <terreate/core/result.hpp>
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/upload_batch.hpp>

/// Connects Asset and Graphics for images, through their public APIs only (#279,
/// part 2; part 1 is `mesh_upload.hpp`). Asset stays free of Vulkan and Graphics
/// stays free of Asset; neither depends on this Integration.
///
/// Getting an `asset::ImageData` onto the GPU takes three explicit steps, and
/// nothing in this namespace records a command, submits work, waits, creates an
/// `ImageView` or a `Sampler`, generates mips or transitions a layout:
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
namespace terreate::integration::asset_graphics {

/// How mips are produced. The only value, `none`, uploads exactly the mip levels
/// present in the `ImageData`. Reserved for an explicit opt-in follow-up (#467);
/// nothing is generated implicitly.
enum class MipGeneration : std::uint8_t {
  /// Upload the mip levels of the `ImageData` as they are.
  none,
};

/// Whether the Integration records a layout transition. The only value, `none`,
/// records nothing: the application transitions the image with
/// `graphics::imageBarrier`. Reserved for an explicit opt-in follow-up (#468).
enum class LayoutTransition : std::uint8_t {
  /// Record no transition; `ImageUploadPlan::copy_layout` is the layout the
  /// application must have established.
  none,
};

/// What to create and how. Plain data with documented defaults.
struct ImageUploadDescription {
  /// Usage added to the image. `eTransferDst` and `eSampled` are always set; add
  /// for example `eTransferSrc` to read it back.
  vk::ImageUsageFlags extra_usage{};
  /// Memory of the image. The default (VMA automatic selection of an optimally
  /// tiled image) is device local where the device has such memory.
  graphics::ImageAllocationDescription allocation{};
  /// Reserved: mips are not generated (#467).
  MipGeneration mip_generation = MipGeneration::none;
  /// Reserved: no layout transition is recorded (#468).
  LayoutTransition layout_transition = LayoutTransition::none;
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
/// The plan holds no pixels. The upload items borrow the pixels from the
/// `ImageData` the plan was made from (see `imageUploadItems`), so the plan
/// stays small and the pixels are copied only once, into staging.
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
  /// One entry per (mip, layer) of the `ImageData`, layer-major: layer 0 with
  /// mips 0..n-1, then layer 1, and so on. Every subresource is uploaded as it
  /// is.
  std::vector<ImageUploadSubresource> subresources;
  /// Always `none` (reserved, #467).
  MipGeneration mip_generation = MipGeneration::none;
  /// Always `none` (reserved, #468).
  LayoutTransition layout_transition = LayoutTransition::none;
};

/// The failures of this part of the Integration. An image that fails
/// `asset::validate` is not in this list: its `asset::ImageDataError` is returned
/// unchanged. Errors of `graphics::createImage` (`graphics::ImageError`, native
/// failures) are also returned unchanged. Values start at 1; new values are
/// appended.
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
  /// A reserved choice holds a value that is not defined (only `none` is).
  unsupported_option,
  /// A plan is not self-consistent, or the `ImageData` or the `Image` is not the
  /// one the plan was made for.
  plan_mismatch,
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
/// unchanged), the reserved choices (`unsupported_option`), the format
/// (`unsupported_format`), the dimensionality (`unsupported_image`), then each
/// subresource (`unaligned_row_pitch`, `size_overflow`). Nothing is repaired.
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
/// The items borrow from the arguments: `bytes` points into `image.data` and
/// `destination` is `&destination`. Keep `image` alive and unchanged until
/// `graphics::prepareUploadBatch` has returned (it copies the bytes into staging
/// and keeps no span), and keep `destination` alive and in place through every
/// GPU use. `ImageUploadError::plan_mismatch` when `image` is not the one the
/// plan was made from (format mapping, extent, mips, layers or any subresource
/// range differs) or `destination` is invalid or not the image created from the
/// plan.
[[nodiscard]] auto imageUploadItems(const ImageUploadPlan &plan, const asset::ImageData &image,
                                    const graphics::Image &destination)
    -> terreate::Result<std::vector<graphics::ImageUploadItem>>;

} // namespace terreate::integration::asset_graphics

namespace std {
/// Lets ImageUploadError convert to std::error_code.
template <>
struct is_error_code_enum<terreate::integration::asset_graphics::ImageUploadError> : true_type {};
} // namespace std

#endif // TERREATE_INTEGRATION_ASSET_GRAPHICS_IMAGE_UPLOAD_HPP

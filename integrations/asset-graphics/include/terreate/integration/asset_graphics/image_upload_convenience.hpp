#ifndef TERREATE_INTEGRATION_ASSET_GRAPHICS_IMAGE_UPLOAD_CONVENIENCE_HPP
#define TERREATE_INTEGRATION_ASSET_GRAPHICS_IMAGE_UPLOAD_CONVENIENCE_HPP

#include <cstdint>

#include <terreate/asset/image_data.hpp>
#include <terreate/core/result.hpp>
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/completion.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/synchronization.hpp>
#include <terreate/graphics/upload_batch.hpp>
#include <terreate/integration/asset_graphics/image_upload.hpp>

/// The beginner-oriented layer over the explicit image steps (#463, images; the
/// mesh half is `mesh_upload_convenience.hpp`).
///
/// Everything here is written with the public steps (`planImageUpload`,
/// `createImage`, `imageUploadItems`, `recordImageUploadTransition`), the
/// Graphics barrier and upload batch helpers and `graphics::submitFenced`; the
/// lower-level equivalent is documented step by step in
/// docs/architecture/asset_graphics.md. It is a separate header so that code
/// that only wants the explicit steps does not see it. Two ways to use it:
///
/// - `submitImageUpload` does not wait. The application owns the command buffer,
///   the queue, the fence and the retirement.
/// - `uploadImageBlocking` waits for the GPU, and its name says so. It owns its
///   own command pool, command buffer and fence, and returns only the image.
///
/// Neither creates an `ImageView` or a `Sampler`: the application creates them
/// with `graphics::createImageView` and `graphics::createSampler`.
namespace terreate::integration::asset_graphics {

/// What to create and how, for the convenience. The same choices as
/// `ImageUploadDescription` except for one documented default: the convenience
/// exists so that a first program can sample the image, so `layout_transition`
/// defaults to `LayoutTransition::shader_read_only` here (the explicit steps keep
/// `none`). Set it to `none` to keep the image in `eTransferDstOptimal`.
struct ImageUploadConvenienceDescription {
  /// Usage added to the image (`eTransferDst` and `eSampled` are always set).
  vk::ImageUsageFlags extra_usage{};
  /// Memory of the image (see `ImageUploadDescription::allocation`).
  graphics::ImageAllocationDescription allocation{};
  /// The layout the image is left in. Default `shader_read_only`: after the
  /// upload the image is in `eShaderReadOnlyOptimal`. `none`: it stays in
  /// `eTransferDstOptimal` and the application transitions it.
  LayoutTransition layout_transition = LayoutTransition::shader_read_only;
  /// Only with `shader_read_only`: the stages that read the image afterwards (see
  /// `ImageUploadDescription::consumer_stages`). Default: the fragment shader.
  vk::PipelineStageFlags2 consumer_stages = vk::PipelineStageFlagBits2::eFragmentShader;
};

/// The `ImageUploadDescription` the convenience description means: the same
/// fields, `mip_generation` `none`. Plain data; edit it and call the explicit
/// steps when you need more control.
[[nodiscard]] auto toImageUploadDescription(const ImageUploadConvenienceDescription &description)
    -> ImageUploadDescription;

/// Step 1 of the convenience, CPU only: `planImageUpload(image,
/// toImageUploadDescription(description))`. With the default description the plan
/// has `layout_transition == shader_read_only`, one entry in
/// `transition_barriers` and `final_layout == eShaderReadOnlyOptimal`. Errors are
/// those of `planImageUpload`, unchanged.
[[nodiscard]] auto planImageUploadWithDefaults(const asset::ImageData &image,
                                               const ImageUploadConvenienceDescription &description)
    -> terreate::Result<ImageUploadPlan>;

/// What `submitImageUpload` returns. Move-only.
///
/// The GPU has been given work that reads `staging` and writes `image`, and
/// nothing has proved it complete. Keep all three alive until the fence is
/// signaled, then hand `staging` to a `graphics::RetirementQueue` with `receipt`
/// (`retirement.retire(submission.staging, {&submission.receipt, 1})`) and retire
/// or keep the image as for any image. Destroying this value early is a contract
/// violation that nothing tracks.
struct ImageUploadSubmission {
  /// What was planned (the effective state: format, subresources, the transition
  /// and `final_layout`).
  ImageUploadPlan plan;
  /// The created image. Its contents are valid, and it is in `plan.final_layout`,
  /// once the fence has signaled.
  graphics::Image image;
  /// The staging owner. Ready for `RetirementQueue::retire` with `receipt`.
  graphics::PreparedUploadBatch staging;
  /// The evidence of the one successful submission, for `retire`. Not a
  /// completion claim.
  graphics::SubmissionReceipt receipt;
};

/// Non-blocking upload of `image_data`: plans (`planImageUploadWithDefaults`),
/// creates the image, prepares the staging batch, then records into `command`
///
/// 1. a barrier from `eUndefined` to `plan.copy_layout` for every mip and layer
///    (top of pipe to transfer write: the image has no earlier use),
/// 2. the upload batch (`graphics::recordUploadBatch`),
/// 3. unless `layout_transition` is `none`, `recordImageUploadTransition` (transfer
///    write to sampled reads in `consumer_stages`),
///
/// ends `command` and submits it once with `fence` (`graphics::submitFenced`). It
/// never waits, resets or reuses anything.
///
/// Preconditions, all the application's:
/// - `command` is in the recording state (begun by the application, usually
///   with `eOneTimeSubmit`) and was allocated from a pool of `queue`'s family,
///   which must support transfer, and graphics or compute as well for the default
///   transition (its destination stage is `consumer_stages`); on a transfer-only
///   queue use `LayoutTransition::none`. Nothing else may be recorded into it before
///   the upload; this function ends it, so it cannot be extended afterwards.
/// - `fence` is unsignaled, not in use by another submission, and `identity` is a
///   valid `CompletionIdentity` the application never reuses.
/// - `device` and `allocator` belong together, `queue` belongs to `device`, and
///   the device has synchronization2.
///
/// Lifetimes: `image_data` and `description` are only read during the call and may
/// be destroyed after it returns (the pixels are copied into staging). `device`,
/// `allocator`, `command`'s pool and `fence` must outlive every GPU use of the
/// result. The command buffer and the fence must stay alive and untouched (no
/// reset, no reuse) until the fence is signaled and the staging is retired.
///
/// Visibility: with the default transition the barrier in step 3 makes the copy
/// visible to sampled reads in `consumer_stages` for later work on the same
/// queue. A later submission on the same device also sees it after a host wait on
/// `fence`; another queue needs the application's semaphore and ownership
/// transfer. With `none` no such barrier exists and the image stays in
/// `eTransferDstOptimal`.
///
/// Errors are those of the explicit steps, unchanged (`asset::ImageDataError`,
/// `ImageUploadError`, `graphics::ImageError`, `graphics::UploadError`,
/// `graphics::CommandError`, `graphics::SubmissionError`, native errors). On any
/// error everything this call created (image, staging) is destroyed before
/// returning, which is right for every failure that happens before the native
/// submit and for a rejected submit; a submit that failed with a lost device is
/// the application's lost-device case. After an error `command` is left as it was
/// reached (recording, or ended) and the application resets or discards its pool.
[[nodiscard]] auto submitImageUpload(const graphics::Device &device,
                                     const graphics::Allocator &allocator, graphics::Queue queue,
                                     graphics::CommandBuffer &command, const graphics::Fence &fence,
                                     graphics::CompletionIdentity identity,
                                     const asset::ImageData &image_data,
                                     const ImageUploadConvenienceDescription &description = {})
    -> terreate::Result<ImageUploadSubmission>;

/// What `uploadImageBlocking` returns: the plan and the image. The upload is
/// complete and the image is in `plan.final_layout`.
struct ImageUploadResult {
  ImageUploadPlan plan;
  graphics::Image image;
};

/// BLOCKING upload of `image_data`: creates its own command pool (on `queue`'s
/// family), command buffer and fence, performs `submitImageUpload`, then waits on
/// the fence for at most `timeout_ns` (`UINT64_MAX` waits without limit, 0 only
/// polls and so abandons the resources unless the copy already finished; there is
/// no default), then destroys the helper objects, the staging included. It
/// returns only when the copies and the transition have completed, so the image
/// holds the pixels, is in `plan.final_layout` and any later submission on
/// `device` may use it.
///
/// Use it for start-up code where stalling is acceptable. It owns the submission
/// on `queue`: the application externally synchronizes `queue` and its family as
/// for any `graphics::submit`. `image_data` and `description` are only read during
/// the call; `device`, `allocator` and `queue` must outlive the returned image.
///
/// Errors before the submit are those of `submitImageUpload`. A fence wait that
/// returns a native error is returned unchanged; one that times out returns
/// `ImageUploadError::wait_timeout`. In both of those cases the GPU may still be
/// using the staging memory, the image and the helper objects, so none of them is
/// destroyed: they are deliberately leaked (destroying them would be undefined) and
/// the application should treat the device as lost or hung.
[[nodiscard]] auto uploadImageBlocking(const graphics::Device &device,
                                       const graphics::Allocator &allocator, graphics::Queue queue,
                                       const asset::ImageData &image_data, std::uint64_t timeout_ns,
                                       const ImageUploadConvenienceDescription &description = {})
    -> terreate::Result<ImageUploadResult>;

} // namespace terreate::integration::asset_graphics

#endif // TERREATE_INTEGRATION_ASSET_GRAPHICS_IMAGE_UPLOAD_CONVENIENCE_HPP

#ifndef TERREATE_INTEGRATION_ASSET_GRAPHICS_MESH_UPLOAD_CONVENIENCE_HPP
#define TERREATE_INTEGRATION_ASSET_GRAPHICS_MESH_UPLOAD_CONVENIENCE_HPP

#include <cstdint>
#include <vector>

#include <terreate/asset/mesh_data.hpp>
#include <terreate/core/result.hpp>
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/completion.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/synchronization.hpp>
#include <terreate/graphics/upload_batch.hpp>
#include <terreate/integration/asset_graphics/mesh_upload.hpp>

/// The beginner-oriented layer over the three explicit mesh steps (#463). Meshes
/// only: the image convenience waits for the layout-transition opt-in (#468),
/// because without it an uploaded image stays in the copy layout.
///
/// Everything here is written with the public three-step API
/// (`planMeshUpload`, `createMeshBuffers`, `meshUploadItems`) and the Graphics
/// upload batch, and the lower-level equivalent is documented step by step in
/// docs/architecture/asset_graphics.md. There are two ways to use it:
///
/// - `submitMeshUpload` does not wait. The application owns the command buffer,
///   the queue, the fence and the retirement.
/// - `uploadMeshBlocking` waits for the GPU, and its name says so. It owns its
///   own command pool, command buffer and fence, and returns only the buffers.
namespace terreate::integration::asset_graphics {

/// The default shader-location mapping of a mesh, as an editable value:
///
/// | attribute | location |
/// | --- | --- |
/// | position | 0 |
/// | normal | 1 |
/// | texcoord set 0 | 2 |
/// | color set 0 | 3 |
/// | tangent | 4 |
///
/// Only attributes the mesh has are listed, in this order. Other texcoord or
/// color sets and every `custom` attribute are never in the default: they are
/// reported by `MeshUploadPlan::unmapped`, never guessed. A valid mesh always has
/// a position, so the list of a valid mesh is never empty; for a mesh without
/// one (which `asset::validate` rejects) the list simply holds what exists, and
/// the plan reports the `asset::MeshDataError` unchanged. Edit the list (or
/// build your own) when the shader uses other locations.
[[nodiscard]] auto defaultMeshAttributeBindings(const asset::MeshData &mesh)
    -> std::vector<MeshAttributeBinding>;

/// Step 1 of the convenience, CPU only: like `planMeshUpload`, except that an
/// empty `description.attributes` means `defaultMeshAttributeBindings(mesh)` and
/// the plan says so (`MeshUploadPlan::used_default_attributes`, and the
/// resolved list is in `MeshUploadPlan::attributes`). A non-empty list is used
/// exactly as given and the flag is false. `planMeshUpload` itself still rejects
/// an empty list with `MeshUploadError::no_attributes`. All other errors are
/// those of `planMeshUpload`.
[[nodiscard]] auto planMeshUploadWithDefaults(const asset::MeshData &mesh,
                                              const MeshUploadDescription &description)
    -> terreate::Result<MeshUploadPlan>;

/// What `submitMeshUpload` returns. Move-only.
///
/// The GPU has been given work that reads `staging` and writes `buffers`, and
/// nothing has proved it complete. Keep all three alive until the fence is
/// signaled, then hand `staging` to a `graphics::RetirementQueue` with `receipt`
/// (`retirement.retire(submission.staging, {&submission.receipt, 1})`) and
/// retire or keep the buffers as for any buffer. Destroying this value early
/// is a contract violation that nothing tracks.
struct MeshUploadSubmission {
  /// What was planned (the effective state: bytes, native vertex input structs,
  /// `used_default_attributes`, `unmapped`), usable with `plan.applyTo(pipeline)`.
  MeshUploadPlan plan;
  /// The created vertex (and index) buffer; their contents are valid once the
  /// fence has signaled and a memory dependency to the later use exists.
  MeshBuffers buffers;
  /// The staging owner. Ready for `RetirementQueue::retire` with `receipt`.
  graphics::PreparedUploadBatch staging;
  /// The evidence of the one successful submission, for `retire`. Not a
  /// completion claim.
  graphics::SubmissionReceipt receipt;
};

/// Non-blocking upload of `mesh`: plans (`planMeshUploadWithDefaults`), creates
/// the buffers, prepares the staging batch, records the copies into `command`,
/// ends `command` and submits it once with `fence` (`graphics::submitFenced`).
/// It never waits, resets or reuses anything.
///
/// Preconditions, all the application's:
/// - `command` is in the recording state (begun by the application, usually
///   with `eOneTimeSubmit`) and was allocated from a pool of `queue`'s family.
///   Nothing else may be recorded into it before the upload; this function ends
///   it, so it cannot be extended afterwards.
/// - `fence` is unsignaled, not in use by another submission, and `identity` is
///   a valid `CompletionIdentity` the application never reuses.
/// - `device` and `allocator` belong together, and `queue` belongs to `device`.
///
/// Lifetimes: `mesh` and `description` are only read during the call and may be
/// destroyed after it returns. `device`, `allocator`, `command`'s pool and
/// `fence` must outlive every GPU use of the result. The command buffer and the
/// fence must stay alive and untouched (no reset, no reuse) until the fence is
/// signaled and the staging is retired; the fence is the completion evidence
/// the receipt refers to, so do not reuse it for another submission while the
/// receipt is still needed.
///
/// No barrier is recorded: the buffers are written by a transfer, so the
/// application makes that write visible to the later use (the vertex input
/// stage) with a barrier, a semaphore, or a host wait on `fence` followed by a
/// later submission on the same device.
///
/// Errors are those of the explicit steps, unchanged (`asset::MeshDataError`,
/// `MeshUploadError`, `graphics::BufferError`, `graphics::UploadError`,
/// `graphics::CommandError`, `graphics::SubmissionError`, native errors). On any
/// error everything this call created (buffers, staging) is destroyed before
/// returning, which is right for every failure that happens before the native
/// submit and for a rejected submit; the application must treat a submit that
/// failed with a lost device like any other lost-device case. After an error
/// `command` is left as it was reached (recording, or ended) and the application
/// resets or discards its pool.
[[nodiscard]] auto
submitMeshUpload(const graphics::Device &device, const graphics::Allocator &allocator,
                 graphics::Queue queue, graphics::CommandBuffer &command,
                 const graphics::Fence &fence, graphics::CompletionIdentity identity,
                 const asset::MeshData &mesh, const MeshUploadDescription &description = {})
    -> terreate::Result<MeshUploadSubmission>;

/// What `uploadMeshBlocking` returns: the plan (for `applyTo`, `unmapped`,
/// `used_default_attributes`) and the buffers. The upload is complete.
struct MeshUploadResult {
  MeshUploadPlan plan;
  MeshBuffers buffers;
};

/// BLOCKING upload of `mesh`: creates its own command pool (on `queue`'s family),
/// command buffer and fence, performs `submitMeshUpload`, then waits on the
/// fence for at most `timeout_ns` (`UINT64_MAX` waits without limit; there is no
/// default), then destroys the helper objects, the staging included. It returns
/// only when the copies have completed, so the buffers hold the mesh and any
/// later submission on `device` may read them (the fence wait makes the
/// transfer writes available to later submissions).
///
/// Use it for start-up code where stalling is acceptable. It owns the
/// submission on `queue`: the application externally synchronizes `queue` and
/// its family as for any `graphics::submit`. The queue family must support
/// transfer. `mesh` and `description` are only read during the call; `device`,
/// `allocator` and `queue` must outlive the returned buffers.
///
/// Errors before the submit are those of `submitMeshUpload`. A fence wait that
/// returns a native error is returned unchanged; one that times out returns
/// `MeshUploadError::wait_timeout`. In both of those cases the GPU may still be
/// using the staging memory, the buffers and the helper objects, so none of
/// them is destroyed: they are deliberately leaked (destroying them would be
/// undefined) and the application should treat the device as lost or hung.
[[nodiscard]] auto uploadMeshBlocking(const graphics::Device &device,
                                      const graphics::Allocator &allocator, graphics::Queue queue,
                                      const asset::MeshData &mesh, std::uint64_t timeout_ns,
                                      const MeshUploadDescription &description = {})
    -> terreate::Result<MeshUploadResult>;

} // namespace terreate::integration::asset_graphics

#endif // TERREATE_INTEGRATION_ASSET_GRAPHICS_MESH_UPLOAD_CONVENIENCE_HPP

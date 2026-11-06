# Explicit Buffer and Image staging/upload batches (#338 Phases A/B)

This module provides Phase A Buffer Upload and a **bounded Phase B** Image
Upload path. The Image path deliberately supports only RGBA8 2D single-sample
Images and explicitly described subresources/pitch. Neither path is a transfer
scheduler, background uploader or alternative queue-submission system.
The application owns and triggers every operation:

1. Prepare a set of explicit destination Buffer ranges and CPU byte spans.
2. Call prepareUploadBatch(allocator, description): validate **all** entries,
   allocate one VMA-backed TRANSFER_SRC Buffer, map, copy, flush, and unmap.
3. While a CommandBuffer is recording, call
   recordUploadBatch(device, command, prepared). It validates the whole
   prepared batch before recording one vkCmdCopyBuffer2 per write using #264.
4. The application records any required memory barriers, ends recording, and
   calls #258 submit(device, queue, explicit_submission_description) itself.
   The only credible completion evidence is that native-success
   SubmissionReceipt—there is no fabricated upload-completion token.
5. Once the last GPU use of staging is known, transfer the move-only
   PreparedUploadBatch into the application-owned #259 RetirementQueue using
   retire(batch, all_receipts), then invoke collect() at caller-chosen time.
   The receipt array must cover **every** use, including multiple queues.
   No destructor waits.

## API and data ownership

The public header terreate/graphics/upload_batch.hpp provides
BufferUploadItem, UploadBatchDescription, PreparedUploadBatch,
prepareUploadBatch(), and recordUploadBatch(). Input byte spans are read
**only** during preparation, so caller storage may change or disappear
after preparation. The resulting owner holds a staging Buffer and value
snapshots of each source offset, borrowed destination Buffer pointer,
destination offset, and byte size. copies() provides effective recording
values; requestedStagingAllocation() and
stagingBuffer()->configuration()/allocationInfo() expose requested and
effective VMA allocation details. No source-data span is retained.

Default staging allocation: VMA AUTO, explicit
HOST_ACCESS_SEQUENTIAL_WRITE and required HOST_VISIBLE. One TRANSFER_SRC
Buffer has precisely the packed byte length. The caller may override the
allocation description, subject to the existing VMA Buffer API and
host-visible mapping requirements. Neither target usage nor Device features
are implicitly added.

PreparedUploadBatch is noncopyable, noexcept movable, and owns exactly
one staging Buffer. Its valid() observer meets #259's generic move-only
owner contract. It is **not** reference-counted. Moved-from batches are
invalid. The caller retains Allocator/Device/Instance and target Buffers
through all GPU references and externally synchronizes conflicting host
use. Borrowed destination Buffer wrappers must not move or be destroyed
between preparation and recording.

**Lifetime boundary:** after recordUploadBatch(), destroying or
move-assigning the staging owner before discarding the recorded command
or observing all corresponding native GPU completions is a caller error.
No implicit liveness check, GPU use tracker, scheduler, or wait exists.
If submission fails, retain staging: no successful receipt was obtained
and no implicit completion/retirement may be asserted. The application
must determine safe recovery. Native Device Loss is not normal GPU
completion; only #259's observed-device-loss path supports explicit
exceptional recovery.

## Validation boundary and restrictions

For this bounded native Buffer-copy path, each payload must be nonempty
and a multiple of four bytes; the destination offset must be four-byte
aligned. Preparation checks target TRANSFER_DST usage, offset/range bounds
using subtraction without overflow, total staging size, and overlapping
destination ranges of the same native Buffer handle. One staging Buffer
packs all source writes contiguously with four-byte aligned offsets.
Bad input fails *before* staging allocation or native recording.

The convenience path requires every target and staging Buffer to borrow
the **same native VmaAllocator**, rejecting a common provenance mismatch.
This restriction applies only to this uploader, **not** to the native #264
path or to legitimate same-Device transfers between separate Allocators.
The Buffer API does not expose native Device identity, so the caller still
guarantees that the staging Allocator and supplied CommandBuffer/Device
refer to the same logical Device. Other Vulkan constraints and underlying
memory aliasing not evident from native Buffer handles remain caller
obligations.

Recording validates live Device, CommandBuffer native parent, recording
state, unprotected command pool, staging/target usage, Allocator identity,
and all entry sizes/bounds before **any** vkCmd call. It does not track
resource states or make implicit barriers. Native vkCmdCopyBuffer2 returns
void: recording is not transactional. If anything unexpected fails after
native recording begins, the application must explicitly recover/reset
as permitted by Vulkan. There is no hidden rollback.

No implicit Queue selection, Queue-family transfer, barriers, target
cache flush/invalidate, submission, fence creation, completion polling,
retirement, mipmap generation or Image layout inference is supplied.
Phase B adds explicit Image upload *recording* but no implicit transitions.
#339 Readback remains a separate Issue.

## Test and packaging expectations

Headless graphics.upload_batch tests invalid inputs, overlapping writes,
actual native Copy2 offsets and call counts, source span snapshotting,
VMA map/flush native error propagation and staging cleanup, explicit
Queue Submit error/success, real Fence completion, pending RetirementQueue
collection, eventual exactly-once staging destruction, and Buffer→Buffer
readback byte equality. The readback is a **test-only fixture**, not a
claim that the future #339 public API has been implemented.

Tests must not fake PASS when Vulkan Loader, Device, transfer Queue or
memory requirements are unavailable. GNU linker wrappers observe Vulkan
and VMA entrypoints; they do not establish portability to other linkers.
Installed package smoke compiles/links the new public header from an
external consumer. This document defines the contract, not proof of tests.

## Bounded Phase B: explicit RGBA8 Image upload

An ImageUploadItem borrows a destination Image, specifies a VkBufferImageCopy2
region (correct sType, null pNext, and **request bufferOffset == 0**), an
explicit copy_layout (TRANSFER_DST_OPTIMAL or GENERAL), and a byte span.
Its region chooses the actual Image aspect, mip level, array layer, 2D
texel offset and copy extent; these are not inferred or rewritten.
Preparation assigns an **effective** staging bufferOffset (which will
usually be nonzero for mixed Buffer/Image batches), snaps the native
region/layout/borrowed Image handle and copies the CPU bytes. These
values are available through PreparedUploadBatch::images().

The bounded convenience path accepts only VK_FORMAT_R8G8B8A8_UNORM,
VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, COLOR aspect, depth 1, and
exactly **one layer per operation**; a caller may explicitly upload
separate mip levels or distinct nonoverlapping regions. All regions of
one Image mip/layer in the same batch MUST declare the same copy_layout;
conflicting layouts are rejected before native recording because there is
no place to insert a per-region transition. The image
must be TRANSFER_DST capable and use the same VMA Allocator as the
staging Buffer. The Image's parent logical Device is checked again
at record time. Unsupported formats, samples, 3D, compressed/
multiplanar/depth/stencil or overlapping writes to the same image
mip/layer are rejected rather than processed under false assumptions.

The native bufferRowLength and bufferImageHeight fields are in
**texels, not bytes**, per the Vulkan Copy Commands specification.
Either may be zero (tight packing) or must meet the corresponding
region extent. For one RGBA8 2D layer, the highest byte accessed in
the staging region ends at:

    (((imageExtent.height - 1) * effectiveRowLength) + imageExtent.width) * 4

where effectiveRowLength = bufferRowLength or imageExtent.width if zero.
The byte span must have **exactly** that length, including inter-row
padding but not inventing unused trailing-row padding. All products
and combined batch offsets are overflow-checked; the row stride
obeys Vulkan's signed-31-bit row-size limit. Each requested native
region has bufferOffset 0 because the batch owns its stage offsets.

The application records oldLayout -> copy_layout transitions explicitly
through #264 recordBarriers *before* recordUploadBatch, with true
stage/access masks, ownership and subresource scopes. It records any
post-copy transition explicitly too. recordUploadBatch records one
vkCmdCopyBufferToImage2 per ImageUploadItem, no barriers and no implicit
mipmap generation, Queue selection, submit or waits. As in Phase A, the
staging batch is one move-only RAII owner; application-controlled #258
SubmissionReceipt and #259 RetirementQueue govern its GPU lifetime.

Vulkan format transfer features, Queue transfer granularity, any other
native validity preconditions, and actual prior Image layout/ownership
remain caller responsibilities. This is not a state tracker or a
substitute for native Vulkan escape hatches.

### Phase B native validation

The isolated graphics.image_upload_batch test covers mixed Buffer +
multiple RGBA8 Image region staging, row-padded two-region Image copy,
explicit pre/post Synchronization2 layout transitions, native argument
observations, host readback content, nontrivial effective staging
offsets, invalid pitches/footprints/subresources/layouts/formats,
prevalidation before *any* native copies, conflicting-layout rejection,
compatible Queue minImageTransferGranularity selection, explicit submit/fence
completion, and deferred staging destruction. Unsupported devices
must return BLOCKED and fail, never be counted as a fake PASS.

# Explicit Buffer staging/upload batches (#338 Phase A)

This module implements **only** the bounded Buffer Upload MVI. It is not an
image uploader, transfer scheduler, or alternative queue-submission system.
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
Images, pitch, format and subresource rules belong to separately gated
#338 Phase B. #339 Readback is an independent Issue.

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

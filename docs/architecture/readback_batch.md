# Explicit Buffer GPU Readback batches (#339 Phase A)

Phase A supplies an explicit **Buffer-only** readback utility built on the
existing #271 VMA Buffers, #264 Copy2 recording, #258 caller-owned submission
and native completion receipts, and #259 retirement. This is NOT an Image
readback utility, scheduler, submitter, automatic barrier engine, or owner of
the application loop. Image readback is separately gated Phase B.

## Caller-owned execution

1. Create one or more source Buffers with TRANSFER_SRC and fill/produce their
   contents using caller-selected commands/synchronization.
2. Build BufferReadbackItem ranges, then call prepareReadbackBatch. This
   validates **all** reads and allocates one packed host-visible
   VMA TRANSFER_DST staging Buffer. No CPU data are read or mapped here.
3. Record all producer -> TRANSFER_READ dependencies, then call
   recordReadbackBatch on an already recording CommandBuffer. It records one
   vkCmdCopyBuffer2 per item through #264; it does not start/end recording.
4. Record an appropriate TRANSFER_WRITE -> HOST_READ memory dependency, end
   recording, and call #258 submit on a caller-chosen Queue with explicitly
   owned Fence or Timeline Semaphore and completion identity. No hidden wait
   or queue choice occurs.
5. Supply the real SubmissionReceipt(s) to
   tryCopyCompletedReadbackBatch(device, prepared, all_uses). This method
   polls each receipt nonblocking and refuses to map/invalidate/expose bytes
   if ANY term is not completed. Caller may poll/retry or wait explicitly
   outside the utility. This method never creates a receipt.
6. On confirmed completion, it maps the VMA staging, calls invalidate,
   copies every byte into a **CPU-owned** CompletedReadbackBatch and unmaps.
   CPU snapshots survive staging destruction and carry per-item byte-offset
   metadata; no borrowed mapped span is returned.
7. Only AFTER CPU copying and ALL GPU uses complete may the application
   release staging directly, or pass the still-valid PreparedReadbackBatch
   and all receipts to #259 RetirementQueue and explicitly collect.

## Ownership and limitations

- PreparedReadbackBatch is a noncopyable, noexcept-movable RAII owner; it
  contains staging and immutable metadata. The public API does not provide
  a direct mapped pointer to internal staging. Source wrappers are borrowed
  and must be kept stable through record and live through GPU execution.
- No hidden lifetime tracking, resource retention, or wait exists.
  Destruction/move-replacement while pending GPU references exist violates
  the caller's contract. Device/Allocator/CommandPool and native Fence/
  Semaphore lifetimes and external synchronization remain caller-owned.
- A successful SubmissionReceipt proves a native submission and allows
  querying its Fence/Timeline, but does NOT identify which CommandBuffer,
  copy or staging was submitted. The caller must supply the genuine
  receipt(s) for **all known GPU uses**, including multiple queues, and
  must not reset/reuse the native completion signal while it is queried.
  Supplying an unrelated completed receipt cannot be detected: the convenience
  API enforces its declared receipt contract, not global Vulkan access.
  The explicit Device parameter verifies same Device *wrapper address*
  and Queue correlation for every receipt.
- The caller supplies needed HOST_WRITE -> TRANSFER_READ, producer ->
  TRANSFER_READ, TRANSFER_WRITE -> HOST_READ and Queue ownership dependencies.
  GPU completion alone is not a substitute for the required memory dependency.
  Native low-level Buffer::map / Vulkan escape hatches intentionally remain.
- Native submit failure creates **no** receipt; do not read or retire on an
  invented completion. Keep staging while handling uncertain submission state.
  On polling Device Lost/native error, no staging bytes are exposed.
  Map/invalidate errors preserve the staging owner and return no partial bytes.
  The helper never invokes DeviceWaitIdle, creates a Fence, or waits.
  RetirementQueue's device-loss recovery still requires native device loss
  observed by that Queue itself (not a synthetic boolean).

## Phase A requests and effective metadata

- One or more BufferReadbackItem values specify a borrowed source Buffer,
  source_offset and size in bytes. Source Buffers require TRANSFER_SRC.
  A repeated/overlapping **source read** is valid; destinations always occupy
  distinct packed intervals in the staging Buffer.
- For VkBufferCopy2 there is **no general 4-byte requirement** on byte offsets
  or size; this is intentionally different from #338's narrowly constrained
  Buffer upload convenience contract. All ranges and total staging/CPU sizes
  are overflow-checked. sType/pNext are internal known-good values.
- Default staging uses VMA_MEMORY_USAGE_AUTO,
  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, and required HOST_VISIBLE.
  The caller can override the staging allocation; AUTO without a HOST_ACCESS
  flag or non-HOST_VISIBLE actual allocation is rejected.
- stagingSize(), buffers(), requestedStagingAllocation(),
  stagingConfiguration(), and stagingAllocationInfo() allow requested and
  effective configuration inspection without revealing a mapped pointer.
- CompletedReadbackBatch::bytes() returns the owned packed bytes,
  buffers() describes each source offset/data offset/size, and dataFor(i)
  returns a read-only span of exactly one result. None borrows staging.

## Testing and packaging gates

The Phase A headless test must cover multiple and overlapping source
ranges, non-four-byte copies, exact native vkCmdCopyBuffer2 observations,
input validation before native commands, moved-from source detection,
no hidden submit or DeviceWaitIdle, actual Vulkan Fence completion,
simulated pending/error polling without host access, VMA map/invalidate
failures preserving ownership, CPU-owned byte equality, and #259 retirement
only after materialization. BLOCKED is failure, not a fake PASS.

Use `nix develop --no-update-lock-file --command just project::check`,
focused CTest, installed Graphics package consumer smoke and independently
review the isolated source-only candidate. Do not publish without separate
human authorization for Commit/Push/Draft PR, exact-head re-verification,
independent PR-mode review and then separate Ready/Merge gates.

#282 Buffer round-trip integration should use this public helper later.
#283 offscreen Image readback still requires Phase B and rendering dependencies
#260/#265/#266/#267. No Image formats, row pitch or automatic barriers in A.

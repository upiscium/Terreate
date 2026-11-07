# Explicit Buffer and RGBA8 Image GPU Readback batches (#339 Phases A/B)

Phase A supplies explicit Buffer Readback built on #271 VMA Buffers, #264
Copy2 recording, #258 caller-owned submission and native completion receipts,
and #259 retirement. Phase B adds bounded RGBA8 Image Readback to those
same public batch operations. Neither phase owns an application loop,
scheduler, Queue selection, implicit transitions, barriers or waits.

## Phase A — caller-owned Buffer execution

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

## Phase B: bounded RGBA8 Image Readback

Phase B extends the same `ReadbackBatchDescription` with an **appended**
`images` span. A batch may contain Buffer reads only (Phase A, unchanged),
Image reads only, or a mixture of both. A single private, host-visible
`TRANSFER_DST` staging Buffer packs all Buffer items first and Image items
after them; each Image uses a checked 4-byte-aligned staging offset. For mixed
batches the first Image offset can exceed the end of a preceding odd-sized
Buffer entry. Inter-entry gaps are **not** returned as meaningful GPU data.

`ImageReadbackItem` declares a borrowed source `Image`, a native
`VkBufferImageCopy2` region and an explicit layout (only
`TRANSFER_SRC_OPTIMAL` or `GENERAL`). Its input region has
`bufferOffset == 0`; preparation assigns the *effective* staging offset,
which is available through `PreparedReadbackBatch::images()`. Nothing
tracks or changes an Image's actual layout on the caller's behalf. The
application records the real producer-to-transfer-read barrier, any required
Image layout transition and queue-family ownership transfers **before**
`recordReadbackBatch`, then records the transfer-write-to-host-read
dependency before submitting. Submit, Queue, Fence/Timeline, and wait/poll
timing remain application-controlled and fully observable.

The convenience Image subset is deliberately narrow:
`VK_FORMAT_R8G8B8A8_UNORM`, `VK_IMAGE_TYPE_2D`, one sample, COLOR
aspect, exactly one mip level and one array layer **per item**, 2D extent with
depth 1. Each item explicitly selects a legal mip, layer, offset and positive
copy extent. Separate items may select other mips/layers or overlapping
read-only source rectangles. If the same Image mip/layer is declared with
two different copy layouts in a single batch, preparation rejects it:
the utility does not infer intermediate transitions. Compressed,
depth/stencil, multi-planar, 3D, multisample or other color formats are
deliberately unsupported here; use the native #264 path for other cases.

### Texel pitch, valid footprint, and defined CPU bytes

`VkBufferImageCopy2::bufferRowLength` and `bufferImageHeight` are in
**texels**, not bytes; zero means tightly packed in that dimension.
For supported RGBA8 one-layer/depth-one regions:

```text
effective_row_texels = bufferRowLength != 0 ? bufferRowLength : imageExtent.width
row_pitch_bytes = effective_row_texels * 4
valid_row_bytes = imageExtent.width * 4
touched_footprint_bytes = (imageExtent.height - 1) * row_pitch_bytes + valid_row_bytes
```

The helper checks region bounds against the actual mip-reduced Image
extent; valid native structure/colour aspect; offset/size arithmetic
overflow; `bufferRowLength == 0 || bufferRowLength >= width`; and
`bufferImageHeight == 0 || bufferImageHeight >= height`. It checks
`row_pitch_bytes <= INT32_MAX` per Vulkan's RGBA8 BufferImageCopy2
row-pitch limit, and validates the **complete touched footprint**
against the staging range before any native command. Since this bounded
path has a single layer and depth=1, `bufferImageHeight` does not add
an additional slice stride. Image-to-Buffer destination `bufferOffset`
is aligned to the RGBA8 texel block size (4 bytes). Do not mistake this
for a general 4-byte alignment restriction on Phase A Buffer-to-Buffer
copies, which retain their byte-exact ranges.

**Vulkan does not define the contents of skipped row padding.** The
completed CPU snapshot therefore starts zero-initialized. After all
submitted GPU uses have genuinely completed and VMA memory was explicitly
invalidated, the helper copies only the active `valid_row_bytes` per
row; untouched row padding and inter-entry packing gaps remain zero.
The result is `CompletedReadbackBatch::bytes()` plus
`CompletedReadbackBatch::images()` snapshots of format, explicit
copy layout, native effective region, CPU data offset, touched footprint
and `row_pitch_bytes`. `imageDataFor(index)` is an independently
CPU-owned read-only view covering the *touched footprint*, including
deterministic zero padding between rows. `dataFor(index)` and
`buffers()` retain Phase A semantics. The CPU result does not point at
mapped staging; it remains readable after the application explicitly
retires/destroys that staging owner.

### Provenance, synchronization, and failure boundaries

The Image wrapper's live native Device is checked against the selected
CommandBuffer/Device at recording time, and the Image's allocator must
match the batch staging allocator. **Buffer wrappers do not expose a
native Device identity**; the application must ensure all Buffer and
staging objects, CommandBuffer and selected Queue belong to the same
logical Device (including the mixed Batch case). Only a real successful
`submit()` provides a `SubmissionReceipt`; the existing receipt API
does not authenticate which batch's work the caller submitted. The
application must supply every relevant real receipt, keep Fence/Timeline
sync identities alive and unreused, and preserve Image/Buffer/parent
lifetimes until completion. The library never manufactures a receipt,
waits, or guesses a transfer dependency.

For empty, pending, cross-Device, or native-error completion evidence,
`tryCopyCompletedReadbackBatch` returns **no** mapped data, Image bytes
or partial result. Map/invalidate failures likewise keep the exclusive
staging owner with the caller. The application releases or retires
staging **after** successful CPU materialization and all GPU uses
complete. GPU device loss remains exceptional recovery under existing
#259 contract; no hidden recovery/wait is added.

### Bounded verification

The Phase B test exercises Image-only and mixed Buffer/Image batches,
explicitly padded and tightly packed row pitches, exact native Image
Copy2 offsets and region metadata, overlapping source reads and
same-subresource conflicting-layout rejection, explicit application
barriers, real GPU pixels and zeroed CPU padding, native Fence completion,
no hidden submit/wait/Queue/DeviceIdle, failure paths and retirement.
A genuine GPU block is reported as BLOCKED/failure rather than fabricated
PASS. The installed package smoke verifies the extended public
Image Readback API without depending on optional Shader/Pipeline/GUI
modules. This is transfer-only; the separate #282 Buffer integration
and #283 offscreen rendering smoke are not implemented in Phase B.

# Explicit Synchronization2 and transfer command recording (Issue #264)

This module provides caller-triggered recording helpers under
`terreate/graphics/transfer_commands.hpp`. No implicit frame loop, queue
selection, submission, wait, memory allocation, staging or layout tracking exists.
Application code calls `CommandBuffer::begin()`, records these commands, then
`end()` and explicitly submits through #258 (or native Vulkan). #259 governs
completion-based resource retirement; neither recording nor CPU-observed
`CommandBufferState::executable` is evidence that GPU execution has finished.

## Vulkan-native descriptions and effective behavior

`recordBarriers(device, command, description)` consumes explicit borrowed
spans of native Vulkan 1.3 `VkMemoryBarrier2`, `VkBufferMemoryBarrier2` and
`VkImageMemoryBarrier2`, and a native `VkDependencyFlags`. It maps those
descriptions to precisely one `VkDependencyInfo` and records
`vkCmdPipelineBarrier2`; masks, old/new layouts, queue-family release/acquire
indices, and subresource ranges are not invented or silently rewritten.

`recordCopyBuffer`, `recordCopyImage`, `recordCopyBufferToImage` and
`recordCopyImageToBuffer` take caller-specified `Vk*Copy2` regions and
explicit image layouts and record exactly one corresponding Vulkan 1.3
`vkCmd*2` call each. These are native low-level inputs by design, not a
promise to abstract all Vulkan-valid formats/layouts/offsets; Vulkan-Hpp
`vk::Buffer`, `vk::Image`, layouts and command-buffer handles remain
reachable through the existing resource APIs and native escape hatches.

The wrappers validate a live Device and recording CommandBuffer, matching
CommandBuffer Device handle, actually enabled synchronization2 for barriers,
non-empty regions, null `pNext` and correct `sType`, required transfer usage
and basic Buffer offsets/sizes, and Image subresource/mip/layer/extent bounds.
Image parent Device correlation is checked through `Image::nativeDevice()`.
`Buffer` does **not** expose its parent logical Device, so its provenance
cannot be proven by this helper: callers guarantee that all buffers belong
to the selected Device. A `Result<void>` success establishes only that the
preconditions tested by Terreate passed and a native `vkCmd*` call was
recorded. Vulkan `vkCmd*` has no execution success/failure return.

## Caller preconditions / limits

- **No implicit image state:** caller supplies true old and desired new layouts,
  stage/access scopes, explicit queue-family ownership release/acquire barriers,
  and valid layout usage. Multi-queue transfers may require different commands
  submitted on the source and target queues; a single helper call is not a
  cross-queue ownership protocol.
- **Native Vulkan validity remains in force:** proper stage/access support for
  the selected queue, flags and access/layout relationships, subresource aspect
  and format compatibility, block compression/multiplanar plane rules,
  alignment, non-overlapping forbidden copies, and image-buffer row/slice
  footprints (including actual byte coverage, format block size and offsets).
  `bufferOffset < Buffer::configuration()->size` is only a *minimal* check
  for buffer/image copies: this does **not** prove that the full data footprint
  fits the Buffer. The caller must validate complete native region bounds.
- `vk::Image` and Buffer/Image owners must be alive through every GPU use.
  The caller keeps CommandPool, Device, Allocator and sync handles alive, and
  externally synchronizes command recording and native use. Copying one
  resource does not establish that other in-flight uses have completed.
- Native `pNext` extension chains in barrier/region descriptions are rejected
  by this narrow helper; advanced callers may record arbitrary Vulkan-native
  commands through the borrowed `CommandBuffer::nativeHandle()`.
- Vulkan 1.3 is mandatory for the Copy2 commands. Synchronization2 is
  **separately enabled** for `vkCmdPipelineBarrier2`; support alone is not
  sufficient. Copy2 does not silently enable synchronization2, and its
  availability is determined by the Vulkan 1.3 native API version.
- No implicit `vkQueueSubmit2`, `vkWaitForFences`, `vkDeviceWaitIdle`,
  barriers, implicit buffer mapping, host cache flush/invalidate or retry.

## Verification scope

The standalone `graphics.transfer_commands` headless test creates real
VMA Buffers and optimal Images, records explicit transitions and all four
Copy2 operations, submits one executable primary command buffer through #258,
waits on a real fence, and verifies Buffer→Buffer and
Buffer→Image→Image→Buffer bytes after explicit host invalidation. GNU linker
wrappers count real `vkCmd*` entry points and confirm invalid preconditions
never record commands; the test checks that no implicit device-idle wait occurs.
The test must report **BLOCKED** instead of pretending to PASS if the required
Vulkan 1.3/format/memory conditions cannot be exercised. The build/package
gate checks that the installed public API is linkable from an external consumer.
This document is a design/test contract and is not itself proof of any run.

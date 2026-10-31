# Explicit command lifecycle (Issue #263)

Command pools and command buffers belong to Graphics, independently of Platform.
The application chooses a queue family explicitly from the families enabled on
its logical Device and supplies native Vulkan command-pool flags. A physically
supported family is not sufficient if it was not enabled on that Device. Pool
creation must not silently select a queue, enable another family, or augment the
caller's flags. Requested and effective descriptions, family selection and
borrowed native handles must remain inspectable.

## CPU lifecycle is not GPU completion

The primary-buffer lifecycle is explicit: allocate an initial buffer, begin
recording, end recording, and reset before reuse when required. Invalid parents,
invalid families and invalid lifecycle transitions are structured Result errors,
not invitations to issue invalid Vulkan calls. Pool reset affects every allocated
buffer: wrapper observations must not continue to report a pre-reset recording or
executable state after a successful pool reset.

Recording and ending a command buffer do not submit it. No command API owns a
frame loop, schedules work, selects an execution queue, or establishes completion.
There is no implicit device-idle wait. Before resetting, freeing, destroying or
displacing an owner that might be in use by the GPU, the application must establish
the Vulkan-required completion itself. A CPU state observation cannot prove that
a command buffer is no longer pending on a queue. Resource lifetime, dependencies,
queue-family ownership transfers and GPU hazards remain application obligations.

## Borrowed handles and tracking boundaries

Native handles are borrowed escape hatches, not ownership transfers or lifetime
extensions. Never separately free or destroy a wrapper-owned native object.
Native recording, begin/end/reset operations and native pool resets bypass wrapper
tracking. In particular, a wrapper's semantic state cannot be treated as a query of
Vulkan's live state after such operations. Do not mix native state-changing calls
with tracked operations under the assumption that the wrapper notices them.
Borrowing a handle also does not remove GPU-completion or host-synchronization
requirements.

## Thread use and host synchronization

There is no application-thread scheduler or implicit thread-affinity mechanism.
Use a pool on one recording thread at a time, or externally serialize every
conflicting access. Vulkan's command-pool host synchronization covers operations
on buffers allocated from the same pool; two different buffers do not make
concurrent use of their common pool safe. A practical concurrent-recording design
uses a separate pool per recording thread and queue family.

Moving work between host threads requires caller-controlled synchronization and
exclusive access, not merely a C++ move. Observation and borrowed-handle use must
not race recording, reset, ownership transfer, displacement or destruction.
The same requirements apply when some accesses occur through native handles.
Independent pools do not remove synchronization requirements on other shared
Vulkan objects or on application data.

## Ownership ordering

Device ownership and pool/buffer ownership are distinct. Device is a borrowed
parent, not a shared owner retained by command objects. Keep the native Device
alive through all pool and buffer cleanup, and destroy it before its parent
Instance. Moving a Device wrapper while preserving the native Device differs
from replacing it: move-assignment displacement destroys the destination's old
native Device and therefore requires all of that old Device's dependents to have
been released first. Neither native handles nor correlation observations extend
the parent's lifetime.

Likewise, pool displacement must be treated as destruction of the destination's
previous native pool, not as reassociation of its old buffers with the incoming
pool. Destroying a native pool releases its allocated native buffers. A buffer
must never be freed again after that release. The public pool/buffer ownership
contract determines whether dependent wrappers must be destroyed first or can
remain as invalidated values; callers must not assume stale-parent detection or
shared ownership merely from the existence of a wrapper. Application GPU
completion is required in either model.

## Secondary-level boundary

The initial lifecycle coverage concerns primary command buffers only. It does
not imply a secondary-recording API, inheritance configuration, render-pass
continuation or command-buffer execution support. Native secondary allocation or
recording requires its own Vulkan inheritance and usage prerequisites; primary
begin/end coverage is not evidence that those prerequisites are handled. No
submission, completion primitive, barrier, raster or compute abstraction is
introduced here.

## Malformed native outputs

A Vulkan implementation that returns `VK_SUCCESS` must provide valid output
handles for command-pool and command-buffer creation. The wrapper nevertheless
rejects a synthetic successful result with an empty handle, instead of exposing
a value that looks valid but could later pass `VK_NULL_HANDLE` to Vulkan.
This is defensive validation for a nonconforming native result, not evidence
that ordinary Vulkan drivers exhibit such behavior.

## Validation boundary

Headless coverage must create real Vulkan objects and exercise CPU lifecycle
operations without submitting GPU work. It must distinguish environment BLOCKED
from assertions FAIL and completed coverage PASS, and verify that lifecycle and
ownership cleanup do not call `vkDeviceWaitIdle`. Build/package consumers must
include the installed command headers rather than reach into source-private
paths. This document records responsibilities, not evidence of executed tests.

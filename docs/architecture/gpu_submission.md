# Explicit GPU submission

`terreate/graphics/submission.hpp` is the application-controlled Vulkan 1.3
submission boundary. `submit(device, queue, description)` validates a nonempty
batch of executable primary CommandBuffer borrows, then makes exactly one
`vkQueueSubmit2(queue, 1, ...)` call. It never enables features, submits secondary
buffers, or supports protected work/device groups. synchronization2 must actually
be enabled in the DevicePlan; timelineSemaphore must actually be enabled for any
timeline declaration. Capability support alone is insufficient.

The caller supplies waits/signals as `SubmissionSemaphore` arrays, declaring
binary or timeline type and setting the native `SemaphoreSubmitInfo` semaphore,
stageMask and value. Binary values are zero. pNext and deviceIndex must be zero;
duplicate entries and contradictory wait/signal declarations are rejected.
Stage masks and values are not inferred or widened. Native semaphore type,
Device ownership, valid stage support for the queue, signal history, fence
unsignaled state, and all other Vulkan execution validity remain caller
preconditions: raw handles cannot prove those facts. No barriers or dependencies
are inferred. Native handles remain available for direct Vulkan use.

## Submission is not completion

Supply a `CompletionDescription` whose Device/Queue match the selected
submission Device/Queue, whose caller-issued `requested` and `effective`
identities are each valid (they may differ), and whose condition is non-null.
For fence completion, `description.fence` must be the identical fence in the
condition. For timeline completion, an explicit timeline signal must name the
identical semaphore and value and include `ALL_COMMANDS`. This full-work scope
is required even when a narrower signal would satisfy another application need.
Identity serials are never generated or reused by the library.

Only native SUCCESS returns a `SubmissionReceipt`. Native failure returns
`Result`/`Error` with the exact Vulkan error code, including device loss; no
failure receipt or completion assertion is manufactured.

The receipt's `initialCompletion()` creates the #257 borrowed `Completion`
token with outcome `unavailable`, preserving caller-supplied requested and
effective identities (which may differ). This performs **no native query**:
a successfully submitted batch is not evidence that GPU execution is pending
or completed. A copied #257 `Completion` is caller-reported observation,
not independent proof of submission; only the receipt's successful native
submission establishes that association.

`receipt.poll()` performs one `vkGetFenceStatus` or
`vkGetSemaphoreCounterValue`. `receipt.wait(timeout_ns)` performs one
`vkWaitForFences` or `vkWaitSemaphores`, with the explicit nanosecond timeout
(`UINT64_MAX` means infinite). Results distinguish pending, completed and timeout;
device loss and other native failures retain their native error codes in Error.
A successful counter query below the target is pending **with native SUCCESS**
and the actual counter value, not fabricated NOT_READY. No automatic conversion
to #257 Completion is attempted: that value's pending/native-result constraints
do not represent this successful below-target query.

## Borrows, CPU state and synchronization

The batch arrays and CommandBuffer view objects survive the submit call. The
Device, its Instance parents, command pools/allocations, synchronization objects,
and every referenced resource must survive all GPU use. Receipts borrow the
Device at the same address, the Queue, native completion object, and identity
domains; those parents must survive every query/copy. Do not move/destroy their
Device or reset/reuse their fence/semaphore for different work while querying a
receipt. A dangling borrow cannot be detected by this API. Copies retain no
ownership and destruction performs no native work.

CommandBuffer provenance exposes the borrowed native Device origin, pool family,
protected allocation, primary level and CPU-observed executable state. A submit
does **not** mutate this state or mark buffers pending. An executable observation
does not authorize resubmission, reset, or resource destruction. In particular,
ONE_TIME_SUBMIT work needs caller-controlled reset/re-recording after completion;
the CPU observation does not track Vulkan's post-execution invalidation. Pending
use and simultaneous-use restrictions, duplicate buffers in a batch, reuse and
every queue's outstanding uses remain the caller's responsibility.

Externally synchronize Queue submissions (including aliases), pools/buffers,
fences, semaphore operations and receipt queries as Vulkan requires. There are
no implicit waits, resets, retries, background work, global owners, resource
retirement or in-flight tracking. Completion of this batch alone does not prove
that every use of a resource on other queues has finished.

The registered source tests intercept native calls for exact argument/result
checks and exercise real headless fence completion. The separate timeline test
requires genuinely enabled timeline support; capability-unavailable is SKIP,
never PASS. Project verification and package smoke are host-owned.

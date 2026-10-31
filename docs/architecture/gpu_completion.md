# GPU completion contract (#257)

## Boundary and status

This document specifies the public C++23 completion-token and synchronization
contract, not a submission or resource-management implementation. Descriptive
terms below are semantic requirements, not promises of particular C++ type,
member, enum, or factory names. Existing interfaces and tests remain authoritative
for the API actually available; in particular, future poll/wait and submission
behavior described here must not be mistaken for implemented operations.

#257 defines inspectable borrowed completion identity and its limits independently
of #258 (explicit submission) and #259 (explicit collection/retirement). It has no
dependency on #263. The application continues to own scheduling, synchronization,
native object lifetimes, resource coverage declarations, and all execution timing.

## Borrowed identity and requested versus effective state

A completion description identifies:

- The borrowed logical `Device` and the explicitly chosen borrowed `Queue`,
  including its native `vk::Queue` and effective family/index coordinates.
- The completion primitive kind: a borrowed native `vk::Fence`, or a borrowed
  native timeline `vk::Semaphore` together with an explicit unsigned 64-bit
  signal/target value. A binary semaphore is not a timeline completion point.
- Requested identity (caller intent) separately from effective identity (the
  validated association available for use). An absent effective association is
  not replaced with a guessed queue, primitive, value, or successful result.

Queue selection uses the existing `Device::queue(caller_id)`, queue views, and
`DevicePlan` assignment/alias evidence. An unknown caller ID yields an invalid
queue view; an optional request may have been declined. A requested caller ID
is not itself a native queue identity: multiple accepted requests may alias the
same effective allocation. Family/index coordinates alone do not identify a
queue across devices. Retaining requested intent must not overwrite the
effective queue or pretend that an unavailable request was accepted.

The primitive handle, kind, parent correlation, and timeline value must remain
inspectable without submitting or querying work. A timeline value is part of
the completion point, not an automatically allocated sequence number. Different
values on one semaphore are different targets; values on different semaphores
are not comparable completion evidence. No value is silently incremented,
reserved, or signaled. Fence identity also requires the caller to preserve the
intended use: a raw handle cannot distinguish a reset/reuse cycle.

The existing `Device::nativeHandle()` and `Queue::nativeHandle()` escape hatches
remain available. Merely possessing non-null device, queue, fence, or semaphore
handles proves neither native parentage, successful submission, nor completion.
Borrowed-handle descriptions are declarations, not Vulkan provenance queries.

## Ownership, copies, and parent lifetimes

Completion descriptions/tokens are non-owning values. Copying one copies its
identity and explicit metadata, not a native primitive or a new completion
event. Copies refer to the same fence use or timeline target and confer no
additional ownership, synchronization, or independently tracked progress.
Destroying, replacing, or moving a description must not wait, reset, signal,
destroy, or release any native object. No shared ownership or registry is implied.
This does not change the move-only RAII ownership of `Device` or the plain
copyable borrowed semantics of `Queue`.

The caller must keep the logical-device implementation and its borrowed parent
`Instance` alive, and keep the native completion primitive valid, for every
operation using the token. The primitive must belong to that logical device.
Borrowed queue views follow the existing Device implementation through a Device
move; this is not a promise that an arbitrary stored reference to a moved-from
wrapper is usable. Device move assignment can destroy the old implementation
and invalidate its borrowed views. Tokens neither extend these lifetimes nor
add tombstones or liveness checks.

The caller must not reset/reuse or destroy a fence while any use of its old
completion point still relies on it. Recreating an object with the same native
handle bits does not revive an old token. Timeline semaphore destruction/reuse
has the same borrowed-lifetime constraint. The caller supplies all Vulkan host
external synchronization needed for native access and concurrent operations;
copyability is not a thread-safety guarantee.

## Future explicit submission boundary (#258)

Future submission may produce a completion token only with an explicit
association between the actual chosen queue, submitted work, and the fence or
timeline signal point. Before native use it must validate the available Device/
Queue correlation, non-null required handles, primitive kind and explicit value,
and the effective prerequisites for the chosen Vulkan operation. Native handle
parentage, correct semaphore type, legal fence state, and externally performed
operations remain caller obligations where the library cannot verify them.

Timeline operations require the relevant feature to have been explicitly
requested and effectively enabled and the applicable native entry points to be
available. Supported capability alone, a handle, or an API version is not proof
of enablement. No construction, validation, poll, or wait silently enables a
feature/extension, recreates a Device, or substitutes another primitive.

Constructing an identity description is not a successful submit. A successful
native submit establishes the declared submission association, not completed
work. Failed submission must remain distinguishable as submit-failed (or the
more specific device-lost/native error); it must not fabricate a pending
successful submission or a completed token. The exact result representation
belongs to #258, not an invented submission API in #257.

Native submissions through escape hatches remain caller-controlled. If a future
API accepts their completion descriptions, it must preserve that they are
caller-declared associations, not retroactively verified submission receipts.
A pre-signaled fence, an already-reached timeline value, or a host signal can
satisfy a primitive observation without proving that the intended work was
submitted. The caller/submission producer must establish the correct signal
scope and association before using an observation as work-completion evidence.

## Future explicit observation: poll and wait

Polling and waiting are explicit caller operations, not side effects of token
construction, copying, inspection, destruction, or resource release. The caller
chooses when to poll, when to wait, and the wait timeout/budget, including whether
to request an unbounded wait. Poll is a nonblocking status observation; wait
observes the same completion point with the caller's explicit timeout. Neither
operation submits work, signals/resets a primitive, or calls queue/device idle.

Future observation must distinguish these semantic outcomes, without prescribing
unverified public enumerator names or a cached state machine:

| Outcome | Meaning and limit |
| --- | --- |
| Pending | A valid observation has not reached the point; a poll reports not-ready or a wait times out. Timeout is not completion or submission failure. |
| Completed | The fence is signaled, or the timeline counter has reached at least the declared target. This is evidence only for the correctly associated signal scope. |
| Submit-failed | The associated submit failed; no successful completion claim may be derived from that attempt. This is submission evidence, not something a raw status query can reconstruct. |
| Device-lost | A native operation reports device loss. It is not completed and does not authorize ordinary safe retirement. |
| Native-error | Another native failure is reported with useful native error detail, not collapsed into pending or completed. |

Invalid descriptions, missing effective identity, unavailable required features/
entry points, and invalid or moved-from Device/Queue inputs must not yield a
successful native observation. Represent these as explicit validation or
availability failures, distinct from a valid pending observation. Do not promise
that every moved-from non-owning value becomes empty: the concrete public type's
move contract controls that, and the current Queue has defaulted value moves.

Stale-parent or destroyed/reused-native-object access violates the borrowed
lifetime preconditions. There is no guarantee that it can be detected, returned
as a recoverable invalid result, or safely queried at all. Validation of live
inputs is not a liveness oracle. Device loss or another error does not trigger
automatic waits, recovery, destruction, or a fabricated completion.

## Resource release requires caller-declared AND coverage

A completion token does not enumerate resources, discover in-flight uses, or
mean "this queue/device is idle." For release safety the caller must declare
the completion condition for **every known in-flight use** of the resource and
require their logical **AND**, across every relevant queue and submission:

```text
release eligible only if every caller-declared in-flight use is completed
and the caller has accounted for every such use and prevents new uses
```

For example, separate graphics and compute uses require completion of both
declared points. Waiting for either one is insufficient. Two submissions on one
queue also require coverage of both; matching queue handles, alias groups, or
primitive handles cannot infer that coverage. A later timeline target or an
explicit join may cover earlier uses only when the caller establishes the
actual Vulkan synchronization/signal scope and declares that coverage, never
from numerical comparison or queue identity alone. Unknown or omitted uses
remain unsafe even if all listed points report completed. An empty list is
not evidence that no work is in flight.

Pending, invalid/unavailable, submit-failed, device-lost, and native-error
outcomes cannot be treated as completed terms of this AND. A failed attempt
does not discharge earlier successful uses. Caller-directed error recovery and
Vulkan destruction rules remain separate from normal completion-based release.

#259 may later consume caller-declared conditions via explicit collect/retire
operations; #257 does not implement a deletion queue or perform retirement.
There is no automatic tracking of resources, submissions, or token copies, no
inferred resource coverage, and no hidden progress worker or global registry.

## Non-goals

This contract adds no command pools, command buffers, barriers, workload/frame
loop, native primitive ownership, implicit waits, native allocation/submission,
automatic collection/retirement, feature enabling, or changes to Device
semantics. It does not require #258, #259, or #263 to make the borrowed identity
contract meaningful, nor claim that those future implementations already exist.

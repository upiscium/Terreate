# Explicit GPU retirement utility (#259)

The application constructs and owns RetirementQueue. There is no global queue,
background poller, hidden submission/use tracking, frame scheduler, automatic
synchronization, wait, queue idle or device idle. Only native-success
SubmissionReceipt copies are valid completion evidence. An arbitrary
Completion value reporting completed does not confer retirement permission.

## Explicit ownership and completion coverage

retire(owner, allUses) transfers exactly one move-only RAII owner (Buffer,
Image, ImageView, Sampler, or a caller-defined aggregate). The receipt array
must be nonempty and cover every known in-flight use of this resource. The
caller prevents any new use after declaring the final set, including uses on
different queues. Every receipt in an entry, and every receipt anywhere in
a single RetirementQueue, must reference the SAME live Device wrapper.
Distinct Queues on that Device are allowed; cross-Device queues are not
silently combined. The library verifies the borrowed Device/Queue correlation,
not the resource's underlying native provenance or full use coverage. Parent
Instance, Device, Allocator, fences, semaphores, and other needed resources must
outlive the retirement queue's operations.

Owner validity, receipt presence, Device/Queue correlation and all metadata
allocations are checked before the noexcept move into queue storage. Rejection
or allocation failure retains the live caller-owned resource. No shared_ptr,
global owner registry or auto-use association is introduced. The resource is
moved-from after a successful transfer and may not be reused by the caller.

## Normal collection

collect() runs only when explicitly invoked by the application. It performs
nonblocking SubmissionReceipt::poll() of ALL declared conditions.
An entry is released only if its entire AND set is natively completed.
The operation has two phases: if any native poll fails, none of the entries
are released. A successful timeline counter below target is pending, never
completed. The report lists collected and still-pending entry counts.

Entries are otherwise independent and can be released out of insertion order.
Do NOT retire dependent Image and ImageView as separate entries and assume
their destruction order or GPU-use coverage is inferred. Instead create a
caller-defined move-only aggregate with Image declared before ImageView, so
the aggregate's reverse member destruction destroys ImageView before Image.
The caller supplies a combined completion set sufficient for both resources.
No dependency graph or semantic lifetime inference is implemented.

## Shutdown and Device Lost

requireEmpty() reports retained owners explicitly. Destroying the queue
while it still owns any resource is a fail-fast invariant violation via
std::terminate: no implicit wait, no vkDeviceWaitIdle, and no silent
destruction of potentially in-flight resources. Normal shutdown collects
all entries after externally guaranteeing completion, and destroys the
queue only when empty. A native poll failure other than Device Lost retains
every owner and may be retried after explicit application recovery.

collect() on an actual native VK_ERROR_DEVICE_LOST returns that error,
retains every owner, and latches deviceLossObserved(). This is never
translated into normal GPU completion. The application may explicitly call
reclaimAfterDeviceLoss(the_same_device) only after the queue has observed
that native lost-device result, with the live original Device wrapper still
available. This destroys the retained owners without waits, resets, or native
polling. The caller must guarantee ALL owned native objects belong to that
lost logical Device, and ensure dependent children are inside correctly
ordered aggregates. Parent Device/Allocator/Instance must outlive teardown.
A healthy Device or other native errors cannot activate this path.
Reclaim does not reset/recreate the lost Device or make the queue reusable.

Vulkan's Lost Device rules require explicit child destruction before parent
destruction; a native VK_ERROR_DEVICE_LOST is treated as success only for
determining whether resources on that lost logical Device are in-use. It does
not prove execution normally completed. External interop with another live
Device must be accounted for separately and is not eligible for implicit
reclamation. Test-only injected device loss checks our state machine; it is
not proof that any physical GPU actually became lost.

## Verification

The headless source tests submit real GPU work and wait for actual completion,
then use scoped native query injection to check deterministic pending, completed,
native error, AND, and lost-device paths without destroying genuinely in-flight
resources. They test explicit aggregate View-before-Image destruction, owner
transfer, no unintended device idle calls, and subprocess fail-fast behavior.
The timeline-specific case is SKIP on truly unsupported devices, not PASS.
Installed package smoke also links the public RetirementQueue API.

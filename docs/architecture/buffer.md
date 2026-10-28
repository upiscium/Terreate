# VMA-backed Buffer (Issue #271)

`createBuffer(const Allocator &, const BufferDescription &) -> Result<Buffer>`
explicitly borrows a live allocator. The caller owns the description; creation
copies it and never mutates or retains a reference to it. There is no default
allocator, upload, staging, memcpy, command recording, submission, feature enabling,
GPU-use tracking or implicit wait. This document does not claim executed validation.

## Configuration and observation

`BufferDescription` has logical `size`, Vulkan-Hpp `usage`, and a value
`BufferAllocationDescription`: VMA `usage`, `flags`, Vulkan-Hpp `required_flags`,
`preferred_flags`, and `memory_type_bits`. Defaults are zero size/usage (invalid
until supplied), `VMA_MEMORY_USAGE_AUTO`, zero allocation flags/property masks and
zero memory-type bits (VMA chooses among compatible types). Host access is never
added implicitly. Select a HOST_ACCESS flag explicitly when mapping AUTO memory.

`configuration()` exposes a copied `requested` description separately from effective
`size`, `usage`, and `allocation`. No rounding or flag augmentation is performed;
these effective fields describe the arguments successfully used for creation, not
the memory selected by VMA. `allocationInfo()` returns a fresh value containing
`VmaAllocationInfo native` and actual `memory_properties`. Its allocation size may
exceed logical size. Native information includes memory type, device memory,
allocation-relative mapped pointer, memory offset and allocation size. Preferred
properties are preferences, not guarantees. Native memory can be shared with other
allocations; it must not be freed, mapped directly, rebound or otherwise invalidated.

This initial API uses exclusive queue-family sharing with no buffer creation flags,
pNext, custom pools, user data or persistent mapping. Supported buffer usage is the
core Vulkan 1.0 set plus shader-device-address when the allocator has its explicit
device-address flag. No device address query is added. Unsupported usage bits fail
before calling VMA. The application remains responsible for device limits and
usage-specific Vulkan prerequisites and for queue-family ownership transfers.

Supported allocation flags are dedicated memory, never allocate, within budget,
one host-access mode (sequential write or random), and one allocation strategy.
Unknown flags, mutually exclusive host modes/strategies, dedicated plus never
allocate, and unsupported memory usages are rejected. Legacy non-lazy memory usages,
UNKNOWN and AUTO variants are supported. Memory property requests are limited to
device-local, host-visible, coherent and cached; lazy/protected/vendor memory needs
an explicitly reviewed extension. Zero memory-type bits means no extra restriction;
nonzero bits further restrict VMA's compatible types. Persistent mapping, unbound
allocations and aliasing ownership are deliberately not supported.

## Exclusive ownership and native escape hatches

`Buffer` has no default or copy construction. Move construction/assignment transfer
one coupled buffer/allocation owner. `vmaDestroyBuffer` is called exactly once for
each successfully owned pair, including the displaced value on move assignment.
Destruction balances a still-outstanding wrapper map first; it never flushes it.
Self-move follows `unique_ptr` ownership semantics and does not duplicate ownership.

`nativeHandle()` returns borrowed `vk::Buffer`; `nativeBuffer()` returns `VkBuffer`;
`nativeAllocation()` and `nativeAllocator()` expose the borrowed VMA handles.
`allocationInfo()` provides allocation/native-memory escape hatches, not ownership.
Do not destroy/free those handles, separately destroy the buffer, change its binding,
defragment the allocation, or use native map/unmap to alter wrapper map state.
Native access does not authorize concurrent operations or extend any lifetime.

Moved-from values are invalid: handles/pointers are null, `mapped()` and `valid()`
are false, `configuration()` is null, and fallible operations return `invalid_buffer`
without entering VMA. Configuration borrows should be reacquired after moves.

The native allocator and its Device/Instance parents must outlive every Buffer and
all outstanding native or mapped access. Moving the allocator wrapper is allowed
only while preserving that native allocator; destroying or replacing it is not.
Destroy resources, then allocator, then device, then instance. Physical-device
views are borrowed from the instance. No shared ownership or stale-parent detection
is provided. Use after parent destruction is a caller lifetime violation, not a
recoverable invalid-buffer state.

## Mapping, ranges and coherency

`map() -> Result<void *>` explicitly calls `vmaMapMemory` and establishes one wrapper
mapping of host-visible memory. Duplicate maps fail. AUTO allocations also require
an explicit HOST_ACCESS allocation flag. `mappedData()` is a borrowed allocation-
start pointer; only `[0, configuration()->size)` is exposed for application access.
`unmap() -> Result<void>` balances the map; unmapping without a map fails. Mapping
ownership moves with the Buffer, so an existing pointer survives transfer but not
unmap, destruction or displacement. The application must stop pointer access first.

`flush(offset = 0, size = VK_WHOLE_SIZE)` and `invalidate` return `Result<void>` and
require an outstanding wrapper map. Offsets are buffer/allocation-relative, never
device-memory-relative. WHOLE_SIZE resolves to the remaining **logical** bytes.
Zero-length ranges, offsets at/past logical size, and oversized/overflowing ranges
fail before VMA; subtraction-based validation avoids overflow. VMA handles atom-size
alignment and allocation boundaries; callers must not add the native memory offset.
Both operations still call VMA for coherent memory, where VMA can perform a no-op.

Mapping does not synchronize GPU access. For noncoherent memory the application
flushes CPU writes before GPU consumption, and establishes GPU completion plus
required memory dependencies before invalidating and reading GPU writes. Coherent
memory removes cache-maintenance requirements, not ordering, visibility barriers,
data-race restrictions or GPU completion. Unmap is not flush. Sequential-write memory
must be used consistently with that access promise. No thread may access stale
mapping pointers, and simultaneous CPU/GPU access must obey Vulkan's rules.

All mutation, observation and borrowed-handle use must be externally synchronized
against conflicting access, moves and destruction. VMA's default internal allocator
locking does not make Buffer or mapped memory thread-safe. If the allocator uses
EXTERNALLY_SYNCHRONIZED, callers must additionally serialize the allocator-wide VMA
operations as required by VMA. Cache maintenance can cover atom-rounded bytes; obey
the associated synchronization constraints, not merely the requested byte interval.

## Failures and scope boundaries

Semantic failures use `BufferError` in `terreate.graphics.buffer`, with Core
`Error` operation/context. Failed VMA create/map/flush/invalidate calls preserve the
native `VkResult` through `vk::make_error_code`, not a message-only replacement.
Failed map leaves wrapper map state unchanged. VMA owns partial-creation cleanup;
the wrapper publishes ownership only on success. As with Allocator, C++ allocation
failure may throw; Result is not a blanket exception boundary. No independent
diagnostic sink or logging policy is introduced; existing Vulkan diagnostics remain
the parent's responsibility and callers can route returned Core errors themselves.

CPU ownership and GPU completion are distinct. The application must establish GPU
completion before destruction or move-assignment displacement. Future #259 can move
this owner into an application-chosen retirement container; this API implements no
retirement queue, fences, timeline tracking, hidden submission or waitIdle.

Integration review must resolve any requirement for concurrent sharing, persistent
mapping, broader allocation/usage flags, defragmentation, or custom pools explicitly;
none is silently added. Build/source registration, tests and installed-consumer
verification are host-owned and outside these three product files. Their absence
from this leaf's edits is not a claim of linkability or passing validation.

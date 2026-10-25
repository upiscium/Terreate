# Graphics allocator integration candidate (Issue #365)

This is an **isolated integration candidate**, not a merged, published, or
released allocator feature. The candidate Graphics CMake configuration includes
VMA as a public Graphics dependency and builds native Allocator tests; completion
and verification results must be reported with the exact source snapshot.
The presence of tests alone is not evidence of execution. The original Project
and dirty worktrees remain unchanged pending separate publication authorization.

## Ownership and application control

An `Allocator` exclusively owns exactly one `VmaAllocator`, created by
`vmaCreateAllocator` and released by `vmaDestroyAllocator`. It is move-only;
moving transfers ownership, not a second right to destroy the handle. A native
handle observer is a borrowed escape hatch, never permission to destroy it.
Independent construction produces independent allocators and configurations;
there is no Context, singleton, Engine, or shared allocator ownership.

The existing `Instance`, selected `PhysicalDeviceCandidate`/`PhysicalDevice`,
and `Device` are borrowed parents, not objects created or retained by the
allocator. A physical-device view is ultimately owned by its Vulkan instance.
Capability snapshots do not extend native lifetimes. The application must keep
the native parents alive and destroy objects in this order:

1. Buffer/Image resources and their allocations, after their GPU use ends.
2. Allocator.
3. Device.
4. Instance.

The same rules apply when move assignment displaces a live allocator. Destroying
a wrapper does not make outstanding resource borrows safe. The caller owns the
loop, scheduling, queue submissions, and synchronization; destruction must not
hide `waitIdle`. The caller establishes GPU completion and obeys Vulkan/VMA host
synchronization requirements, including external serialization when requesting
VMA's externally synchronized mode.

## Requested and effective configuration

Caller-requested VMA creation flags and an explicit Vulkan API version are
configuration inputs. An omitted API version follows the allocator's documented
default policy (Vulkan 1.0 in this candidate), not an implicit request for the
newest loader version. The
effective version must be compatible with the instance and physical device.
The request and effective configuration, including effective flags and native
handle, must remain inspectable; requested flags are not proof that a native
allocator actually uses them.

Prerequisite checking uses the parent's **enabled** extensions/features, not
merely physical-device support. Incompatible versions, unsupported flags, wrong
parents, or missing flag prerequisites are rejected. For example, requesting
buffer-device-address allocation support must not silently enable that device
feature; requesting memory-budget support must not silently enable its extension.
The caller must explicitly construct compatible parents first. Rejection must
not mutate the original request or retroactively change a Device plan.

Semantic prerequisite failures and native creation failures remain distinct.
A failed native Vulkan/VMA call preserves its `VkResult` in structured error
information rather than replacing it with a success, empty owner, or message-only
diagnostic. Missing native prerequisites in headless tests are failures, not
synthetic successful runs.

## Observation without execution control

Future #341 consumers need practical typed/native budget and statistics access,
plus physical memory heap/type visibility and the mapping from each memory type
to its heap. Native VMA budget/statistics structures and Vulkan memory properties
are useful escape hatches; typed snapshots should preserve sizes, counts, heap
indices, and availability rather than stringify them. Budgets are point-in-time
observations, not reservations or promises of successful allocation. Unsupported
budget-extension data must remain distinguishable from a measured zero.

These hooks do not imply a polling thread, hidden frame boundary, QueryPool, or
unrelated profiling subsystem. Observation cadence and synchronization belong to
the application. Integration must validate both ordinary project consumers and
installed-package consumers, including VMA symbol ownership, header/dependency
visibility, real linking, native creation/destruction, and configuration reporting.

The candidate test uses GNU-compatible linker wrapping of `vmaCreateAllocator`
and `vmaDestroyAllocator` (`--wrap=vmaCreateAllocator` and
`--wrap=vmaDestroyAllocator`). The future test target must arrange for these
references to be wrappable; static objects without cross-unit inlining are the
intended setup. This is not installed-consumer configuration. Failure injection
returns only errors; successful calls delegate to real VMA. Destruction counts
check exclusive ownership through moves and scope exit. Actual project linking
must establish that wrapping is effective; the test also checks consumption of
each injected failure. No linker configuration is changed by this candidate.
In this integration candidate, CMake compiles `allocator.cpp` twice with
distinct definitions: `terreate_graphics_vma_impl` includes the single
`VMA_IMPLEMENTATION` object with
`TERREATE_GRAPHICS_VMA_IMPLEMENTATION_ONLY=1`, while the normal
`terreate_graphics` allocator API object retains *undefined external*
`vmaCreateAllocator` / `vmaDestroyAllocator` references. The API and VMA
implementation objects are then archived in `Terreate::Graphics`. This makes
GNU linker wrapping observable at the final test executable without changing
the installed consumer API or adding an extra repository source file.

The focused linked headless test must consume each injected failure, retain its
native `VkResult` code, and verify exactly one destruction per successful
owned allocator. It also exercises actual VMA native creation, budgets,
statistics, move construction and move assignment. Project-wide CTest, package
consumers and static checks must be run in the isolated integration clone;
none of these source-level checks constitutes GitHub merge/release approval.

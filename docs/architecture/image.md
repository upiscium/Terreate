# VMA-backed Image, ImageView and Sampler (Issue #272)

`createImage(const Allocator &, const ImageDescription &) -> Result<Image>`
explicitly creates one coupled `VkImage`/`VmaAllocation` through the supplied live
allocator. It does not allocate image memory separately, adopt existing images,
create a default allocator or retain shared parent ownership. The value is C++23,
Vulkan-Hpp, move-only RAII, following Allocator (#365) and Buffer (#271).

## Requested and effective state

`ImageDescription` contains `format`, `extent`, `dimensions` (`vk::ImageType`),
`mip_levels`, `array_layers`, `samples`, `tiling`, `usage`, `initial_layout`, and
an `ImageAllocationDescription`. Defaults are undefined format, zero extent,
2D, one mip/layer/sample, optimal tiling, empty usage, undefined initial layout,
AUTO memory usage, zero allocation flags/property masks and zero memory-type bits.
The caller must supply format, extent and usage. Zero memory-type bits means no
extra restriction on VMA's compatible memory types.

`configuration()` exposes a copied `requested` description and separate effective
fields with the same names. Successful creation uses exactly these values: no
extent rounding, inferred usage, mip generation, feature enabling or allocation
policy augmentation. Mutating the caller's description cannot change the snapshot.
Effective policy is not a promise about VMA's selected memory. `allocationInfo()`
returns a fresh `ImageAllocationObservation` containing `VmaAllocationInfo native`
and actual Vulkan-Hpp `memory_properties`. Allocation size is not image texel size;
memory can be suballocated with other resources. Preferred flags are preferences.

The initial API supports core Vulkan 1.0 formats, 1D/2D/3D, optimal/linear tiling,
exclusive sharing, and core transfer, sampled, storage, color/depth-stencil and
input-attachment usage. It has no image creation flags, pNext, sparse/disjoint,
cube compatibility, transient/lazy/protected memory or extension format support.
Multisampling requires optimal 2D with one mip; multisample storage is rejected
because this allocator-only interface cannot evidence the enabled feature.
Format/type/tiling/usage support, extent/mip/layer limits and sample counts are
checked with a physical-device capability query, not by executing GPU commands.
Applications still own usage-specific enabled-feature and Vulkan prerequisites.
Material extensions of this contract require integration review, not silent defaults.

Allocation policy mirrors Buffer: non-lazy legacy usages, UNKNOWN and AUTO variants;
dedicated, never-allocate, within-budget, one host-access mode and one strategy.
Unknown/conflicting flags, dedicated plus never-allocate, unsupported usage and
memory properties outside device-local/host-visible/coherent/cached are rejected.
There are no custom pools, user data, persistent mapping, aliasing/unbound memory,
or wrapper image mapping operations.

## Initial layout is not layout tracking

Only Vulkan-valid creation layouts UNDEFINED and PREINITIALIZED are accepted.
`initial_layout` is passed directly to `VkImageCreateInfo::initialLayout`; this
never records a barrier or transitions the image. The configuration is immutable
creation history, **not** the current image layout. PREINITIALIZED does not upload
data, expose mapping, or establish visibility. Choosing a creation layout does not
make an image ready for sampling or attachment use. Callers perform transitions,
queue ownership transfers, cache maintenance and synchronization explicitly.

## Ownership, parents and native access

Image has no default or copy construction. Moves transfer the coupled owner;
move assignment immediately destroys the displaced pair. Each successful pair is
released once with `vmaDestroyImage`, never independent `vkDestroyImage` plus a
memory free. Failed native creation publishes no owner; VMA handles partial cleanup.
Moved-from Image has null handles, null configuration, false validity and
`allocationInfo()` returns `ImageError::invalid_image` without entering VMA.
Reacquire borrowed configuration/native observations after moves.

`nativeHandle()` returns borrowed `vk::Image`, `nativeImage()` returns `VkImage`,
`nativeAllocator()`/`nativeAllocation()` expose borrowed VMA handles, and
`nativeDevice()` identifies the borrowed logical device for View compatibility.
None extends a lifetime or authorizes destruction/freeing, rebinding, defragmenting
or otherwise invalidating owned memory. Native observation pointers and allocation
snapshots do not confer ownership. No stale-parent detection is promised.

ImageView borrows Image and Device; only matching native device parents are valid.
Sampler is an independent Device child, not an Image owner. Neither keeps Image,
Allocator, Device or Instance alive. A parent wrapper may be moved only if the
native parent remains live; replacing/destroying its owner while children remain
is invalid. Required destruction order is:

**ImageView -> Image -> Allocator -> Device -> Instance**

Destroy **Sampler before Device** as well. Physical-device views borrow Instance.
Destroy every other allocator child before Allocator. Destroy dependent views
before displacing an Image through move assignment, not just before its destructor.

All host access, moves, destruction and borrowed-native use require application
synchronization. VMA's internal locking does not synchronize wrapper ownership,
GPU use or application memory access; EXTERNALLY_SYNCHRONIZED additionally requires
allocator-wide external synchronization. The caller must establish completion of
all GPU use before destruction or move-assignment displacement. Immediate RAII
destruction is **not** safe merely because a GPU-used handle has left CPU scope.

## Errors and boundaries

Semantic errors use `ImageError` in `terreate.graphics.image` and Core Error
operation/context: invalid parent, invalid description, unsupported configuration
or invalid moved-from image. VMA and unexpected capability-query failures preserve
native `VkResult` via `vk::make_error_code`. Unsupported formats use the semantic
unsupported-configuration category. C++ allocation failure can still throw; Result
is not a blanket exception boundary. Parent diagnostics remain caller-controlled.

There are no submissions, barriers, uploads, render passes, hidden waits/waitIdle,
scheduling, implicit layout state or shared ownership. Transfers/transitions belong
to #264/#338; external/swapchain ownership belongs to #273. Future #259 may accept
these move-only owners into application-controlled, completion-based retirement;
this API adds no retirement behavior or automatic completion tracking.

## Headless resource coverage

`tests/test_graphics_image.cpp` uses real headless Instance/Device/Allocator and
successful native Image/View/Sampler creation without submitting work. GNU ld
wrapping of `vmaCreateImage` and `vmaDestroyImage` observes coupled ownership and
injects only explicit native failure results, never successful fake handles.
The host must register the source and those two wrap options, as with Buffer tests.
Tests cover descriptions, creation policy, allocation observations, parent matching,
moves, displacement, destroy-once and dependency-safe teardown. Missing Vulkan
capability prints BLOCKED and returns failure, never a successful PASS marker.
The `graphics.image: headless-image=PASS` marker is emitted only after successful
runtime checks. Build registration, execution and acceptance are host-owned; this
document does not assert tests have run or passed.

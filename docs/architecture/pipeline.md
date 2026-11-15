# Explicit pipeline layout and compute pipeline (#267, Phase B1)

`Terreate::Graphics` provides two exclusive, move-only RAII values:
`PipelineLayout` and `ComputePipeline`. The application owns device selection,
resource lifetimes, command recording, submission, completion and synchronization.
These factories perform synchronous native object creation only. There is no
GameLoop, Engine, RenderGraph, hidden submit, wait, descriptor lookup by name,
shader compiler, reflection backend or shared ownership.

## Descriptor-free layout and bounded push constants

Include `<terreate/graphics/pipeline_layout.hpp>` and pass an explicit
`PipelineLayoutDescription` to `createPipelineLayout(device, description)`.
An empty description means **zero descriptor set layouts and zero push constant
ranges**, not an inferred layout. This existing two-argument overload remains
descriptor-free. B1 adds a separate explicit set-layout overload; neither overload
allocates or binds descriptor sets.

The description optionally owns `std::vector<vk::PushConstantRange>` values.
Validation supports explicit masks composed of the six core stages: vertex,
tessellation control, tessellation evaluation, geometry, fragment and compute.
`eAllGraphics` is their graphics subset; extension stages, unknown bits, zero
masks and the open-ended `eAll` mask are rejected in this phase. No stage or
device feature is enabled implicitly.

Every range must have a nonzero size, a four-byte-aligned offset and size, and
a nonoverflowing 32-bit endpoint. No shader-stage bit may appear in two ranges,
**even if their byte intervals do not overlap**. Byte overlap with disjoint
stage masks is permitted by Vulkan and by this API. Consequently at most six
ranges are accepted, and validation is bounded and linear.

`validatePipelineLayoutDescription(description, max_push_constants_size)` is a
CPU-only check against an explicitly supplied byte limit. It needs no Instance
or Device and does not create a native object. The factory repeats that same
check using `device.plan()->capabilities().properties.limits.maxPushConstantsSize`.
An endpoint equal to the limit is valid. A successful CPU check against a
synthetic limit is not evidence about any particular Device.

`PipelineLayoutConfiguration` owns separate `requested` and `effective`
descriptions, plus the actual `max_push_constants_size` capability used during
creation. Requested and effective ranges are identical, including order and
masks: nothing is normalized, guessed or reflected. Native creation uses zero
flags, null `pNext`, `setLayoutCount = 0` and null `pSetLayouts`.

## B1: ordered descriptor set layouts

`createPipelineLayout(const Device&, std::span<const DescriptorSetLayout *const>,
const PipelineLayoutDescription&)` accepts an explicit ordered sequence of live
layouts. Span index defines the set number. Repeated layouts are legal and count
once for each occurrence; null holes are not legal. To represent an unused set
position, supply an explicit live empty layout. Sparse binding numbers within a
layout remain legal under the descriptor A0 contract. `PipelineLayoutDescription`
remains pointer-free and push constant validation is unchanged.

Before any native call, the factory rejects null, invalid/moved-from or null-native
layouts (`invalid_set_layout`) and another logical Device (`device_mismatch`).
It checks the set count against both the Vulkan ABI and retained DevicePlan
`maxBoundDescriptorSets`, then validates the aggregate across **all** sets against
core `maxDescriptorSet*`, `maxPerStageDescriptor*` and `maxPerStageResources` limits.
Limit violations use `PipelineLayoutError::unsupported_configuration`.

The public CPU seam `validatePipelineLayoutSetLayouts(span<const
DescriptorSetLayoutDescription>, const vk::PhysicalDeviceLimits&)` performs the
same aggregate checks without a Device or native calls, including structural A0
validation of each description. Synthetic limits do not establish real Device
capabilities. Checked subtraction before addition prevents overflow, even at
`UINT32_MAX`. Following [descriptor A0 accounting](descriptor_api.md), dynamic
buffers consume both ordinary and dynamic aggregate limits and the ordinary
per-stage buffer limit. Combined image samplers consume sampler and sampled-image
budgets but only one per-stage resource; standalone samplers consume no per-stage
resources. Stage budgets accumulate independently for each visible stage.

All temporary storage and configuration copies are prepared before the single
`vkCreatePipelineLayout` call. Allocation failure returns `allocation_failed` with
zero native calls. Native handles are passed in caller order; an empty span uses
zero count and null `pSetLayouts`. Flags remain zero and `pNext` null: graphics
pipeline libraries, update-after-bind and push descriptors are not supported or
implicitly enabled. Native `VkResult` failures and success-with-null handling are
preserved, including when diagnostic text allocation fails.

`PipelineLayoutConfiguration::set_layout_handles` contains ordered native identity
copies and `set_layout_descriptions` contains independent effective description
copies in the same order. `limits` snapshots the actual DevicePlan limits used;
the existing requested/effective push constants and `max_push_constants_size`
remain available. This configuration is pointer-free (apart from opaque native
handle representations): no wrapper or input-array pointers survive the call.
Stored handles are **observation-only and potentially stale**, not liveness proofs,
ownership, or permission to destroy or reuse a native layout.

Set-layout wrappers and their native layouts need survive only the synchronous
factory call, not the resulting PipelineLayout. The Device must outlive the
PipelineLayout, including move-assignment displacement. Callers own conflicting
host synchronization, GPU completion and later bound-descriptor-set compatibility
(#265). B1 performs no shader reflection (#262), shader/name inference, shared
ownership or hidden execution control, and adds no RenderGraph or CommandBuffer
dependency. Descriptor allocation, writes and later binding are separate APIs.

## Explicit compute stage and entry point

Include `<terreate/graphics/compute_pipeline.hpp>` and call:

```cpp
// device and shader_module are live caller-owned values on the same Device.
terreate::graphics::PipelineLayoutDescription layout_description;
layout_description.push_constant_ranges.emplace_back(
    vk::ShaderStageFlagBits::eCompute, 0, 16);
auto layout = terreate::graphics::createPipelineLayout(device, layout_description);
if (!layout) {
  // Propagate or handle layout.error(); do not continue to pipeline creation.
  return;
}
terreate::graphics::ComputePipelineDescription compute_description{"main"};
auto pipeline = terreate::graphics::createComputePipeline(
    device, shader_module, *layout, compute_description);
if (!pipeline) {
  // Propagate or handle pipeline.error().
  return;
}
// No command buffer, dispatch or submission has been created by these calls.
// At scope exit pipeline is destroyed before layout.
```

`ComputePipelineDescription::entry_point` is required caller intent. Its empty
initial value is invalid; there is no default `"main"`. The string must contain
1–4096 bytes and no embedded NUL. `max_compute_entry_point_bytes` is a library
input bound, not a claimed Vulkan limit. The owned snapshot supplies the
null-terminated native `pName`. `validateComputePipelineDescription` exposes
the same lexical check without requiring a Device.

Creation requires a live, explicit `Device`, `ShaderModule` and `PipelineLayout`.
Both children must correlate with that Device. The native stage is explicitly
`VK_SHADER_STAGE_COMPUTE_BIT` (`vk::ShaderStageFlagBits::eCompute`); it is not
deduced from SPIR-V. Exactly one `VkComputePipelineCreateInfo` is passed with
zero flags, null extension chains and no derivative/base pipeline. The
pipeline cache is null unless the explicit cache overload is used (see
[Explicit pipeline cache](#explicit-pipeline-cache-399-d1)). `pSpecializationInfo` is null unless
`ComputePipelineDescription::specialization` is nonempty (see
[Specialization constants](#specialization-constants-399-d0)). The
configuration retains independent requested/effective entry-point strings and
specializations, and the actual compute stage.

The caller must supply a real compute entry point whose shader interface is
compatible with the explicit set layouts and push constant ranges. Neither
factory validates SPIR-V instructions, entry-point existence, shader interface,
workgroup limits, capabilities, extensions, or **SPIR-V version versus the
effective Vulkan Device API version**. The structural acceptance provided by
`SpirvBinary` and the success of `createShaderModule` do not prove those facts.
Optional compiler (#261) and reflection (#262) adapters are not prerequisites.

## Borrowing, moves and destruction

- `nativeHandle()` returns a borrowed `vk::PipelineLayout` or `vk::Pipeline`;
  `nativePipelineLayout()` / `nativePipeline()` return the corresponding C
  handle. `nativeDevice()` and `nativeVkDevice()` expose the borrowed parent in
  Hpp and C forms. `ComputePipeline::nativeLayout()` is a borrowed Hpp layout.
  Never destroy any of these handles yourself.
- `correlatedWith(Device)` compares live native parent handles. A pipeline can
  also be correlated with its specific live `PipelineLayout`. These are
  same-parent checks, not liveness tracking; stale handles after destruction
  are invalid by contract. No parent wrapper address is retained.
- Device (and its parent Instance) must outlive all these children. Moving a
  Device or layout transfers ownership without changing its native handle.
  Move-replacing an owner destroys its previous native object immediately;
  dependent objects and GPU uses of that displaced object must already be done.
- A ShaderModule is borrowed only for the synchronous pipeline creation call.
  It can be destroyed immediately after that call returns, even while the
  successfully created pipeline remains alive. No module handle is retained
  as a supposedly live configuration observer.
- Retain PipelineLayout while using the pipeline and when recording commands
  that require that layout. The simple conservative lifetime rule for this API
  is to destroy ComputePipeline before its PipelineLayout, then Device. Vulkan
  has more permissive pipeline-layout lifetime rules, but this wrapper does not
  track command-buffer references or manage those advanced lifetime choices.
- The application must finish all GPU use and satisfy Vulkan host synchronization
  rules before destruction or move-assignment displacement. No destructor waits
  for idle or submits work. Configuration pointers are borrowed read-only views;
  reacquire them after moves and never retain them beyond owner destruction.
- Moved-from owners are invalid and expose null native handles and null
  `configuration()` pointers. Move construction transfers one owner; move
  assignment destroys the displaced native handle exactly once.

## Errors

Factories and CPU validators return `terreate::Result<T>` / `Result<void>`.
`PipelineLayoutError` and `ComputePipelineError` distinguish invalid descriptions,
invalid or mismatched parents, unsupported push constant limits, null-success
results and wrapper allocation failures. Native failures preserve the exact
`VkResult` through `vk::make_error_code`, with the native operation as context.

Wrapper and snapshot allocations happen before native creation and are caught;
the allocation-failure Error is code-only so reporting it needs no further
string allocation. Successful native calls returning null are errors, not empty
owners. Compute multi-create output is adopted before checking the result, so
any nonnull pipeline returned on a failed call is destroyed exactly once. There
is no implicit retry, fallback stage, entry-point substitution or device recovery.

## Specialization constants (#399 D0)

`<terreate/graphics/specialization.hpp>` defines `SpecializationDescription`,
shared by compute and graphics pipelines. It owns
`std::vector<vk::SpecializationMapEntry> map_entries` and
`std::vector<std::byte> data`. `ComputePipelineDescription::specialization`
and `GraphicsPipelineDescription::vertex_specialization` /
`fragment_specialization` are appended fields, so existing aggregate
initialization keeps working. An empty description (no entries, no data) means
no `pSpecializationInfo`, exactly as before.

```cpp
terreate::graphics::ComputePipelineDescription description{"main"};
const std::uint32_t factor = 2;
description.specialization.map_entries = {
    vk::SpecializationMapEntry{/*constantID*/ 5, /*offset*/ 0, sizeof(factor)}};
description.specialization.data.resize(sizeof(factor));
std::memcpy(description.specialization.data.data(), &factor, sizeof(factor));
```

Creation copies the description into the pipeline's requested/effective
configuration. The native `VkSpecializationInfo` points into that owned
effective copy (data) and into native `VkSpecializationMapEntry` values copied
field by field for the call, so caller storage is not borrowed and stays
unchanged.

`validateSpecializationDescription` runs, through the pipeline validators,
before any native call and returns a typed `SpecializationError`:

- `entry_out_of_range`: an entry's offset is not below `dataSize`
  (VUID-VkSpecializationInfo-offset-00773) or its size exceeds
  `dataSize - offset` (-pMapEntries-00774). Written so no sum overflows.
- `duplicate_constant_id`: constant IDs must be unique (-constantID-04911).
- `empty_entry`: a zero size can match no shader constant
  (VUID-VkSpecializationMapEntry-constantID-00776).
- `data_without_entries`: data with no entries is rejected as a caller
  mistake.
- `too_large`: the entry count must fit `uint32_t`.

Terreate does not parse SPIR-V. Whether each entry's size matches the declared
constant type (00776; booleans use `sizeof(VkBool32)`) and which constant IDs
the shader declares remain caller responsibilities. Entries for IDs the shader
does not declare are valid Vulkan and ignored.

## Explicit pipeline cache (#399 D1)

Both factories have an overload that takes a caller-owned `vk::PipelineCache`:

```cpp
// The application created `cache` on `device` and owns it.
auto pipeline = terreate::graphics::createComputePipeline(
    device, shader_module, *layout, compute_description, vk::PipelineCache{cache});
```

`createGraphicsPipeline(device, layout, vertex, fragment, description, cache)`
works the same way. The existing overloads are unchanged and still pass a null
cache.

- A null cache passed to the explicit overload is rejected with
  `invalid_pipeline_cache` (appended to `ComputePipelineError` and
  `GraphicsPipelineError`; existing values unchanged), before any native call.
  Use the existing overload for "no cache".
- The cache is borrowed only during the synchronous creation call. Terreate
  never creates, retains, serializes or persists it.
- `configuration()->pipeline_cache` records which cache was passed
  (`VK_NULL_HANDLE` for the existing overloads). It is an observation only and
  does not keep the cache alive.
- **Provenance is not verified.** Vulkan offers no query for a cache's Device,
  so Terreate cannot prove that the cache belongs to `device`
  (VUID-vkCreateComputePipelines-pipelineCache-parent and its graphics
  equivalent). Passing a cache from another Device is a caller error.
- Host synchronization is the application's: a cache created with
  `VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT` must not be used
  concurrently (VUID-vkCreateComputePipelines-pipelineCache-02873 and
  VUID-vkCreateGraphicsPipelines-pipelineCache-02876). Keep the
  cache alive through the call and destroy it when the application chooses.

## Tests and installed consumer

- `graphics.pipeline.cpu`: pure description/entry-point validation, count and
  arithmetic bounds, allowed/disallowed stage masks, overlapping/disjoint
  ranges, limit edges and structured errors. No Instance, Device or native create.
- `graphics.pipeline_layout`: headless native empty and push-constant layouts,
  requested/effective snapshots, Device limits, move-only ownership, parent
  correlation, failure injection and success-with-null interception.
- `graphics.compute_pipeline`: the existing offline-compiled 70-word SPIR-V 1.0
  compute fixture, exact `pName = "main"`, compute stage, module/layout/Device
  correlation, two-Device mismatch rejection, native failures, null-success,
  partial-output cleanup, module release after creation and exact move cleanup.
  It also checks that an empty specialization passes a null
  `pSpecializationInfo`, that a nonempty one reaches Vulkan with exact entries
  and bytes pointing into the pipeline's own copy, and that an invalid one is
  rejected with its typed error before native creation. For #399 D1 it creates a
  test-owned `VkPipelineCache`, checks that the explicit overload rejects a null
  cache without native creation, that the existing overload still passes null,
  and that the explicit cache reaches `vkCreateComputePipelines` and is
  observable; `graphics.graphics_pipeline` does the same for graphics.
- `graphics.pipeline.cpu` additionally covers every `SpecializationError`.
- `graphics.pipeline_commands` dispatches a specialized compute shader (two
  constants at swapped data offsets) and reads back
  `(base + i * scale) * factor + bias`, proving the GPU used both constants.

Tests intercept the actual four Vulkan C create/destroy functions using the
existing GCC linker-wrapper convention. Artificial null-success interception
does not call the real create function and cannot discard a real native handle.
No test records or submits GPU work. Residual tracked objects on assertion
failure are reported and cleaned before Device destruction.

When available, the Validation Layer and Debug Utils are enabled. A synthetic
WARNING|VALIDATION callback verifies delivery to the application recorder; only
that synthetic warning is subtracted. Actual warnings/errors, including native
teardown, fail the test. A missing layer is reported separately, not counted as
validation coverage. Missing loader, physical device or usable compute queue is
**BLOCKED with nonzero exit**, never a skipped native success.

Package smoke explicitly permits and requires both installed headers, compiles
their public types, links their validators/factories/observers and creates a real
layout and compute pipeline in the external Graphics consumer without dispatch.
It also creates a specialized compute pipeline, checks that `data_without_entries`
is rejected, and must print `package.specialization: runtime=PASS`. It then
passes a consumer-owned native `VkPipelineCache` through the explicit overload,
checks the null-cache rejection and the observed cache, and must print
`package.pipeline_cache: runtime=PASS`.
The existing Vulkan-free Core-only consumer and isolation checks remain intact.

## Deferred surface

Graphics pipelines and later descriptor binding (#265) remain separate
work; B1 only integrates explicit layouts from #266. Pipeline cache creation,
serialization and persistence (#330) are not provided; only the explicit
borrowed cache hook above exists. Extension
stages/chains, descriptor allocation, reflection,
pipeline compilation tooling, recording conveniences and execution scheduling
are not silently provided by these synchronous creation APIs.

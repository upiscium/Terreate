# Descriptor API — Issue #266, Phase A0

Phase A0 is **DescriptorSetLayout only**, not a complete descriptor API. The
installed entry point is `<terreate/graphics/descriptor_set_layout.hpp>` in
`Terreate::Graphics`. It does not add a dependency to Core-only consumers.

## Explicit, owned descriptions

`DescriptorSetLayoutDescription` owns an ordered vector of
`DescriptorSetLayoutBindingDescription` values. Each binding explicitly supplies
`binding`, `descriptor_type`, positive `descriptor_count`, and `stage_flags`.
Binding numbers must be unique, but need not be contiguous or increasing. The
vector's order is preserved in native creation and both configuration snapshots;
there is no sorting, normalization, implicit numbering, or semantic inference.

An explicitly empty description is valid. A default binding is intentionally
invalid (unsupported type sentinel, zero count, empty stages), not an implicit
sampler declaration. There are no borrowed input arrays, resource pointers,
immutable sampler options, native create flags, or extension chains in the
description. Shader reflection, compilation, and automatic layout inference are
outside this API.

Supported types and their accounting are bounded to these eight core types:

| Descriptor type | `maxDescriptorSet…` budget | `maxPerStageDescriptor…` budget |
| --- | --- | --- |
| `eUniformBuffer` | `UniformBuffers` | `UniformBuffers` |
| `eUniformBufferDynamic` | both `UniformBuffers` and `UniformBuffersDynamic` | `UniformBuffers` |
| `eStorageBuffer` | `StorageBuffers` | `StorageBuffers` |
| `eStorageBufferDynamic` | both `StorageBuffers` and `StorageBuffersDynamic` | `StorageBuffers` |
| `eSampler` | `Samplers` | `Samplers` |
| `eSampledImage` | `SampledImages` | `SampledImages` |
| `eStorageImage` | `StorageImages` | `StorageImages` |
| `eCombinedImageSampler` | both `Samplers` and `SampledImages` | both `Samplers` and `SampledImages` |

Input attachments and texel buffers are deliberately unsupported in A0. So are
mutable descriptors, inline uniform blocks, acceleration structures, extension
types, descriptor indexing, variable descriptor counts, update-after-bind,
push descriptors, immutable samplers, and non-null native extension chains.
Unsupported input fails closed; no features or extensions are silently enabled.

Stage visibility must be a nonempty subset of vertex, tessellation control,
tessellation evaluation, geometry, fragment, and compute. `eAllGraphics` is the
explicit combination of the five graphics bits and is accepted, also together
with compute. `eAll`, extension bits, and unknown bits are rejected. A visibility
bit does not create a shader, require a queue for that stage, or enable a Device
feature. Later shader/pipeline creation must satisfy its own feature requirements.

## CPU validation and effective state

`validateDescriptorSetLayoutDescription(description, limits)` takes an explicit
`vk::PhysicalDeviceLimits` value by const reference and returns `Result<void>`.
It makes no Vulkan calls and needs no Instance, Device, GPU, or mutable global
state. Duplicate detection can allocate CPU storage; allocation failure is a
structured error, not native work. Input is not modified.

The validator checks the binding count before narrowing to Vulkan's `uint32_t`
ABI and uses checked subtraction before every count addition. It checks each
set category and, independently for **each visible stage**, each stage category
and `maxPerStageResources`. Dynamic buffer descriptors count against BOTH the
aggregate uniform/storage buffer set limit and their dynamic-specific set limit;
regular and dynamic buffers also share their corresponding per-stage limit. A
combined image sampler consumes both sampler and sampled-image category budgets, but counts
only once against `maxPerStageResources`. Standalone samplers do **not** consume
`maxPerStageResources`. Counts are neither multiplied by the number of stages
for the set budgets nor summed across disjoint stages for stage budgets.

These checks bound one layout's contribution to the core pipeline-layout
budgets. They do not reserve capacity, combine multiple set layouts, check
shader compatibility, query implementation-specific layout support, or guarantee
native allocation success. Later pipeline-layout integration must validate the
combined limits again. `maxBoundDescriptorSets` is not a binding-number limit,
and A0 does not invent an arbitrary maximum binding index or resource total.

`createDescriptorSetLayout(Device&, description)` repeats validation using
`device.plan()->capabilities().properties.limits`. The resulting
`DescriptorSetLayoutConfiguration` contains independent, identical `requested`
and `effective` descriptions and a pointer-free `limits` snapshot of those
actual capabilities. Native creation uses a temporary owned array with exactly
those bindings, zero flags, a null extension chain, and null immutable sampler
arrays. Empty layouts use zero bindings and a null binding array. Neither the
caller vector nor the temporary native array is retained by pointer.

For example, with a live application-owned `Device &device`:

```cpp
terreate::graphics::DescriptorSetLayoutDescription description{
    {{7, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex},
     {2, vk::DescriptorType::eCombinedImageSampler, 4, vk::ShaderStageFlagBits::eFragment}}};

auto layout = terreate::graphics::createDescriptorSetLayout(device, description);
if (!layout) {
  // Handle layout.error(); there is no native owner on failure.
} else {
  const auto *configuration = layout->configuration(); // borrowed owned snapshot
  VkDescriptorSetLayout native = layout->nativeDescriptorSetLayout(); // borrowed
  // Inspect configuration, or use native with explicit application-owned Vulkan code.
  // Do not destroy native. Keep device alive until this owner is destroyed.
}
```

## Ownership, parent correlation, and failure policy

`DescriptorSetLayout` is a move-only PImpl RAII value owning exactly one native
`VkDescriptorSetLayout`. It borrows the **native VkDevice**, not the address of
the `Device` wrapper. Moving that wrapper does not change the child's parent.
`correlatedWith(Device)` compares live native handles; a separately created
logical Device is not interchangeable, even on the same PhysicalDevice.

`valid()`, explicit `operator bool()`, `nativeHandle()` (`vk::DescriptorSetLayout`),
`nativeDescriptorSetLayout()` (`VkDescriptorSetLayout`), `nativeDevice()`,
`nativeVkDevice()`, and `configuration()` expose the ownership state. Moved-from
values are invalid, return null native handles and a null configuration pointer,
and correlate with no Device. Reacquire configuration observers after a move.
Move assignment destroys the displaced native layout exactly once using its own
creating Device, then takes the source ownership.

All native observers are borrowed escape hatches, not ownership transfers. The
application must destroy the layout before its Device, including resources
displaced by move assignment. It must enforce host synchronization, the Vulkan
lifetime rules for any native dependent objects it creates, and completion of
GPU use before teardown. A0 does not allocate sets or manage their lifetimes.
There is no shared ownership, liveness registry, hidden wait, command recording,
submission, dispatch, or execution loop. RAII is not GPU synchronization.

Malformed descriptions, unsupported types/limits, moved-from parents, wrapper
allocation failures, and success with a null native handle are explicit
`DescriptorSetLayoutError` values carried in `terreate::Result` / `terreate::Error`.
Native failures retain `vk::make_error_code(vk::Result)` and the native operation
context. If allocating diagnostic text fails, the original error code is still
retained without text. CPU allocations and configuration copies catch
`std::bad_alloc`; no real handle is created before these copies succeed. A
single-object native failure transfers no ownership, and no undefined failed
output is destroyed. Successful non-null output is immediately adopted.

## Test and package boundaries

The source registers two separate CTests:

- `graphics.descriptor_set_layout.cpu`: synthetic limits, supported/unsupported
  types, malformed bindings, unordered sparse indices, exact endpoints, checked
  accumulation, combined sampler/image accounting, dynamic buffer accounting,
  and all six stage budgets. It does not initialize Vulkan or create a Device.
- `graphics.descriptor_set_layout`: real headless Vulkan with native C linker
  wrappers observing the exact create info and ordered bindings, input-storage
  release during creation, forced failure/null success without driver allocation,
  two real Devices, parent/owner moves, and balanced destruction. The Validation
  Layer and Debug Utils are required. A synthetic warning probes the actual
  callback transport; real creation and complete teardown must produce zero
  validation warnings/errors. Missing prerequisites are **BLOCKED**, with nonzero
  exit status, never a synthetic pass or skip. No GPU work is submitted.

Strict CTest success/failure expressions distinguish these modes. Package smoke
requires the installed public header and actually calls the installed factory,
borrows native handles, inspects effective configuration, and destroys the
layout before its Device. Its existing compute-pipeline consumer resolves an
explicit compute Device independently of the main consumer's possible
transfer-only queue. Core-only Vulkan independence is unchanged. These are
test definitions, not a claim that this source-only implementation ran them;
execution and independent review belong to the separate quality actor.

## Deferred phases and design questions

- **B0:** DescriptorPool and DescriptorSet allocation, ownership, reset/free
  policy, and parent lifetime design. None is exposed by A0.
- **B1:** descriptor writes/updates, explicit buffer/image/sampler validation,
  and resource provenance. No writes, updates, or binds exist in A0.
- **#267 integration:** the existing Phase A `PipelineLayout` remains
  descriptor-free and unchanged. Passing set layouts into it, aggregate limit
  validation, and descriptor binding are deferred. Native Vulkan consumers can
  already borrow `VkDescriptorSetLayout` under their own explicit contracts.

Before B1, discuss `Buffer`'s lack of public native Device provenance: it exposes
its native Allocator and Buffer, but no `nativeDevice()` / Device correlation.
A future write API must not infer Device identity from a raw buffer, equate an
Allocator handle with a Device, or silently accept cross-Device writes. Resolve
that provenance contract explicitly in a separately approved change; A0 neither
changes Buffer nor pretends that complete resource-write validation exists.

# Asset-Graphics Integration: uploading a MeshData and an ImageData (#279)

`Terreate::IntegrationAssetGraphics` (`terreate::integration::asset_graphics`) turns an Asset
`MeshData` into Graphics buffers and the upload items of `prepareUploadBatch`. Its header is
`<terreate/integration/asset_graphics/mesh_upload.hpp>`.

This page describes part 1 of #279 (meshes) first; part 2, uploading an `ImageData` as an `Image`
(`<terreate/integration/asset_graphics/image_upload.hpp>`), is described in its own section after the mesh
sections, in the same three explicit steps.

## Responsibility and non-goals

Asset describes geometry without a GPU and Graphics owns Vulkan without knowing what a mesh is.
Neither depends on the other, and neither depends on this Integration, which is
the one place that knows both and uses only their public APIs.

It does three things, as three explicit steps:

1. `planMeshUpload(mesh, description)` describes the conversion on the CPU and creates nothing.
2. `createMeshBuffers(allocator, plan)` creates the vertex buffer and, for an indexed mesh, the
   index buffer.
3. `meshUploadItems(plan, buffers)` returns the `graphics::BufferUploadItem` values the
   application passes to `graphics::prepareUploadBatch`.

Non-goals: it records no command, selects no queue, submits nothing and waits for nothing. It
creates no pipeline and has no draw helper. It does not guess shader locations, deduplicate
vertices or narrow or widen indices. It handles no `ModelData` (#276); images are part 2 below. A one-call convenience for beginners is planned separately (issue #463, below) and is
not part of this work.

## Public dependencies

Exactly `Terreate::Asset` and `Terreate::Graphics` (and, through them, Core, Vulkan and VMA). Not
Platform, not GLFW. The component matrix and the package smoke test check that the target links
exactly these two, that an Asset-only build has no Graphics target and no Vulkan, that a
Graphics-only build has no Asset target, and that no module links the Integration.

## Ownership, borrowing and lifetime

- `MeshUploadPlan` owns the bytes: `vertex_bytes` and `index_bytes`. It is a plain value you can
  inspect, copy and edit before the next step.
- `MeshBuffers` owns the Vulkan buffers: move-only `graphics::Buffer` values.
- `meshUploadItems` returns items that **borrow**: `bytes` points into the plan and `destination`
  points at the buffers. **The plan must outlive `prepareUploadBatch`** (which copies the bytes
  into staging and keeps no span) and must not change in between. The buffers must stay alive and
  in place through every GPU use.
- C++ lifetime is not GPU lifetime. Destroying `MeshBuffers` never waits; retire them through a
  `graphics::RetirementQueue` after the submission that uses them completes.

## Execution authority and threading

The Integration owns no thread, queue or wait. `planMeshUpload` is a pure function of its
arguments. `createMeshBuffers` calls `graphics::createBuffer` and inherits its host
synchronization rules. The application keeps the timing: when to prepare, which command buffer,
the barriers, the queue and the fence or timeline that proves completion
(`recordUploadBatch`, `submitFenced`, `RetirementQueue`).

## Requested, supported and effective configuration

`MeshUploadDescription` is the request; `MeshUploadPlan` is the effective result, observable
before anything exists:

| Request (`MeshUploadDescription`) | Effective (`MeshUploadPlan`) |
| --- | --- |
| `attributes`: `{AttributeKey, location}` in order | `attributes` (`vk::VertexInputAttributeDescription`: location, binding, format, offset) |
| `binding` (default 0) | `binding` (`vk::VertexInputBindingDescription`: number, stride, per-vertex rate) |
| the mesh's topology | `topology` (`vk::PrimitiveTopology`) |
| the mesh's indices | `index_type` (`eUint16`, `eUint32`, or `eNoneKHR` when not indexed), `index_count`, `index_bytes` |
| the mesh's vertices | `vertex_count`, `vertex_bytes` (interleaved and packed, equal to `asset::interleave`) |
| `extra_vertex_usage`, `vertex_allocation` | `vertex_buffer` (`graphics::BufferDescription`: size, usage, allocation) |
| `extra_index_usage`, `index_allocation` | `index_buffer` (an optional `BufferDescription`; empty when not indexed) |
| attributes of the mesh without a binding | `unmapped` |

The rules, none of them hidden:

- **Shader locations are the application's.** Each binding names the mesh attribute and its
  location. Nothing is guessed from the semantic, so there is no default "position 0, normal 1".
- **Unbound attributes are reported.** A mesh attribute with no binding is not in the vertex
  stream and is listed in `plan.unmapped`: never dropped silently.
- **Formats**: float32 with 1 to 4 components map to `eR32Sfloat`, `eR32G32Sfloat`,
  `eR32G32B32Sfloat` and `eR32G32B32A32Sfloat`, the only unambiguous mapping. A wider Asset
  format set adds entries later.
- **Topologies** map one to one: triangle list, triangle strip, line list, point list.
- **Buffer usage** always includes `eTransferDst` (the upload target) plus `eVertexBuffer` or
  `eIndexBuffer`, plus the application's extra usage (for example `eTransferSrc` to read it back).
  The default memory is the `device_local` intent (`graphics::bufferAllocationFor`); any
  `BufferAllocationDescription` can replace it.
- **Indices are used as stored**: `uint16` stays `uint16`, `uint32` stays `uint32`.
- **Applying the plan to a pipeline**: `plan.applyTo(pipeline_description)` replaces
  `vertex_bindings` and `vertex_attributes` and sets `topology` on a
  `graphics::GraphicsPipelineDescription`; every other member is left alone. The plan holds the
  native structs, so you may also copy them yourself.

## Errors and diagnostics

Fallible functions return `terreate::Result`. Errors of this Integration are `MeshUploadError` in
the category `terreate.integration.asset_graphics`; the Error's context is the function and its
detail names the offending attribute or location. Two kinds of error pass through unchanged: an
invalid mesh returns its `asset::MeshDataError` (from `asset::validate`), and a buffer creation
failure returns the `graphics::BufferError` or native error of `graphics::createBuffer`.

| `MeshUploadError` | Meaning |
| --- | --- |
| `no_attributes` | `MeshUploadDescription::attributes` is empty (it never means "everything") |
| `attribute_not_found` | a binding names an attribute the mesh does not have |
| `duplicate_attribute` | two bindings name the same mesh attribute |
| `duplicate_location` | two bindings use the same shader location |
| `unsupported_format` | a bound attribute's format has no Vulkan mapping here |
| `unsupported_topology` | the mesh topology has no Vulkan mapping here |
| `empty_mesh` | no vertices, or an indexed mesh with no indices: a zero-sized buffer cannot be created |
| `size_overflow` | a count, stride or size does not fit its 32-bit Vulkan field |
| `plan_mismatch` | the plan is not self-consistent (sizes, required usage, index buffer), or the buffers do not match the plan |

`planMeshUpload` checks in this order: the mesh (`asset::validate`), `no_attributes`,
`unsupported_topology`, then each binding in order (`attribute_not_found`, `duplicate_attribute`,
`duplicate_location`, `unsupported_format`), `empty_mesh`, `size_overflow`. Since `validate` refuses
an enumerator Asset does not define, `unsupported_format` and `unsupported_topology` become
reachable when Asset adds formats or topologies this Integration does not map yet. Allocation
failure throws `std::bad_alloc`.

## Optional and required dependencies

Asset and Graphics are both required; Vulkan and VMA come with Graphics. The CMake option
`TERREATE_BUILD_INTEGRATION_ASSET_GRAPHICS` is on by default only when `TERREATE_BUILD_ASSET` and
`TERREATE_BUILD_GRAPHICS` are; turning it on without one of them is a configure error that names
the missing option, and Terreate never enables anything implicitly. `find_package(Terreate
REQUIRED COMPONENTS IntegrationAssetGraphics)` imports Asset, Graphics and Core, with the same
required-or-optional Vulkan semantics as `IntegrationPlatformGraphics`: a required request fails
when Vulkan is missing, an optional one leaves other requested components usable.

## Native or backend escape hatch

The plan is made of native Vulkan-Hpp values (`vk::VertexInputBindingDescription`,
`vk::VertexInputAttributeDescription`, `vk::PrimitiveTopology`, `vk::IndexType`) and ordinary
`graphics::BufferDescription` values, and `graphics::Buffer` exposes `nativeHandle()`. Nothing
requires the Integration after the buffers exist: bind the buffers with the Graphics recording
functions or with raw Vulkan. Skipping the Integration is also fine: `asset::interleave` and
`graphics::createBuffer` are the lower-level API it is written on.

## Minimal explicit example

```cpp
#include <terreate/integration/asset_graphics/mesh_upload.hpp>

#include <terreate/asset/primitives.hpp>
#include <terreate/graphics/upload_batch.hpp>

namespace asset = terreate::asset;
namespace ag = terreate::integration::asset_graphics;
namespace graphics = terreate::graphics;

// 1. CPU only: describe it. The shader reads position at location 0 and uv at location 2.
auto box = terreate::unwrap(asset::box({}));
ag::MeshUploadDescription description;
description.attributes = {{{asset::AttributeSemantic::position, 0, {}}, 0},
                          {{asset::AttributeSemantic::texcoord, 0, {}}, 2}};
auto plan = terreate::unwrap(ag::planMeshUpload(box, description));
// plan.unmapped lists the normal: no binding, so not in the stream.

// The same plan feeds the pipeline description.
graphics::GraphicsPipelineDescription pipeline;
plan.applyTo(pipeline);

// 2. Create the buffers (no data yet).
auto buffers = terreate::unwrap(ag::createMeshBuffers(allocator, plan));

// 3. The items borrow plan and buffers; keep the plan alive until prepareUploadBatch returns.
auto items = terreate::unwrap(ag::meshUploadItems(plan, buffers));
auto batch = terreate::unwrap(
    graphics::prepareUploadBatch(allocator, graphics::UploadBatchDescription{.buffers = items}));

// The application records the batch, submits with its own fence, waits, and retires the staging:
//   graphics::recordUploadBatch(device, command_buffer, batch);
//   graphics::submitFenced(device, queue, fence, identity, {.command_buffers = commands});
//   graphics::RetirementQueue::retire(batch, receipts) ... collect()
```

`integration.asset_graphics` runs this headless against a real device: it creates the buffers from
a plan, uploads with `prepareUploadBatch` and `recordUploadBatch`, submits with a fence, waits for
it, reads both buffers back through a `ReadbackBatch` and compares them with `vertex_bytes` and
`index_bytes`, with the validation layer required and silent. `integration.asset_graphics.plan`
checks the plan fields and the errors reachable without a device on the CPU (`plan_mismatch` is checked in the GPU test, which needs an Allocator), and the installed-package consumer in
`cmake.package_smoke` plans a mesh against an installed package.

## Convenience API and its lower-level equivalent

This API is the explicit layer. A beginner-oriented one-call convenience, which would plan, create
the buffers and upload in one step for someone who does not want to see the three steps, is
planned as issue #463 on top of this API. It is not part of this work, and this API does not
change to make room for it. Until then, the lower-level equivalent of anything it would do is
exactly the three calls above, and below them `asset::interleave`, `graphics::createBuffer` and
`graphics::prepareUploadBatch` can be called directly.

## What the consistency checks cover

`createMeshBuffers` and `meshUploadItems` check that the plan agrees with its own bytes and with the
buffers: the byte counts equal the buffer sizes, `eTransferDst` and the vertex or index usage are set, and
the index buffer exists exactly when there are index bytes. They do not re-derive `stride`, `index_count`
or the attribute offsets from the bytes, so a plan edited in those fields is the application's
responsibility. Device limits (`maxVertexInputBindingStride`, attribute counts) are checked when the
pipeline is created, not here.

## Part 2: uploading an ImageData (#279)

Header: `<terreate/integration/asset_graphics/image_upload.hpp>`. It follows the mesh shape exactly: three
explicit steps, nothing records, submits or waits, and the application keeps the timing.

1. `planImageUpload(image, description)` describes the conversion on the CPU and creates nothing.
2. `createImage(allocator, plan)` creates the `graphics::Image` the plan describes (no data, no layout
   transition, **no `ImageView` and no `Sampler`**).
3. `imageUploadItems(plan, image, destination)` returns the `graphics::ImageUploadItem` values for
   `graphics::prepareUploadBatch`: one per (mip, layer).

### What the plan says (`ImageUploadPlan`)

| Field | Meaning |
| --- | --- |
| `format` | the Vulkan format (mapping below) |
| `image` | the `graphics::ImageDescription`: extent, `mip_levels`, `array_layers`, 2D, one sample, optimal tiling, usage, `eUndefined` initial layout, allocation |
| `copy_layout` | `eTransferDstOptimal`: the layout each subresource must be in when `recordUploadBatch` runs |
| `subresources` | one entry per (mip, layer), layer-major: mip, layer, byte offset and count inside `ImageData::data`, the row pitch in bytes, and the `VkBufferImageCopy2` region (color aspect, that mip and layer, the mip extent, `bufferRowLength` = the pitch in texels, `bufferImageHeight` 0, `bufferOffset` 0) |
| `mip_generation`, `layout_transition` | reserved choices, see below |

- **Mapping** (pixel format and `ColorInterpretation`): `srgb` maps to the sRGB format where one exists
  (`r8`, `rg8`, `rgba8` give `R8_SRGB`, `R8G8_SRGB`, `R8G8B8A8_SRGB`); `linear` and `normal_map` map to
  the unorm or float one (`R8`/`R8G8`/`R8G8B8A8_UNORM`, `R16`/`R16G16`/`R16G16B16A16_UNORM`,
  `R32`/`R32G32`/`R32G32B32A32_SFLOAT`). `rgb8` has no portable Vulkan format (three-component 8-bit formats
  need not support sampling or transfer) and is refused with `unsupported_format`; repack it as `rgba8`.
  The alpha mode is not part of the plan: Vulkan has no such property, so shaders and blend state interpret it.
- **Usage** always includes `eTransferDst` and `eSampled`, plus `ImageUploadDescription::extra_usage`.
  Whether the device supports the format for them is not checked by the plan: ask
  `graphics::queryFormatProperties`; `createImage` returns the device's refusal unchanged.
- **Every subresource present is uploaded as it is.** There is no mip generation, no conversion and no
  re-packing: a padded row pitch becomes an explicit `bufferRowLength`, and the item's bytes are the
  subresource's bytes from its first pixel to the end of its last row (`ImageData::bytes(mip, layer)`),
  which is exactly the footprint `prepareUploadBatch` requires. A row pitch that is not a multiple of the
  texel size cannot be expressed in texels: `unaligned_row_pitch` (repack with `asset::packedLayout`).
  A 3D image (depth above 1) is `unsupported_image`.
- **Reserved opt-in choices.** `ImageUploadDescription` has `mip_generation` and `layout_transition`, each
  an `enum class` whose only value is `none`. They exist so that the follow-ups can add an explicit opt-in
  without changing this API: mip generation is #467 and a recorded layout transition is #468. Until
  then nothing is generated and nothing is transitioned; any other value is `unsupported_option`.
- **Layouts are the application's.** The plan states `copy_layout`; before `recordUploadBatch` the
  application moves every subresource from `eUndefined` to it with `graphics::imageBarrier` (through
  `recordBarriers` or `recordImageBarrier`), and after the upload to whatever it needs next
  (`eShaderReadOnlyOptimal`, `eTransferSrcOptimal`).

### Lifetime rule

The plan holds **no pixels**. `imageUploadItems` returns items that borrow: `bytes` points into
`ImageData::data` of the `image` argument and `destination` points at the created image. **The `ImageData`
must outlive `prepareUploadBatch`** (which copies the bytes into staging and keeps no span) and must not
change in between; it may be destroyed afterwards. The image must stay alive and in place through every GPU
use. As with meshes, C++ lifetime is not GPU lifetime: retire the image and the staging batch through a
`graphics::RetirementQueue`. `imageUploadItems` checks that `image` and `destination` are the ones the plan
was made for and returns `plan_mismatch` if the format mapping, extent, mips, layers or any subresource
range differs.

### Errors

The category is `terreate.integration.asset_graphics.image_upload` (`ImageUploadError`). An image that
fails `asset::validate` returns its `asset::ImageDataError` unchanged, and `graphics::createImage` failures
are returned unchanged.

| `ImageUploadError` | Meaning |
| --- | --- |
| `unsupported_format` | the pixel format has no portable Vulkan format (`rgb8`) |
| `unsupported_image` | a 3D image |
| `unaligned_row_pitch` | a row pitch is not a multiple of the texel size |
| `size_overflow` | a row exceeds 2^31 - 1 bytes |
| `unsupported_option` | a reserved choice holds a value other than `none` |
| `plan_mismatch` | the plan is not self-consistent, or the `ImageData` or `Image` is not the plan's |

`planImageUpload` checks in this order: `asset::validate`, the reserved choices, the format, the
dimensionality, then each subresource.

### Example

```cpp
#include <terreate/integration/asset_graphics/image_upload.hpp>
#include <terreate/graphics/barriers.hpp>
#include <terreate/graphics/upload_batch.hpp>

namespace ag = terreate::integration::asset_graphics;
namespace graphics = terreate::graphics;

// 1. CPU only: describe it (an `ImageData` decoded or generated elsewhere).
auto plan = terreate::unwrap(ag::planImageUpload(image));  // srgb rgba8 -> R8G8B8A8_SRGB

// 2. Create the image (undefined layout, no view, no sampler).
auto texture = terreate::unwrap(ag::createImage(allocator, plan));

// 3. The items borrow `image` and `texture`; keep `image` alive until prepareUploadBatch returns.
auto items = terreate::unwrap(ag::imageUploadItems(plan, image, texture));
auto batch = terreate::unwrap(
    graphics::prepareUploadBatch(allocator, graphics::UploadBatchDescription{.images = items}));

// The application records the whole sequence, submits with its own fence and waits:
//   recordImageBarrier(device, cmd, texture, {undefined -> plan.copy_layout, ...});
//   recordUploadBatch(device, cmd, batch);
//   recordImageBarrier(device, cmd, texture, {plan.copy_layout -> eShaderReadOnlyOptimal, ...});
//   submitFenced(...); fence.wait(...); RetirementQueue::retire(batch, receipts) ... collect()
// then creates its own ImageView and Sampler.
```

`integration.asset_graphics.image` runs this headless against a real device, for each of the twelve Vulkan
formats the mapping produces: it creates the image from a plan (two mips, two layers, one layer with padded
rows), transitions with `recordImageBarrier`, uploads, submits with a fence, waits for it, copies every
(mip, layer) into a buffer, reads that buffer back through a `ReadbackBatch` and compares with the
`ImageData`, with the validation layer required and silent. A format the device cannot create or use for
transfer and sampling is reported as `format=... SKIP reason=...` and only an all-formats miss makes the test
`image=SKIP`; a skipped format is never counted as a pass. `integration.asset_graphics.image_plan` checks
the plan for every format family, the mip and layer table, padding and every error on the CPU, and the installed
package consumer plans an image against an installed package.

### The lower-level equivalent

Without this Integration: `asset::packedLayout` or your own table gives the rows, `graphics::createImage`
creates the image with the format you pick, and you build one `graphics::ImageUploadItem` per (mip, layer)
with a `VkBufferImageCopy2` (zero `bufferOffset`, the row length in texels) and `image.bytes(mip, layer)`.
The plan is that code made observable. Beyond the Integration, `graphics::prepareUploadBatch` accepts the
twelve formats listed in [the upload batch](upload_batch.md); the one-call convenience for beginners
(#463) is planned on top of these functions.

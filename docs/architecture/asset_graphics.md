# Asset-Graphics Integration: uploading a MeshData (#279, part 1: meshes)

`Terreate::IntegrationAssetGraphics` (`terreate::integration::asset_graphics`) turns an Asset
`MeshData` into Graphics buffers and the upload items of `prepareUploadBatch`. Its header is
`<terreate/integration/asset_graphics/mesh_upload.hpp>`.

This is part 1 of #279: meshes. Uploading an `ImageData` as an `Image` is part 2 and waits for
`ImageData` (#276); it will extend this same Integration.

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
vertices or narrow or widen indices. It handles no `ImageData` (part 2) and no `ModelData`
(#276). A one-call convenience for beginners is planned separately (issue #463, below) and is
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

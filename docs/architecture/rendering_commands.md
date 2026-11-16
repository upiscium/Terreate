# Rendering commands (#265 Phases C1 and C2, #403)

`<terreate/graphics/rendering_commands.hpp>` records Vulkan 1.3 Dynamic
Rendering instances, vertex and index buffer bindings, and draws. It follows the
`pipeline_commands.hpp` model: free functions taking the application's `Device`
and recording `CommandBuffer`, typed `RenderingCommandError`s, and exactly one
native `vkCmd*` call per successful function, none on error.

The application owns when commands are recorded, image layout transitions and
other barriers, submission (#258), completion (#259) and every resource
lifetime. These functions never submit, wait, transition layouts or retain
objects. There is no RenderPass or RenderGraph.

## API

| Function | Native call |
| --- | --- |
| `recordBeginRendering(device, cmd, RenderingDescription)` → `RenderingScope` | `vkCmdBeginRendering` |
| `recordEndRendering(device, cmd, RenderingScope&&)` | `vkCmdEndRendering` |
| `recordSetViewport(device, cmd, vk::Viewport)` | `vkCmdSetViewport` (viewport 0, count 1) |
| `recordSetScissor(device, cmd, vk::Rect2D)` | `vkCmdSetScissor` (scissor 0, count 1) |
| `recordDraw(device, cmd, scope, vertex_count, instance_count, first_vertex, first_instance)` | `vkCmdDraw` |
| `recordBindVertexBuffers(device, cmd, first_binding, span<const VertexBufferBinding>)` | `vkCmdBindVertexBuffers` |
| `recordBindIndexBuffer(device, cmd, buffer, offset, vk::IndexType)` | `vkCmdBindIndexBuffer` |
| `recordDrawIndexed(device, cmd, scope, index_count, instance_count, first_index, vertex_offset, first_instance)` | `vkCmdDrawIndexed` |

The pure CPU seams `validateRenderingDescription`, `validateViewport` and
`validateScissor` expose the attachment-independent checks.

## Rendering description

`RenderingDescription` holds a render area, a layer count, ordered color
attachments (the order is the fragment output location order) and optional
depth and stencil attachments. Each `RenderingAttachmentDescription` borrows a
`const Image *` and a `const ImageView *` only during `recordBeginRendering`.
Both are required because `ImageView` does not expose the image extent, usage
or sample count. It also carries a layout (default `eUndefined`, deliberately
invalid), load/store ops and a clear value.

Every native instance uses `flags = 0`, `viewMask = 0` and no resolve.

## Validation before vkCmdBeginRendering

- Live Device with plan; `dynamicRendering` **enabled** in the Device plan
  (`feature_not_enabled`; never enabled implicitly).
- Live CommandBuffer of that Device, recording (`not_recording`), on a
  graphics-capable queue family (`unsupported_queue`).
- Description: color count ≤ `maxColorAttachments`; nonempty render area with
  nonnegative offset whose end fits `int32_t`, `maxFramebufferWidth` and
  `maxFramebufferHeight` (`invalid_render_area`); layer count in
  `[1, maxFramebufferLayers]` (`invalid_layer_count`).
- If both depth and stencil attachments are given, they must use the same
  image view.
- Each attachment (`invalid_attachment` unless noted):
  - image and view are nonnull, valid, and on the Device (`parent_mismatch`
    otherwise);
  - the view is correlated with the image;
  - the image is single-sample and has `COLOR_ATTACHMENT` or
    `DEPTH_STENCIL_ATTACHMENT` usage;
  - the view aspect is exactly color, or includes depth or stencil;
  - the view has one mip level and is `2D` or `2D_ARRAY`;
  - the layout is in the writable allowlist: `GENERAL`, `ATTACHMENT_OPTIMAL`,
    plus `COLOR_ATTACHMENT_OPTIMAL` for color, `DEPTH(_STENCIL)_ATTACHMENT_OPTIMAL`
    for depth, `STENCIL_ATTACHMENT_OPTIMAL` / `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`
    for stencil;
  - the load op is `LOAD`, `CLEAR` or `DONT_CARE`; the store op is `STORE`,
    `DONT_CARE` or `NONE`;
  - the render area fits the view's mip-level extent (`invalid_render_area`);
  - the layer count is within the view's layers (`invalid_layer_count`).

Format feature support (whether a format may be used as an attachment) is
not queried. It remains the caller's responsibility, as for pipelines.

## RenderingScope

A successful begin returns a move-only `RenderingScope`. It holds no ownership.

- `active()` is true until `recordEndRendering` consumes the scope, or until
  the scope is moved from.
- `nativeCommandBuffer()` observes the command buffer it was begun on.
- `state()` returns the pointer-free effective `RenderingState`: render area,
  layer count and, per attachment, native view (observation only), format,
  mip extent, layout, load/store ops and clear value.

`recordDraw` and `recordEndRendering` require an active scope of the same
command buffer (`invalid_rendering_scope`). This makes "draw inside a
rendering instance" a property of the types, without hidden state inside
`CommandBuffer`. A failed `recordEndRendering` leaves the scope unchanged.

Destroying an active scope records nothing. The command buffer then stays
inside the rendering instance, so end it explicitly. Native `vkCmd*` calls may
still be interleaved on the recording command buffer.

## Viewport, scissor and draw

Viewport and scissor are set at index 0 with count 1, matching the dynamic
state of `GraphicsPipeline`. They may be set inside or outside a rendering
instance.

- Viewport: positive width, nonzero height (negative height is allowed),
  extent within `maxViewportDimensions`, corners within `viewportBoundsRange`,
  depths in `[0, 1]`. NaN fails every check (`invalid_viewport`).
- Scissor: nonnegative offset, and `offset + extent` representable as
  `int32_t` (`invalid_scissor`).
  A zero-width or zero-height scissor is valid Vulkan and is accepted on
  purpose: it discards every fragment.

`recordDraw` needs a graphics queue and an active scope. Whether a compatible
graphics pipeline, viewport, scissor and descriptor or push-constant state are
bound is the caller's responsibility. Nothing is tracked.

## Vertex and index buffers

Each `VertexBufferBinding` borrows a `const Buffer *` and an offset only during
`recordBindVertexBuffers`; `recordBindIndexBuffer` borrows its `Buffer` only
during the call. The caller keeps every buffer alive through all GPU use.
Binding is allowed inside or outside a rendering instance; only
`recordDrawIndexed` needs an active scope.

Validation before the native call, in addition to the shared Device,
recording CommandBuffer and graphics queue checks:

- Vertex buffers: a nonempty span with
  `first_binding + count <= maxVertexInputBindings`, computed without
  overflow (`invalid_binding_range`). Each Buffer is nonnull and valid
  (`invalid_buffer`), on the Device (`parent_mismatch`), has `VERTEX_BUFFER`
  usage and an offset below its size (`invalid_buffer`).
- Index buffer: index type `UINT16` or `UINT32` (`invalid_index_type`;
  `UINT8` needs `indexTypeUint8`, which a Terreate Device cannot enable because
  `DeviceDescription` takes only pNext-free Vulkan 1.0–1.3 feature structures).
  The Buffer is valid, on the Device (`parent_mismatch`), has `INDEX_BUFFER`
  usage and an offset below its size. The offset plus the buffer's offset in
  its `VkDeviceMemory` (the VMA allocation offset) must be a multiple of the
  index size (VUID-vkCmdBindIndexBuffer-offset-08783; `invalid_buffer`).

The stateless functions above track nothing, so with them these remain caller
responsibilities:

- the bound index buffer covers `[first_index, first_index + index_count)`;
- the bound vertex buffers match the bound pipeline's vertex input bindings
  and attributes;
- a compatible graphics pipeline is bound.

The opt-in explicit binding state below (#403) checks the first two before a
draw.

## Explicit binding state (#403)

`GraphicsBindingState` is a plain, pointer-free value owned by the caller.
Nothing is stored inside `CommandBuffer`; the state is passed explicitly to
stateful overloads:

| Function | Effect on the state |
| --- | --- |
| `recordBindPipeline(device, cmd, const GraphicsPipeline&, state)` | snapshots the pipeline's effective vertex bindings and attributes |
| `recordBindVertexBuffers(device, cmd, first, bindings, state)` | records the bytes from each bind offset to the buffer end |
| `recordBindIndexBuffer(device, cmd, buffer, offset, type, state)` | records the index type and the whole indices available |
| `recordDraw(device, cmd, scope, const state&, …)` | checks, then records `vkCmdDraw` |
| `recordDrawIndexed(device, cmd, scope, const state&, …)` | checks, then records `vkCmdDrawIndexed` |

The binders run exactly the validation of their stateless counterparts
(`recordBindPipeline` returns its `PipelineCommandError` unchanged), record
one native call, and update the state only on success. A state filled on
another CommandBuffer starts over when a binder is called with a new one; the
state records which CommandBuffer it belongs to.

Before the native draw (after the usual recording and scope checks):

- `binding_state_mismatch`: the state belongs to another CommandBuffer, or no
  pipeline was bound through it.
- `missing_vertex_buffer`: a vertex binding of the bound pipeline has no
  buffer bound through the state. This is deliberately stricter than Vulkan:
  VUID-vkCmdDraw-None-04007/04008 cover only bindings the vertex shader
  accesses, but Terreate does not reflect SPIR-V, so every declared binding
  (and, for `recordDraw`, its range) is checked. Leave unused bindings out of
  the pipeline's vertex input, or use the stateless overloads.
- `vertex_range_out_of_bounds` (`recordDraw`): for each pipeline binding with
  attributes, `(last index) * stride + max(attribute offset +
  vk::blockSize(format))` must fit the bound bytes, where the last index is
  `first_vertex + vertex_count - 1` for per-vertex bindings and
  `first_instance + instance_count - 1` for per-instance bindings. Zero counts
  fetch nothing. The arithmetic is overflow-checked.
- `missing_index_buffer` / `index_range_out_of_bounds` (`recordDrawIndexed`):
  `[first_index, first_index + index_count)` must lie inside the bound index
  buffer.

Which vertices an indexed draw fetches depends on index values in GPU memory,
so `recordDrawIndexed` checks vertex-buffer presence but not vertex ranges.
`validateDrawBindings` and `validateIndexedDrawBindings` expose the same
checks as pure CPU functions.

Not proven by the state: native `vkCmd*` calls made by the caller, binds made
through the stateless overloads, a CommandBuffer reset, and buffer lifetime
between bind and draw. These stay caller responsibilities.

## Deliberately excluded

- **Resolve attachments:** every public `GraphicsPipeline` is currently
  single-sample (#267 B0), so multisample rendering is not possible through
  public APIs yet. Resolve will be added together with multisample pipeline
  support. The native escape hatch remains.
- Rendering flags (suspend/resume, secondary command buffer contents),
  `viewMask ≠ 0`, fragment shading rate and density map attachments.
- Read-only depth/stencil layouts and multi-level views.

## Tests

- The rendering test also checks every binding-state error on the CPU, then
  draws the indexed quad a third time through the stateful overloads (plus a
  non-indexed draw of its first triangle). Over-long index and vertex ranges,
  a missing vertex buffer and foreign or empty states are rejected with zero
  native calls, and readback matches the stateless quad.
- Package smoke binds through an explicit state, checks the recorded bytes
  and indices, checks that a draw is rejected without a pipeline bound through
  the state, and must print `package.binding_state: runtime=PASS`.

- `graphics.rendering_commands.cpu` covers the CPU seams: color count, render
  area bounds and overflow, layer count, viewport bounds/depth/NaN, scissor
  bounds, and the `RenderingScope` type traits.
- `graphics.rendering_commands` uses real headless validation-enabled Vulkan
  with `dynamicRendering` explicitly enabled:
  - It renders a 64×64 `R8G8B8A8_UNORM` color attachment plus a `D16_UNORM`
    depth attachment after explicit barriers.
  - It binds a B0 GraphicsPipeline, sets viewport and scissor, and draws a
    triangle generated from `gl_VertexIndex`. The vertex and fragment shaders
    are offline-compiled.
  - The test itself submits, waits on a Fence and reads the image back: the
    center pixel is the triangle color and every corner is the clear color.
  - Linker wraps assert the exact `VkRenderingInfo` and draw arguments.
  - Rejections make zero native calls: Device without the feature, foreign
    Device, foreign attachment, missing usage, mismatched view, layouts, area,
    layers, depth/stencil views, inactive and foreign scopes, viewport and
    scissor.
  - A second rendering instance draws an indexed quad. Its 4 vertices and 6
    `UINT16` indices are uploaded through the Upload Batch after a padding
    vertex and two padding indices, so both bind offsets are nonzero. The
    pipeline reads binding 0 (stride 8) as attribute 0 `R32G32_SFLOAT`.
    Readback finds the quad color at the center and inside each triangle,
    and the clear color outside the quad.
  - Wraps assert the exact vertex bind, index bind and indexed draw
    arguments. Rejections make zero native calls: empty, over-limit and
    overflowing binding ranges, missing usage, out-of-range offsets, index
    alignment, `UINT8` and `NONE_KHR` index types, foreign Device buffers,
    and inactive or foreign scopes.
  - Zero validation warnings. Missing prerequisites report BLOCKED.
- Package smoke records begin, viewport, scissor, vertex and index buffer
  binds and end from the installed package without submitting. With no
  pipeline bound it records no draw, and checks that `recordDrawIndexed`
  rejects the consumed scope (`package.rendering_commands: runtime=PASS`).

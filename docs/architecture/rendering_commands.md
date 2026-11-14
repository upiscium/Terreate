# Rendering commands (#265 Phase C1)

`<terreate/graphics/rendering_commands.hpp>` records Vulkan 1.3 Dynamic
Rendering instances and non-indexed draws. It follows the
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

`recordDraw` needs a graphics queue and an active scope. Whether a compatible
graphics pipeline, viewport, scissor and descriptor or push-constant state are
bound is the caller's responsibility. Nothing is tracked.

## Deliberately excluded

- **Resolve attachments:** every public `GraphicsPipeline` is currently
  single-sample (#267 B0), so multisample rendering is not possible through
  public APIs yet. Resolve will be added together with multisample pipeline
  support. The native escape hatch remains.
- Rendering flags (suspend/resume, secondary command buffer contents),
  `viewMask ≠ 0`, fragment shading rate and density map attachments.
- Read-only depth/stencil layouts and multi-level views.
- Vertex and index buffers and `drawIndexed` (Phase C2).

## Tests

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
  - Zero validation warnings. Missing prerequisites report BLOCKED.
- Package smoke records begin, viewport, scissor and end from the installed
  package without submitting (`package.rendering_commands: runtime=PASS`).

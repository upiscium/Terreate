# Pipeline commands (#265 Phase C0)

`<terreate/graphics/pipeline_commands.hpp>` records the compute-path commands
needed to run work with Terreate's explicit pipeline and descriptor objects:
pipeline binding, descriptor set binding, push constants and dispatch. It
follows the `transfer_commands.hpp` model: free functions taking the
application's `Device` and recording `CommandBuffer`, typed errors, and exactly
one native `vkCmd*` call per successful function.

The application owns the command buffer lifetime, when commands are recorded,
barriers, submission (#258), completion (#259) and resource lifetimes. These
functions never submit, wait, insert barriers, or retain any object.

## API

| Function | Native call |
| --- | --- |
| `recordBindPipeline(device, cmd, const ComputePipeline&)` | `vkCmdBindPipeline` (compute) |
| `recordBindPipeline(device, cmd, const GraphicsPipeline&)` | `vkCmdBindPipeline` (graphics) |
| `recordBindDescriptorSets(device, cmd, bind_point, layout, first_set, sets, dynamic_offsets)` | `vkCmdBindDescriptorSets` |
| `recordPushConstants(device, cmd, layout, stages, offset, bytes)` | `vkCmdPushConstants` |
| `recordDispatch(device, cmd, x, y, z)` | `vkCmdDispatch` |

The pure CPU seams `validateDynamicOffsets`, `validatePushConstants` and
`validateDispatch` expose the same checks without a Device.

## Shared preconditions

Every recorder rejects, before any native call:

- a moved-from or plan-less `Device` (`invalid_device`);
- an empty `CommandBuffer` (`invalid_command_buffer`), one allocated from
  another logical Device (`parent_mismatch`), or one not in the recording state
  (`not_recording`);
- a command queue family whose flags do not include `eCompute` (compute bind
  point and dispatch) or `eGraphics` (graphics bind point) — `unsupported_queue`.
  Push constants are accepted on any queue family.

Errors carry the function name as context. If building the diagnostic text
itself runs out of memory, a code-only error is returned instead.

## Descriptor sets

`recordBindDescriptorSets` binds `sets` to set numbers
`[first_set, first_set + sets.size())` of `layout`:

- `bind_point` must be compute or graphics (`invalid_bind_point`).
- `layout` must be live with a configuration and on the same Device
  (`invalid_layout`, `parent_mismatch`).
- The range must be nonempty and inside the layout's set layouts
  (`invalid_set_range`).
- Each set must be nonnull, valid and on the same Device
  (`invalid_descriptor_set`, `parent_mismatch`).
- **Each set's allocation-time `DescriptorSetLayout` handle must equal the
  layout's handle at that set number** (`set_layout_mismatch`). This identity
  rule is stricter than Vulkan, which accepts identically defined but distinct
  layouts. Use the native escape hatch for that case.
- Dynamic offsets are validated against the PipelineLayout's recorded
  `set_layout_descriptions`. Their count must equal the number of dynamic
  uniform/storage descriptors in the bound sets
  (`dynamic_offset_count_mismatch`). They are matched in Vulkan order: set by
  set, then by **ascending binding number** (not description order), then by
  array element. Each offset must be a multiple of
  `minUniformBufferOffsetAlignment` or `minStorageBufferOffsetAlignment` for its
  descriptor type (`dynamic_offset_misaligned`).

Descriptor contents are not tracked. Whether `offset + descriptor range` stays
inside the written Buffer, and whether the written resources are still alive,
remain caller responsibilities. Sets and the layout are borrowed only during
the call. A temporary native handle array is the only allocation;
`allocation_failed` is returned without a native call if it cannot be made.

## Push constants

`recordPushConstants` copies `bytes` (borrowed only during the call) and checks
them against the PipelineLayout's effective push constant ranges and
`maxPushConstantsSize`:

- `stages` must be a nonempty subset of the six core stages.
- `offset` and size must be multiples of 4, size nonzero, and
  `offset + size <= maxPushConstantsSize`.
- Every byte must be covered, for every requested stage, by some range
  (VUID-vkCmdPushConstants-offset-01795).
- For every range overlapping the update, `stages` must include all of that
  range's stages (VUID-vkCmdPushConstants-offset-01796).

Violations return `invalid_push_constants`.

## Dispatch

`recordDispatch` requires a compute-capable queue family and group counts
within `maxComputeWorkGroupCount` (`invalid_dispatch`). Zero counts are valid
and record a no-op dispatch.

## What is not tracked

No bound pipeline, bound sets or pushed values are remembered. Whether a
dispatch has a compatible bound compute pipeline, sets and push constants is
the application's responsibility, as are barriers between the dispatch and
later reads. Native `vkCmd*` calls may be interleaved between these functions
on the same recording command buffer.

## Tests

- `graphics.pipeline_commands.cpu` covers the CPU seams: dynamic offset order
  with unordered bindings, count and per-type alignment; push constant coverage
  and overlap rules; and dispatch limits.
- `graphics.pipeline_commands` uses a real headless validation-enabled Vulkan
  Device and an offline-compiled compute shader. It binds a dynamic storage
  buffer at a nonzero aligned dynamic offset, pushes `base` and `scale`, and
  dispatches. The test (not the library) then submits, waits on a Fence and
  reads back `base + i * scale`. Linker wraps assert the exact native
  arguments, and rejection cases are checked to make zero native calls.
  Missing prerequisites report BLOCKED with a nonzero exit.
- Package smoke records bind, push constants and dispatch from the installed
  package (`package.pipeline_commands: runtime=PASS`) without submitting.

## Later phases

Dynamic Rendering (`beginRendering`/`endRendering`, attachments, viewport and
scissor, `draw`) is Phase C1. Vertex/index buffer binding and `drawIndexed`
are Phase C2.

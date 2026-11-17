# Debug Utils naming and labels (#340)

`<terreate/graphics/debug_utils.hpp>` exposes `VK_EXT_debug_utils` object
naming and command buffer / queue labels as optional, explicit debug
primitives. External tools such as RenderDoc and Nsight, and the validation
layer, read these names and labels; Terreate never adds them on its own.

The application decides whether debugging is on, which objects get names, and
where label regions begin and end. Nothing here submits, waits, or retains an
object.

## Enabling

Debug Utils is an Instance extension. The application requests it through
`InstanceDescription::debug_utils` (`disabled`, `optional` or `required`) and
observes the effective result with `InstancePlan::debugUtilsEnabled()`.

`createDebugUtils(instance, device)` then loads the seven entry points through
`vkGetInstanceProcAddr` and returns a `DebugUtils` value for that Device. It
fails, without loading anything, with:

| Error | When |
| --- | --- |
| `invalid_instance` / `invalid_device` | a moved-from Instance or Device |
| `parent_mismatch` | the Device was not created from that Instance |
| `extension_not_enabled` | the Instance plan did not enable `VK_EXT_debug_utils` |
| `function_unavailable` | an entry point could not be loaded |

There is no silent no-op mode. An application that turns debugging off keeps
no `DebugUtils` (for example an empty `std::optional<DebugUtils>`) and skips
the calls. Every function also rejects a default-constructed, invalid value
with `not_enabled` without a native call, so a disabled path costs one pointer
check and is never reported as success.

`DebugUtils` is a plain, copyable, non-owning value. It holds the native
`VkDevice`, the Device's queue views and the loaded function pointers. Like
`Queue`, it is invalid by contract after its Device or Instance is destroyed.

Correlation compares native handles, as every Graphics object's
`correlatedWith(Device)` does. An object, CommandBuffer or Device belongs to the
DebugUtils Device when its `VkDevice` is the same; a Queue belongs when its
`VkQueue` is one of that Device's queues. Vulkan may reuse a destroyed Device's
handle, so a stale `DebugUtils` could match a later Device. Stale use is
therefore a caller contract, not a checked condition. Holding a private Device
identity instead would not detect it either, since freed memory can be reused.

## Object naming

| Function | Native call |
| --- | --- |
| `setObjectName(debug, const T&, name)` | `vkSetDebugUtilsObjectNameEXT` |
| `setObjectName(debug, vk::ObjectType, handle, name)` | `vkSetDebugUtilsObjectNameEXT` |

The typed overloads cover `Buffer`, `Image`, `ImageView`, `Sampler`,
`ShaderModule`, `PipelineLayout`, `ComputePipeline`, `GraphicsPipeline`,
`DescriptorSetLayout`, `DescriptorPool`, `CommandBuffer` and `Queue`. Each
rejects an invalid object (`invalid_object`) and an object of another logical
Device (`parent_mismatch`) before its single native call.

The generic overload is the escape hatch for any other object of the Device,
including the Device itself. The caller vouches that the handle is a live
object of that Device and of that type; only `vk::ObjectType::eUnknown` and a
null handle are rejected.

The name is copied during the call into a NUL-terminated buffer; short names
stay on the stack. A name containing NUL is `invalid_name`. An empty name
removes the object's name: `pObjectName` is passed as NULL, as Vulkan defines.

## Labels

| Function | Native call |
| --- | --- |
| `recordBeginDebugLabel(debug, cmd, label)` | `vkCmdBeginDebugUtilsLabelEXT` |
| `recordEndDebugLabel(debug, cmd)` | `vkCmdEndDebugUtilsLabelEXT` |
| `recordInsertDebugLabel(debug, cmd, label)` | `vkCmdInsertDebugUtilsLabelEXT` |
| `beginQueueDebugLabel(debug, queue, label)` | `vkQueueBeginDebugUtilsLabelEXT` |
| `endQueueDebugLabel(debug, queue)` | `vkQueueEndDebugUtilsLabelEXT` |
| `insertQueueDebugLabel(debug, queue, label)` | `vkQueueInsertDebugUtilsLabelEXT` |

A `DebugLabel` is a name and an RGBA color; an all-zero color means no color.
The command buffer functions require a CommandBuffer of the DebugUtils Device
(`parent_mismatch`) in the recording state (`not_recording`). The queue
functions require a Queue of that Device (`invalid_queue`,
`parent_mismatch`); like `submit`, the caller externally synchronizes the
queue.

## Not proven

- The command buffer's queue family is not checked. Vulkan accepts labels on
  pools of any compute, graphics, transfer, video or optical-flow family, so
  only a sparse-binding-only family is excluded.
- Begin/end balance is not tracked. Vulkan lets a command buffer label region
  span command buffers, and the validation layer reports an unbalanced end.
- A name or label on an object that is destroyed later is the caller's
  concern; nothing is retained.
- Command buffer and queue labels that the validation layer attaches to its
  messages are not copied into `DiagnosticEvent`. Object names are: the #350
  translator copies each object's name, so a named object appears by name in
  validation events.
- Object tags (`vkSetDebugUtilsObjectTagEXT`), automatic naming at creation
  and scoped (RAII) labels are out of scope.

## Native escape hatch

`DebugUtils::nativeFunctions()` returns the seven loaded function pointers and
`nativeDevice()` the `VkDevice`. Calls made through them bypass every Terreate
check.

## Tests

`graphics.debug_utils.cpu` checks the disabled value and error category without
a GPU. `graphics.debug_utils` wraps `vkGetInstanceProcAddr` so the loaded entry
points become counting thunks that forward to Vulkan. It names one object of
every typed overload plus the Device through the generic overload, records a
labeled compute dispatch inside a labeled queue region around a real submit,
and checks the exact native calls, object types, handles, names and colors. All
rejections make no native call. With the validation layer, the run must be free
of warnings. Without `VK_EXT_debug_utils` the test reports SKIP, never PASS.

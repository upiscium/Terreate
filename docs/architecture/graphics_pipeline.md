# Explicit dynamic-rendering graphics pipeline (#267, B0-a)

`Terreate::Graphics` provides `GraphicsPipeline`, an exclusive, move-only RAII
owner for a Vulkan 1.3 graphics pipeline. Include
`<terreate/graphics/graphics_pipeline.hpp>`. Creation is synchronous native object
creation only: the application owns the loop, scheduling, resource lifetimes,
host synchronization, GPU completion and command recording (#265). There is no
Engine, hidden submission or wait.

## Description and configuration

The public API is in `terreate::graphics`. `GraphicsPipelineDescription` is an
owned, pointer-free value with exactly these members:

| Type | Member |
| --- | --- |
| `std::string` | `vertex_entry_point` |
| `std::string` | `fragment_entry_point` |
| `std::vector<vk::VertexInputBindingDescription>` | `vertex_bindings` |
| `std::vector<vk::VertexInputAttributeDescription>` | `vertex_attributes` |
| `vk::PrimitiveTopology` | `topology` |
| `bool` | `primitive_restart` |
| `vk::CullModeFlags` | `cull_mode` |
| `vk::FrontFace` | `front_face` |
| `bool` | `depth_test_enable` |
| `bool` | `depth_write_enable` |
| `vk::CompareOp` | `depth_compare_op` |
| `std::vector<vk::Format>` | `color_attachment_formats` |
| `std::vector<vk::PipelineColorBlendAttachmentState>` | `color_blend_attachments` |
| `vk::Format` | `depth_attachment_format` |
| `vk::Format` | `stencil_attachment_format` |

Depth and stencil attachment formats default to `vk::Format::eUndefined`, meaning
absent. Both entry points are explicit caller intent: their empty initial values
are invalid and there is **no default `"main"`**. Each must contain 1–4096 bytes,
excluding the terminator, with no embedded NUL.
`max_graphics_entry_point_bytes = 4096` is a library input bound, not a Vulkan
limit. Owned strings provide null-terminated native entry-point names.

`GraphicsPipelineConfiguration` contains:

```cpp
GraphicsPipelineDescription requested;
GraphicsPipelineDescription effective;
vk::ShaderStageFlags stages;
vk::PhysicalDeviceLimits limits;
```

Requested and effective descriptions are independent owned snapshots, not pointers
into caller arrays or strings. B0 does not infer shader state or silently substitute
entry points. `stages` is exactly vertex plus fragment; `limits` snapshots the
actual DevicePlan limits used for validation. Fixed native states below are not
additional description members.

## Factory and CPU validation

```cpp
auto createGraphicsPipeline(
    const Device &device, const PipelineLayout &layout,
    const ShaderModule &vertex_module, const ShaderModule &fragment_module,
    const GraphicsPipelineDescription &description)
    -> terreate::Result<GraphicsPipeline>;

auto validateGraphicsPipelineDescription(
    const GraphicsPipelineDescription &description,
    const vk::PhysicalDeviceLimits &limits) -> terreate::Result<void>;
```

The validator is CPU-only and requires no Device or native call. Synthetic limits
do not establish real hardware support. The factory repeats validation against
the retained DevicePlan limits before native creation. It requires a live Device
with a plan and **already-enabled**
`device.plan()->enabledFeatures13().dynamicRendering`. Merely supporting the
feature is insufficient; the factory never enables it implicitly. The layout
and both modules must be live, have nonnull native handles and belong to that
same Device.

Pre-call validation includes:

- Both bounded, nonempty, NUL-free entry-point names.
- Unique vertex binding numbers below `maxVertexInputBindings`, binding counts
  within that limit, strides within `maxVertexInputBindingStride`, and only
  vertex or instance input rates.
- Unique attribute locations below `maxVertexInputAttributes`, attribute counts
  within that limit, references to existing bindings, and offsets within
  `maxVertexInputAttributeOffset`. Count conversions and arithmetic are checked
  rather than allowed to wrap.
- Color attachment counts within `maxColorAttachments`, no undefined color
  formats, and exactly one blend attachment state per color format.
- Correct depth/stencil aspects for the corresponding formats. If both depth
  and stencil are present, they must use the same combined depth/stencil format.
  Depth test or depth write requires a depth format.
- Structurally valid enum values, flags and combinations within the supported B0
  topology, primitive-restart, rasterization, depth and blend configuration;
  unsupported configurations are rejected rather than enabled or normalized.

**Format feature support remains the caller's responsibility**, including vertex
input, attachment and blending support. CPU validation does not query format
properties or prove shader entry-point existence, execution model, SPIR-V/API
compatibility, shader interfaces, capabilities or layout compatibility. The
application must supply compatible vertex and fragment shaders and an explicit
compatible layout; shader-module creation alone does not establish these facts.

## Fixed B0 native creation

B0 uses fill polygon mode, line width `1`, sample count `e1`, and dynamic viewport
and scissor with count `1` each. Viewport and scissor are the only dynamic states;
the caller records their values before drawing. These fixed choices are not
description members and do not imply enabling additional device features.

After validation and allocation, exactly one `vkCreateGraphicsPipelines` call
creates one pipeline: count `1`, null pipeline cache, null `renderPass`, subpass
`0`, zero flags and no derivatives or base pipeline. Its only extension chain is
the rendering-info `pNext` (`VkPipelineRenderingCreateInfo`) with `viewMask = 0`
and the explicit color, depth and stencil formats. There are exactly two stages,
vertex and fragment, with no specialization data. All other extension chains are
null. No RenderPass object or RenderGraph is introduced.

All strings, arrays, snapshots and native call backing data are owned and stable
through the synchronous call. All potentially failing wrapper/storage allocations
are prepared before native creation: pre-call allocation failure makes zero
native calls. Native failure preserves the exact `VkResult` via
`vk::make_error_code`; a nonnull pipeline returned on failure is destroyed exactly
once. Success with a null pipeline is rejected, not returned as an empty owner.
There is no implicit retry, fallback or device recovery.

## Ownership and observers

`GraphicsPipeline` has no default construction or copying. Move construction and
move assignment are `noexcept`; destruction releases the owned native pipeline.
Its observers match `ComputePipeline`, with the graphics configuration type:

```cpp
bool valid() const noexcept;
explicit operator bool() const noexcept;
vk::Pipeline nativeHandle() const noexcept;
VkPipeline nativePipeline() const noexcept;
vk::Device nativeDevice() const noexcept;
VkDevice nativeVkDevice() const noexcept;
vk::PipelineLayout nativeLayout() const noexcept;
bool correlatedWith(const Device &device) const noexcept;
bool correlatedWith(const PipelineLayout &layout) const noexcept;
const GraphicsPipelineConfiguration *configuration() const noexcept;
```

Native handles are borrowed escape hatches, not transferred ownership. Correlation
checks native identity, not ongoing liveness. Moved-from values are invalid and
expose null native handles and a null configuration pointer. The configuration
pointer is a borrowed read-only view; reacquire it after moves and do not retain
it beyond the owner lifetime. Move assignment destroys the displaced pipeline
exactly once.

**Both Device and PipelineLayout must outlive the pipeline. Destroy the pipeline
before its layout, and then destroy the Device.** This includes any pipeline
displaced by move assignment. Vertex and fragment modules are needed only through
creation and may be destroyed after the factory returns; they are not retained
as owned dependencies. There is no shared ownership or parent-liveness tracking.

The application must satisfy conflicting host synchronization requirements and
complete all GPU use before destruction or replacement. It owns later attachment
compatibility, command recording (#265), submission and completion. The wrapper
does not record commands or wait for GPU idle in creation, moves or destruction.

## Errors and phase boundary

`GraphicsPipelineError` has underlying type `std::uint8_t`, with this ordered
enumerator contract:

```cpp
enum class GraphicsPipelineError : std::uint8_t {
  invalid_device = 1,
  invalid_shader_module,
  invalid_layout,
  parent_mismatch,
  invalid_entry_point,
  invalid_description,
  unsupported_configuration,
  feature_not_enabled,
  native_null_handle,
  allocation_failed,
};
```

`graphics_pipeline_error_category() noexcept` returns
`const std::error_category &`; `make_error_code(GraphicsPipelineError) noexcept`
returns `std::error_code`. A `std::is_error_code_enum` specialization integrates
the enum with standard error codes and `terreate::Result` errors. Invalid parents,
names and descriptions, unsupported limits/configurations and the missing enabled
dynamic-rendering feature are reported before native creation. Native errors
retain their Vulkan category and result rather than being replaced by a generic
wrapper error. Null-success and allocation failure use the corresponding wrapper
codes; allocation-failure reporting must not require another allocation.

B0-a covers the API documentation and module registration here. Tests and installed
package smoke belong to B0-b, not this change. Specialization constants, pipeline
caches, other shader stages and other dynamic states are deferred to B2. Reflection
(#262), compiler integration (#261), RenderPass/RenderGraph abstractions, shared
ownership and waits are outside this API. Existing layout and compute contracts
remain documented in [pipeline.md](pipeline.md).

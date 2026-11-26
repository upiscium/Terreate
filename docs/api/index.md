# API reference

The reference is generated from the public headers by Doxygen. Every component's public namespace has a page
below. For the contract behind each part (what holds, ownership, lifetimes, errors), read the matching
[architecture page](../architecture/index.md); the reference gives the signatures and the doc comments.

```{toctree}
:maxdepth: 1

core
platform
graphics
integration
```

## Components

| Component | CMake target | Namespace | Page |
| --- | --- | --- | --- |
| Core | `Terreate::Core` | `terreate` | [Core](core.md) |
| Platform | `Terreate::Platform` | `terreate::platform` | [Platform](platform.md) |
| Graphics | `Terreate::Graphics` | `terreate::graphics` | [Graphics](graphics.md) |
| Platform-Graphics Integration | `Terreate::IntegrationPlatformGraphics` | `terreate::integration` | [Integration](integration.md) |

## Graphics headers by topic

Include only the headers you use: `#include <terreate/graphics/buffer.hpp>`.

| Topic | Headers | Contract |
| --- | --- | --- |
| Instance, devices | `instance.hpp`, `physical_device.hpp`, `device.hpp`, `format_properties.hpp`, `debug_utils.hpp` | [Physical device](../architecture/physical-device.md), [Logical device](../architecture/logical-device.md), [Debug utils](../architecture/debug_utils.md) |
| Memory and resources | `allocator.hpp`, `buffer.hpp`, `image.hpp`, `image_view.hpp`, `sampler.hpp` | [Allocator](../architecture/allocator.md), [Buffer](../architecture/buffer.md), [Image](../architecture/image.md) |
| Upload and readback | `upload_batch.hpp`, `readback_batch.hpp`, `transfer_commands.hpp`, `vertex_layout.hpp` | [Upload](../architecture/upload_batch.md), [Readback](../architecture/readback_batch.md), [Transfer commands](../architecture/transfer_commands.md) |
| Shaders and descriptors | `shader_module.hpp`, `spirv_binary.hpp`, `specialization.hpp`, `descriptor_set_layout.hpp`, `descriptor_pool.hpp`, `descriptor_set.hpp`, `descriptor_write.hpp` | [Shader module](../architecture/shader_module.md), [Descriptors](../architecture/descriptor_api.md) |
| Pipelines | `pipeline_layout.hpp`, `compute_pipeline.hpp`, `graphics_pipeline.hpp` | [Pipeline](../architecture/pipeline.md), [Graphics pipeline](../architecture/graphics_pipeline.md) |
| Recording | `command_pool.hpp`, `command_buffer.hpp`, `pipeline_commands.hpp`, `rendering_commands.hpp`, `barriers.hpp` | [Command lifecycle](../architecture/command_lifecycle.md), [Pipeline commands](../architecture/pipeline_commands.md), [Rendering commands](../architecture/rendering_commands.md) |
| Submission and lifetime | `submission.hpp`, `synchronization.hpp`, `completion.hpp`, `retirement.hpp` | [Submission](../architecture/gpu_submission.md), [Completion](../architecture/gpu_completion.md), [Retirement](../architecture/gpu_retirement.md) |
| Presentation | `surface.hpp`, `swapchain.hpp`, `swapchain_frame.hpp`, `swapchain_retirement.hpp`, `presentation.hpp` | [Surface](../architecture/surface.md), [Presentation](../architecture/presentation.md) |

## Complete Doxygen output

The class hierarchy, file list and source browser are published beside these pages (not part of the Sphinx
navigation), as the Doxygen site below. Locally they are built by `just project::docs` under
`.build/default/docs/doxygen/html`.

```{raw} html
<p><a href="../doxygen/index.html">Open the complete Doxygen output</a> (on the published site)</p>
```

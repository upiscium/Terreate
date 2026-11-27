# Terreate API reference {#mainpage}

Terreate is a C++23/Vulkan library. The application owns its loop, frame
scheduling, resource lifetimes and synchronization; the library never starts a
loop or hides a wait.

This reference is generated from the public headers only. Components, one
namespace each:

| Component | Namespace | Responsibility |
| --- | --- | --- |
| Core | `terreate` | `Result`/`Error`, diagnostics: no Vulkan, no window system |
| Asset | `terreate::asset` | CPU-side `MeshData`: no Vulkan, no window system |
| Platform | `terreate::platform` | Runtime, Window and events |
| Graphics | `terreate::graphics` | Vulkan Instance to Swapchain, resources, commands, submission |
| Platform-Graphics Integration | `terreate::integration::platform_graphics` | A Window's Surface |
| Asset-Graphics Integration | `terreate::integration::asset_graphics` | A MeshData to Graphics buffers and upload items |

Use the *Namespaces* and *Files* tabs to navigate by component. Contracts and
design notes are in the architecture documentation (see the repository's
`docs/architecture`).

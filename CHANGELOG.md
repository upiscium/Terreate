# Changelog

All notable changes to Terreate are recorded here, following
[Keep a Changelog](https://keepachangelog.com/). Terreate is `0.x`: a minor version may change the
public API (the installed package is compatible only within the same `MAJOR.MINOR`).

## [Unreleased]

Nothing yet. After `v0.1.0`, `MeshData` and the primitive generators (the geometry line, #214) are the
planned next release.

## [0.1.0] - draft (release candidate `v0.1.0-rc.1`, then `v0.1.0`)

First release of the rewritten, modular library. The previous tags (`v0.0.1-alpha` to `v0.0.4`) belong to an
earlier code base and are not described here. This draft is finalized when the release candidate is cut.

Supported and verified: Linux, GCC/g++, Vulkan 1.3, in the pinned Nix development shell. Other platforms
and compilers are not verified.

### Components (CMake `Terreate::` targets)

- **Core**: `Result`/`Error` and the explicit fail-fast `unwrap`; structured diagnostics with
  caller-owned sinks; the Vulkan debug-utils bridge.
- **Platform**: a GLFW `Runtime` and `Window` with explicit event polling and waiting, and the native handle
  as an escape hatch.
- **Graphics**: Instance, physical device selection and Device with requested/supported/effective
  state; the VMA Allocator, Buffer, Image, ImageView and Sampler; shader modules and SPIR-V binaries;
  descriptors, compute and graphics pipelines (dynamic rendering); command recording (transfer,
  pipeline and rendering commands); explicit submission, completion tokens and completion-based
  retirement; Fence and Semaphore objects; upload and readback batches; Surface, presentation support,
  Swapchain, acquire/present, recreation and old-generation retirement.
- **Platform-Graphics Integration**: a Vulkan Surface from a Window.

### Added since the first design of the Graphics foundation

- Image barrier builders and one-barrier recording (`imageBarrier`, `recordImageBarrier`).
- Format properties queries (`queryFormatProperties`, `firstSupportedFormat`).
- Vertex layouts built from the application's own struct (`vertexLayout`, `vertexAttribute`, a typed `uploadItem`).
- Fenced submission helpers (`submitFenced`, `fenceCompletion`) and a safe one-shot upload pattern.
- Descriptor write builders and a pool description helper; buffer memory intents (`bufferAllocationFor`).

### Examples and documentation

- `examples/getting_started` (headless), `examples/triangle` (windowed, application-owned frame loop with
  Swapchain recreation) and `examples/textured_quad` (vertex, index and uniform buffers, a texture, depth).
- Architecture contracts, a development guide, a test matrix, and a generated API reference
  (Doxygen) with Sphinx pages, built locally by `just project::docs`.

### Quality gate

`just project::check` (format, lint, build, tests including the CMake component matrix and the installed
package smoke test and the documentation tests). A `just project::check-fast` loop is partial and is not
the gate.

### Known limits

- No hosted CI: the gate runs locally.
- Window-system coverage: minimized-window recovery, a guaranteed `out_of_date`, and the reaction to a
  window-manager close request are verified by hand, not by a test (see the release verification procedure).
- Native Vulkan calls the examples still make: `vkQueueWaitIdle` where no present fence proves the presents
  are done, and a raw `VkMemoryBarrier2` for vertex and index data.
- The documentation is not published to a site; it is built from the repository.

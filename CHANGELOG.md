# Changelog

All notable changes to Terreate are recorded here, following
[Keep a Changelog](https://keepachangelog.com/). Terreate is `0.x`: a minor version may change the
public API (the installed package is compatible only within the same `MAJOR.MINOR`).

## [Unreleased]

### Added

- **Asset** (`Terreate::Asset`, `terreate::asset`, option `TERREATE_BUILD_ASSET`, `find_package` component `Asset`):
  a new CPU-only component that depends on Core alone, so a consumer of it needs neither Vulkan nor GLFW.
  Its first type is `MeshData` (#345), a backend-independent geometry value: topology, deinterleaved
  float32 attribute streams (position, normal, tangent, texcoord, color, custom), `uint16`/`uint32` indices
  and optional bounds, with `validate`, `computeBounds`, `layoutOf` and `interleave` and the named error
  category `MeshDataError`. Conventions: right-handed, +Y up, counter-clockwise front faces, texture
  origin at the top-left.
- **Primitive generators** (`<terreate/asset/primitives.hpp>`, #346): `triangle`, `quad`, `box`, `plane`,
  `uvSphere` and `cylinder` (a cone is a cylinder with top radius 0) return `Result<MeshData>` from a plain
  description struct (sizes, segment counts, `normals`/`uvs`/`tangents` flags). Output is validated, has
  bounds, is deterministic, uses `uint16` indices up to 65536 vertices and `uint32` beyond, and failures are
  the named `PrimitiveError` category. Winding, normals, UV orientation and tangents follow the documented
  conventions and are tested against the triangle data.
- **ImageData** (`<terreate/asset/image_data.hpp>`, #276 part 1): a backend-independent, CPU-only decoded image,
  in the style of `MeshData`. Its own `PixelFormat` (r8, rg8, rgb8, rgba8, r16, rg16, rgba16, r32f, rg32f,
  rgba32f), a separate `ColorInterpretation` (srgb, linear, normal_map) and `AlphaMode`, an extent with array
  layers and mip levels, one byte vector with a `{mip, layer, offset, row_pitch, slice_pitch}` table, the
  `packedLayout` helper, `bytes(mip, layer)` spans, `custom` key/value entries and an `unsupported` list
  (`{kind, name, detail}`; nothing is dropped silently). `validate` returns the named error category
  `ImageDataError` and uses overflow-safe arithmetic; nothing is repaired. Rows run top-first, matching the
  top-left texture origin. Cube maps are not a separate kind in this part.
- **ModelData** (`<terreate/asset/model_data.hpp>`, #276 part 2): a backend-independent, CPU-only decoded
  model, in the style of `MeshData` and `ImageData`. It owns `meshes` (`MeshData`), `primitives` (mesh plus
  optional material), `model_meshes` (glTF meshes: groups of primitives; a node's optional `mesh` indexes them,
  so the shape is `scene -> node -> ModelMesh -> ModelPrimitive -> MeshData + material` as in glTF, and nodes may
  instance one mesh), `images` (`ImageData`), `samplers` (Asset's own filter, mipmap and wrap enums),
  `textures`, minimal glTF metallic-roughness `materials` (texture references carry a texcoord set), a node
  hierarchy (TRS with an xyzw quaternion, or a column-major 4x4 matrix, as a variant), `scenes` and a default
  scene, all linked by indices, plus `custom` and `unsupported` lists (animation, skins, cameras and lights
  are not represented; a decoder lists them in `unsupported`). `validate` returns the named error category
  `ModelDataError` (index range, empty meshes, node cycles and double parents, transforms, material values, texcoord sets,
  wrapped mesh and image errors); it is iterative, so a long node chain is safe, and nothing is repaired.
  Converting a whole model to GPU resources is not defined by this change.
- **Asset-Graphics Integration** (`Terreate::IntegrationAssetGraphics`, `terreate::integration::asset_graphics`,
  header `<terreate/integration/asset_graphics/mesh_upload.hpp>`, option
  `TERREATE_BUILD_INTEGRATION_ASSET_GRAPHICS`, `find_package` component `IntegrationAssetGraphics`; #279
  part 1, meshes): an optional component that connects Asset and Graphics through their public APIs only,
  so neither depends on the other or on it. A `MeshData` reaches the GPU in three explicit steps that
  record, submit and wait for nothing: `planMeshUpload` (CPU only; the observable `MeshUploadPlan` holds the
  interleaved bytes, the native vertex binding, attribute, topology and index type, the buffer descriptions
  and the attributes left `unmapped`, and `plan.applyTo(pipeline_description)` fills a
  `GraphicsPipelineDescription`), `createMeshBuffers` and `meshUploadItems` (items for
  `prepareUploadBatch`; the plan must outlive it). The application gives each attribute's shader location,
  nothing is guessed, and the named error category `MeshUploadError` passes `MeshDataError` through for an
  invalid mesh. The option is on by default only when Asset and Graphics are; asking for it without one is a
  configure error. Uploading an `ImageData` is part 2 (next entry); the one-call mesh convenience is the entry after it.
- **Asset-Graphics Integration, part 2: images** (`<terreate/integration/asset_graphics/image_upload.hpp>`, #279):
  an `ImageData` reaches the GPU in three explicit steps that record, submit and wait for nothing:
  `planImageUpload` (CPU only; the observable `ImageUploadPlan` holds the Vulkan format, the `ImageDescription`
  with usage `TransferDst | Sampled` plus the application's extra usage, the copy layout
  `TransferDstOptimal` and one `VkBufferImageCopy2` region with an explicit texel row length per (mip, layer)),
  `createImage` (no `ImageView`, no `Sampler`) and `imageUploadItems` (items borrow the pixels from the
  `ImageData`, which must outlive `prepareUploadBatch`). `srgb` maps to the sRGB format where one exists, `linear`
  and `normal_map` to the unorm or float one; `rgb8` has no portable Vulkan format and is `unsupported_format`.
  Every subresource present is uploaded as it is: there is no mip generation and no layout transition (the
  application uses `imageBarrier`); `ImageUploadDescription::mip_generation` and `layout_transition` are reserved
  choices whose only value is `none`, for the explicit opt-in follow-ups #467 and #468. The named error category is
  `ImageUploadError` (an invalid image returns its `ImageDataError` unchanged).
- **Asset-Graphics Integration: beginner mesh upload** (`<terreate/integration/asset_graphics/mesh_upload_convenience.hpp>`, #463;
  meshes only, additive): `defaultMeshAttributeBindings(mesh)` returns the default shader locations (position 0,
  normal 1, texcoord set 0 at 2, color set 0 at 3, tangent 4; only what the mesh has, never custom) as an editable
  value; `planMeshUploadWithDefaults` treats an empty `attributes` list as that mapping and
  `MeshUploadPlan::used_default_attributes` reports it (`planMeshUpload` still rejects an empty list);
  `submitMeshUpload` is non-blocking: given a recording command buffer, a queue and an unsignaled fence it plans,
  creates the buffers, records, ends the command buffer and calls `submitFenced`, and returns the plan, the buffers,
  the staging owner and the `SubmissionReceipt` for a `RetirementQueue` (it never waits); `uploadMeshBlocking`
  creates its own command pool, command buffer and fence, waits on the fence for the given timeout and returns the
  buffers and the plan. `MeshUploadError::wait_timeout` is appended (on a timeout the GPU may still use the
  resources, which are then deliberately not destroyed). The image convenience waits for the layout-transition
  opt-in (#468). The lower-level equivalent is documented step by step.
- **Graphics upload batch: more Image formats** (#279): the Image upload of `prepareUploadBatch` and
  `recordUploadBatch`, until now `R8G8B8A8_UNORM` only, accepts the uncompressed single-plane color formats R8, R8G8
  and R8G8B8A8 (UNORM and SRGB), R16, R16G16 and R16G16B16A16 (UNORM) and R32, R32G32 and R32G32B32A32 (SFLOAT). The
  texel size comes from the Vulkan-Hpp format traits and the footprint, the row-length limit and the staging offset
  (a multiple of the texel size and of 4) follow it; 2D, one sample, one layer per operation and every existing error
  are unchanged, and compressed, multi-plane, 3D, depth/stencil, packed and integer formats stay `unsupported_image`.

## [0.1.0] - 2026-11-26

First release of the rewritten, modular library. The previous tags (`v0.0.1-alpha` to `v0.0.4`) belong to an
earlier code base and are not described here. This section was final at `v0.1.0-rc.1`; `v0.1.0-rc.2` added only documentation.

License: BSD 3-Clause (`LICENSE`, installed with the package).

Supported: Linux, GCC/g++ (the build refuses other compilers; some tests need GNU ld), Vulkan 1.3, built in the
pinned Nix development shell. Static libraries only. Dependencies: GLFW 3.4 or newer, the Vulkan headers and
loader (1.3), and VulkanMemoryAllocator. Other platforms and compilers are not verified, and the real-hardware
verification (see `docs/development/release-verification.md`) was done during the candidate period (Linux; NVIDIA, Intel and llvmpipe).

### Components (CMake `Terreate::` targets)

- **Core**: `Result`/`Error` and the explicit fail-fast `unwrap`; structured diagnostics with
  caller-owned sinks.
- **Platform**: a GLFW `Runtime` and `Window` with explicit event polling and waiting, and the native handle
  as an escape hatch.
- **Graphics**: Instance, physical device selection and Device with requested/supported/effective
  state; the VMA Allocator, Buffer, Image, ImageView and Sampler; shader modules and SPIR-V binaries;
  descriptors, compute and graphics pipelines (dynamic rendering); command recording (transfer,
  pipeline and rendering commands); explicit submission, completion tokens and completion-based
  retirement; Fence and Semaphore objects; upload and readback batches; Surface, presentation support,
  Swapchain, acquire/present, recreation and old-generation retirement. The Vulkan debug-utils bridge (validation messages reach the diagnostic sink).
- **Platform-Graphics Integration**: a Vulkan Surface from a Window.

### Graphics helpers added late in the cycle

- Image barrier builders and one-barrier recording (`imageBarrier`, `recordImageBarrier`).
- Format properties queries (`queryFormatProperties`, `firstSupportedFormat`).
- Vertex layouts built from the application's own struct (`vertexLayout`, `vertexAttribute`, a typed `uploadItem`).
- Fenced submission helpers (`submitFenced`, `fenceCompletion`) and a safe one-shot upload pattern.
- Descriptor write builders and a pool description helper; buffer memory intents (`bufferAllocationFor`).

### Examples and documentation

- `examples/getting_started` (headless), `examples/triangle` (windowed, application-owned frame loop with
  Swapchain recreation) and `examples/textured_quad` (vertex, index and uniform buffers, a texture, depth).
- A "Getting started" guide (install, first program, where to read next), architecture contracts, a development
  guide, a test matrix, and a generated API reference
  (Doxygen) with Sphinx pages (Furo theme, light and dark), built locally by `just project::docs` and published to
  GitHub Pages from `main`.

### Quality gate

`just project::check` (format, lint, build, tests including the CMake component matrix and the installed
package smoke test and the documentation tests). A `just project::check-fast` loop is partial and is not
the gate.

### Known limits

- No hosted CI: the gate runs locally.
- Window-system coverage: a guaranteed `out_of_date`, the behaviour of a hidden window and the reaction to a
  window-manager close request are checked by hand, not by a test (see the release verification procedure).
  Recovery from a minimized window (a zero-size framebuffer) is not verified on a compositor that ignores
  minimize requests, such as Hyprland.
- `platform.native_events` is skipped on Wayland (an unmapped Wayland window gets no resize event).
- Native Vulkan calls the examples still make: `vkQueueWaitIdle` where no present fence proves the presents
  are done, and a raw `VkMemoryBarrier2` for vertex and index data.
- The documentation is built from the repository (`just project::docs`) and published to GitHub Pages by `.github/workflows/pages.yml`; the gate does not run in CI.

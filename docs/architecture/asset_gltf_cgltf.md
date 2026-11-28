# cgltf Adapter: decoding glTF and GLB to ModelData (#278)

`Terreate::AdapterAssetGltfCgltf` (`terreate::adapter::asset_gltf_cgltf`) decodes a glTF 2.0
document, as `.gltf` JSON or `.glb` binary, into an Asset `ModelData`. Its header is
`<terreate/adapter/asset_gltf_cgltf/decoder.hpp>`. It implements the decode contracts of Asset
(`asset::DecodeInput`, `asset::DecodeLimits`, `asset::ResolveSession`, `asset::DecodeDiagnostic`,
`asset::DecodeProvenance`; see [Asset](asset.md)) with one backend,
[cgltf](https://github.com/jkuhlmann/cgltf), which is a private implementation detail.

## Responsibility and non-goals

One function does the job: `decodeGltf(input, resolver, image_decoder, options)` returns a
`GltfDecodeResult`: a valid `ModelData`, the diagnostics, the provenance (the source and every external
resource that was requested) and what the file header said.

Non-goals:

- **No file system, no network, no path.** The input is a borrowed span of bytes plus a source identity
  (`asset::DecodeInput`). An external buffer or image is asked for through the application's
  `asset::ResourceResolverView`; the Adapter never opens anything itself and cgltf's own file and URI
  loading is never called. A path-based convenience is composed outside, see
  [Composing a file-reading layer](#composing-a-file-reading-layer).
- **No image codec.** glTF images are decoded to `ImageData` by an application-provided callable
  (`ImageDecoderView`); the Adapter has no dependency on the stb_image Adapter or any other codec.
- **No GPU resource.** The result is CPU data; uploading it is the job of the
  [Asset-Graphics Integration](asset_graphics.md). The Adapter is built and tested without Graphics.
- No animation, skinning, morph targets, cameras, lights, extensions, tangent or normal generation, unit or
  handedness conversion, texture transforms, material variants, Draco or meshopt decompression. All of
  these are *reported*, never silently dropped (see [What is supported](#what-is-supported)).
- No hidden loader, cache, thread or global state. The call decodes on the calling thread and returns.

## Public dependencies

Exactly `Terreate::Asset` (and Core through it). cgltf is **not** a dependency of anything a consumer
sees: it is compiled into the Adapter's static library, no public header includes it, no usage
requirement names it, and the installed package needs no `find_dependency` for it. The stb_image Adapter
is not a dependency either, and the two Adapters do not link each other. The component matrix and the
package smoke test check that the target links exactly Asset, that no cgltf header or reference is
installed, that a consumer built with the Vulkan and GLFW searches disabled links, and that no other
component links the Adapter. Asset never depends on the Adapter.

## What is supported

| glTF | Result in `ModelData` |
| --- | --- |
| scenes, `scene` | `ModelScene` per scene (name, roots), `default_scene` |
| nodes: `children`, `mesh`, `name`, TRS or `matrix` | one `ModelNode` per node, in file order; `TrsTransform` or `MatrixTransform` |
| meshes, primitives | one `ModelMesh` per mesh and one `ModelPrimitive` plus one `MeshData` per primitive |
| attributes `POSITION`, `NORMAL`, `TANGENT`, `TEXCOORD_n`, `COLOR_n` | float32 streams of `MeshData` (`float32x3`, `x3`, `x4`, `x2`, `x3` or `x4`) |
| `indices` (unsigned byte, short, int) | `uint16` (byte and short) or `uint32`; none stays non-indexed |
| modes points, lines, triangles, triangle strip | `point_list`, `line_list`, `triangle_list`, `triangle_strip` |
| sparse accessors, normalized 8/16-bit integer attributes, accessors without a buffer view | read by the Adapter (see below) |
| materials: `pbrMetallicRoughness` factors and textures, normal, occlusion and emissive textures and factors, `alphaMode`, `alphaCutoff`, `doubleSided` | `MaterialData` with `TextureRef` (texture index and `texCoord`) |
| textures, samplers, images | `TextureData`, `SamplerData`, `ImageData` (through the image decoder), all in file order |
| asset header | `GltfSourceInfo` and `ModelData::custom` (`gltf.container`, `gltf.version`, `gltf.min_version`, `gltf.generator`, `gltf.copyright`) |

Everything else the file contains goes to `ModelData::unsupported` **and** to a `warning` diagnostic with
the code `gltf.unsupported.<kind>` (never silently dropped):

| Kind | What it names | What happens to the rest |
| --- | --- | --- |
| `animation`, `skin`, `camera`, `light` | each animation, skin, camera and `KHR_lights_punctual` light (name, or the index in the detail) | the nodes and meshes are kept |
| `morph-target` | each primitive with targets, each mesh or node with morph weights | the primitive's base geometry is kept |
| `attribute` | `JOINTS_n`, `WEIGHTS_n` and custom (`_NAME`) attributes of a primitive | the other streams are kept. (`MeshData` could hold them as `custom` float streams, but integer joints would be converted without a defined rule, so they are reported instead.) |
| `primitive-mode` | `LINE_LOOP`, `LINE_STRIP`, `TRIANGLE_FAN` | **the primitive is skipped**, as `MeshData` has no such topology |
| `primitive` (`no-position`) | a primitive without `POSITION` | the primitive is skipped |
| `mesh` | a mesh none of whose primitives could be kept | no `ModelMesh` is made; the nodes that placed it have no mesh |
| `compression` | `KHR_draco_mesh_compression` (the primitive is skipped), `EXT_meshopt_compression` (the fallback data is read) | |
| `texture-transform` | each `KHR_texture_transform` | the texture reference is kept, the transform is ignored |
| `material-extension` | each material extension that cgltf recognizes (`KHR_materials_clearcoat`, `..._pbrSpecularGlossiness`, `..._unlit`, ...) | the core fields are kept |
| `mesh-gpu-instancing`, `material-variants` | `EXT_mesh_gpu_instancing`, `KHR_materials_variants` | the mesh is placed once, the variants are not kept |
| `extension` | every name in `extensionsUsed` and every extension name found on an object | |
| `extras` | the objects that carry `extras`, counted per kind of object | the extras themselves are not kept |

A file that lists an extension in `extensionsRequired` is **refused** with
`GltfDecodeError::unsupported_required_extension` (the Adapter implements no extension, so decoding would
produce something other than the author meant). A document whose `asset.version` is not 2.x, or whose
`minVersion` is above 2.0, is `unsupported_version`.

At most 64 items of one diagnostic code and 64 unsupported entries of one kind are listed individually;
one more item with the same code or kind (`name` `(further)`) counts the rest, so the totals are exact
and a hostile file cannot make the report unbounded.

## Mapping rules

1. **Scene, node, mesh and primitive map 1:1** onto `ModelData::scenes`, `nodes`, `model_meshes` and
   `primitives`. The indices of nodes, scenes, materials, textures, samplers and images are the glTF
   indices. No node is invented per primitive; two nodes that name one mesh share one `ModelMesh`
   (instancing). Only `model_meshes` and `primitives` can have fewer entries than the file (skipped
   primitives and meshes, see above), so their indices are the order of the kept ones.
2. **Each glTF primitive becomes one `MeshData`** (plus a `ModelPrimitive` that names it and the material
   index). Vertex data is copied per primitive: accessors that several primitives share are read more than
   once. `MeshData::bounds` is computed from the positions (known, not copied from the file's `min`/`max`,
   which are untrusted); when a position is not finite the bounds stay absent and a warning says so.
3. **Attributes.** `POSITION` and `NORMAL` are `VEC3`, `TANGENT` is `VEC4`, `TEXCOORD_n` is `VEC2`,
   `COLOR_n` is `VEC3` or `VEC4`. Components are `FLOAT`, or a *normalized* 8 or 16-bit integer (signed:
   `max(v / max, -1)`, unsigned: `v / max`), which is converted to float32 with the info diagnostic
   `gltf.attribute.normalized_converted`. A non-normalized integer attribute (the
   `KHR_mesh_quantization` layout) is refused as `invalid_accessor`. Missing normals and tangents are
   **not** generated. The texture coordinate space is not converted: v grows downward in glTF and in
   `MeshData`.
4. **Indices.** Unsigned byte indices are widened to `uint16` (info `gltf.index.widened`); every index must
   be below the vertex count (checked by the Adapter, not left to the later validation). Whether the index
   or vertex count suits the topology is checked by `asset::validate` (an invalid count is reported as the
   `MeshDataError`).
5. **Node transforms.** `matrix` becomes a `MatrixTransform` (column-major, as in glTF), translation,
   rotation and scale a `TrsTransform`; a node with neither is the identity TRS. A node with both a matrix and
   TRS values is `malformed_gltf`.
6. **Materials.** The metallic-roughness factors and textures, `normalTexture.scale`,
   `occlusionTexture.strength`, the emissive factor and texture, `alphaMode`, `alphaCutoff` and
   `doubleSided` are copied; the glTF defaults apply to what the file leaves out. A material that
   uses `KHR_materials_pbrSpecularGlossiness` keeps the metallic-roughness defaults (and says so as
   unsupported). A material that samples a `texCoord` set that a primitive using it does not have makes the
   model invalid (`asset::ModelDataError::missing_texcoord_set`): the Adapter does not repair it.
7. **Samplers.** `magFilter` 9728 and 9729 map to nearest and linear; `minFilter` 9728 and 9729 to nearest and
   linear without mip levels, and the four mipmap values to the filter and `SamplerMipmapMode`
   (`nearest_mipmap_linear` is nearest filter, linear mip mode). An undefined filter is the `SamplerData`
   default (linear; linear mip mode for minification). `wrapS` and `wrapT` 10497, 33648 and 33071 map to
   repeat, mirrored repeat and clamp to edge. Any other value is `invalid_resource`.
8. **Images and their color interpretation.** Every glTF image is decoded, in file order, through the
   image decoder, with an `ImageDecodeRequest` that carries the bytes, the identity, the declared media
   type, the limits and the **color interpretation chosen from how materials use the image**: `srgb` for
   a base color or emissive texture, `normal_map` for a normal texture, `linear` for a metallic-roughness
   or occlusion texture. An image that no supported material slot uses is decoded as `srgb` (info
   `gltf.image.interpretation_defaulted`). An image used in two ways is decoded **once**, with the
   interpretation of its first use (materials in file order; base color, metallic-roughness, normal,
   occlusion, emissive), and the warning `gltf.image.interpretation_conflict` says so. If the callable
   returns another interpretation than it was asked for, the result is kept as it is and the warning
   `gltf.image.interpretation_mismatch` says so. A texture without an image source is `invalid_resource`.
9. **External resources.** A buffer or image URI that is not a `data:` URI is requested through the resolver
   with `ResourceKind::buffer` or `image` (the declared `byteLength` is the `declared_size`): the
   `asset::ResolveSession` normalizes it against the source's base directory (`..` may not escape the
   root, no scheme, no absolute path unless the application allows it in `GltfDecodeOptions::references`),
   enforces the count and size limits and records the request in the provenance. A buffer is loaded the
   first time a kept primitive or image needs it (an animation's or a skin's buffer is never loaded), and
   only once.
10. **`data:` URIs.** Only `data:[<media type>];base64,<payload>` is understood, with padded canonical
    base64. It is decoded by the Adapter, never passed to the resolver (so `allowed_schemes` need not list
    `data`), after its decoded size is checked against `max_resource_bytes` and the total against
    `max_total_resource_bytes`. It is not an external request: it is noted as info `gltf.data_uri`, and
    shows in `ImageDecodeRequest::input.source` as `<source name>#images[i]`.
    `GltfDecodeOptions::embedded_data_uris = false` makes a data URI an ordinary reference.
11. **GLB.** The first chunk is the JSON, the optional second is the BIN chunk (it is the data of `buffers[0]`
    when that buffer has no `uri`); any other chunk layout is `malformed_gltf`. Bytes after the length that
    the header declares are ignored and noted (info `gltf.glb.trailing_bytes`).

## Limits and security notes

The input may be hostile. The order is fixed and every step rejects before the next allocates:

1. the input size (`asset::checkInputSize`, `DecodeError::input_too_large`);
2. the container: a GLB header and chunks against the input length, so no chunk reads past the input;
3. the JSON nesting against 128 levels (`malformed_gltf`), before the parser sees the text, because
   cgltf skips unknown values recursively and a text nested a million levels would overflow the stack;
4. the parse: cgltf allocates through a budget of 16 MiB plus 64 bytes per byte of JSON, so a short text
   that describes millions of objects fails as `out_of_memory` instead of exhausting memory;
5. the version and `extensionsRequired`;
6. the counts of nodes, meshes and primitives (`DecodeError::count_exceeded`) and the hierarchy (depth
   against `max_hierarchy_depth`, `DecodeError::hierarchy_too_deep`; a cycle is `malformed_gltf`),
   both iterative, so a chain of any depth is safe;
7. the data: every accessor, buffer view and sparse accessor is checked for fit (with overflow-safe
   arithmetic: offset, stride and count come from the file and may be near 2^64) before it is read,
   every sparse index and every vertex index is checked, and a buffer must hold at least its declared
   `byteLength`. The Adapter reads the accessors itself (with `memcpy`, so alignment is not assumed)
   instead of using cgltf's readers, which dereference unaligned pointers, trust sparse indices and are
   not used for that reason; `cgltf_validate` is not used either, as it does not check every bound;
8. `GltfDecodeOptions::max_decoded_bytes` (default 1 GiB) limits the decoded vertex streams, indices and
   images together, checked *before* each allocation: an accessor without a buffer view reads as zeros and can
   describe far more elements than the file holds, so the file size alone does not bound the memory;
9. the extent of every image the callable returns is checked against `max_dimension` and `max_pixels`
   again, and `asset::validate(ImageData)` runs on it; resolver limits (count, per resource, total,
   depth) are enforced by the session.

Known gaps: cgltf is not a hardened decoder. Its parse of a hostile JSON text allocates up to the budget
above, which scales with the input size limit (the default 256 MiB limit allows a large budget; lower
`max_input_bytes` for untrusted input). A glTF index of -1 is read by cgltf as an absent reference. A
JSON string with an embedded NUL is cut at it. The Adapter's test decodes every truncation and many byte
flips of its fixtures under tight limits, and the same test and a mutation run of several hundred thousand
inputs were run under address and undefined-behavior sanitizers during development; neither is part of the
gate, and neither is a fuzzing campaign.

## Ownership, borrowing and lifetime

- `decodeGltf` borrows `input.bytes`, the resolver and the image decoder for the duration of the call only.
  The result owns everything: the model, the diagnostics, the provenance (with copies of the identities) and
  the header information.
- A resolved resource is kept alive by the Adapter while a primitive or an image uses it and released when the
  call returns. The image decoder's `ImageDecodeRequest` and its bytes are valid during the callable only.
- cgltf's parse result is freed before the call returns, on every path.

## Execution authority and threading

The Adapter owns no thread, queue, cache or wait. `decodeGltf` is a pure function of its arguments (and of
what the resolver and the image decoder do): it uses no global state, so any number of calls may run
concurrently on different inputs. The resolver and the image decoder are called synchronously, one at a
time, on the calling thread; an application that gets bytes asynchronously resolves ahead of time and
passes a `MemoryResolver`.

## Requested, supported and effective configuration

`GltfDecodeOptions` is the request; the result is the effective state, observable in the model, the
diagnostics, the provenance and `GltfSourceInfo`:

| Option | Default | Effect |
| --- | --- | --- |
| `limits` | `asset::defaultDecodeLimits()` | input size, node, mesh and primitive counts, hierarchy depth, external resource count and sizes, image dimensions and pixels |
| `references` | no scheme allowed | how URIs are validated before the resolver sees them; adding `https` is the application's explicit act |
| `max_decoded_bytes` | 1 GiB | decoded vertex streams, indices and images, checked before each allocation |
| `embedded_data_uris` | `true` | `data:` URIs of buffers and images are decoded by the Adapter |

The glTF features the Adapter implements are the ones in the table above; the rest is in `unsupported`.

## Errors and diagnostics

Failures are `terreate::Result` errors. The Adapter's category `GltfDecodeError` (name
`terreate.adapter.asset_gltf_cgltf`) has `malformed_gltf`, `unsupported_version`,
`unsupported_required_extension`, `missing_resolver`, `missing_image_decoder`, `image_decoder_failed`,
`invalid_buffer`, `invalid_accessor`, `invalid_resource`, `decoded_size_exceeded` and `out_of_memory`; the
error detail names the offending object (for example `accessors[3]`) and the reason. Everything that
already has a name is returned **unchanged**: `asset::DecodeError` (limits), `asset::ResolveError` (the
resolver: not found, rejected reference, limits, failure, and the data URI limits), the image decoder's own
errors, `asset::ImageDataError` (an image the callable returned that is not valid) and the
`asset::MeshDataError` or `asset::ModelDataError` of the final `asset::validate`. `missing_resolver` is
reported before the resolver is looked at (an unbound view), `missing_image_decoder` only when an image has to
be decoded.

Diagnostics (`asset::DecodeDiagnostic`) are stable codes with the source name and a path inside the file.
Decisions are `info` (`gltf.container`, `gltf.data_uri`, `gltf.attribute.normalized_converted`,
`gltf.index.widened`, `gltf.image.interpretation_defaulted`, `gltf.glb.trailing_bytes`); `warning` marks a
feature that was not applied (`gltf.unsupported.<kind>`), an interpretation conflict or mismatch and bounds
that could not be computed.

## Optional and required dependencies

- Required: Asset (and Core). Nothing else at build or consume time.
- Build input: `cgltf.h`. cgltf is **not** in nixpkgs, so the project pins it as a *flake input*
  (`github:jkuhlmann/cgltf`, a non-flake source tree locked in `flake.lock`), and the devShell exports the
  directory that contains `cgltf.h` as `CGLTF_INCLUDE`. Nothing is vendored in the repository and CMake
  never downloads anything. The option `TERREATE_BUILD_ADAPTER_ASSET_GLTF_CGLTF` is **on by default only
  when Asset is built and `cgltf.h` is found**; asking for it without Asset, or without the header, is a
  configure error that names what is missing, never a silent correction. The header is looked up in this
  order, and the first source that is set decides: the cache variable `TERREATE_CGLTF_INCLUDE_DIR`, the
  environment variable `CGLTF_INCLUDE`, then a standard search for `cgltf.h` (also below a `cgltf`
  directory of a prefix). A set but wrong directory turns the default OFF; an explicit `ON` then fails with
  an error that names the variable. CMake holds no Nix-specific logic, so a build without Nix only has to
  provide a `cgltf.h` through one of these (building without Nix is tracked in #473).
- A consumer writes `find_package(Terreate REQUIRED COMPONENTS AdapterAssetGltfCgltf)` and links
  `Terreate::AdapterAssetGltfCgltf`; that imports Asset and Core and searches for nothing.
- The header is third-party code from the build environment, so it is a system include of the Adapter's one
  implementation file: project warnings and clang-tidy do not apply to it (the Adapter's own `.cpp` files are
  linted and formatted like all other code). The pinned revision is the one in `flake.lock`; raising it is a
  deliberate `nix flake update cgltf` and a review of the decode tests.

## Native or backend escape hatch

`GltfDecodeResult::native` (`GltfSourceInfo`) and `ModelData::custom` keep what the file header said: the
container, `asset.version`, `minVersion`, `generator`, `copyright` and the list of `extensionsUsed`. cgltf
itself is not exposed: there is no cgltf type or handle in any header, and the raw JSON is not kept.

## Minimal explicit example

```cpp
#include <terreate/adapter/asset_gltf_cgltf/decoder.hpp>

namespace gltf = terreate::adapter::asset_gltf_cgltf;
namespace asset = terreate::asset;

// `bytes` was read by the application; `files` holds the sidecar files it loaded ahead of time.
asset::MemoryResolver files;                                   // keys are normalized references
files.add("models/ship.bin", ship_bin_bytes);
auto resolver = asset::ResourceResolverView::bind(files);

auto image_decoder = [](const gltf::ImageDecodeRequest &request) -> terreate::Result<asset::ImageData> {
  return decodeSomehow(request);                               // see the next section
};
auto images = gltf::ImageDecoderView::bind(image_decoder);

asset::DecodeInput input{bytes, asset::makeSourceIdentity("models/ship.gltf")};
auto decoded = gltf::decodeGltf(input, resolver, images);     // default limits
if (!decoded) { /* decoded.error(): GltfDecodeError, asset::DecodeError, asset::ResolveError, ... */ }
const asset::ModelData &model = decoded->model;                // validated
for (const auto &feature : model.unsupported) { /* what the file had and the model does not */ }
for (const auto &resource : decoded->provenance.resources) { /* every external request */ }
```

## Composing with the stb_image Adapter

The glTF Adapter does not depend on the image Adapter: the application passes the callable. The stb_image
Adapter's `decodeImage` is wrapped like this (the same wrapping is built and run by the test that links both
Adapters):

```cpp
#include <terreate/adapter/asset_image_stb/decoder.hpp>

namespace stb = terreate::adapter::asset_image_stb;

struct StbImageDecoder {
  auto operator()(const gltf::ImageDecodeRequest &request) const
      -> terreate::Result<asset::ImageData> {
    stb::ImageDecodeOptions options;
    options.limits = request.limits;                       // the limits of the glTF decode
    options.interpretation = request.interpretation;       // srgb, linear or normal map, from the materials
    options.channels = stb::ChannelRequest::four;          // the Integration uploads four-component images
    auto decoded = stb::decodeImage(request.input, options);
    if (!decoded)
      return std::unexpected(std::move(decoded.error()));  // returned unchanged by decodeGltf
    return std::move(decoded->image);
  }
};

StbImageDecoder stb_decoder;
auto images = gltf::ImageDecoderView::bind(stb_decoder);
auto decoded = gltf::decodeGltf(input, resolver, images);
```

Any other codec (a KTX or a basis decoder, a GPU-format container reader) is wrapped the same way. The
callable must honor `request.interpretation` and `request.limits`; the Adapter checks the result.

## Composing a file-reading layer

The Adapter reads nothing, so a path-based convenience is a thin layer of the application (or of a separate
I/O component) that reads the `.gltf` or `.glb`, gives its identity (`makeSourceIdentity("models/ship.gltf")`
makes the base `models/`), and binds a resolver that reads the sidecar files below a root it chooses:

```cpp
auto loadModel(const std::filesystem::path &path, const gltf::GltfDecodeOptions &options = {})
    -> terreate::Result<gltf::GltfDecodeResult> {
  std::vector<std::byte> bytes = readWholeFile(path);               // the application's reading layer
  FileResolver resolver{path.parent_path().parent_path()};          // maps normalized references to files
  auto resolve = asset::ResourceResolverView::bind(resolver);
  return gltf::decodeGltf({bytes, asset::makeSourceIdentity(path.generic_string())}, resolve,
                          images, options);
}
```

The reading layer owns every file-system decision (where, how much, symbolic links, sandboxing, errors); the
`DecodeLimits` bound the decode and `ReferenceOptions` bound what the file may ask for.

## Convenience API and its lower-level equivalent

There is no convenience layer; `decodeGltf` is the lowest level and the only one. A caller that wants fewer
arguments passes a default `GltfDecodeOptions` and, for a self-contained GLB, unbound views.

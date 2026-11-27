# Asset: MeshData (#345), primitive generators (#346), ImageData and ModelData (#276), decode contracts (#280)

`Terreate::Asset` (`terreate::asset`) holds CPU-side asset data that primitive
generators, model decoders and rendering code share. Its types are `MeshData`, declared in
`<terreate/asset/mesh_data.hpp>`, and `ImageData` ([below](#imagedata-276-part-1)), declared in
`<terreate/asset/image_data.hpp>`, and `ModelData` ([below](#modeldata-276-part-2)), declared in
`<terreate/asset/model_data.hpp>`.

## Responsibility and non-goals

Asset defines what geometry looks like in memory before any GPU exists, and
checks that it is consistent.

- `MeshData` is one glTF "primitive" or one generated shape: a topology, a set
  of attribute streams (position, normal, tangent, texture coordinates, vertex
  colors, named custom streams), an optional index buffer and optional bounds.
- `validate`, `computeBounds`, `layoutOf` and `interleave` are the operations
  on it.
- Non-goals: no GPU handle, no `Buffer`, no Vulkan type, no file or image
  decoding, no materials, nodes or submesh ranges (those belong to the
  `ModelData` of #276), no GPU upload or
  `VertexLayout` conversion (the [Asset-Graphics Integration](asset_graphics.md), #279), no
  repair of malformed data.

## Public dependencies

Asset's only public dependency is `Terreate::Core` (`Result` and `Error`). It
never links Graphics, Platform or an Integration, and it needs neither Vulkan
nor GLFW: a consumer that requests only `Asset` is never asked for them. The
component matrix and the package smoke test check this.

## Ownership, borrowing and lifetime

`MeshData` is an owning, copyable value. It is plain data (public members), so
a decoder or generator fills it directly, and copying CPU data is explicit and
legitimate. Nothing borrows from it except the pointers returned by `find` and
`positions`, which are valid until the mesh is changed or destroyed.
Attribute bytes are stored in host byte order as IEEE 754 binary32 floats; the
typed accessors read through a byte copy and assume no alignment.

## Execution authority and threading

Asset owns no thread, loop or wait. Every function is a pure function of its
arguments, so independent meshes can be used from different threads; one mesh
must not be modified while another thread reads it.

## Requested, supported and effective configuration

Not applicable: Asset has no device or backend to negotiate with. The only
"effective" description is `layoutOf` / `interleave`, which state exactly how
the bytes are laid out.

## Errors and diagnostics

Fallible functions return `terreate::Result` with an `Error` whose code is a
`MeshDataError` in the category `terreate.asset.mesh_data`. The Error's context
is the operation and its detail names the offending stream or index.

| `MeshDataError` | Meaning |
| --- | --- |
| `missing_position` | no position stream (`computeBounds`: none or empty) |
| `count_mismatch` | a stream's count differs from the position count |
| `format_not_allowed` | the format does not suit the semantic (position x3, normal x3, tangent x4, texcoord x2, color x3 or x4, custom any) |
| `index_out_of_range` | an index is not below the vertex count |
| `invalid_index_count` | the element count (indices, or vertices when not indexed) does not suit the topology |
| `duplicate_attribute` | two streams (or two `interleave` order entries) have the same semantic, set and name |
| `bounds_not_enclosing` | present bounds do not enclose every position (a NaN position is not enclosed) |
| `data_size_mismatch` | a stream's bytes differ from `count * element size`, or float values do not fill whole elements |
| `invalid_attribute_key` | a set index above 0 on position, normal or tangent, a custom attribute without a name, or a name on another semantic |
| `non_finite_position` | `computeBounds` met NaN or infinity |
| `attribute_not_found` | `interleave` was asked for an attribute the mesh lacks |

`validate` reports the first violation in the order: stream keys, duplicates,
formats and sizes, position presence, counts, element count for the topology,
indices, bounds. It never repairs anything. Allocation failure throws
`std::bad_alloc`, as for any standard container.

## Optional and required dependencies

Core only, required. There is no optional dependency.

## Native or backend escape hatch

There is no backend. The stored bytes are the escape hatch: `AttributeStream::data`
and the typed index vectors can be handed to any API unchanged.

## Conventions

These are the conventions of the data in a `MeshData`. Asset states them and
never converts between them silently; the generators (#346) produce them and decoders
convert into them.

- **Position space** (positions, normals, tangents): right-handed, +Y up.
- **Winding**: a front-facing triangle is counter-clockwise when seen from
  outside the surface, so its normal points toward the viewer.
- **Texture coordinates** are a different space from positions: the origin is the
  top-left of the image, `u` grows to the right and `v` grows downward, like image
  rows. The application flips `v` or the clip space where its sampler or API
  expects the opposite.
- **Storage** is deinterleaved: one tightly packed stream per attribute. `interleave`
  packs a chosen set of attributes, in a chosen order, into one vertex buffer with
  no padding.
- **Formats**: float32 with 1 to 4 components for now; indices are `uint16` or
  `uint32`. Further formats (for example normalized 8/16-bit) are added as enum
  values later.
- **Topologies**: triangle list, triangle strip, line list, point list. A strip is
  valid with 0 or at least 3 elements; no primitive restart value is defined.

## Relation to ModelData (#276) and GPU conversion (#279)

A `ModelData` (#276, [below](#modeldata-276-part-2)) holds a list of `MeshData` plus materials, nodes and
metadata about unsupported features; a glTF primitive maps to one `MeshData`.
`MeshData` stays usable alone, which is how generators use it.

Converting a mesh for the GPU is the job of the [Asset-Graphics Integration](asset_graphics.md)
(#279), not of this component. `layoutOf(mesh)` and
`interleave(mesh, order)` return everything it needs without a Vulkan type:
topology, index type and counts, and for each attribute its semantic, set,
format, offset and stride. Mapping them to `vk::` structs, creating the buffers and producing the
upload items is done there, through the public APIs of both components; Asset itself has no
Graphics dependency.

## Minimal explicit example

```cpp
#include <terreate/asset/mesh_data.hpp>

#include <cstdint>
#include <vector>

namespace asset = terreate::asset;

asset::MeshData triangle() {
  const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  asset::MeshData mesh;
  mesh.attributes.push_back(terreate::unwrap(asset::makeAttributeStream(
      {asset::AttributeSemantic::position, 0, {}},
      asset::AttributeFormat::float32x3, positions)));
  mesh.indices = std::vector<std::uint16_t>{0, 1, 2};
  mesh.bounds = terreate::unwrap(asset::computeBounds(mesh));
  terreate::unwrap(asset::validate(mesh));
  return mesh;
}
```

The installed-package consumer in `cmake.package_smoke` does the same against an
installed Asset with Vulkan and GLFW disabled.

## Convenience API and its lower-level equivalent

`makeAttributeStream` and `computeBounds` are conveniences. The same result is
reached by filling `AttributeStream::count` and `data` and `MeshData::bounds`
directly; `validate` then checks it.

## Primitive generators (#346)

`<terreate/asset/primitives.hpp>` produces common shapes as `MeshData` on the CPU:
`triangle`, `quad`, `box`, `plane` (a grid), `uvSphere` and `cylinder` (a cone is a
cylinder whose `radiusTop` is 0). Each takes a plain description struct and returns
`terreate::Result<MeshData>`:

```cpp
#include <terreate/asset/primitives.hpp>

namespace asset = terreate::asset;

auto sphere = asset::uvSphere({.radius = 1.0F, .segments = 24, .rings = 12, .tangents = true});
auto cone = asset::cylinder({.radiusTop = 0.0F, .radiusBottom = 0.5F, .height = 1.0F});
```

Every returned mesh is a `triangle_list` with an index buffer, passes `validate` and has
`bounds` computed. Each description has the flags `normals` (default true), `uvs`
(default true) and `tangents` (default false); the returned streams are position,
normal (float32x3), texcoord set 0 (float32x2) and tangent (float32x4) as requested, so
presence and layout are read from the mesh (`find`, `layoutOf`). Generated meshes are
deterministic: equal descriptions give equal bytes.

| Generator | Description members (defaults) | Vertices | Indices |
| --- | --- | --- | --- |
| `triangle` | `width` 1, `height` 1 | 3 | 3 |
| `quad` | `width` 1, `height` 1 | 4 | 6 |
| `box` | `width`, `height`, `depth` 1 | 24 | 36 |
| `plane` | `width` 1, `depth` 1, `segmentsX` 1, `segmentsZ` 1 | (sx+1)(sz+1) | 6 sx sz |
| `uvSphere` | `radius` 0.5, `segments` 32, `rings` 16 | (s+1)(r+1) | 6 s (r-1) |
| `cylinder` | `radiusTop` 0.5, `radiusBottom` 0.5, `height` 1, `segments` 32, `heightSegments` 1, `capTop`, `capBottom` true | (s+1)(h+1) + (s+2) per cap | 6 s h - 3 s per zero radius, + 3 s per cap |

A cap whose radius is 0 is not generated, and triangles collapsed by a zero radius (the
poles of a sphere, a cone's apex) are not emitted, so no generated triangle is degenerate.
Defaults for the default sphere: 561 vertices and 2880 indices; for the default cylinder:
134 vertices and 384 indices.

### Geometry conventions of the generators

These are the conventions of `MeshData`, checked by the `asset.primitives` test against the
triangle data of every shape (geometric normal from positions, agreement with the normal
stream, unit normals, UV range and orientation, tangent frame, bounds, and, for the box,
sphere, cylinder and cone, that every edge is shared by exactly two consistently oriented
triangles):

- Right-handed, +Y up; triangles wind counter-clockwise seen from outside.
- Every shape is centered on the origin. `triangle` and `quad` lie in the XY plane facing
  +Z, `plane` in the XZ plane at y = 0 facing +Y, `cylinder` and `uvSphere` stand on the Y axis.
- Texture coordinates start at the top-left (v grows downward). `quad`: the +Y edge has
  v = 0. `plane`: u follows +X and v follows +Z. Spheres and cylinders: v = 0 at the +Y end,
  u = 0 and u = 1 meet at the +Z meridian, with the seam vertices duplicated. Box faces map
  the whole 0..1 square each. Caps map a planar disc.
- Flat shapes use one normal per face; sphere and cylinder side use smooth normals (the
  cylinder's follow the slope).
- Tangents point toward increasing u and have w = +1; `cross(normal, tangent.xyz) * w` points
  toward decreasing v (the glTF convention). No generated shape is mirrored.
- The index type is `uint16` when the vertex count is at most 65536 and `uint32` otherwise.

### Generator errors

Failures return `PrimitiveError` in the category `terreate.asset.primitives`; the Error's
context is the generator and its detail names the offending description member. Parameters
are checked in this order: sizes, attribute combination, segment counts, element counts.

| `PrimitiveError` | Meaning |
| --- | --- |
| `invalid_size` | a size or radius is zero, negative, NaN or infinite (a cylinder radius may be 0, not both) |
| `invalid_attribute_combination` | `tangents` requested without both `normals` and `uvs` |
| `too_few_segments` | `plane` segments below 1, sphere `segments` below 3 or `rings` below 2, cylinder `segments` below 3 or `heightSegments` below 1 |
| `too_many_elements` | the vertex count or the index count would exceed 2^32 - 1 (checked without wrapping, before any allocation) |

Allocation failure throws `std::bad_alloc`, as elsewhere in Asset.

## ImageData (#276 part 1)

`<terreate/asset/image_data.hpp>` holds one decoded image on the CPU, the way `MeshData`
holds geometry: an owning, copyable value with public members, a `validate` that returns a
`Result` with a named error category, no repair and no GPU type. It is what image decoders
(#277, #278) produce and what the GPU conversion of #279 part 2 consumes.

- **Format**: `PixelFormat` is Asset's own enum: `r8`, `rg8`, `rgb8`, `rgba8`, `r16`, `rg16`,
  `rgba16` (unsigned normalized) and `r32f`, `rg32f`, `rgba32f` (binary32 floats). Components
  are stored r, g, b, a, in host byte order, tightly packed per pixel. Compressed formats are
  added as enumerators later. `componentCount`, `componentSize`, `byteSize`, `isFloat` and
  `hasAlpha` describe a format.
- **Interpretation** is a separate field, `ColorInterpretation`, so the same bytes can be a
  color or a data texture: `srgb` (a color texture; sRGB-encoded color, linear alpha),
  `linear` (a data texture or linear color, values used as stored) and `normal_map` (a
  tangent-space normal remapped from -1..1 to 0..1, stored linearly). `AlphaMode` is `none`,
  `straight` or `premultiplied`.
- **Shape**: `extent` (width, height, depth), `array_layers` and `mip_levels`. 1D is height 1
  and depth 1, 2D is depth 1, 3D has depth above 1 and exactly one layer. `mip_levels` is
  the number of levels present: 1 is valid, and a partial chain is valid up to
  `maxMipLevels(extent)`. Each level halves every dimension, rounding down, never below 1
  (`mipExtent`).
- **Storage**: one `data` byte vector and a `subresources` table of `{mip, layer, offset,
  row_pitch, slice_pitch}`. `packedLayout(format, extent, mips, layers)` builds the tightly
  packed table and the total size (layer-major, contiguous, `row_pitch` the tight row size,
  `slice_pitch = row_pitch * height`). A decoder may use larger pitches (row padding, for
  example); the table order is free and offsets may leave gaps.
- **Access**: `find(mip, layer)` returns the table entry; `bytes(mip, layer)` returns a
  `std::span` over the subresource, from its first pixel to the end of its last row
  (`(depth-1)*slice_pitch + (height-1)*row_pitch + width*pixel size`; trailing padding is not
  part of the span). It checks that the entry fits `data` and its pitches (the pitch and bounds
  checks of `validate`), so it is safe on an image that was never validated; it does not check
  the rest of the image.
- **Custom and unsupported lists**: `custom` is a list of `{key, string-or-bytes}` entries for
  format-specific information a decoder wants to keep (keys are non-empty and unique).
  `unsupported` is a list of `{kind, name, detail}` for things the decoder could not
  represent. The rule is that a decoder never drops such a thing silently: it lists it, and
  validate only requires a non-empty `kind`.
- **Cube maps** are left out of this part. A cube map is a 2D image with a multiple of 6
  layers; the consumer decides at GPU conversion. A flag can be added later without changing
  the layout.

### ImageData conventions

- **Row order** is top-first: row 0 is the top row of the image, matching the top-left origin
  of texture coordinates in `MeshData` (`v` grows downward like rows). Asset never flips rows;
  a decoder of a bottom-up format reorders rows into this order, and the application flips for
  its own convention.
- **Color interpretation** is only a statement; Asset never converts values between sRGB and
  linear. `srgb` is allowed for the 8-bit formats only (there is no 16-bit sRGB storage
  format to map to), `normal_map` for integer formats with two or more components, and
  float formats are `linear` only.
- **Alpha mode** `straight` and `premultiplied` need an rgba format; `none` is allowed
  everywhere (an alpha channel of an rgba format is then ignored).
- **Byte order** is host order; no alignment of `data` is assumed by Asset.

### ImageData errors

Failures return `ImageDataError` in the category `terreate.asset.image_data`; the Error's
context is the operation and its detail names the offending field or subresource.

| `ImageDataError` | Meaning |
| --- | --- |
| `zero_extent` | a dimension of the extent is 0 |
| `invalid_layer_count` | `array_layers` is 0, or a 3D image has more than one layer |
| `invalid_mip_count` | `mip_levels` is 0 or exceeds `maxMipLevels(extent)` |
| `interpretation_not_allowed` | float formats allow `linear` only; `srgb` needs an 8-bit format; `normal_map` needs an integer format with at least two components |
| `alpha_mode_not_allowed` | `straight` or `premultiplied` on a format without alpha |
| `invalid_subresource` | a table entry names a mip or layer outside the image |
| `duplicate_subresource` | two entries name the same (mip, layer) |
| `missing_subresource` | a (mip, layer) has no entry (`bytes`: the entry is absent) |
| `pitch_too_small` | a row pitch below the tight row size, or (3D only) a slice pitch below `row_pitch * height` |
| `data_out_of_bounds` | a subresource reaches past the end of `data` |
| `size_overflow` | a size or offset computation does not fit `std::size_t` (all arithmetic is checked, also for the extent alone) |
| `invalid_custom_entry` | a custom key is empty or repeated |
| `invalid_unsupported_entry` | an unsupported entry has an empty `kind` |

`validate` reports the first violation in the order: extent, layer count, mip count,
interpretation, alpha mode, tight size of the whole image, custom entries, unsupported
entries, each table entry in order (range, pitches, bounds), duplicates, missing entries. It
never repairs anything. Overlap between subresources is not checked. Allocation failure
throws `std::bad_alloc`.

### Relation to ModelData and GPU conversion

A `ModelData` ([#276 part 2](#modeldata-276-part-2)) holds its images as a list of `ImageData`,
with samplers, textures and materials referring to them by index; the decoders #277 and #278
produce both. Converting an image for the GPU (format mapping, staging, mip upload) is the
job of the [Asset-Graphics Integration](asset_graphics.md), #279 part 2, through the public
APIs of both components. Asset has no Graphics dependency, and `ImageData` stays usable alone.

```cpp
#include <terreate/asset/image_data.hpp>

#include <cstddef>
#include <utility>

namespace asset = terreate::asset;

asset::ImageData whiteTexture() {
  asset::ImageData image;
  image.format = asset::PixelFormat::rgba8;
  image.interpretation = asset::ColorInterpretation::srgb;
  image.extent = {2, 2, 1};
  auto layout = terreate::unwrap(asset::packedLayout(image.format, image.extent, 1, 1));
  image.data.assign(layout.byte_size, std::byte{255});
  image.subresources = std::move(layout.subresources);
  terreate::unwrap(asset::validate(image));
  return image;
}
```

## ModelData (#276 part 2)

`<terreate/asset/model_data.hpp>` holds one decoded model on the CPU: the meshes, images, samplers,
textures, materials and node hierarchy that a glTF or similar decoder (#277, #278) produces. Like
`MeshData` and `ImageData` it is an owning, copyable value with public members, a `validate` that returns
a `Result` with a named error category and no repair, and no GPU type. It adds no dependency: the same
`terreate_asset` target, Core only.

### Structure and ownership

`ModelData` owns every list by value; all cross references are indices into sibling lists of the same
`ModelData`, never pointers, so a copy or a move keeps them valid and a copy is fully independent.

| List | Element | References |
| --- | --- | --- |
| `meshes` | `MeshData` | none |
| `primitives` | `ModelPrimitive {mesh, material?}` | `meshes`, `materials` |
| `model_meshes` | `ModelMesh {name, primitives}` (at least one primitive) | `primitives` |
| `images` | `ImageData` | none |
| `samplers` | `SamplerData {min_filter, mag_filter, mipmap_mode, wrap_u, wrap_v}` | none |
| `textures` | `TextureData {image, sampler?}` | `images`, `samplers` |
| `materials` | `MaterialData` (below) | `textures` through `TextureRef {texture, texcoord}` |
| `nodes` | `ModelNode {name, children, transform, mesh?}` | `nodes`, `model_meshes` |
| `scenes` | `ModelScene {name, roots}` | `nodes` |
| `default_scene` | optional index | `scenes` |
| `custom`, `unsupported` | `CustomEntry`, `UnsupportedFeature` (the types of `ImageData`) | none |

Unused entries are valid (an unreferenced mesh, material or node is not an error). Sampler enumerators are
Asset's own (`SamplerFilter`, `SamplerMipmapMode` with a `none` value, `SamplerWrap`) and name no Vulkan
value; mapping them is the consumer's job. Vectors are plain `std::array<float, N>`; there is no math
dependency.

### The glTF structure, mirrored

glTF places geometry through four levels, and `ModelData` has the same four:

```text
glTF:       scene -> node -> mesh      -> primitive      -> geometry + material
Terreate:   scene -> node -> ModelMesh -> ModelPrimitive -> MeshData + material
            (ModelScene, ModelNode::mesh, ModelData::model_meshes, ModelMesh::primitives,
             ModelPrimitive{mesh, material?})
```

- **Naming.** `MeshData` is the geometry of one glTF *primitive* (vertex attributes and indices), not a
  glTF *mesh*; it is the shared geometry type of Asset and also describes generated shapes. The glTF mesh
  (a group of primitives) is `ModelMesh`, stored in `ModelData::model_meshes`; the name differs from
  `ModelData::meshes` (the `MeshData` list) so the two cannot be confused. `ModelPrimitive::mesh` indexes
  `meshes` (a `MeshData`), while `ModelNode::mesh` indexes `model_meshes` (a `ModelMesh`).
- **Instancing.** Two or more nodes may name the same `ModelMesh`, and several `ModelMesh` entries may list
  the same `ModelPrimitive`. Each node places the whole mesh with its own transform; the geometry is stored
  once.
- **Decoders map 1:1.** A glTF mesh becomes one `ModelMesh` with one `ModelPrimitive` per glTF primitive,
  and a glTF node with a mesh becomes one `ModelNode` whose `mesh` names it. A decoder invents no nodes
  (no node per primitive) and no extra mesh level.

### Node and transform conventions

- The space is right-handed with +Y up, as for `MeshData`; Asset converts nothing.
- A transform is relative to the parent and is a `std::variant<TrsTransform, MatrixTransform>`, like glTF.
  `TrsTransform` holds a translation, a rotation quaternion in (x, y, z, w) order (the identity is
  `0, 0, 0, 1`) and a scale; the matrix it stands for is `T * R * S`. `MatrixTransform` holds 16 floats in
  column-major order (element `4 * c + r`, translation in elements 12 to 14). The default node transform is
  the identity TRS.
- The hierarchy is a forest: a node has at most one parent, and `scenes[i].roots` lists parentless nodes.
  A node that no scene reaches is valid.

### Materials

`MaterialData` is the glTF 2.0 metallic-roughness model, with the glTF defaults: `base_color_factor` and
optional `base_color_texture`, `metallic_factor` and `roughness_factor` with an optional
`metallic_roughness_texture`, an optional `normal_texture` with `normal_scale`, `emissive_factor` with an
optional `emissive_texture`, an optional `occlusion_texture` with `occlusion_strength`, `alpha_mode`
(`opaque`, `mask`, `blend`) with `alpha_cutoff`, and `double_sided`. A `TextureRef` is a texture index plus
the texcoord set that samples it (`TEXCOORD_n` is the `MeshData` attribute `texcoord` with set `n`).

- **Mapping**: core glTF 2.0 PBR (no extensions) maps to these fields without loss: every core material property has a
  field with the same meaning, and the factor ranges follow core glTF.
- **Deliberately out of scope**: glTF material extensions (clearcoat, transmission, specular, sheen, IOR,
  emissive strength, unlit, texture transform and others) and the legacy specular-glossiness model; a
  decoder lists each it meets in `unsupported`. Rendering the material (shaders, lighting) is the PBR work
  of #250, not part of Asset.
- The color interpretation of a texture image (`srgb` for base color and emissive, `linear` or
  `normal_map` otherwise) belongs to the `ImageData` and is not checked against how a material uses it,
  because one image may legitimately serve several roles.

### What is not represented

Animation, skins, cameras and lights are not represented in `ModelData`. A decoder that meets any of them
must record it in `unsupported` (for example `{"animation", "Walk", "..."}`) rather than drop it silently.
The same holds for anything else it cannot represent. `custom` keeps format-specific data (for example the
generator name) as key/value entries with non-empty, unique keys.

### ModelData errors

Failures return `ModelDataError` in the category `terreate.asset.model_data`; the Error's context is
`validate(ModelData)` and its detail names the offending list and index. An invalid `MeshData` or
`ImageData` is reported as `invalid_mesh` or `invalid_image` with the inner error's category, message and
detail in the detail text (for example `meshes[2]: terreate.asset.mesh_data: ...`); the inner error code
itself is not passed through, so test a model with `ModelDataError`.

| `ModelDataError` | Meaning |
| --- | --- |
| `index_out_of_range` | any index past the end of its list, including a default scene without scenes |
| `invalid_mesh` / `invalid_image` | a mesh or image fails its own `validate` |
| `invalid_enum` | a sampler or alpha-mode enumerator is not a declared value |
| `invalid_material_value` | base color, metallic, roughness, emissive or occlusion strength outside 0..1 or not finite; `normal_scale` not finite; `alpha_cutoff` negative or not finite |
| `invalid_transform` | a non-finite TRS or matrix value, or a quaternion whose length is not within 1e-3 of 1 |
| `node_cycle` | a node is its own ancestor |
| `multiple_parents` | a node is a child more than once (of two nodes or twice of one) |
| `invalid_scene_root` | a scene root has a parent or is listed twice in a scene |
| `empty_model_mesh` | a `ModelMesh` lists no primitive |
| `missing_texcoord_set` | a material used by a primitive that a node's mesh reaches samples a texcoord set that the primitive's `MeshData` lacks |
| `invalid_custom_entry` / `invalid_unsupported_entry` | an empty or repeated custom key, an empty unsupported kind |

`validate` reports the first violation in the order: meshes, images, samplers, textures, materials,
primitives, model meshes (not empty, primitive indices), nodes (mesh index, transforms, child lists), cycles, scenes and default scene, texcoord sets,
custom entries, unsupported entries. The texcoord rule applies to the primitives that a node's mesh reaches and
to the materials they use; a `MeshData` may lack a texcoord set that an unused material, or a primitive no
node reaches, names. A matrix transform is only checked for finiteness;
whether it decomposes into translation, rotation and scale is not. `validate` is iterative with memory
linear in the node count, so a very long node chain cannot overflow the stack. Allocation failure throws
`std::bad_alloc`.

### Relation to decoders, GPU conversion and rendering

The decoders #277 and #278 produce `ImageData` and `ModelData`. Converting an image or a mesh for the GPU
is the job of the [Asset-Graphics Integration](asset_graphics.md) (#279: `MeshData` in part 1, `ImageData`
in part 2); converting a whole `ModelData` to GPU resources is not defined here or by that part, and an
application walks the lists with the public APIs of both components. Resolving external files a model
refers to is the [resolver of #280](#decode-contracts-and-the-resource-resolver-280). Material rendering and PBR shading are #250. Asset has no Graphics
dependency, and `ModelData` stays usable on the CPU alone.

```cpp
#include <terreate/asset/model_data.hpp>
#include <terreate/asset/primitives.hpp>

namespace asset = terreate::asset;

asset::ModelData oneBox() {
  asset::ModelData model;
  model.meshes.push_back(terreate::unwrap(asset::box({})));
  model.materials.emplace_back();
  model.primitives.push_back({0, 0});
  model.model_meshes.push_back({"box", {0}});
  model.nodes.resize(1);
  model.nodes[0].mesh = 0;
  model.scenes.push_back({"main", {0}});
  model.default_scene = 0;
  terreate::unwrap(asset::validate(model));
  return model;
}
```

## Decode contracts and the resource resolver (#280)

Decoders (the image decoder #277 and the glTF decoder #278) are adapters outside Asset, but the contracts
they share live here, in `<terreate/asset/decode.hpp>` and `<terreate/asset/resource_resolver.hpp>`. They
are CPU-only, add no dependency, and give a decoder no authority: **a decoder never opens a file, a socket
or an archive**. Everything it needs beyond its input comes through an application-provided resolver.

### Byte-source rule

A decoder takes its input as `ByteView` (`std::span<const std::byte>`) plus a `SourceIdentity`
(`DecodeInput` bundles the two). The caller owns the bytes and keeps them alive for the call; a decoder
that needs them later copies them. Bytes obtained from an external reference come back from the resolver
as a `ResolvedResource` and are then decoded under the same rule, so there is one input convention.

`SourceIdentity` is metadata: `name` is what the application calls the source (a path, an archive entry, a
URL) and `base` is the root-less directory against which the source's relative references resolve. It
grants no access and is used for provenance and for resolving references.

### Resolver

A reference found in an asset (a glTF `uri`) becomes a `ResourceRequest`: the reference exactly as written,
the `SourceIdentity` that contains it, a `ResourceKind` (`buffer`, `image`, `other`) and an optional
declared size and media type (untrusted hints). The application answers with a `ResolvedResource`: owned or
shared bytes plus the identity they were resolved to.

`ResourceResolverView` is a type-erased, non-owning, synchronous view bound to an application object or
callable with `ResourceResolverView::bind(target)`, the same pattern as `DiagnosticSinkView` (the target is
an object lvalue that outlives every call; binding allocates nothing). An unbound view fails with
`ResolveError::no_resolver`. `MemoryResolver` is the reference resolver: a map from a normalized reference to
bytes, for tests and for archive-like use (the application adds the entries of an archive it has read).
A resolver backed by a file system or a network is application code written against the same signature,
`Result<ResolvedResource>(const ResourceRequest &)`.

**Sync and async.** The contract is synchronous: the call returns bytes or an error. An application that
fetches asynchronously resolves ahead of time (it can read the asset, ask a decoder or its own code for the
reference list, fetch everything) and gives the decoder a `MemoryResolver` that already holds the bytes.
Asset starts no thread and never waits.

### Limits

`DecodeLimits` bounds a decode; `defaultDecodeLimits()` returns the conservative defaults below. Raising a
limit is the application's explicit act (assign the field). Exceeding a limit is a named error, never
truncation; a limit of 0 allows none of the thing.

| Field | Default | Enforced by |
| --- | --- | --- |
| `max_input_bytes` | 256 MiB | `checkInputSize` (`DecodeError::input_too_large`) |
| `max_pixels` | 67108864 (8192 x 8192) | `checkImageSize` (`pixel_count_exceeded`) |
| `max_dimension` | 16384 | `checkImageSize` (`dimension_exceeded`) |
| `max_nodes` | 65536 | the glTF decoder (`DecodeError::count_exceeded`) |
| `max_meshes` | 16384 | the glTF decoder (`count_exceeded`) |
| `max_primitives` | 65536 | the glTF decoder (`count_exceeded`) |
| `max_hierarchy_depth` | 256 | the glTF decoder (`DecodeError::hierarchy_too_deep`) |
| `max_external_resources` | 256 | `ResolveSession` (`ResolveError::limit_exceeded`) |
| `max_resource_bytes` | 256 MiB | `ResolveSession` (`limit_exceeded`) |
| `max_total_resource_bytes` | 1 GiB | `ResolveSession` (`limit_exceeded`) |
| `max_resolve_depth` | 4 | `ResolveSession` (`ResolveError::depth_exceeded`) |

The size checks run before the decoder allocates or decodes, and use overflow-safe arithmetic.

### Reference rules

`normalizeReference(reference, base_directory, options)` is the path-traversal information for a reference
from an untrusted asset. It is lexical (no file system, no network) and returns the normalized reference
(a root-less `/`-separated path) or a `ReferenceError` (category `terreate.asset.reference`).

- Rejected: empty reference; longer than `ReferenceOptions::max_length` (2048); NUL or another control
  character, raw or percent-encoded; a leading `/` (absolute, `//host`); `\` raw or `%5C`; a drive letter
  (`C:`); an unencoded `?` or `#` and any `:` in a path; a `%2F`; a malformed `%` escape; a reference naming
  a directory (ending in `/`, `.` or `..`); a `..` that would leave the root.
- Percent-decoding: each path segment is decoded exactly once, before the `.`/`..` handling. `%2e%2e` is
  `..` (rejected when it escapes); `%252e` is the literal name `%2e`. Non-ASCII bytes pass unchanged.
- Normalization: empty and `.` segments are dropped, `..` pops. The reference is applied to the source's
  `base` directory, so `../textures/a.png` from `models/` is `textures/a.png`, while `../../x` is rejected.
- URI schemes are rejected unless `ReferenceOptions::allowed_schemes` lists them. An allowed URI keeps its
  remainder as written (only a `..` segment, control characters and `\` are refused) and is the resolver's
  responsibility. `data:` URIs carry their bytes inline; a decoder either decodes them itself, without
  I/O, or the application allows `data` and handles it in the resolver.

This does not make a file system lookup safe alone. Symbolic links, case-insensitive or
Unicode-normalizing file systems and operating-system device names are the resolver's concern; a
file-system resolver should resolve the normalized path under a fixed root and re-check the result.

### ResolveSession and provenance

A decoder resolves every external reference through one `ResolveSession(resolver, limits, source, options)`,
so the limits are enforced centrally. `session.makeRequest(reference, kind)` builds a request whose base is
the source; for a reference inside a resolved resource, set `request.base = resource.identity()` and pass
`depth + 1`. `resolve(request, depth)` checks, in order: the request count and depth limits, the resolver
(`no_resolver`), the reference (`reference_rejected`, the detail names the reason), a declared size against
the per-resource and remaining-total limits, then calls the resolver and checks the actual size.

Errors: a failure from the resolver that already carries a `ResolveError` code (for example `not_found`) is
returned unchanged; any other failure, or an exception, is wrapped as `ResolveError::resolver_failed`
with the original category, value and message in the detail.

| `ResolveError` | Meaning |
| --- | --- |
| `no_resolver` | an external resource is needed and no resolver is bound |
| `reference_rejected` | `normalizeReference` refused the reference |
| `not_found` | the resolver has no resource for it |
| `limit_exceeded` | the request count, a resource size or the total size is over its limit |
| `resolver_failed` | the resolver failed with another error (wrapped) |
| `depth_exceeded` | the request is nested too deeply |

`session.provenance()` (`DecodeProvenance`) holds the source identity and one `ResolveRecord` per request,
including refused and failed ones: the request (reference as written and the normalized reference), the
depth, the identity it resolved to, the size and the outcome code.

### Diagnostics

`DecodeDiagnostic` is `{severity (info, warning, error), code, message, location}` with a `DecodeLocation`
(source name, a format-specific path such as `/nodes/3`, an optional byte offset). Decoders fill them for
what they could not represent, next to the `unsupported` lists of `ModelData` and `ImageData`; nothing is
dropped silently. `DecodeError` (category `terreate.asset.decode`) names the shared decode failures
(`input_too_large`, `dimension_exceeded`, `pixel_count_exceeded`, `count_exceeded`,
`hierarchy_too_deep`, `malformed_input`, `unsupported_input`).

### Security notes for untrusted input

Treat every reference, declared size and media type as attacker-controlled. Keep the default limits unless
the application has a reason and a trusted source; do not bind a resolver that reads outside a root you
chose; allow URI schemes only deliberately; check sizes before allocating (the session does so with the
declared size and again with the real size); and keep the provenance of what was loaded.

### How the adapters use it

The image decoder (#277) takes `DecodeInput` and `DecodeLimits`, calls `checkInputSize` and `checkImageSize`
before decoding, and needs no resolver. The glTF decoder (#278) takes `DecodeInput`, `DecodeLimits` and a
`ResourceResolverView`, builds a `ResolveSession` for the external buffers and images, resolves a
reference once and shares the result, reports diagnostics and the `DecodeProvenance` to the caller, and does
no I/O of its own. An application that already has the bytes passes a `MemoryResolver`.

### Further notes

- `ResourceRequest::max_bytes` is filled by the session with the smaller of the per-resource limit and the
  remaining total. A resolver that reads a file or stream should stop there; the session checks the real size
  again, but only after the resolver has allocated the bytes.
- A URI reference is returned undecoded. A resolver for it decodes exactly once. `%2F` in a URI is refused
  and the length limit (default 2048) also applies, so a long `data:` URI needs a larger `max_length` or is
  better decoded by the decoder itself. `file` in `allowed_schemes` would allow `file:///...`: that is the
  application's explicit choice.
- The reference is untrusted text; error details contain only a short printable prefix of it. The reason a
  reference was refused is in the detail (the message of the `ReferenceError`); call `normalizeReference`
  directly to branch on the code.
- The base directory must not start with `/`, may not escape with `..`, and may not contain control
  characters, `\` or `:`; it is normalized again with each reference.
- Windows ignores trailing dots and spaces, reserves device names and treats some non-ASCII forms
  specially; a file-system resolver on such a system must handle that itself.

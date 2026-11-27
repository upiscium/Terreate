# Asset: MeshData (#345), primitive generators (#346) and ImageData (#276)

`Terreate::Asset` (`terreate::asset`) holds CPU-side asset data that primitive
generators, model decoders and rendering code share. Its types are `MeshData`, declared in
`<terreate/asset/mesh_data.hpp>`, and `ImageData` ([below](#imagedata-276-part-1)), declared in
`<terreate/asset/image_data.hpp>`.

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

A `ModelData` (#276) will hold a list of `MeshData` plus materials, nodes and
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

A `ModelData` (#276 part 2, a later change) will hold its images as a list of `ImageData`,
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

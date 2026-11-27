# Asset: the MeshData contract (#345)

`Terreate::Asset` (`terreate::asset`) holds CPU-side asset data that primitive
generators, model decoders and rendering code share. Its first type is
`MeshData`, declared in `<terreate/asset/mesh_data.hpp>`.

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
  `ModelData` of #276), no primitive generators (#346), no GPU upload or
  `VertexLayout` conversion (#279), no repair of malformed data.

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
never converts between them silently; generators (#346) produce them and decoders
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

Converting a mesh for the GPU is #279's job. `layoutOf(mesh)` and
`interleave(mesh, order)` return everything it needs without a Vulkan type:
topology, index type and counts, and for each attribute its semantic, set,
format, offset and stride. Mapping them to `vk::` structs or Graphics'
`VertexLayout` is outside this component.

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

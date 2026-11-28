# stb_image Adapter: decoding image bytes to ImageData (#277)

`Terreate::AdapterAssetImageStb` (`terreate::adapter::asset_image_stb`) is the first practical
decoder: it turns the bytes of one image in memory into an Asset `ImageData`. Its header is
`<terreate/adapter/asset_image_stb/decoder.hpp>`. It implements the decode contracts of Asset
(`asset::DecodeInput`, `asset::DecodeLimits`, `asset::DecodeDiagnostic`; see
[Asset](asset.md)) with one backend, [stb_image](https://github.com/nothings/stb), which is a
private implementation detail.

## Responsibility and non-goals

One function does the job: `decodeImage(input, options)` returns an `ImageDecodeResult`: a valid
`ImageData`, the diagnostics, the identity of the source and what the backend found in the file.

Non-goals:

- **No file system, no network, no path.** The input is a borrowed span of bytes plus a source
  identity (`asset::DecodeInput`). A path-based convenience is composed outside, see
  [Composing a path-based convenience](#composing-a-path-based-convenience).
- **No GPU resource.** The result is CPU data; uploading it is the job of the
  [Asset-Graphics Integration](asset_graphics.md). The Adapter is built and tested without
  Graphics.
- No mip generation, no cube-map or array assembly, no color management (ICC profiles and gamma are
  reported, not applied), no animation (a GIF is refused), no writing of images.
- No hidden loader, cache or thread. The call decodes on the calling thread and returns.

## Public dependencies

Exactly `Terreate::Asset` (and Core through it). stb_image is **not** a dependency of anything a
consumer sees: it is compiled into the Adapter's static library, no public header includes it, no
usage requirement names it, and the installed package needs no `find_dependency` for it. The component
matrix and the package smoke test check that the target links exactly Asset, that no stb header or
reference is installed, that a consumer built with the Vulkan and GLFW searches disabled links, and that
no other component links the Adapter. Asset never depends on the Adapter.

## Formats

Enabled in the backend (and documented as the supported set):

| Container | Result formats | Notes |
| --- | --- | --- |
| PNG | `r8` `rg8` `rgb8` `rgba8`; 16-bit: `r16` `rg16` `rgba16` | gray, gray+alpha, RGB, RGBA, palette, 1/2/4/8/16-bit, `tRNS`; interlaced |
| JPEG | `r8` `rgb8` (`rg8`/`rgba8` when forced) | baseline and progressive, 8-bit samples; not arithmetic coded |
| BMP | `rgb8` `rgba8` | uncompressed and bitfield; not RLE |
| TGA | `r8` `rg8` `rgb8` `rgba8` | recognized by stb_image's header heuristic (TGA has no magic number) |
| Radiance HDR | `r32f` `rg32f` `rgba32f` | decoded to 32-bit floats |

Disabled on purpose (`STBI_NO_*`): GIF, PSD, PIC and PNM, plus all file I/O (`STBI_NO_STDIO`: the
backend has no entry point that opens a file) and the SIMD paths (`STBI_NO_SIMD`: one portable code
path, identical on every compiler and CPU). These formats are large attack surfaces or carry
features (animation, layers) that `ImageData` cannot represent. Their signatures (and those of WebP,
TIFF, DDS, KTX and HEIF/AVIF) are recognized and the call fails with
`ImageDecodeError::unsupported_format`, naming the format in the error detail; an unrecognized input
fails the same way. A supported container that uses something the backend cannot decode (arithmetic
JPEG, RLE BMP, an unknown critical PNG chunk) fails with `ImageDecodeError::unsupported_feature`.

`ImageData` has no 3-component 16-bit or float format. As decoded, a 16-bit RGB PNG and an RGB
Radiance HDR therefore become `rgba16` and `rgba32f` with an opaque alpha and
`AlphaMode::none`, and the diagnostic `image.channels_widened` says so. (An `rgb8` result is
representable in `ImageData`, but the Asset-Graphics Integration cannot upload it, because
three-component 8-bit formats are not portable in Vulkan: force `ChannelRequest::four` when the
image is meant for the GPU.) A gray image with alpha has no `ImageData` format either; it is stored
as `rg` (gray, alpha) with `AlphaMode::none`, with the diagnostic `image.gray_alpha_stored_as_rg`.

## The stb_image snapshot

The backend is the `stb` package of nixpkgs, a snapshot of `stb_image.h` v2.28 (the repository
snapshot of 2023-01-29), which is the one the project devShell provides. Relevant to security in
that version: `STBI_MAX_DIMENSIONS` exists (the default, 2^24 in each dimension, is not changed:
the Adapter's `DecodeLimits` are far stricter and checked first), the size arithmetic checks
overflow (`stbi__mad3sizes_valid`) and the failure reason is thread-local when the compiler
supports `thread_local` (C++23 does). Known weaknesses of stb_image that the Adapter does not
hide: it is not a hardened decoder (it has had a steady stream of memory-safety fixes and is only
meant for trusted or sandboxed input), it zero-fills a short BMP, TGA or HDR instead of
failing (the Adapter checks the file size against the header for uncompressed BMP, uncompressed
TGA and flat HDR only: a short RLE TGA, a short run-length HDR, an OS/2 BMP header or JPEG data cut
in the middle of a scan may still decode, partly zero-filled, without an error), it does not
verify PNG CRCs, and it uses a lot of memory on progressive JPEGs. Treat this Adapter as a
**baseline** decoder: decode untrusted images in a process with limits, and raise the application's
limits only on purpose. The Adapter's test decodes every truncation and byte flip of its fixtures (tight limits, a
named error or a valid image each time); the same test was also run once under address and
undefined-behavior sanitizers during development, which is not part of the gate. That is not a
fuzzing campaign.

## Ownership, borrowing and lifetime

- `decodeImage` borrows `input.bytes` for the duration of the call only; the bytes may be
  destroyed afterwards. The result owns everything: the pixels (`ImageData`), the diagnostics and a
  copy of the source identity.
- The backend's pixel buffer is allocated, copied into the result and freed inside the call; the
  peak is several times the decoded size (the backend's working buffers, its output and the
  copy), and more for a progressive JPEG.

## Execution authority and threading

The Adapter owns no thread, queue, cache or wait. `decodeImage` is a pure function of its
arguments: it uses no global state (the backend's flip switch is not used; flipping is done on the
result), so any number of calls may run concurrently on different inputs.

## Requested, supported and effective configuration

`ImageDecodeOptions` is the request; the result is the effective state, observable in the image and
the diagnostics:

| Option | Default | Effect |
| --- | --- | --- |
| `limits` | `asset::defaultDecodeLimits()` | the input size, dimensions and pixel count are checked before any pixel buffer is allocated |
| `interpretation` | empty: `srgb` for 8-bit, `linear` for 16-bit and float | an explicit value is checked against the resulting format (the `ImageData` rules) and refused with `invalid_option` |
| `alpha` | `AlphaHandling::straight` | `premultiply` multiplies the color by alpha and marks the image premultiplied; `ignore` keeps the channel and marks the image `AlphaMode::none`. An image whose source has no alpha is always `none` |
| `channels` | `ChannelRequest::as_decoded` | `one`, `two` or `four` make the backend convert (RGB to luminance, an opaque alpha added) |
| `flip_vertically` | `false` | row 0 of the result is the top row, as in `ImageData`; every supported format is decoded top-down already, so the flip is only for a consumer that wants the other order |

The channel count of the source is the one the decode produces (a palette PNG counts 3, 4 with a
transparency table; a `tRNS` color key adds an alpha channel).

## Errors and diagnostics

Failures are `terreate::Result` errors. Limit failures are the shared `asset::DecodeError`
values, unchanged: `input_too_large`, `dimension_exceeded` (a zero dimension too),
`pixel_count_exceeded`. The Adapter's own category `ImageDecodeError` (name
`terreate.adapter.asset_image_stb`) has `malformed_image`, `unsupported_format`,
`unsupported_feature`, `invalid_option` and `out_of_memory`; the error detail holds the backend's
short reason.

The order is fixed and each step rejects before the next allocates: input size, signature, the
size the header claims (against the limits, and for BMP, TGA and flat HDR against the file size),
the backend's header probe, the reported size against the limits, the options against the
resulting format, and only then the pixel decode. A truncated or hostile input never makes the
Adapter read outside the span or let the backend allocate without bound: every backend allocation
goes through a per-call byte budget of 4 MiB plus 48 bytes per pixel of the (limit-checked) header
size, so a decompression bomb (a tiny PNG that inflates to gigabytes) fails as
`ImageDecodeError::out_of_memory`. The memory a legal image needs is a small multiple of
`max_pixels` times the pixel size. The backend takes an `int` length, so an input of 2 GiB or more is
refused as `input_too_large` whatever the limit.

Diagnostics (`asset::DecodeDiagnostic`) are stable codes with the source name and, where it is
known, the byte offset. Every decision is `info` (`image.interpretation_defaulted`,
`image.channels_converted`, `image.channels_widened`, `image.gray_alpha_stored_as_rg`,
`image.alpha_premultiplied`, `image.alpha_ignored`, `image.flipped`, `image.png.transparency_as_alpha`,
`image.png.low_bit_depth_expanded`). Every source feature that is not applied is a `warning` **and**
an entry of `ImageData::unsupported`, never silently dropped: a PNG `iCCP`, `gAMA` or `cHRM` chunk
(`image.png.*_not_applied`), a JPEG ICC profile and a JPEG EXIF block (`image.jpeg.*_not_applied`:
an EXIF orientation is neither read nor applied). Metadata of BMP, TGA and HDR files is not
inspected.

## Optional and required dependencies

- Required: Asset (and Core). Nothing else at build or consume time.
- Build input: `stb_image.h`. The option `TERREATE_BUILD_ADAPTER_ASSET_IMAGE_STB` is **on by default
  only when Asset is built and `stb_image.h` is found**; asking for it without Asset, or without a
  header, is a configure error that names what is missing, never a silent correction. The header is
  looked up in this order, and the first source that is set decides:
  the cache variable `TERREATE_STB_INCLUDE_DIR`, the environment variable `STB_INCLUDE` (the project
  devShell sets it, like `VULKAN_HEADERS_INCLUDE`), then a standard search for `stb_image.h` (also
  below an `stb` directory of a prefix). A set but wrong directory turns the default OFF; an
  explicit `ON` then fails with an error that names the variable.
- A consumer writes `find_package(Terreate REQUIRED COMPONENTS AdapterAssetImageStb)` and links
  `Terreate::AdapterAssetImageStb`; that imports Asset and Core and searches for nothing.
- The header is third-party code from the build environment, so it is a system include of the
  Adapter's one implementation file: project warnings and clang-tidy do not apply to it (the Adapter's own
  `.cpp` files are linted and formatted like all other code).

## Native or backend escape hatch

`ImageDecodeResult::native` (`ImageSourceInfo`) and `ImageData::custom` keep what the backend
found: the container (`stb.container`), the source channel count (`source.channels`), the bits per
channel (`source.bits_per_channel`) and, for PNG, the color type (`png.color_type`), the file bit
depth (`png.bit_depth`), the gamma value (`png.gamma_x100000`) and the presence of an `sRGB` chunk
(`png.srgb_chunk`). stb_image itself is not exposed: there is no stb type or handle in any header.

## Minimal explicit example

```cpp
#include <terreate/adapter/asset_image_stb/decoder.hpp>

namespace stb = terreate::adapter::asset_image_stb;

// `bytes` was read by the application (a file, an archive, a download, an embedded array).
terreate::asset::DecodeInput input{bytes, terreate::asset::makeSourceIdentity("textures/brick.png")};

stb::ImageDecodeOptions options;                         // defaults: srgb, straight alpha, as decoded
options.channels = stb::ChannelRequest::four;            // a GPU-uploadable rgba8
auto decoded = stb::decodeImage(input, options);
if (!decoded) { /* decoded.error(): asset::DecodeError or stb::ImageDecodeError */ }
const terreate::asset::ImageData &image = decoded->image;   // validated
for (const auto &d : decoded->diagnostics) { /* info and warnings */ }
```

## Composing a path-based convenience

The Adapter takes no path on purpose, so a path-based convenience is a few lines in the application
(or in a separate I/O layer) that reads the file and passes the bytes:

```cpp
auto loadImage(const std::filesystem::path &path, const stb::ImageDecodeOptions &options = {})
    -> terreate::Result<stb::ImageDecodeResult> {
  std::vector<std::byte> bytes = readWholeFile(path);   // the application's own reading layer
  return stb::decodeImage({bytes, terreate::asset::makeSourceIdentity(path.generic_string())},
                          options);
}
```

The reading layer owns every file-system decision (where, how much, sandboxing, errors); the
`DecodeLimits` it passes bound the decode. The source identity it chooses gives the provenance and
the base for relative references.

## Relation to the glTF adapter (#278)

The glTF adapter turns glTF images into `ImageData` through an application-provided decode callable
and does not depend on this Adapter. `decodeImage` can be wrapped as that callable without change:
it takes the bytes and their identity and returns an `ImageData` (and diagnostics), for example
`[](const asset::DecodeInput &in) -> Result<asset::ImageData> { auto r = decodeImage(in);
if (!r) return std::unexpected(r.error()); return std::move(r->image); }`. The application passes
the function; nothing here knows about glTF.

## Convenience API and its lower-level equivalent

There is no convenience layer; `decodeImage` is the lowest level and the only one. Uploading the
result is the Asset-Graphics Integration's `planImageUpload`, `createImage` and
`imageUploadItems`.

#ifndef TERREATE_ASSET_MIP_CHAIN_HPP
#define TERREATE_ASSET_MIP_CHAIN_HPP

#include <cstdint>
#include <system_error>

#include <terreate/asset/image_data.hpp>
#include <terreate/core/result.hpp>

namespace terreate::asset {

/// What `generateMipChain` produces. Plain data with documented defaults.
struct MipChainOptions {
  /// The number of levels of the result, the base level included. 0 (the
  /// default) means the full chain down to 1x1x1 (`maxMipLevels(extent)`). A value
  /// from 1 to `maxMipLevels(extent)` gives exactly that many levels (1 returns
  /// the base level repacked tightly); anything above is
  /// `MipChainError::invalid_level_count`.
  std::uint32_t levels = 0;
  /// Only for `ColorInterpretation::normal_map` images with at least three
  /// components: after averaging, the first three components are decoded as a
  /// vector (`2 * value / max - 1`), scaled to length 1 and encoded again. Without
  /// it the averaged components are stored as they are, which shortens the
  /// vectors. Ignored for every other image. Default true.
  bool renormalize_normals = true;
};

/// The failures of `generateMipChain` that are not `ImageDataError`. An input that
/// fails `validate` returns its `ImageDataError` unchanged, as does a size that
/// does not fit `std::size_t` (`ImageDataError::size_overflow`, checked before any
/// allocation). Values start at 1; new values are appended.
enum class MipChainError : std::uint8_t {
  /// The input has more than one mip level (`mip_levels` above 1). The chain is
  /// never completed or overwritten: pass an image with the base level only.
  already_has_mips = 1,
  /// `MipChainOptions::levels` is above `maxMipLevels(extent)`.
  invalid_level_count,
};

/// The category of MipChainError codes (name "terreate.asset.mip_chain").
[[nodiscard]] auto mip_chain_error_category() noexcept -> const std::error_category &;
/// Makes the std::error_code of a MipChainError.
[[nodiscard]] auto make_error_code(MipChainError error) noexcept -> std::error_code;

/// Generates the mip levels of `image` on the CPU and returns them as a new,
/// tightly packed, validated ImageData. `image` is only read. Nothing is generated
/// unless the application calls this function; no other API in Terreate calls it
/// implicitly.
///
/// Input: a valid image with `mip_levels == 1` (any array layer count, any
/// depth, any pitches). More than one level is `MipChainError::already_has_mips`.
///
/// Result: the same format, interpretation, alpha mode, extent, array layers,
/// `custom` and `unsupported` lists as the input; `mip_levels` is `options.levels`
/// (the full chain by default); the table is `packedLayout` of the result. Level 0
/// has the pixels of the input. Every layer is processed on its own.
///
/// The filter, exactly. Level k is computed from level k-1 of the result (not from
/// the base), so each level is quantized to the format before the next one is
/// derived. Per axis, a source extent n becomes `m = max(1, n / 2)` (the
/// `mipExtent` rule). Destination texel `i` covers the source interval
/// `[i*n/m, (i+1)*n/m)` where source texel `j` covers `[j, j+1)`; the weight of
/// source texel `j` is the length of the overlap. So for an even n every
/// destination texel is the plain mean of 2 texels; for n = 1 it is a copy; for
/// n = 3 -> 1 the weights are 1, 1, 1; for n = 5 -> 2 they are (1, 1, 0.5) and
/// (0.5, 1, 1), always summing to n/m. The weight of a source texel is the
/// product of its three axis weights, so a 3D image (depth above 1) is filtered
/// with a 2x2x2 box under the same rule (nothing is refused for depth). Sums are
/// accumulated in IEEE double precision in a fixed order, so the result is
/// deterministic on one platform; results on different platforms may differ only
/// where a value lies within floating-point error of a rounding boundary.
///
/// Values, by component kind (before rounding):
///
/// - Integer formats (r8..rgba16) with `linear` or `normal_map`: the raw integer
///   values are averaged. With `srgb` (8-bit only) the color components (all
///   components except component 3 of an rgba format) are decoded with the exact
///   sRGB transfer function (`c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ^
///   2.4`, c = raw / 255), averaged as linear values and encoded again
///   (`l <= 0.0031308 ? 12.92 * l : 1.055 * l ^ (1 / 2.4) - 0.055`); the alpha
///   component of an rgba format is always linear and averaged as stored.
/// - Float formats: the float values are averaged as they are (always linear);
///   NaN and infinity propagate; no clamping.
/// - `AlphaMode::straight` (rgba only): each color component is the mean weighted
///   by alpha, `sum(w * a * c) / sum(w * a)` (a is the stored alpha, c the linear
///   color value); alpha itself is the plain mean `sum(w * a) / sum(w)`. When
///   `sum(w * a)` is 0 the color is the plain mean. `premultiplied` and `none`:
///   every component is the plain mean `sum(w * x) / sum(w)`; for `none` the
///   alpha component of an rgba format is carried like any other component.
/// - `normal_map`: see `MipChainOptions::renormalize_normals`. A vector whose
///   length is below 1e-6 (in units of the encoded half range) is left unchanged.
///
/// Rounding: integer results are rounded to nearest, ties up
/// (`floor(x + 0.5)`), then clamped to 0..max of the format. Float results are
/// converted to float by the default conversion.
///
/// Limits: the size of the result is computed with `packedLayout` before anything
/// is allocated (`ImageDataError::size_overflow` or `std::bad_alloc` on
/// exhaustion); the temporary memory beyond the result is linear in the largest
/// extent (the weight tables of one axis), never the image size.
///
/// Errors: `ImageDataError` of `validate(image)` unchanged, `already_has_mips`,
/// `invalid_level_count`, `ImageDataError::size_overflow`. Allocation failure
/// throws `std::bad_alloc`.
[[nodiscard]] auto generateMipChain(const ImageData &image, const MipChainOptions &options = {})
    -> terreate::Result<ImageData>;

} // namespace terreate::asset

namespace std {
/// Lets MipChainError convert to std::error_code.
template <> struct is_error_code_enum<terreate::asset::MipChainError> : true_type {};
} // namespace std

#endif // TERREATE_ASSET_MIP_CHAIN_HPP

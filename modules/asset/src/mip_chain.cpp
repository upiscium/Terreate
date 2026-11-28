#include <terreate/asset/mip_chain.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace terreate::asset {
namespace {

class MipChainErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.asset.mip_chain"; }
  std::string message(int value) const override {
    switch (static_cast<MipChainError>(value)) {
    case MipChainError::already_has_mips:
      return "the image already has more than one mip level";
    case MipChainError::invalid_level_count:
      return "the requested level count exceeds the full mip chain";
    }
    return "unknown mip chain error";
  }
};

// Error construction allocates; on exhaustion a code-only Error is returned,
// which does not allocate.
Error makeError(MipChainError error, const char *operation, const std::string &detail) {
  try {
    return Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return Error{make_error_code(error)};
  }
}

// One source texel of an axis and the length of its overlap with the footprint
// of a destination texel, in units of 1/m of a source texel.
struct Tap {
  std::uint32_t index;
  double weight;
};

// The taps of every destination texel of one axis, flattened: destination i owns
// taps[begin[i], begin[i + 1]).
struct AxisTaps {
  std::vector<Tap> taps;
  std::vector<std::size_t> begin;
};

// Source extent n becomes m = max(1, n / 2). Destination i covers [i*n, (i+1)*n)
// and source j covers [j*m, (j+1)*m), in units of 1/m; the weight is the overlap.
AxisTaps axisTaps(std::uint32_t n, std::uint32_t m) {
  AxisTaps axis;
  axis.begin.reserve(static_cast<std::size_t>(m) + 1);
  for (std::uint32_t i = 0; i < m; ++i) {
    axis.begin.push_back(axis.taps.size());
    const std::uint64_t low = static_cast<std::uint64_t>(i) * n;
    const std::uint64_t high = low + n;
    const std::uint64_t first = low / m;
    const std::uint64_t last = std::min<std::uint64_t>((high + m - 1) / m, n);
    for (std::uint64_t j = first; j < last; ++j) {
      const std::uint64_t a = std::max(low, j * m);
      const std::uint64_t b = std::min(high, (j + 1) * m);
      if (b > a)
        axis.taps.push_back({static_cast<std::uint32_t>(j), static_cast<double>(b - a)});
    }
  }
  axis.begin.push_back(axis.taps.size());
  return axis;
}

double srgbToLinear(double c) noexcept {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double l) noexcept {
  return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

struct Decode {
  std::array<double, 256> srgb{};
  Decode() {
    for (std::size_t i = 0; i < srgb.size(); ++i)
      srgb[i] = srgbToLinear(static_cast<double>(i) / 255.0);
  }
};

// How one image is read and written during filtering.
struct Pixel {
  PixelFormat format;
  std::size_t components;
  std::size_t component_size;
  std::size_t texel;
  double max;         // integer maximum (255 or 65535); unused for floats
  bool srgb;          // color components are sRGB-encoded
  std::size_t colors; // number of color components (alpha excluded if rgba)
  bool straight;      // alpha weights the colors
  bool renormalize;   // renormalize the first three components
  const Decode *lut;
};

void load(const Pixel &p, const std::byte *at, std::array<double, 4> &v) noexcept {
  for (std::size_t c = 0; c < p.components; ++c) {
    const std::byte *src = at + c * p.component_size;
    double value = 0;
    if (p.component_size == 1) {
      const auto raw = static_cast<std::uint8_t>(*src);
      value = (p.srgb && c < p.colors) ? p.lut->srgb[raw] : static_cast<double>(raw);
    } else if (p.component_size == 2) {
      std::uint16_t raw = 0;
      std::memcpy(&raw, src, sizeof raw);
      value = static_cast<double>(raw);
    } else {
      float raw = 0;
      std::memcpy(&raw, src, sizeof raw);
      value = static_cast<double>(raw);
    }
    v[c] = value;
  }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::uint32_t roundClamp(double x, double max) noexcept {
  if (!(x > 0.0)) // also NaN
    return 0;
  const double rounded = std::floor(x + 0.5);
  return rounded >= max ? static_cast<std::uint32_t>(max) : static_cast<std::uint32_t>(rounded);
}

void store(const Pixel &p, std::byte *at, std::array<double, 4> &v) noexcept {
  if (p.renormalize) {
    const double half = p.max / 2.0;
    const double x = v[0] - half;
    const double y = v[1] - half;
    const double z = v[2] - half;
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length >= 1e-6) {
      const double scale = half / length;
      v[0] = x * scale + half;
      v[1] = y * scale + half;
      v[2] = z * scale + half;
    }
  }
  for (std::size_t c = 0; c < p.components; ++c) {
    std::byte *dst = at + c * p.component_size;
    if (p.component_size == 1) {
      const double value = (p.srgb && c < p.colors) ? linearToSrgb(v[c]) * 255.0 : v[c];
      *dst = static_cast<std::byte>(roundClamp(value, 255.0));
    } else if (p.component_size == 2) {
      const auto raw = static_cast<std::uint16_t>(roundClamp(v[c], 65535.0));
      std::memcpy(dst, &raw, sizeof raw);
    } else {
      const auto raw = static_cast<float>(v[c]);
      std::memcpy(dst, &raw, sizeof raw);
    }
  }
}

// Filters one tightly packed level (all slices) into the next.
void downsample(const Pixel &p, const std::byte *src, const ImageExtent &s, std::byte *dst,
                const ImageExtent &d) {
  const AxisTaps tx = axisTaps(s.width, d.width);
  const AxisTaps ty = axisTaps(s.height, d.height);
  const AxisTaps tz = axisTaps(s.depth, d.depth);
  const double total =
      static_cast<double>(s.width) * static_cast<double>(s.height) * static_cast<double>(s.depth);
  const std::size_t row = static_cast<std::size_t>(s.width) * p.texel;
  const std::size_t slice = row * s.height;
  std::size_t out = 0;
  std::array<double, 4> v{};
  for (std::uint32_t z = 0; z < d.depth; ++z) {
    for (std::uint32_t y = 0; y < d.height; ++y) {
      for (std::uint32_t x = 0; x < d.width; ++x) {
        std::array<double, 4> sum{};
        std::array<double, 4> weighted{};
        double alpha_weight = 0;
        for (std::size_t kz = tz.begin[z]; kz < tz.begin[z + 1]; ++kz) {
          for (std::size_t ky = ty.begin[y]; ky < ty.begin[y + 1]; ++ky) {
            const double wzy = tz.taps[kz].weight * ty.taps[ky].weight;
            for (std::size_t kx = tx.begin[x]; kx < tx.begin[x + 1]; ++kx) {
              const double w = wzy * tx.taps[kx].weight;
              const std::byte *at = src + static_cast<std::size_t>(tz.taps[kz].index) * slice +
                                    static_cast<std::size_t>(ty.taps[ky].index) * row +
                                    static_cast<std::size_t>(tx.taps[kx].index) * p.texel;
              load(p, at, v);
              for (std::size_t c = 0; c < p.components; ++c)
                sum[c] += w * v[c];
              if (p.straight) {
                const double wa = w * v[3];
                alpha_weight += wa;
                for (std::size_t c = 0; c < p.colors; ++c)
                  weighted[c] += wa * v[c];
              }
            }
          }
        }
        for (std::size_t c = 0; c < p.components; ++c)
          v[c] = sum[c] / total;
        if (p.straight && alpha_weight != 0.0)
          for (std::size_t c = 0; c < p.colors; ++c)
            v[c] = weighted[c] / alpha_weight;
        store(p, dst + out, v);
        out += p.texel;
      }
    }
  }
}

// Copies the pixels of one subresource of `image` into tightly packed `dst`.
void copyTight(const ImageData &image, const ImageSubresource &sub, std::byte *dst) noexcept {
  const ImageExtent e = image.extent;
  const std::size_t row = static_cast<std::size_t>(e.width) * byteSize(image.format);
  for (std::uint32_t z = 0; z < e.depth; ++z)
    for (std::uint32_t y = 0; y < e.height; ++y) {
      const std::byte *from =
          image.data.data() + sub.offset + z * sub.slice_pitch + y * sub.row_pitch;
      std::memcpy(dst, from, row);
      dst += row;
    }
}

} // namespace

auto mip_chain_error_category() noexcept -> const std::error_category & {
  static const MipChainErrorCategory category;
  return category;
}

auto make_error_code(MipChainError error) noexcept -> std::error_code {
  return {static_cast<int>(error), mip_chain_error_category()};
}

auto generateMipChain(const ImageData &image, const MipChainOptions &options)
    -> terreate::Result<ImageData> {
  constexpr const char *op = "asset::generateMipChain";
  if (auto valid = validate(image); !valid)
    return std::unexpected(std::move(valid.error()));
  if (image.mip_levels != 1)
    return std::unexpected(makeError(MipChainError::already_has_mips, op,
                                     std::to_string(image.mip_levels) + " mip levels"));
  const std::uint32_t full = maxMipLevels(image.extent);
  const std::uint32_t levels = options.levels == 0 ? full : options.levels;
  if (levels > full)
    return std::unexpected(
        makeError(MipChainError::invalid_level_count, op,
                  std::to_string(levels) + " levels, at most " + std::to_string(full)));
  auto layout = packedLayout(image.format, image.extent, levels, image.array_layers);
  if (!layout)
    return std::unexpected(std::move(layout.error()));

  ImageData result;
  result.format = image.format;
  result.interpretation = image.interpretation;
  result.alpha = image.alpha;
  result.extent = image.extent;
  result.array_layers = image.array_layers;
  result.mip_levels = levels;
  result.custom = image.custom;
  result.unsupported = image.unsupported;
  result.data.resize(layout->byte_size);
  result.subresources = std::move(layout->subresources);

  static const Decode decode;
  const std::size_t components = componentCount(image.format);
  Pixel pixel{};
  pixel.format = image.format;
  pixel.components = components;
  pixel.component_size = componentSize(image.format);
  pixel.texel = byteSize(image.format);
  pixel.max = pixel.component_size == 1 ? 255.0 : 65535.0;
  pixel.srgb = image.interpretation == ColorInterpretation::srgb;
  pixel.colors = hasAlpha(image.format) ? 3 : components;
  pixel.straight = image.alpha == AlphaMode::straight;
  pixel.renormalize = options.renormalize_normals &&
                      image.interpretation == ColorInterpretation::normal_map && components >= 3;
  pixel.lut = &decode;

  for (std::uint32_t layer = 0; layer < image.array_layers; ++layer) {
    const ImageSubresource *base = image.find(0, layer);
    const ImageSubresource &first = *result.find(0, layer);
    copyTight(image, *base, result.data.data() + first.offset);
    for (std::uint32_t mip = 1; mip < levels; ++mip) {
      const ImageSubresource &from = *result.find(mip - 1, layer);
      const ImageSubresource &to = *result.find(mip, layer);
      downsample(pixel, result.data.data() + from.offset, result.mipExtent(mip - 1),
                 result.data.data() + to.offset, result.mipExtent(mip));
    }
  }
  if (auto valid = validate(result); !valid)
    return std::unexpected(std::move(valid.error()));
  return result;
}

} // namespace terreate::asset

#ifndef TERREATE_GRAPHICS_SAMPLER_HPP
#define TERREATE_GRAPHICS_SAMPLER_HPP

#include <terreate/graphics/device.hpp>

namespace terreate::graphics {

/// Core sampler options only; no extension chains or implicit feature enabling.
struct SamplerDescription {
  vk::Filter mag_filter = vk::Filter::eNearest;
  vk::Filter min_filter = vk::Filter::eNearest;
  vk::SamplerMipmapMode mipmap_mode = vk::SamplerMipmapMode::eNearest;
  vk::SamplerAddressMode address_mode_u = vk::SamplerAddressMode::eClampToEdge;
  vk::SamplerAddressMode address_mode_v = vk::SamplerAddressMode::eClampToEdge;
  vk::SamplerAddressMode address_mode_w = vk::SamplerAddressMode::eClampToEdge;
  float mip_lod_bias = 0.0F;
  bool anisotropy_enable = false;
  float max_anisotropy = 1.0F;
  bool compare_enable = false;
  vk::CompareOp compare_op = vk::CompareOp::eNever;
  float min_lod = 0.0F;
  float max_lod = 0.0F;
  vk::BorderColor border_color = vk::BorderColor::eFloatTransparentBlack;
  bool unnormalized_coordinates = false;
};

struct SamplerConfiguration {
  SamplerDescription requested{};
  SamplerDescription effective{};
};

enum class SamplerError : std::uint8_t {
  invalid_parent = 1,
  invalid_description,
  unsupported_configuration,
};
[[nodiscard]] const std::error_category &sampler_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SamplerError error) noexcept;

/// Owns exactly one VkSampler; Device is borrowed and must outlive it.
/// Complete GPU use before destruction or move-assignment displacement. Native
/// handles are borrowed, never caller-destroyed. Host synchronization is caller-owned.
class Sampler final {
public:
  Sampler() = delete;
  Sampler(const Sampler &) = delete;
  Sampler &operator=(const Sampler &) = delete;
  Sampler(Sampler &&) noexcept;
  Sampler &operator=(Sampler &&) noexcept;
  ~Sampler();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::Sampler nativeHandle() const noexcept;
  [[nodiscard]] VkSampler nativeSampler() const noexcept;
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &) const noexcept;
  /// Null for moved-from values.
  [[nodiscard]] const SamplerConfiguration *configuration() const noexcept;

private:
  struct Impl;
  explicit Sampler(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createSampler(const Device &, const SamplerDescription &)
      -> terreate::Result<Sampler>;
};

[[nodiscard]] auto createSampler(const Device &, const SamplerDescription & = {})
    -> terreate::Result<Sampler>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::SamplerError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SAMPLER_HPP

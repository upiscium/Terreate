#include <terreate/graphics/sampler.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
class SamplerErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.sampler"; }
  std::string message(int value) const override {
    switch (static_cast<SamplerError>(value)) {
    case SamplerError::invalid_parent:
      return "invalid sampler device";
    case SamplerError::invalid_description:
      return "invalid sampler description";
    case SamplerError::unsupported_configuration:
      return "unsupported sampler configuration";
    }
    return "unknown sampler error";
  }
};
auto failure(SamplerError error, const char *detail) {
  return std::unexpected(terreate::Error{make_error_code(error), "createSampler", detail});
}
bool filter(vk::Filter value) {
  return value == vk::Filter::eNearest || value == vk::Filter::eLinear;
}
bool address(vk::SamplerAddressMode value) {
  return value == vk::SamplerAddressMode::eRepeat ||
         value == vk::SamplerAddressMode::eMirroredRepeat ||
         value == vk::SamplerAddressMode::eClampToEdge ||
         value == vk::SamplerAddressMode::eClampToBorder;
}
} // namespace

const std::error_category &sampler_error_category() noexcept {
  static const SamplerErrorCategory category;
  return category;
}
std::error_code make_error_code(SamplerError error) noexcept {
  return {static_cast<int>(error), sampler_error_category()};
}
struct Sampler::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  SamplerConfiguration configuration{};
  ~Impl() {
    if (sampler != VK_NULL_HANDLE)
      vkDestroySampler(device, sampler, nullptr);
  }
};
Sampler::Sampler(std::unique_ptr<Impl> impl) noexcept : implementation_(std::move(impl)) {}
Sampler::Sampler(Sampler &&) noexcept = default;
Sampler &Sampler::operator=(Sampler &&) noexcept = default;
Sampler::~Sampler() = default;
bool Sampler::valid() const noexcept { return nativeSampler() != VK_NULL_HANDLE; }
vk::Sampler Sampler::nativeHandle() const noexcept { return vk::Sampler{nativeSampler()}; }
VkSampler Sampler::nativeSampler() const noexcept {
  return implementation_ ? implementation_->sampler : VK_NULL_HANDLE;
}
vk::Device Sampler::nativeDevice() const noexcept {
  return vk::Device{implementation_ ? implementation_->device : VK_NULL_HANDLE};
}
bool Sampler::correlatedWith(const Device &device) const noexcept {
  return valid() && device.valid() && nativeDevice() == device.nativeHandle();
}
const SamplerConfiguration *Sampler::configuration() const noexcept {
  return implementation_ ? &implementation_->configuration : nullptr;
}
auto createSampler(const Device &device, const SamplerDescription &description)
    -> terreate::Result<Sampler> {
  if (!device.valid() || !device.plan())
    return failure(SamplerError::invalid_parent, "live borrowed Device required");
  const auto &d = description;
  if (!filter(d.mag_filter) || !filter(d.min_filter) ||
      (d.mipmap_mode != vk::SamplerMipmapMode::eNearest &&
       d.mipmap_mode != vk::SamplerMipmapMode::eLinear) ||
      !address(d.address_mode_u) || !address(d.address_mode_v) || !address(d.address_mode_w) ||
      static_cast<unsigned>(d.compare_op) > static_cast<unsigned>(vk::CompareOp::eAlways) ||
      static_cast<unsigned>(d.border_color) >
          static_cast<unsigned>(vk::BorderColor::eIntOpaqueWhite))
    return failure(SamplerError::unsupported_configuration,
                   "only core sampler enum values supported");
  if (!std::isfinite(d.mip_lod_bias) || !std::isfinite(d.max_anisotropy) ||
      !std::isfinite(d.min_lod) || !std::isfinite(d.max_lod) || d.min_lod > d.max_lod)
    return failure(SamplerError::invalid_description,
                   "finite values and ordered LOD range required");
  const auto &limits = device.plan()->capabilities().properties.limits;
  if (std::abs(d.mip_lod_bias) > limits.maxSamplerLodBias ||
      (d.anisotropy_enable &&
       (!device.plan()->enabledFeatures().samplerAnisotropy || d.max_anisotropy < 1.0F ||
        d.max_anisotropy > limits.maxSamplerAnisotropy)))
    return failure(SamplerError::unsupported_configuration,
                   "sampler limits or enabled feature unavailable");
  const auto unnormalized_address = [](vk::SamplerAddressMode mode) {
    return mode == vk::SamplerAddressMode::eClampToEdge ||
           mode == vk::SamplerAddressMode::eClampToBorder;
  };
  if (d.unnormalized_coordinates &&
      (d.min_filter != d.mag_filter || d.mipmap_mode != vk::SamplerMipmapMode::eNearest ||
       d.min_lod != 0.0F || d.max_lod != 0.0F || d.anisotropy_enable || d.compare_enable ||
       !unnormalized_address(d.address_mode_u) || !unnormalized_address(d.address_mode_v)))
    return failure(SamplerError::invalid_description, "incompatible unnormalized sampler options");
  auto impl = std::make_unique<Sampler::Impl>();
  impl->device = static_cast<VkDevice>(device.nativeHandle());
  impl->configuration = {description, description};
  VkSamplerCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = static_cast<VkFilter>(d.mag_filter);
  info.minFilter = static_cast<VkFilter>(d.min_filter);
  info.mipmapMode = static_cast<VkSamplerMipmapMode>(d.mipmap_mode);
  info.addressModeU = static_cast<VkSamplerAddressMode>(d.address_mode_u);
  info.addressModeV = static_cast<VkSamplerAddressMode>(d.address_mode_v);
  info.addressModeW = static_cast<VkSamplerAddressMode>(d.address_mode_w);
  info.mipLodBias = d.mip_lod_bias;
  info.anisotropyEnable = d.anisotropy_enable;
  info.maxAnisotropy = d.max_anisotropy;
  info.compareEnable = d.compare_enable;
  info.compareOp = static_cast<VkCompareOp>(d.compare_op);
  info.minLod = d.min_lod;
  info.maxLod = d.max_lod;
  info.borderColor = static_cast<VkBorderColor>(d.border_color);
  info.unnormalizedCoordinates = d.unnormalized_coordinates;
  VkSampler sampler = VK_NULL_HANDLE;
  const auto result = vkCreateSampler(impl->device, &info, nullptr, &sampler);
  if (result != VK_SUCCESS)
    return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                           "vkCreateSampler", "native sampler creation failed"});
  if (sampler == VK_NULL_HANDLE)
    return failure(SamplerError::invalid_parent, "native success returned a null sampler");
  impl->sampler = sampler;
  return Sampler{std::move(impl)};
}
} // namespace terreate::graphics

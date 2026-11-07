#include <terreate/graphics/shader_module.hpp>

#include <new>
#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
class ShaderModuleErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.shader_module"; }
  std::string message(int value) const override {
    switch (static_cast<ShaderModuleError>(value)) {
    case ShaderModuleError::invalid_device:
      return "invalid ShaderModule Device";
    case ShaderModuleError::invalid_binary:
      return "invalid SPIR-V binary";
    case ShaderModuleError::native_null_handle:
      return "Vulkan returned null ShaderModule on success";
    case ShaderModuleError::allocation_failed:
      return "ShaderModule wrapper allocation failed";
    }
    return "unknown ShaderModule error";
  }
};
auto fail(ShaderModuleError error, const char *detail) {
  return std::unexpected(terreate::Error{make_error_code(error), "createShaderModule", detail});
}
} // namespace

const std::error_category &shader_module_error_category() noexcept {
  static const ShaderModuleErrorCategory category;
  return category;
}
std::error_code make_error_code(ShaderModuleError error) noexcept {
  return {static_cast<int>(error), shader_module_error_category()};
}

struct ShaderModule::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkShaderModule handle = VK_NULL_HANDLE;
  ShaderModuleConfiguration config{};

  ~Impl() {
    if (handle != VK_NULL_HANDLE)
      vkDestroyShaderModule(device, handle, nullptr);
  }
};

ShaderModule::ShaderModule(std::unique_ptr<Impl> impl) noexcept
    : implementation_(std::move(impl)) {}
ShaderModule::ShaderModule(ShaderModule &&) noexcept = default;
ShaderModule &ShaderModule::operator=(ShaderModule &&) noexcept = default;
ShaderModule::~ShaderModule() = default;

bool ShaderModule::valid() const noexcept { return nativeShaderModule() != VK_NULL_HANDLE; }
vk::ShaderModule ShaderModule::nativeHandle() const noexcept {
  return vk::ShaderModule{nativeShaderModule()};
}
VkShaderModule ShaderModule::nativeShaderModule() const noexcept {
  return implementation_ ? implementation_->handle : VK_NULL_HANDLE;
}
vk::Device ShaderModule::nativeDevice() const noexcept {
  return vk::Device{implementation_ ? implementation_->device : VK_NULL_HANDLE};
}
bool ShaderModule::correlatedWith(const Device &device) const noexcept {
  return valid() && device.valid() && nativeDevice() == device.nativeHandle();
}
const ShaderModuleConfiguration *ShaderModule::configuration() const noexcept {
  return implementation_ ? &implementation_->config : nullptr;
}

auto createShaderModule(const Device &device, const SpirvBinary &spirv)
    -> terreate::Result<ShaderModule> {
  if (!device.valid() || !device.plan())
    return fail(ShaderModuleError::invalid_device, "live owning Device required");
  if (!spirv.valid())
    return fail(ShaderModuleError::invalid_binary, "live SPIR-V binary required");
  std::unique_ptr<ShaderModule::Impl> impl;
  try {
    impl = std::make_unique<ShaderModule::Impl>();
  } catch (const std::bad_alloc &) {
    return fail(ShaderModuleError::allocation_failed, "cannot allocate ShaderModule wrapper");
  }
  impl->device = static_cast<VkDevice>(device.nativeHandle());
  impl->config = {spirv.byteSize(), spirv.versionWord()};

  const VkShaderModuleCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .codeSize = spirv.byteSize(),
      .pCode = spirv.words().data(),
  };
  VkShaderModule handle = VK_NULL_HANDLE;
  const auto result = vkCreateShaderModule(impl->device, &info, nullptr, &handle);
  if (result != VK_SUCCESS)
    return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                           "vkCreateShaderModule",
                                           "native shader module creation failed"});
  if (handle == VK_NULL_HANDLE)
    return fail(ShaderModuleError::native_null_handle, "successful Vulkan call returned null");
  impl->handle = handle;
  return ShaderModule{std::move(impl)};
}
} // namespace terreate::graphics

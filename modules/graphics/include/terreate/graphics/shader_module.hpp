#ifndef TERREATE_GRAPHICS_SHADER_MODULE_HPP
#define TERREATE_GRAPHICS_SHADER_MODULE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

#include <terreate/graphics/device.hpp>
#include <terreate/graphics/spirv_binary.hpp>

namespace terreate::graphics {

enum class ShaderModuleError : std::uint8_t {
  invalid_device = 1,
  invalid_binary,
  native_null_handle,
  allocation_failed,
};

[[nodiscard]] const std::error_category &shader_module_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(ShaderModuleError error) noexcept;

/// Describes the bytes actually passed to vkCreateShaderModule.
/// No shader stage, entry point, descriptors or reflection are inferred.
struct ShaderModuleConfiguration {
  std::size_t code_size_bytes = 0;
  std::uint32_t spirv_version_word = 0;
};

/// Exclusive move-only Vulkan ShaderModule. Borrows its native Device, not the
/// Device wrapper's address; moving Device does not change the borrowed native
/// lifetime. Destroy or move-replace ShaderModule before Device destruction.
/// The application guarantees no in-flight GPU use when destroying it.
/// No hidden waits, submission, shader compilation or shared ownership.
/// vk::ShaderModule and VkShaderModule are non-owning escape hatches.
class ShaderModule final {
public:
  ShaderModule() = delete;
  ShaderModule(const ShaderModule &) = delete;
  ShaderModule &operator=(const ShaderModule &) = delete;
  ShaderModule(ShaderModule &&) noexcept;
  ShaderModule &operator=(ShaderModule &&) noexcept;
  ~ShaderModule();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::ShaderModule nativeHandle() const noexcept;
  [[nodiscard]] VkShaderModule nativeShaderModule() const noexcept;
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  /// Null on moved-from values; requested/effective shape is identical.
  [[nodiscard]] const ShaderModuleConfiguration *configuration() const noexcept;

private:
  struct Impl;
  explicit ShaderModule(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createShaderModule(const Device &, const SpirvBinary &)
      -> terreate::Result<ShaderModule>;
};

/// Validates parents and structural binary state, then creates one native
/// VkShaderModule with pCode pointing at the supplied CPU-owned words.
/// Does NOT retain a pointer to SpirvBinary and does NOT validate SPIR-V
/// instructions, entry points, capabilities or stage semantics itself.
/// Native VkResult failures are preserved in terreate::Error.
[[nodiscard]] auto createShaderModule(const Device &device, const SpirvBinary &spirv)
    -> terreate::Result<ShaderModule>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::ShaderModuleError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SHADER_MODULE_HPP

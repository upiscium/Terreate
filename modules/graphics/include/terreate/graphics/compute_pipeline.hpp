#ifndef TERREATE_GRAPHICS_COMPUTE_PIPELINE_HPP
#define TERREATE_GRAPHICS_COMPUTE_PIPELINE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

#include <terreate/graphics/pipeline_layout.hpp>
#include <terreate/graphics/shader_module.hpp>

namespace terreate::graphics {

/// Library input bound in bytes, excluding the terminator; not a Vulkan limit.
inline constexpr std::size_t max_compute_entry_point_bytes = 4096;

struct ComputePipelineDescription {
  /// Required caller intent. Empty is invalid; there is no default "main".
  std::string entry_point{};
};

struct ComputePipelineConfiguration {
  ComputePipelineDescription requested{};
  ComputePipelineDescription effective{};
  /// Fixed by this compute-only API, never inferred from SPIR-V.
  vk::ShaderStageFlagBits stage = vk::ShaderStageFlagBits::eCompute;
};

enum class ComputePipelineError : std::uint8_t {
  invalid_device = 1,
  invalid_shader_module,
  invalid_layout,
  parent_mismatch,
  invalid_entry_point,
  native_null_handle,
  allocation_failed,
};

[[nodiscard]] const std::error_category &compute_pipeline_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(ComputePipelineError error) noexcept;

/// Pure CPU validation: nonempty, bounded and without embedded NUL. Does not
/// inspect SPIR-V or prove that the named compute entry point exists.
[[nodiscard]] auto validateComputePipelineDescription(const ComputePipelineDescription &description)
    -> terreate::Result<void>;

/// Exclusive VkPipeline owner with borrowed Device and PipelineLayout handles.
/// Device must outlive it. Retain the layout through pipeline use (conservatively,
/// destroy this owner before its layout). ShaderModule is needed only through
/// creation and is not retained. No automatic submission, waits or shared ownership.
/// Complete GPU use and externally synchronize before destruction or replacement.
class ComputePipeline final {
public:
  ComputePipeline() = delete;
  ComputePipeline(const ComputePipeline &) = delete;
  ComputePipeline &operator=(const ComputePipeline &) = delete;
  ComputePipeline(ComputePipeline &&) noexcept;
  ComputePipeline &operator=(ComputePipeline &&) noexcept;
  ~ComputePipeline();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::Pipeline nativeHandle() const noexcept;
  [[nodiscard]] VkPipeline nativePipeline() const noexcept;
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  [[nodiscard]] VkDevice nativeVkDevice() const noexcept;
  [[nodiscard]] vk::PipelineLayout nativeLayout() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  [[nodiscard]] bool correlatedWith(const PipelineLayout &layout) const noexcept;
  /// Borrowed snapshot; null on moved-from values. Reacquire after a move.
  [[nodiscard]] const ComputePipelineConfiguration *configuration() const noexcept;

private:
  struct Impl;
  explicit ComputePipeline(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createComputePipeline(const Device &, const ShaderModule &, const PipelineLayout &,
                                    const ComputePipelineDescription &)
      -> terreate::Result<ComputePipeline>;
};

/// Creates exactly one compute pipeline with no cache, specialization or extension
/// chain. All three native objects must be live and belong to the supplied Device.
/// The explicit description is copied. SPIR-V version/API compatibility, shader
/// capabilities, entry-point semantics and layout compatibility remain caller
/// responsibilities. Native VkResult failures are preserved, including cleanup
/// of any pipeline returned by Vulkan on an unsuccessful multi-create call.
[[nodiscard]] auto createComputePipeline(const Device &device, const ShaderModule &shader_module,
                                         const PipelineLayout &layout,
                                         const ComputePipelineDescription &description)
    -> terreate::Result<ComputePipeline>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::ComputePipelineError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_COMPUTE_PIPELINE_HPP

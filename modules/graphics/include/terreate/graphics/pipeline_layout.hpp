#ifndef TERREATE_GRAPHICS_PIPELINE_LAYOUT_HPP
#define TERREATE_GRAPHICS_PIPELINE_LAYOUT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

#include <terreate/graphics/device.hpp>

namespace terreate::graphics {

/// Phase A supports the six core shader stages, each in at most one range.
/// Extension stages and eAll are deliberately not accepted.
inline constexpr std::size_t max_pipeline_layout_push_constant_ranges = 6;

/// Explicit descriptor-free layout: setLayoutCount is always zero. This does
/// not allocate descriptors or infer a layout from a shader. Range order and
/// masks are preserved; overlapping bytes are allowed only for disjoint stages.
struct PipelineLayoutDescription {
  std::vector<vk::PushConstantRange> push_constant_ranges{};
};

struct PipelineLayoutConfiguration {
  PipelineLayoutDescription requested{};
  PipelineLayoutDescription effective{};
  /// Capability evidence from the creating Device's retained DevicePlan.
  std::uint32_t max_push_constants_size = 0;
};

enum class PipelineLayoutError : std::uint8_t {
  invalid_device = 1,
  invalid_description,
  unsupported_configuration,
  native_null_handle,
  allocation_failed,
};

[[nodiscard]] const std::error_category &pipeline_layout_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(PipelineLayoutError error) noexcept;

/// Pure CPU validation against an explicit byte limit. No Device or native
/// calls; createPipelineLayout repeats this check using the actual DevicePlan.
[[nodiscard]] auto validatePipelineLayoutDescription(const PipelineLayoutDescription &description,
                                                     std::uint32_t max_push_constants_size)
    -> terreate::Result<void>;

/// Exclusive owner of one VkPipelineLayout. Device is borrowed by native handle
/// and must outlive this owner, including move-assignment displacement. Moving
/// Device does not change that native lifetime. All native observers are borrowed;
/// never destroy their handles. Host synchronization and completion of GPU use
/// are application-owned. No hidden waits, submission or shared ownership.
class PipelineLayout final {
public:
  PipelineLayout() = delete;
  PipelineLayout(const PipelineLayout &) = delete;
  PipelineLayout &operator=(const PipelineLayout &) = delete;
  PipelineLayout(PipelineLayout &&) noexcept;
  PipelineLayout &operator=(PipelineLayout &&) noexcept;
  ~PipelineLayout();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] vk::PipelineLayout nativeHandle() const noexcept;
  [[nodiscard]] VkPipelineLayout nativePipelineLayout() const noexcept;
  [[nodiscard]] vk::Device nativeDevice() const noexcept;
  [[nodiscard]] VkDevice nativeVkDevice() const noexcept;
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  /// Borrowed snapshot; null on moved-from values. Reacquire after a move.
  [[nodiscard]] const PipelineLayoutConfiguration *configuration() const noexcept;

private:
  struct Impl;
  explicit PipelineLayout(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_;
  friend auto createPipelineLayout(const Device &, const PipelineLayoutDescription &)
      -> terreate::Result<PipelineLayout>;
};

/// Creates a descriptor-free layout with precisely the requested ranges.
/// Description is copied, not retained by reference. Native VkResult failures
/// are preserved; success with a null handle is an error.
[[nodiscard]] auto createPipelineLayout(const Device &device,
                                        const PipelineLayoutDescription &description)
    -> terreate::Result<PipelineLayout>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::PipelineLayoutError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_PIPELINE_LAYOUT_HPP

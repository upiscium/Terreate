#ifndef TERREATE_GRAPHICS_DIAGNOSTICS_HPP
#define TERREATE_GRAPHICS_DIAGNOSTICS_HPP

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <terreate/core/diagnostics.hpp>

namespace terreate::graphics::detail {

// These values intentionally mirror the Vulkan Debug Utils bit values without
// including a Vulkan header. #268 can populate this neutral structure directly
// from its callback and keep Vulkan confined to its own implementation.
enum class NativeDiagnosticSeverity : std::uint16_t {
  verbose = 0x00000001u,
  info = 0x00000010u,
  warning = 0x00000100u,
  error = 0x00001000u,
};

// These values mirror VkDebugUtilsMessageTypeFlagBitsEXT without including a
// Vulkan header.  Category identity, spelling, and ordering remain private to
// this bridge rather than becoming part of the Core diagnostics vocabulary.
enum class NativeDiagnosticCategory : std::uint8_t {
  general = 0x00000001u,
  validation = 0x00000002u,
  performance = 0x00000004u,
};

inline constexpr std::uint32_t native_verbose_severity =
    static_cast<std::uint32_t>(NativeDiagnosticSeverity::verbose);
inline constexpr std::uint32_t native_info_severity =
    static_cast<std::uint32_t>(NativeDiagnosticSeverity::info);
inline constexpr std::uint32_t native_warning_severity =
    static_cast<std::uint32_t>(NativeDiagnosticSeverity::warning);
inline constexpr std::uint32_t native_error_severity =
    static_cast<std::uint32_t>(NativeDiagnosticSeverity::error);

inline constexpr std::uint32_t native_general_category =
    static_cast<std::uint32_t>(NativeDiagnosticCategory::general);
inline constexpr std::uint32_t native_validation_category =
    static_cast<std::uint32_t>(NativeDiagnosticCategory::validation);
inline constexpr std::uint32_t native_performance_category =
    static_cast<std::uint32_t>(NativeDiagnosticCategory::performance);

// The native category order is also the order used when translating the
// private callback payload into Core's producer-provided category strings.
inline constexpr std::array<NativeDiagnosticCategory, 3> native_category_order{
    NativeDiagnosticCategory::general,
    NativeDiagnosticCategory::validation,
    NativeDiagnosticCategory::performance,
};

/// A borrowed object view used only while translating a native callback.
/// `type` is already a stable backend-neutral spelling; the owning Core event
/// copies it before the callback returns.
struct NativeDiagnosticObject {
  std::string_view type{};
  std::uint64_t handle = 0;
  std::string_view name{};
};

/// A borrowed label view used only while translating a native callback.
struct NativeDiagnosticLabel {
  std::string_view name{};
  std::array<float, 4> color{};
};

/// Vulkan-neutral data copied from VkDebugUtilsMessengerCallbackDataEXT's
/// observable fields. All views are borrowed for the duration of translation;
/// no view is placed in the emitted Core event.
struct NativeDiagnosticCallbackData {
  std::uint32_t severity = native_info_severity;
  std::uint32_t category_bits = native_general_category;
  std::string_view source = "vulkan";
  std::string_view operation{};
  std::string_view message_id_name{};
  std::int64_t message_id_number = 0;
  std::string_view message{};
  std::string_view context{};
  std::span<const NativeDiagnosticLabel> queue_labels{};
  std::span<const NativeDiagnosticLabel> command_buffer_labels{};
  std::span<const NativeDiagnosticObject> objects{};
};

enum class DiagnosticTranslationError : std::uint8_t {
  unknown_severity,
  unknown_category,
};

using TranslationError = DiagnosticTranslationError;
using TranslationResult = std::expected<terreate::DiagnosticEvent, DiagnosticTranslationError>;

/// Translate one native callback payload into an owning Core event.
///
/// The input views are consumed synchronously and every field is copied into
/// the returned event.  Translation can allocate while constructing the event.
/// Unknown severity/category bits are rejected instead of being represented as
/// a less severe Core event.
[[nodiscard]] TranslationResult translate_diagnostic(const NativeDiagnosticCallbackData &native);

/// Translate and synchronously forward one callback payload to a borrowed sink.
/// Rejected payloads are returned without invoking the sink.  The sink sees an
/// owning event and therefore may retain a copy after this call returns.
[[nodiscard]] TranslationResult translate_and_emit(const NativeDiagnosticCallbackData &native,
                                                   const terreate::DiagnosticSinkView &sink);

[[nodiscard]] inline TranslationResult translate(const NativeDiagnosticCallbackData &native) {
  return translate_diagnostic(native);
}

} // namespace terreate::graphics::detail

#endif // TERREATE_GRAPHICS_DIAGNOSTICS_HPP

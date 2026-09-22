#ifndef TERREATE_GRAPHICS_DIAGNOSTICS_HPP
#define TERREATE_GRAPHICS_DIAGNOSTICS_HPP

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <terreate/core/diagnostics.hpp>

namespace terreate::graphics::detail {

// These are semantic values for the private bridge, not Vulkan ABI values.
// The #268 adapter maps Vulkan callback values to these values before entering
// this translation boundary. The `unknown` and `invalid` semantic sentinels
// remain representable as test inputs and are rejected by the translator.
enum class NativeDiagnosticSeverity : std::uint8_t {
  verbose,
  info,
  warning,
  error,
  unknown = 0xfe,
  invalid = 0xff,
};

// These values are semantic category identities for the private bridge. They
// are deliberately independent of Vulkan callback numeric values.
enum class NativeDiagnosticCategory : std::uint8_t {
  general,
  validation,
  performance,
  unknown,
};

// The native category order is also the order used when translating the
// private callback payload into Core's producer-provided category strings.
inline constexpr std::array<NativeDiagnosticCategory, 3> native_category_order{
    NativeDiagnosticCategory::general,
    NativeDiagnosticCategory::validation,
    NativeDiagnosticCategory::performance,
};

inline constexpr std::array<NativeDiagnosticCategory, 1> default_native_categories{
    NativeDiagnosticCategory::general,
};

/// A borrowed object view used only while translating a native callback.
/// `type` is already a stable backend-neutral spelling; the owning Core event
/// copies it before the callback returns.
struct NativeDiagnosticObject {
  std::string_view type{};
  std::uint64_t object_handle = 0;
  std::string_view name{};
};

/// Vulkan-neutral data copied from VkDebugUtilsMessengerCallbackDataEXT's
/// observable fields. All views are borrowed for the duration of translation;
/// no view is placed in the emitted Core event.
struct NativeDiagnosticCallbackData {
  NativeDiagnosticSeverity severity = NativeDiagnosticSeverity::info;
  std::span<const NativeDiagnosticCategory> categories = default_native_categories;
  std::string_view source = "vulkan";
  std::string_view operation{};
  std::string_view message_id_name{};
  std::int64_t message_id_number = 0;
  std::string_view message{};
  std::string_view context{};
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
/// Unknown severity/category values are rejected instead of being represented
/// as a less severe Core event.
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

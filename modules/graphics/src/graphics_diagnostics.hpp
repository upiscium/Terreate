#ifndef TERREATE_GRAPHICS_DIAGNOSTICS_HPP
#define TERREATE_GRAPHICS_DIAGNOSTICS_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  device_address_binding,
  unknown,
};

// The native category order is also the order used when translating the
// private callback payload into Core's producer-provided category strings.
inline constexpr std::array<NativeDiagnosticCategory, 4> native_category_order{
    NativeDiagnosticCategory::general,
    NativeDiagnosticCategory::validation,
    NativeDiagnosticCategory::performance,
    NativeDiagnosticCategory::device_address_binding,
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
  std::span<const NativeDiagnosticCategory> categories{};
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

namespace diagnostic_detail {

[[nodiscard]] inline std::string copy_text(std::string_view text) {
  return text.empty() ? std::string{} : std::string{text};
}

[[nodiscard]] inline std::optional<terreate::DiagnosticSeverity>
translate_severity(NativeDiagnosticSeverity severity) {
  switch (severity) {
  case NativeDiagnosticSeverity::verbose:
    return terreate::DiagnosticSeverity::verbose;
  case NativeDiagnosticSeverity::info:
    return terreate::DiagnosticSeverity::info;
  case NativeDiagnosticSeverity::warning:
    return terreate::DiagnosticSeverity::warning;
  case NativeDiagnosticSeverity::error:
    return terreate::DiagnosticSeverity::error;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] constexpr std::string_view
native_category_name(NativeDiagnosticCategory category) noexcept {
  switch (category) {
  case NativeDiagnosticCategory::general:
    return "GENERAL";
  case NativeDiagnosticCategory::validation:
    return "VALIDATION";
  case NativeDiagnosticCategory::performance:
    return "PERFORMANCE";
  case NativeDiagnosticCategory::device_address_binding:
    return "DEVICE_ADDRESS_BINDING";
  case NativeDiagnosticCategory::unknown:
    return {};
  }
  return {};
}

[[nodiscard]] inline std::expected<std::vector<std::string>, DiagnosticTranslationError>
translate_categories(std::span<const NativeDiagnosticCategory> categories) {
  for (const auto category : categories) {
    switch (category) {
    case NativeDiagnosticCategory::general:
    case NativeDiagnosticCategory::validation:
    case NativeDiagnosticCategory::performance:
    case NativeDiagnosticCategory::device_address_binding:
      break;
    case NativeDiagnosticCategory::unknown:
    default:
      return std::unexpected(DiagnosticTranslationError::unknown_category);
    }
  }

  std::vector<std::string> translated;
  translated.reserve(native_category_order.size());
  for (const auto category : native_category_order) {
    if (std::find(categories.begin(), categories.end(), category) != categories.end()) {
      translated.emplace_back(native_category_name(category));
    }
  }
  return translated;
}

[[nodiscard]] inline std::string object_id(std::uint64_t object_handle) {
  constexpr std::string_view hexadecimal = "0123456789abcdef";
  std::string id{"0x0000000000000000"};
  for (std::size_t digit = 0; digit < 16; ++digit) {
    const auto shift = static_cast<unsigned>((15 - digit) * 4);
    id[2 + digit] = hexadecimal[(object_handle >> shift) & 0x0fu];
  }
  return id;
}

[[nodiscard]] inline std::vector<terreate::DiagnosticObject>
copy_objects(std::span<const NativeDiagnosticObject> objects) {
  std::vector<terreate::DiagnosticObject> copied;
  copied.reserve(objects.size());
  for (const auto &object : objects) {
    copied.push_back(terreate::DiagnosticObject{
        .id = object_id(object.object_handle),
        .type = copy_text(object.type),
        .name = copy_text(object.name),
    });
  }
  return copied;
}

[[nodiscard]] inline terreate::DiagnosticCode
copy_code(const NativeDiagnosticCallbackData &native) {
  return terreate::DiagnosticCode{
      .name = copy_text(native.message_id_name),
      .value = native.message_id_number,
  };
}

} // namespace diagnostic_detail

/// Translate one native callback payload into an owning Core event.
///
/// The input views are consumed synchronously and every field is copied into
/// the returned event.  Translation can allocate while constructing the event.
/// Unknown severity/category values are rejected instead of being represented
/// as a less severe Core event.
[[nodiscard]] inline TranslationResult
translate_diagnostic(const NativeDiagnosticCallbackData &native) {
  const auto severity = diagnostic_detail::translate_severity(native.severity);
  if (!severity) {
    return std::unexpected(DiagnosticTranslationError::unknown_severity);
  }
  const auto categories = diagnostic_detail::translate_categories(native.categories);
  if (!categories) {
    return std::unexpected(categories.error());
  }

  terreate::DiagnosticEvent event{
      .severity = *severity,
      .categories = *categories,
      .source = diagnostic_detail::copy_text(native.source),
      .operation = diagnostic_detail::copy_text(native.operation),
      .code = diagnostic_detail::copy_code(native),
      .message = diagnostic_detail::copy_text(native.message),
      .context = diagnostic_detail::copy_text(native.context),
      .objects = diagnostic_detail::copy_objects(native.objects),
  };
  return event;
}

/// Translate and synchronously forward one callback payload to a borrowed sink.
/// Rejected payloads are returned without invoking the sink.  The sink sees an
/// owning event and therefore may retain a copy after this call returns.
[[nodiscard]] inline TranslationResult
translate_and_emit(const NativeDiagnosticCallbackData &native,
                   const terreate::DiagnosticSinkView &sink) {
  auto translated = translate_diagnostic(native);
  if (translated) {
    sink.emit(*translated);
  }
  return translated;
}

[[nodiscard]] inline TranslationResult translate(const NativeDiagnosticCallbackData &native) {
  return translate_diagnostic(native);
}

} // namespace terreate::graphics::detail

#endif // TERREATE_GRAPHICS_DIAGNOSTICS_HPP

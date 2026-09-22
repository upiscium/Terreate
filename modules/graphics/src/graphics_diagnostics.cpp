#include "graphics_diagnostics.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace terreate::graphics::detail {

namespace {

[[nodiscard]] std::string copy_text(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  return std::string{text};
}

[[nodiscard]] std::optional<terreate::DiagnosticSeverity>
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
  case NativeDiagnosticCategory::unknown:
    return {};
  }
  return {};
}

[[nodiscard]] std::expected<std::vector<std::string>, DiagnosticTranslationError>
translate_categories(std::span<const NativeDiagnosticCategory> categories) {
  for (const auto category : categories) {
    switch (category) {
    case NativeDiagnosticCategory::general:
    case NativeDiagnosticCategory::validation:
    case NativeDiagnosticCategory::performance:
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

[[nodiscard]] std::string object_id(std::uint64_t object_handle) {
  constexpr std::string_view hexadecimal = "0123456789abcdef";
  std::string id{"0x0000000000000000"};
  for (std::size_t digit = 0; digit < 16; ++digit) {
    const auto shift = static_cast<unsigned>((15 - digit) * 4);
    id[2 + digit] = hexadecimal[(object_handle >> shift) & 0x0fu];
  }
  return id;
}

[[nodiscard]] std::vector<terreate::DiagnosticObject>
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

[[nodiscard]] terreate::DiagnosticCode copy_code(const NativeDiagnosticCallbackData &native) {
  // The Debug Utils callback always supplies a message ID tuple.  Preserve it
  // even when its name is empty and its numeric value is zero.
  return terreate::DiagnosticCode{
      .name = copy_text(native.message_id_name),
      .value = native.message_id_number,
  };
}

} // namespace

TranslationResult translate_diagnostic(const NativeDiagnosticCallbackData &native) {
  const auto severity = translate_severity(native.severity);
  if (!severity) {
    return std::unexpected(DiagnosticTranslationError::unknown_severity);
  }
  const auto categories = translate_categories(native.categories);
  if (!categories) {
    return std::unexpected(categories.error());
  }

  terreate::DiagnosticEvent event{
      .severity = *severity,
      .categories = *categories,
      .source = copy_text(native.source),
      .operation = copy_text(native.operation),
      .code = copy_code(native),
      .message = copy_text(native.message),
      .context = copy_text(native.context),
      .objects = copy_objects(native.objects),
  };
  return event;
}

TranslationResult translate_and_emit(const NativeDiagnosticCallbackData &native,
                                     const terreate::DiagnosticSinkView &sink) {
  auto translated = translate_diagnostic(native);
  if (translated) {
    sink.emit(*translated);
  }
  return translated;
}

} // namespace terreate::graphics::detail

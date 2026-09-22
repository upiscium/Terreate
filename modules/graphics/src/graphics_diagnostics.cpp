#include "graphics_diagnostics.hpp"

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
translate_severity(std::uint32_t severity) {
  switch (severity) {
  case native_verbose_severity:
    return terreate::DiagnosticSeverity::verbose;
  case native_info_severity:
    return terreate::DiagnosticSeverity::info;
  case native_warning_severity:
    return terreate::DiagnosticSeverity::warning;
  case native_error_severity:
    return terreate::DiagnosticSeverity::error;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] constexpr bool known_categories(std::uint32_t categories) noexcept {
  constexpr auto known =
      native_general_category | native_validation_category | native_performance_category;
  return (categories & ~known) == 0;
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
  }
  return {};
}

[[nodiscard]] std::vector<std::string> translate_categories(std::uint32_t categories) {
  std::vector<std::string> translated;
  translated.reserve(native_category_order.size());

  for (const auto category : native_category_order) {
    if ((categories & static_cast<std::uint32_t>(category)) != 0) {
      translated.emplace_back(native_category_name(category));
    }
  }
  return translated;
}

[[nodiscard]] std::vector<terreate::DiagnosticObject>
copy_objects(std::span<const NativeDiagnosticObject> objects) {
  std::vector<terreate::DiagnosticObject> copied;
  copied.reserve(objects.size());
  for (const auto &object : objects) {
    copied.push_back(terreate::DiagnosticObject{
        .type = copy_text(object.type),
        .handle = object.handle,
        .name = copy_text(object.name),
    });
  }
  return copied;
}

[[nodiscard]] std::vector<terreate::DiagnosticLabel>
copy_labels(std::span<const NativeDiagnosticLabel> labels) {
  std::vector<terreate::DiagnosticLabel> copied;
  copied.reserve(labels.size());
  for (const auto &label : labels) {
    copied.push_back(terreate::DiagnosticLabel{
        .name = copy_text(label.name),
        .color = label.color,
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
  if (!known_categories(native.category_bits)) {
    return std::unexpected(DiagnosticTranslationError::unknown_category);
  }

  terreate::DiagnosticEvent event{
      .severity = *severity,
      .categories = translate_categories(native.category_bits),
      .source = copy_text(native.source),
      .operation = copy_text(native.operation),
      .code = copy_code(native),
      .message = copy_text(native.message),
      .context = copy_text(native.context),
      .objects = copy_objects(native.objects),
      .queue_labels = copy_labels(native.queue_labels),
      .command_buffer_labels = copy_labels(native.command_buffer_labels),
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

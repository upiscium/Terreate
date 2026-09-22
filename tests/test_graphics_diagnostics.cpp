#include "graphics_diagnostics.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using terreate::DiagnosticSeverity;
namespace graphics_detail = terreate::graphics::detail;
using NativeCallbackData = graphics_detail::NativeDiagnosticCallbackData;
using NativeCategory = graphics_detail::NativeDiagnosticCategory;
using NativeObject = graphics_detail::NativeDiagnosticObject;
using NativeSeverity = graphics_detail::NativeDiagnosticSeverity;

[[nodiscard]] bool check(bool condition, const char *description) {
  if (!condition) {
    std::fputs(description, stderr);
    std::fputc('\n', stderr);
  }
  return condition;
}

struct Recorder {
  int calls = 0;
  std::thread::id caller{};
  terreate::DiagnosticEvent event{};

  void operator()(const terreate::DiagnosticEvent &observed) noexcept {
    ++calls;
    caller = std::this_thread::get_id();
    event = observed;
  }
};

[[nodiscard]] bool check_severity(NativeSeverity native_severity, DiagnosticSeverity expected) {
  const NativeCallbackData native{
      .severity = native_severity,
      .message_id_name = "VUID-test-00001",
      .message_id_number = 19,
      .message = "native message",
  };
  const auto result = graphics_detail::translate_diagnostic(native);
  return result.has_value() && result->severity == expected;
}

[[nodiscard]] bool test_known_translation_copies_payload() {
  const std::array<NativeObject, 4> objects{
      NativeObject{"VkInstance", 0x10, "instance"},
      NativeObject{"VkQueue", 0x20, "queue"},
      NativeObject{"VkDevice", 0, {}},
      NativeObject{"VkBuffer", 0xABCDEF0123456789ULL, "buffer"},
  };
  std::array<NativeCategory, 5> categories{};
  categories[0] = NativeCategory::performance;
  categories[1] = NativeCategory::general;
  categories[2] = NativeCategory::performance;
  categories[3] = NativeCategory::validation;
  categories[4] = NativeCategory::general;
  const NativeCallbackData native{
      .severity = NativeSeverity::error,
      .categories = categories,
      .source = "vulkan",
      .operation = "create-instance",
      .message_id_name = "VUID-vkCreateInstance-test",
      .message_id_number = -7,
      .message = "validation details",
      .context = "explicit context",
      .objects = objects,
  };

  const auto result = graphics_detail::translate_diagnostic(native);
  bool passed = true;
  passed &= check(result.has_value(), "known native diagnostic was rejected");
  if (!result) {
    return false;
  }

  const auto &event = *result;
  const std::vector<std::string> expected_categories{"GENERAL", "VALIDATION", "PERFORMANCE"};
  const bool severity_preserved = event.severity == DiagnosticSeverity::error;
  passed &= check(severity_preserved, "native error severity was not preserved");
  passed &= check(event.categories == expected_categories,
                  "multiple native categories were not translated in canonical order");
  passed &= check(event.source == "vulkan", "native source was not copied");
  passed &= check(event.operation == "create-instance", "native operation was not copied");
  const bool code_preserved = event.code.has_value() && event.code->value == -7 &&
                              event.code->name == "VUID-vkCreateInstance-test";
  passed &= check(code_preserved, "native message code was not copied");
  passed &= check(event.message == "validation details", "native message was not copied");
  passed &= check(event.context == "explicit context", "native context was not copied");
  passed &= check(event.objects.size() == 4, "native object count was not copied");
  const auto &first_object = event.objects[0];
  const bool first_object_preserved = first_object.id == "0x0000000000000010" &&
                                      first_object.type == "VkInstance" &&
                                      first_object.name == "instance";
  passed &= check(first_object_preserved, "first object was not copied as an owned value");
  const auto &second_object = event.objects[1];
  const bool second_object_preserved = second_object.id == "0x0000000000000020" &&
                                       second_object.type == "VkQueue" &&
                                       second_object.name == "queue";
  passed &= check(second_object_preserved, "second object was not copied as an owned value");
  const auto &unnamed_object = event.objects[2];
  const bool unnamed_object_preserved = unnamed_object.id == "0x0000000000000000" &&
                                        unnamed_object.type == "VkDevice" &&
                                        unnamed_object.name.empty();
  passed &= check(unnamed_object_preserved, "unnamed object was not copied as an owned value");
  const auto &mixed_case_handle_object = event.objects[3];
  const bool mixed_case_id = mixed_case_handle_object.id == "0xabcdef0123456789";
  const bool mixed_case_type = mixed_case_handle_object.type == "VkBuffer";
  const bool mixed_case_name = mixed_case_handle_object.name == "buffer";
  const bool mixed_case_handle_preserved = mixed_case_id && mixed_case_type && mixed_case_name;
  passed &= check(mixed_case_handle_preserved,
                  "native object handle was not formatted as lowercase fixed-width hex");

  Recorder recorder;
  const auto sink = terreate::DiagnosticSinkView::bind(recorder);
  const auto forwarded = graphics_detail::translate_and_emit(native, sink);
  const bool forwarded_synchronously = forwarded.has_value() && recorder.calls == 1;
  passed &= check(forwarded_synchronously, "translated diagnostic was not synchronously forwarded");
  passed &= check(recorder.caller == std::this_thread::get_id(),
                  "translated diagnostic was not emitted on the caller thread");
  const bool forwarded_payload =
      recorder.event.objects.size() == 4 && recorder.event.message == event.message;
  passed &= check(forwarded_payload, "forwarded diagnostic did not retain its owning payload");
  return passed;
}

[[nodiscard]] bool test_single_category_translation() {
  const std::array<NativeCategory, 1> categories{NativeCategory::validation};
  const NativeCallbackData native{
      .categories = categories,
      .message = "single category",
  };

  const auto result = graphics_detail::translate_diagnostic(native);
  const std::vector<std::string> expected_categories{"VALIDATION"};
  const bool translated = result.has_value() && result->categories == expected_categories;
  return check(translated, "single native category was not translated");
}

[[nodiscard]] bool test_input_lifetime_and_no_guessed_context() {
  terreate::DiagnosticEvent retained{};
  {
    const std::string source = "temporary-source";
    const std::string operation = "temporary-operation";
    const std::string code_name = "temporary-code";
    const std::string message = "temporary-message";
    const std::string object_type = "VkBuffer";
    const std::string object_name = "temporary-buffer";
    const NativeObject object{object_type, 0x99, object_name};
    const std::array<NativeCategory, 1> categories{NativeCategory::general};
    const NativeCallbackData native{
        .severity = NativeSeverity::info,
        .categories = categories,
        .source = source,
        .operation = operation,
        .message_id_name = code_name,
        .message_id_number = 0,
        .message = message,
        .context = {},
        .objects = std::span<const NativeObject>{&object, 1},
    };

    const auto result = graphics_detail::translate_diagnostic(native);
    if (!result) {
      return check(false, "temporary native diagnostic was rejected");
    }
    retained = *result;
  }

  bool passed = true;
  const bool strings_retained = retained.source == "temporary-source" &&
                                retained.operation == "temporary-operation" &&
                                retained.message == "temporary-message";
  passed &= check(strings_retained, "translated strings did not outlive native callback storage");
  const std::vector<std::string> expected_categories{"GENERAL"};
  passed &= check(retained.categories == expected_categories,
                  "translated categories did not outlive native callback storage");
  const bool zero_code_retained = retained.code.has_value() &&
                                  retained.code->name == "temporary-code" &&
                                  retained.code->value == 0;
  passed &= check(zero_code_retained, "zero-valued diagnostic code was not retained");
  passed &= check(retained.context.empty(), "translator guessed a diagnostic context");
  const bool object_retained = retained.objects.size() == 1 &&
                               retained.objects.front().id == "0x0000000000000099" &&
                               retained.objects.front().type == "VkBuffer" &&
                               retained.objects.front().name == "temporary-buffer";
  passed &= check(object_retained, "translated object did not outlive native callback storage");
  return passed;
}

[[nodiscard]] bool test_empty_native_code_is_preserved() {
  const NativeCallbackData native{
      .severity = NativeSeverity::info,
      .message_id_name = {},
      .message_id_number = 0,
      .message = "message with an empty code name",
  };
  const auto result = graphics_detail::translate_diagnostic(native);
  const bool code_preserved = result.has_value() && result->code.has_value() &&
                              result->code->name.empty() && result->code->value == 0;
  return check(code_preserved, "empty native message code tuple was not preserved");
}

[[nodiscard]] bool test_all_known_severities() {
  bool passed = true;
  passed &= check(check_severity(NativeSeverity::verbose, DiagnosticSeverity::verbose),
                  "verbose severity was not mapped");
  passed &= check(check_severity(NativeSeverity::info, DiagnosticSeverity::info),
                  "info severity was not mapped");
  passed &= check(check_severity(NativeSeverity::warning, DiagnosticSeverity::warning),
                  "warning severity was not mapped");
  passed &= check(check_severity(NativeSeverity::error, DiagnosticSeverity::error),
                  "error severity was not mapped");
  return passed;
}

[[nodiscard]] bool test_unknown_values_are_rejected() {
  // These are deliberately unsupported semantic sentinels, not Vulkan ABI
  // values. The private bridge must keep rejecting them explicitly.
  const NativeCallbackData unknown_severity{
      .severity = NativeSeverity::unknown,
  };
  const NativeCallbackData invalid_severity{
      .severity = NativeSeverity::invalid,
  };
  const std::array<NativeCategory, 2> unknown_categories{
      NativeCategory::general,
      NativeCategory::unknown,
  };
  const NativeCallbackData unknown_category{
      .severity = NativeSeverity::info,
      .categories = unknown_categories,
  };

  const auto first = graphics_detail::translate_diagnostic(unknown_severity);
  const auto second = graphics_detail::translate_diagnostic(invalid_severity);
  const auto third = graphics_detail::translate_diagnostic(unknown_category);
  bool passed = true;
  const bool unknown_severity_rejected =
      !first && first.error() == graphics_detail::DiagnosticTranslationError::unknown_severity;
  passed &= check(unknown_severity_rejected, "unknown severity was not explicitly rejected");
  const bool invalid_severity_rejected =
      !second && second.error() == graphics_detail::DiagnosticTranslationError::unknown_severity;
  passed &= check(invalid_severity_rejected, "invalid severity was not explicitly rejected");
  const bool unknown_category_rejected =
      !third && third.error() == graphics_detail::DiagnosticTranslationError::unknown_category;
  passed &= check(unknown_category_rejected, "unknown category was not explicitly rejected");
  return passed;
}

} // namespace

int main() {
  bool passed = true;
  passed &= test_known_translation_copies_payload();
  passed &= test_single_category_translation();
  passed &= test_input_lifetime_and_no_guessed_context();
  passed &= test_empty_native_code_is_preserved();
  passed &= test_all_known_severities();
  passed &= test_unknown_values_are_rejected();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

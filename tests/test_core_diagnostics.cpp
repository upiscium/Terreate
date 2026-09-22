#include <terreate/core/diagnostics.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] bool check(bool condition, const char *description) {
  if (!condition) {
    std::fputs(description, stderr);
    std::fputc('\n', stderr);
  }
  return condition;
}

struct ThrowingRecorder {
  void operator()(const terreate::DiagnosticEvent &) noexcept(false) {}
};

void free_function_recorder(const terreate::DiagnosticEvent &) noexcept {}

template <typename Target>
concept SinkBindable =
    std::is_nothrow_invocable_r_v<void, Target &, const terreate::DiagnosticEvent &> &&
    requires(Target &target) { terreate::DiagnosticSinkView::bind(target); };

struct Recorder {
  int calls = 0;
  std::thread::id caller{};
  std::vector<terreate::DiagnosticEvent> retained{};

  Recorder() { retained.reserve(4); }

  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    ++calls;
    caller = std::this_thread::get_id();
    retained.push_back(event);
  }
};

struct AddressOverloadedRecorder {
  int calls = 0;

  void operator()(const terreate::DiagnosticEvent &) noexcept { ++calls; }

  AddressOverloadedRecorder *operator&() noexcept { return nullptr; }
};

struct ReentrantRecorder {
  terreate::DiagnosticSinkView sink{};
  int calls = 0;
  int depth = 0;
  int maximum_depth = 0;
  int limit = 0;

  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    ++calls;
    ++depth;
    if (depth > maximum_depth) {
      maximum_depth = depth;
    }
    if (depth < limit) {
      sink.emit(event);
    }
    --depth;
  }
};

[[nodiscard]] terreate::DiagnosticEvent make_event() {
  return terreate::DiagnosticEvent{
      .severity = terreate::DiagnosticSeverity::warning,
      .categories = {"backend.validation", "backend.performance"},
      .source = "test-backend",
      .operation = "submit",
      .code = terreate::DiagnosticCode{.name = "TEST_WARNING", .value = 73},
      .message = "validation message",
      .context = "recording",
      .objects = {terreate::DiagnosticObject{
          .type = "VkDevice",
          .handle = 0x42,
          .name = "device",
      }},
      .queue_labels = {terreate::DiagnosticLabel{
          .name = "frame",
          .color = {1.0F, 0.5F, 0.25F, 1.0F},
      }},
      .command_buffer_labels = {},
  };
}

[[nodiscard]] bool same_event(const terreate::DiagnosticEvent &event) {
  const std::vector<std::string> expected_categories{
      "backend.validation",
      "backend.performance",
  };
  bool passed = true;
  passed &= check(event.severity == terreate::DiagnosticSeverity::warning,
                  "diagnostic severity was not retained");
  passed &= check(event.categories == expected_categories,
                  "diagnostic category strings were not retained in order");
  passed &= check(event.source == "test-backend", "diagnostic source was not retained");
  passed &= check(event.operation == "submit", "diagnostic operation was not retained");
  const bool code_preserved =
      event.code.has_value() && event.code->name == "TEST_WARNING" && event.code->value == 73;
  passed &= check(code_preserved, "diagnostic code was not retained");
  passed &= check(event.message == "validation message", "diagnostic message was not retained");
  passed &= check(event.context == "recording", "diagnostic context was not retained");
  const bool object_preserved =
      event.objects.size() == 1 && event.objects.front().type == "VkDevice" &&
      event.objects.front().handle == 0x42 && event.objects.front().name == "device";
  passed &= check(object_preserved, "diagnostic object was not retained as an owned value");
  const bool label_preserved = event.queue_labels.size() == 1 &&
                               event.queue_labels.front().name == "frame" &&
                               event.queue_labels.front().color[1] == 0.5F;
  passed &= check(label_preserved, "diagnostic label was not retained as an owned value");
  passed &= check(event.command_buffer_labels.empty(), "empty diagnostic labels were not retained");
  return passed;
}

[[nodiscard]] bool test_event_is_an_owning_value() {
  static_assert(std::is_copy_constructible_v<terreate::DiagnosticEvent>);
  static_assert(std::is_copy_assignable_v<terreate::DiagnosticEvent>);
  static_assert(std::is_move_constructible_v<terreate::DiagnosticEvent>);
  static_assert(std::is_move_assignable_v<terreate::DiagnosticEvent>);
  static_assert(std::is_copy_constructible_v<terreate::DiagnosticCode>);
  static_assert(std::is_copy_constructible_v<terreate::DiagnosticObject>);
  static_assert(std::is_copy_constructible_v<terreate::DiagnosticLabel>);

  std::string source = "test-backend";
  std::string message = "validation message";
  std::vector<std::string> categories{
      "backend.validation",
      "backend.performance",
  };
  terreate::DiagnosticEvent event = make_event();
  event.source = source;
  event.message = message;
  event.categories = categories;

  const terreate::DiagnosticEvent copy = event;
  terreate::DiagnosticEvent moved = std::move(event);

  source = "changed";
  message = "changed";
  categories.front() = "changed";

  bool passed = same_event(copy);
  passed &= same_event(moved);
  return passed;
}

[[nodiscard]] bool test_sink_view_contract() {
  static_assert(std::is_nothrow_default_constructible_v<terreate::DiagnosticSinkView>);
  static_assert(std::is_copy_constructible_v<terreate::DiagnosticSinkView>);
  static_assert(std::is_move_constructible_v<terreate::DiagnosticSinkView>);
  using EventReference = const terreate::DiagnosticEvent &;
  static_assert(std::is_nothrow_invocable_v<terreate::DiagnosticSinkView, EventReference>);
  static_assert(sizeof(terreate::DiagnosticSinkView) == 2 * sizeof(void *));
  static_assert(SinkBindable<Recorder>);
  static_assert(!SinkBindable<ThrowingRecorder>);
  using FunctionTarget = decltype(free_function_recorder);
  static_assert(std::is_nothrow_invocable_r_v<void, FunctionTarget &, EventReference>);
  static_assert(!SinkBindable<FunctionTarget>);
  static_assert(SinkBindable<AddressOverloadedRecorder>);

  const terreate::DiagnosticEvent event = make_event();
  terreate::DiagnosticSinkView empty;
  bool passed = true;
  passed &= check(!empty, "default diagnostic sink view was connected");
  empty.emit(event);

  Recorder recorder;
  const auto first = terreate::DiagnosticSinkView::bind(recorder);
  const auto second = first;
  passed &= check(first && second, "bound diagnostic sink view was not connected");
  passed &= check(recorder.calls == 0, "diagnostic sink invoked before emission");
  second.emit(event);
  passed &= check(recorder.calls == 1, "diagnostic sink was not synchronous");
  passed &= check(recorder.caller == std::this_thread::get_id(),
                  "diagnostic sink did not run on the caller thread");
  passed &= check(recorder.retained.size() == 1 && same_event(recorder.retained.front()),
                  "sink did not retain an owning event copy");

  first(event);
  passed &= check(recorder.calls == 2, "diagnostic sink call operator did not emit");

  AddressOverloadedRecorder address_overloaded_recorder;
  const auto address_overloaded_sink =
      terreate::DiagnosticSinkView::bind(address_overloaded_recorder);
  address_overloaded_sink.emit(event);
  passed &= check(address_overloaded_recorder.calls == 1,
                  "diagnostic sink did not bypass an overloaded address operator");
  return passed;
}

[[nodiscard]] bool test_bounded_reentrancy() {
  const terreate::DiagnosticEvent event = make_event();
  ReentrantRecorder recorder{.limit = 4};
  recorder.sink = terreate::DiagnosticSinkView::bind(recorder);
  recorder.sink.emit(event);
  bool passed = true;
  passed &= check(recorder.calls == 4, "reentrant sink did not recurse to its bound limit");
  const bool bounded = recorder.maximum_depth == 4;
  passed &= check(bounded, "reentrant sink did not remain synchronously bounded");
  passed &= check(recorder.depth == 0, "reentrant sink did not unwind completely");
  return passed;
}

} // namespace

int main() noexcept {
  bool passed = true;
  passed &= test_event_is_an_owning_value();
  passed &= test_sink_view_contract();
  passed &= test_bounded_reentrancy();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

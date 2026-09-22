#include <terreate/core/result.hpp>

#include <cstdio>
#include <cstdlib>
#include <expected>
#include <source_location>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class TestCategory final : public std::error_category {
public:
  [[nodiscard]] const char *name() const noexcept override { return "terreate-test"; }

  [[nodiscard]] std::string message(int value) const override {
    return "test-error-" + std::to_string(value);
  }
};

const std::error_category &test_category() noexcept {
  static const TestCategory category;
  return category;
}

struct MoveOnly {
  explicit MoveOnly(int value) noexcept : value(value) {}

  MoveOnly(const MoveOnly &) = delete;
  MoveOnly &operator=(const MoveOnly &) = delete;
  MoveOnly(MoveOnly &&) noexcept = default;
  MoveOnly &operator=(MoveOnly &&) noexcept = default;

  int value;
};

using MoveOnlyResult = terreate::Result<MoveOnly>;

template <typename T>
concept CanUnwrapConstRvalue =
    requires { terreate::unwrap(std::declval<const terreate::Result<T> &&>()); };

struct MoveException {};

struct ThrowingMove {
  ThrowingMove() = default;
  ThrowingMove(const ThrowingMove &) = delete;
  ThrowingMove &operator=(const ThrowingMove &) = delete;
  // This type exists specifically to verify that rvalue unwrap does not hide
  // an exception from a value move.
  ThrowingMove(ThrowingMove &&) noexcept(false) { throw MoveException{}; }
  ThrowingMove &operator=(ThrowingMove &&) = delete;
};

[[nodiscard]] bool check(bool condition, const char *description) {
  if (!condition) {
    std::fputs(description, stderr);
    std::fputc('\n', stderr);
  }
  return condition;
}

[[nodiscard]] terreate::Result<int> propagated_failure(std::error_code code) {
  return std::unexpected(terreate::Error{code, "propagation context", "propagation detail"});
}

[[nodiscard]] MoveOnlyResult propagate_move_only(MoveOnlyResult result) {
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  return std::move(result);
}

[[nodiscard]] bool test_error_copy_move_and_inspection() {
  const std::error_code code{37, test_category()};
  std::string context = "owned context";
  std::string detail = "owned detail";
  const auto location = std::source_location::current();
  const terreate::Error original{code, std::move(context), std::move(detail), location};
  const terreate::Error code_only{code};

  bool passed = true;
  static_assert(std::is_constructible_v<terreate::Error, std::error_code>);
  static_assert(!std::is_default_constructible_v<terreate::Error>);
  static_assert(!std::is_constructible_v<terreate::Error, std::string>);
  static_assert(!std::is_convertible_v<std::error_code, terreate::Error>);
  passed &= check(code_only.code() == code, "Error{code} did not preserve its code");
  passed &= check(code_only.context().empty() && code_only.detail().empty(),
                  "Error{code} did not default its optional text");
  passed &= check(original.code() == code, "error code was not preserved exactly");
  const bool category_preserved = original.code().category() == test_category();
  passed &= check(category_preserved, "error category was not preserved");
  passed &= check(original.code().value() == 37, "error value was not preserved");
  passed &= check(&original.code().category() == &test_category(),
                  "error category lifetime source was not preserved");
  passed &= check(original.context() == "owned context", "error context was not owned");
  passed &= check(original.detail() == "owned detail", "error detail was not owned");
  const bool context_and_detail_are_distinct = original.context() != original.detail();
  passed &= check(context_and_detail_are_distinct, "error context and detail were not distinct");
  passed &= check(original.location().file_name() == location.file_name(),
                  "error source file was not preserved");
  const bool source_line_preserved = original.location().line() == location.line();
  passed &= check(source_line_preserved, "error source line was not preserved");
  const bool source_function_preserved =
      original.location().function_name() == location.function_name();
  passed &= check(source_function_preserved, "error source function was not preserved");

  terreate::Error copied = original;
  passed &= check(copied.context() == original.context() && copied.detail() == original.detail(),
                  "Error copy did not preserve owned text");
  terreate::Error moved = std::move(copied);
  passed &= check(moved.code() == code && moved.context() == "owned context" &&
                      moved.detail() == "owned detail",
                  "Error move did not preserve its value");

  terreate::Error copy_assigned{code, "copy context", "copy detail"};
  copy_assigned = original;
  const bool copied_context_preserved = copy_assigned.context() == "owned context";
  const bool copied_detail_preserved = copy_assigned.detail() == "owned detail";
  const bool copied_assignment_preserved = copied_context_preserved && copied_detail_preserved;
  passed &= check(copied_assignment_preserved, "Error copy assignment did not preserve owned text");
  terreate::Error move_assigned{code, "move context", "move detail"};
  move_assigned = std::move(copy_assigned);
  const bool moved_assignment_preserved =
      move_assigned.code() == code && move_assigned.location().line() == location.line();
  passed &= check(moved_assignment_preserved, "Error move assignment did not preserve its value");

  static_assert(std::is_copy_constructible_v<terreate::Error>);
  static_assert(std::is_copy_assignable_v<terreate::Error>);
  static_assert(std::is_move_constructible_v<terreate::Error>);
  static_assert(std::is_move_assignable_v<terreate::Error>);
  return passed;
}

[[nodiscard]] bool test_result_int_and_void_inspection() {
  const std::error_code code{41, test_category()};
  bool passed = true;

  terreate::Result<int> success = 42;
  passed &= check(success.has_value() && static_cast<bool>(success),
                  "successful Result<int> was not observable");
  passed &= check(*success == 42 && success.value() == 42,
                  "successful Result<int> value was not inspectable");

  terreate::Result<int> failure = propagated_failure(code);
  passed &= check(!failure.has_value() && !static_cast<bool>(failure),
                  "failed Result<int> was not observable");
  passed &= check(failure.error().code() == code, "Result<int> did not preserve its error code");
  passed &= check(failure.error().context() == "propagation context" &&
                      failure.error().detail() == "propagation detail",
                  "Result<int> did not preserve distinct error text");

  bool int_value_threw = false;
  try {
    static_cast<void>(failure.value());
  } catch (const std::bad_expected_access<terreate::Error> &exception) {
    int_value_threw = exception.error().detail() == "propagation detail";
  } catch (...) {
    int_value_threw = false;
  }
  passed &= check(int_value_threw, "Result<int>::value did not throw bad_expected_access<Error>");

  terreate::Result<void> void_success;
  passed &= check(void_success.has_value() && static_cast<bool>(void_success),
                  "successful Result<void> was not observable");

  const terreate::Error void_error{code, "void context", "void detail"};
  terreate::Result<void> void_failure = std::unexpected(void_error);
  passed &= check(!void_failure.has_value() && void_failure.error().code() == code,
                  "failed Result<void> was not inspectable");
  passed &= check(void_failure.error().context() == "void context" &&
                      void_failure.error().detail() == "void detail",
                  "Result<void> did not preserve distinct error text");

  bool void_value_threw = false;
  try {
    void_failure.value();
  } catch (const std::bad_expected_access<terreate::Error> &exception) {
    void_value_threw = exception.error().context() == "void context";
  } catch (...) {
    void_value_threw = false;
  }
  passed &= check(void_value_threw, "Result<void>::value did not throw bad_expected_access<Error>");

  static_assert(std::is_same_v<terreate::Result<int>, std::expected<int, terreate::Error>>);
  static_assert(std::is_same_v<terreate::Result<void>, std::expected<void, terreate::Error>>);
  return passed;
}

[[nodiscard]] bool test_move_only_success_and_propagation() {
  bool passed = true;
  static_assert(!std::is_copy_constructible_v<MoveOnly>);
  using MoveOnlyUnwrap = decltype(terreate::unwrap(std::declval<MoveOnlyResult &&>()));
  static_assert(std::is_same_v<MoveOnlyUnwrap, MoveOnly>);

  terreate::Result<MoveOnly> success{std::in_place, 73};
  MoveOnly value = terreate::unwrap(std::move(success));
  passed &= check(value.value == 73, "unwrap did not move a move-only success value");

  const std::error_code code{53, test_category()};
  const terreate::Error move_only_error{code, "move-only context", "move-only detail"};
  MoveOnlyResult failure = std::unexpected(move_only_error);
  terreate::Result<MoveOnly> propagated = propagate_move_only(std::move(failure));
  passed &= check(!propagated && propagated.error().code() == code,
                  "move-only failure did not propagate");
  passed &= check(propagated.error().context() == "move-only context" &&
                      propagated.error().detail() == "move-only detail",
                  "move-only failure lost its distinct error text");

  using ThrowingMoveResult = terreate::Result<ThrowingMove>;
  static_assert(!noexcept(terreate::unwrap(std::declval<ThrowingMoveResult &&>())));
  ThrowingMoveResult throwing_success{std::in_place};
  bool move_exception_propagated = false;
  try {
    auto moved = terreate::unwrap(std::move(throwing_success));
    (void)moved;
  } catch (const MoveException &) {
    move_exception_propagated = true;
  } catch (...) {
    move_exception_propagated = false;
  }
  passed &= check(move_exception_propagated, "unwrap did not permit a value move exception");
  return passed;
}

[[nodiscard]] bool test_all_unwrap_success_overloads() {
  bool passed = true;

  terreate::Result<int> mutable_success = 42;
  const terreate::Result<int> const_success = 43;
  static_assert(std::is_same_v<decltype(terreate::unwrap(mutable_success)), int &>);
  static_assert(std::is_same_v<decltype(terreate::unwrap(const_success)), const int &>);
  static_assert(std::is_same_v<decltype(terreate::unwrap(std::move(mutable_success))), int>);
  static_assert(!CanUnwrapConstRvalue<int>);

  terreate::unwrap(mutable_success) = 44;
  passed &= check(terreate::unwrap(mutable_success) == 44,
                  "mutable lvalue unwrap did not return a reference");
  passed &= check(terreate::unwrap(const_success) == 43,
                  "const lvalue unwrap did not return a reference");
  passed &= check(terreate::unwrap(std::move(mutable_success)) == 44,
                  "mutable rvalue unwrap did not return a value");

  terreate::Result<void> mutable_void;
  const terreate::Result<void> const_void;
  static_assert(std::is_same_v<decltype(terreate::unwrap(mutable_void)), void>);
  static_assert(std::is_same_v<decltype(terreate::unwrap(const_void)), void>);
  terreate::unwrap(mutable_void);
  terreate::unwrap(const_void);
  terreate::unwrap(terreate::Result<void>{});
  return passed;
}

[[nodiscard]] bool test_unwrap_failure_dies_with_diagnostic() {
  int output_pipe[2]{};
  if (pipe(output_pipe) != 0) {
    return check(false, "could not create unwrap diagnostic pipe");
  }

  const pid_t child = fork();
  if (child < 0) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    return check(false, "could not fork unwrap death test");
  }

  if (child == 0) {
    close(output_pipe[0]);
    if (dup2(output_pipe[1], STDERR_FILENO) < 0) {
      _exit(2);
    }
    close(output_pipe[1]);
    if (setvbuf(stderr, nullptr, _IOFBF, BUFSIZ) != 0) {
      _exit(2);
    }
    const std::error_code code{91, test_category()};
    const terreate::Error death_error{code, "death-test context", "death-test detail"};
    terreate::Result<int> failure = std::unexpected(death_error);
    (void)terreate::unwrap(failure);
    _exit(EXIT_SUCCESS);
  }

  close(output_pipe[1]);
  int status = 0;
  const pid_t waited = waitpid(child, &status, 0);
  std::string diagnostic;
  char buffer[256]{};
  ssize_t count = 0;
  while ((count = read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
    diagnostic.append(buffer, static_cast<std::size_t>(count));
  }
  close(output_pipe[0]);

  bool passed = true;
  passed &= check(waited == child, "unwrap death test waitpid failed");
  const bool child_failed = !WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS;
  passed &= check(child_failed, "unwrap failure child exited successfully");
  passed &= check(diagnostic.find("terreate::unwrap failed") != std::string::npos,
                  "unwrap diagnostic prefix was missing");
  passed &= check(diagnostic.find("death-test context") != std::string::npos,
                  "unwrap diagnostic context was missing");
  passed &= check(diagnostic.find("death-test detail") != std::string::npos,
                  "unwrap diagnostic detail was missing");
  passed &= check(diagnostic.find("terreate-test:91") != std::string::npos,
                  "unwrap diagnostic error code was missing");
  passed &= check(!diagnostic.empty(), "unwrap diagnostic was not flushed to stderr");
  return passed;
}

} // namespace

int main() noexcept {
  bool passed = true;
  passed &= test_error_copy_move_and_inspection();
  passed &= test_result_int_and_void_inspection();
  passed &= test_move_only_success_and_propagation();
  passed &= test_all_unwrap_success_overloads();
  passed &= test_unwrap_failure_dies_with_diagnostic();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

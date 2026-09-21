#ifndef TERREATE_CORE_RESULT_HPP
#define TERREATE_CORE_RESULT_HPP

#include <cstdio>
#include <exception>
#include <expected>
#include <source_location>
#include <type_traits>
#include <utility>

#include <terreate/core/error.hpp>

namespace terreate {

using Location = std::source_location;

/// The Core fallible-operation primitive. Result is intentionally the
/// standard expected type so normal propagation remains explicit and does not
/// convert failures into exceptions.
template <typename T> using Result = std::expected<T, Error>;

namespace detail {

[[noreturn]] inline void terminate_unwrap(const Error &error, Location call_location) noexcept {
  std::fputs("terreate::unwrap failed: ", stderr);
  if (!error.context().empty()) {
    std::fputs(error.context().c_str(), stderr);
    if (!error.detail().empty()) {
      std::fputs(": ", stderr);
    }
  }
  std::fputs(error.detail().c_str(), stderr);
  std::fprintf(stderr, " [%s:%d]", error.code().category().name(), error.code().value());
  std::fprintf(stderr, " at %s:%u in %s", error.location().file_name(), error.location().line(),
               error.location().function_name());
  std::fprintf(stderr, " (unwrap called from %s:%u in %s)\n", call_location.file_name(),
               call_location.line(), call_location.function_name());
  std::fflush(stderr);
  std::terminate();
}

} // namespace detail

template <typename T>
  requires(!std::is_void_v<T>)
[[nodiscard]] T &unwrap(Result<T> &result, Location call_location = Location::current()) noexcept {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
  return *result;
}

template <typename T>
  requires(!std::is_void_v<T>)
[[nodiscard]] const T &unwrap(const Result<T> &result,
                              Location call_location = Location::current()) noexcept {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
  return *result;
}

template <typename T>
  requires(!std::is_void_v<T>)
[[nodiscard]] T unwrap(Result<T> &&result, Location call_location = Location::current()) {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
  return std::move(*result);
}

template <typename T>
  requires(!std::is_void_v<T>)
[[nodiscard]] T unwrap(const Result<T> &&result, Location call_location = Location::current()) {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
  return *result;
}

inline void unwrap(Result<void> &result, Location call_location = Location::current()) noexcept {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
}

inline void unwrap(const Result<void> &result,
                   Location call_location = Location::current()) noexcept {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
}

inline void unwrap(Result<void> &&result, Location call_location = Location::current()) noexcept {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
}

inline void unwrap(const Result<void> &&result,
                   Location call_location = Location::current()) noexcept {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
}

} // namespace terreate

#endif // TERREATE_CORE_RESULT_HPP

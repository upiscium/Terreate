#ifndef TERREATE_CORE_RESULT_HPP
#define TERREATE_CORE_RESULT_HPP

#include <expected>
#include <source_location>
#include <type_traits>
#include <utility>

#include <terreate/core/error.hpp>

namespace terreate {

/// The Core fallible-operation primitive. Result is intentionally the
/// standard expected type so normal propagation remains explicit and does not
/// convert failures into exceptions.
template <typename T> using Result = std::expected<T, Error>;

namespace detail {

[[noreturn]] void terminate_unwrap(const Error &error, std::source_location call_location) noexcept;

} // namespace detail

template <typename T>
  requires(!std::is_void_v<T>)
[[nodiscard]] T &
unwrap(Result<T> &result,
       std::source_location call_location = std::source_location::current()) noexcept {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
  return *result;
}

template <typename T>
  requires(!std::is_void_v<T>)
[[nodiscard]] const T &
unwrap(const Result<T> &result,
       std::source_location call_location = std::source_location::current()) noexcept {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
  return *result;
}

template <typename T>
  requires(!std::is_void_v<T>)
[[nodiscard]] T unwrap(Result<T> &&result,
                       std::source_location call_location = std::source_location::current()) {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
  return std::move(*result);
}

// A const Result rvalue must not bind to the const-lvalue overload: returning
// a reference from that path would dangle as soon as the temporary dies.
template <typename T>
  requires(!std::is_void_v<T>)
[[nodiscard]] T unwrap(const Result<T> &&,
                       std::source_location = std::source_location::current()) = delete;

// Result<void> has no value reference to dangle, so one const-reference
// overload safely accepts every value category.
inline void unwrap(const Result<void> &result,
                   std::source_location call_location = std::source_location::current()) noexcept {
  if (!result) {
    detail::terminate_unwrap(result.error(), call_location);
  }
}

} // namespace terreate

#endif // TERREATE_CORE_RESULT_HPP

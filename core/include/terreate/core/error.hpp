#ifndef TERREATE_CORE_ERROR_HPP
#define TERREATE_CORE_ERROR_HPP

#include <source_location>
#include <string>
#include <system_error>
#include <utility>

namespace terreate {

/// A value-type diagnostic carried by a failed Core operation.
///
/// Error owns its context and detail strings and its source_location value. The
/// native error code is retained exactly, but the error_category referenced by
/// it is not owned and must outlive every Error that uses it.
class Error {
public:
  /// Construct an Error from a required native code and optional owned text.
  ///
  /// `code.category()` is a non-owned reference. Its category object must
  /// remain alive for as long as this Error, or any copy of it, is used.
  explicit Error(std::error_code code, std::string context = {}, std::string detail = {},
                 std::source_location location = std::source_location::current())
      : code_(code), context_(std::move(context)), detail_(std::move(detail)) {
    location_ = location;
  }

  Error(const Error &) = default;
  Error(Error &&) noexcept = default;
  Error &operator=(const Error &) = default;
  Error &operator=(Error &&) noexcept = default;
  ~Error() = default;

  [[nodiscard]] const std::error_code &code() const noexcept { return code_; }

  [[nodiscard]] const std::string &context() const noexcept { return context_; }

  [[nodiscard]] const std::string &detail() const noexcept { return detail_; }

  [[nodiscard]] const std::source_location &location() const noexcept { return location_; }

private:
  std::error_code code_;
  std::string context_;
  std::string detail_;
  std::source_location location_ = std::source_location::current();
};

} // namespace terreate

#endif // TERREATE_CORE_ERROR_HPP

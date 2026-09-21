#ifndef TERREATE_CORE_ERROR_HPP
#define TERREATE_CORE_ERROR_HPP

#include <source_location>
#include <string>
#include <system_error>
#include <utility>

namespace terreate {

/// A value-type diagnostic carried by a failed Core operation.
///
/// Error owns its context and detail strings.  The native error code and
/// source location are retained exactly so an Error can cross an API boundary
/// without borrowing any part of the diagnostic from its caller.
class Error {
public:
  using code_type = std::error_code;
  using location_type = std::source_location;

  Error() = default;

  explicit Error(std::error_code code,
                 std::source_location location = std::source_location::current())
      : code_(code), detail_(code.message()), location_(location) {}

  explicit Error(std::string detail,
                 std::source_location location = std::source_location::current())
      : detail_(std::move(detail)), location_(location) {}

  Error(std::error_code code, std::string detail,
        std::source_location location = std::source_location::current())
      : code_(code), detail_(std::move(detail)), location_(location) {}

  Error(std::string detail, std::error_code code,
        std::source_location location = std::source_location::current())
      : code_(code), detail_(std::move(detail)), location_(location) {}

  Error(std::error_code code, std::string context, std::string detail,
        std::source_location location = std::source_location::current())
      : code_(code), context_(std::move(context)), detail_(std::move(detail)) {
    location_ = location;
  }

  Error(std::string context, std::string detail, std::error_code code,
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

  [[nodiscard]] const std::error_code &error_code() const noexcept { return code_; }

  [[nodiscard]] const std::error_category &category() const noexcept { return code_.category(); }

  [[nodiscard]] int value() const noexcept { return code_.value(); }

  [[nodiscard]] const std::string &context() const noexcept { return context_; }

  [[nodiscard]] const std::string &detail() const noexcept { return detail_; }

  // Keep message() as a spelling for the detail text for callers that need a
  // single diagnostic string; context() and detail() remain independent.
  [[nodiscard]] const std::string &message() const noexcept { return detail_; }

  [[nodiscard]] const std::source_location &location() const noexcept { return location_; }

  [[nodiscard]] const std::source_location &source_location() const noexcept { return location_; }

  [[nodiscard]] const std::source_location &source() const noexcept { return location_; }

  [[nodiscard]] const std::source_location &where() const noexcept { return location_; }

  /// Return a human-readable diagnostic without changing the stored values.
  [[nodiscard]] std::string diagnostic() const {
    std::string result;
    result.reserve(context_.size() + detail_.size() + 64);
    if (!context_.empty()) {
      result += context_;
      if (!detail_.empty()) {
        result += ": ";
      }
    }
    result += detail_;
    result += " [";
    result += code_.category().name();
    result += ':';
    result += std::to_string(code_.value());
    result += "] at ";
    result += location_.file_name();
    result += ':';
    result += std::to_string(location_.line());
    result += " in ";
    result += location_.function_name();
    return result;
  }

private:
  std::error_code code_;
  std::string context_;
  std::string detail_;
  std::source_location location_ = std::source_location::current();
};

} // namespace terreate

#endif // TERREATE_CORE_ERROR_HPP

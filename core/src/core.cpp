#include <terreate/core/result.hpp>

#include <cinttypes>
#include <cstdio>
#include <exception>

namespace terreate::detail {

[[noreturn]] void terminate_unwrap(const Error &error,
                                   std::source_location call_location) noexcept {
  std::fputs("terreate::unwrap failed: ", stderr);
  if (!error.context().empty()) {
    std::fputs(error.context().c_str(), stderr);
    if (!error.detail().empty()) {
      std::fputs(": ", stderr);
    }
  }
  std::fputs(error.detail().c_str(), stderr);
  std::fprintf(stderr, " [%s:%d]", error.code().category().name(), error.code().value());
  std::fprintf(stderr, " at %s:%" PRIuLEAST32 " in %s", error.location().file_name(),
               error.location().line(), error.location().function_name());
  std::fprintf(stderr, " (unwrap called from %s:%" PRIuLEAST32 " in %s)\n",
               call_location.file_name(), call_location.line(), call_location.function_name());
  std::fflush(stderr);
  std::terminate();
}

} // namespace terreate::detail

namespace terreate::core {}

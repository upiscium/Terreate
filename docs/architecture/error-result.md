# Core Error and Result Contract

Terreate Core uses an explicit, value-based failure path for fallible APIs:

```cpp
#include <terreate/core/result.hpp>

terreate::Result<int> read_value();

auto result = read_value();
if (!result) {
  // inspect or return result.error()
  return result;
}
const int value = terreate::unwrap(result);
```

## `Error`

`terreate::Error` is a small diagnostic value. It stores the native
`std::error_code` unchanged, including its category and numeric value, and
stores the `std::source_location` supplied by the producer. Its context and
detail are independent, owned `std::string` values. An Error can therefore be
copied, moved, or returned from an API without borrowing a caller's strings or
losing the native error-code identity.

Error is a diagnostic value, not a logging policy. A domain or backend may
carry its native code in the error's `std::error_code`; Core does not maintain
a global enum and does not include Vulkan, optional backends, or logging
libraries.

## `Result`

`Result<T>` is an alias for `std::expected<T, terreate::Error>`, including the
normal `Result<void>` specialization. Success and failure are inspected with
the standard expected operations (`has_value`, `operator bool`, `operator*`,
`value`, and `error`). Returning a failure is explicit; Core does not convert
exceptions to errors or silently substitute a fallback value.

`std::expected::value()` is the standard checked accessor. On a failed
`Result<T>` or `Result<void>`, it throws `std::bad_expected_access<Error>` with
the stored Error. Code that wants recoverable propagation should inspect the
result and return or transform its Error rather than use `value()` as an error
transport.

The value and Error may allocate while being constructed or copied because
Error owns its context and detail strings. Result operations retain the normal
allocation and exception behavior of `std::string`, `T`, `Error`, and
`std::expected`; Core makes no hidden allocation-free or no-throw guarantee.
In particular, a move of `T` performed by the rvalue `unwrap` overload may
throw and that exception is allowed to propagate.

## Explicit `unwrap`

`terreate::unwrap` is an opt-in fail-fast convenience. Successful calls have
the following safe return forms:

| argument | return |
| --- | --- |
| mutable `Result<T>` lvalue | `T&` |
| const `Result<T>` lvalue | `const T&` |
| mutable `Result<T>` rvalue | `T` (moved by value) |
| const `Result<T>` rvalue | `T` (copied by value when supported) |
| any `Result<void>` value category | `void` |

The rvalue value return is intentional: it does not return a reference into a
temporary `Result`, and it permits a move-only success value to leave the
result. The lvalue reference forms require the caller to keep the Result alive.

Unwrapping a failure writes the diagnostic, including its context, detail,
category/value, and source locations, to `stderr`, flushes `stderr`, and then
calls `std::terminate`. The termination mechanism is implementation/runtime
dependent; callers must only rely on the documented fail-fast behavior, not a
particular signal or exit status. Calls that need recoverable behavior should
inspect the `Result` or propagate its `Error` instead.

## Package and dependency boundary

The public headers live in `core/include/terreate/core/` in the source tree
and install below `include/terreate/core/`. They are exported by
`Terreate::Core`. Core's public interface depends only on the C++23 standard
library. Package consumers can use the error/result primitives without
discovering or linking Vulkan or any optional dependency; backend-specific
error codes are supplied by the backend that owns them.

## Boundaries with related work

Issue #228 owns the configuration-resolution architecture: capability-query
failure, resolution failure, and post-resolution native application failure
remain distinct semantic stages. `Result` and `Error` can transport a
fallible operation at an implementation boundary, but this issue does not
replace #228's structured records, turn an unknown capability into
"unsupported", or define configuration defaults and resolution policy.

Issue #350 is a later integration boundary. This baseline supplies the
dependency-free primitive and explicit fail-fast behavior; it does not add
the higher-level translation, policy, or domain-specific APIs reserved for
#350. Future code at that boundary must preserve the native code and the
context/detail distinction rather than introduce a Core-wide error enum.

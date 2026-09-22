# Structured Diagnostics Contract

Terreate diagnostics are delivered as structured events to an
application-owned sink. Core owns the backend-neutral event shape and delivery
contract; category strings are supplied by each producer. Core does not own a
logger, worker, queue, or global sink.

## Core event and sink view

The public Core surface is `<terreate/core/diagnostics.hpp>`.
`terreate::DiagnosticEvent` is an ordinary owning value. It contains:

* a `DiagnosticSeverity`;
* `std::vector<std::string> categories`, containing producer-supplied,
  backend-neutral strings; Core does not prescribe their vocabulary or order;
* owned source, operation, message, and context strings;
* an optional owned `DiagnosticCode` containing a string name and `int64_t`
  value;
* owned `DiagnosticObject` values containing a backend-neutral type string, a
  `uint64_t` handle, and a name; and
* owned queue and command-buffer label values.

No event field is a span, `string_view`, callback user-data pointer, or other
borrowed producer state. Events may therefore be copied, moved, and retained
after delivery:

```cpp
#include <terreate/core/diagnostics.hpp>

struct ApplicationDiagnostics {
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    retained = event; // The copy owns all text, objects, categories, and labels.
  }

  terreate::DiagnosticEvent retained;
};

ApplicationDiagnostics application;
auto sink = terreate::DiagnosticSinkView::bind(application);
sink.emit(terreate::DiagnosticEvent{.message = "diagnostic"});
```

`DiagnosticSinkView` is a small, copyable, non-owning synchronous view made of
an opaque `void*` state and a non-throwing callback. Binding stores only the
address of the lvalue target; it performs no allocation and does not transfer
ownership. The target must outlive the view and every emission made through any
copy of it. A default-constructed view is a no-op and converts to `false`; a
bound view converts to `true`. Delivery runs immediately on the calling thread.
Core does not catch exceptions, create a thread, or serialize/reorder events.
Reentrant emissions are immediate recursive calls; the application target owns
any depth or reentrancy limit it needs.

The sink callback must be non-throwing. Event construction and native
translation may allocate, but the sink view itself never allocates while being
bound, copied, or emitted.

## Relationship to fallible operations and diagnostics UI

`DiagnosticEvent` is an observational diagnostics value, not the failure
transport for a fallible operation. The `Error`/`Result` values defined by #349
remain the authoritative way to represent, inspect, return, and propagate
operation failure. An operation may produce a `DiagnosticEvent` for an
observation while also returning an `Error`, but the event does not replace,
encode, or implicitly accompany that `Error`/`Result` value.

The explicit `terreate::unwrap` semantics from #349 are unchanged. Successful
results are unwrapped as documented there; unwrapping a failure remains the
opt-in fail-fast operation that writes the Error to `stderr`, flushes `stderr`,
and terminates. Core does not automatically emit a `DiagnosticEvent` when an
`Error`/`Result` is constructed, returned, inspected, or unwrapped. Producers
must explicitly construct and emit any observational event they want to
deliver.

Issue #336's diagnostics UI may reuse `DiagnosticEvent` values that the
application retained from its sink. Core does not own event history, UI
presentation, or persistent storage: retention, replay, presentation, and
lifetime remain application responsibilities. The synchronous sink and its
ownership/lifetime contract above are unchanged.

## Graphics translation boundary

Graphics keeps its translation input private in `modules/graphics/src/`. The
input is a Vulkan-neutral representation of the Debug Utils callback data:
severity bits, message category bits, message ID name/number, message text,
operation/context, queue and command-buffer labels, and object
handle/type/name values. These input fields may borrow the native callback's
transient storage. The translator copies every field into an owning Core event
before synchronous sink delivery; no callback data view escapes the translation
call.

The private translator maps exactly the four Vulkan Debug Utils severities and
maps General, Validation, and Performance category bits to `GENERAL`,
`VALIDATION`, and `PERFORMANCE` in its fixed native order. Multiple category
bits remain present in the event vector. Native object types are supplied as
stable backend-neutral strings, and object handles, names, codes, messages,
operation/context, and labels are copied without inventing missing context.
The native message-ID name/number tuple is always represented by `code`, even
when the name is empty and the number is zero.
Empty operation and context remain empty. These Vulkan bit values, spellings,
and ordering are Graphics implementation details, not Core API vocabulary.

Unknown or combined severity bits return the explicit
`DiagnosticTranslationError::unknown_severity` result. Unknown category bits
return `unknown_category`. Neither case is downgraded to an error event or
silently discarded.

No messenger or Instance is created or owned here. The later Instance work
owns callback registration and lifetime; #268 can connect its callback wiring
to this private bridge without adding Vulkan headers to Core.

## Dependency and package boundary

Core's public diagnostics header depends only on the C++23 standard library. It
is installed with `Terreate::Core`, and the Core-only package smoke test uses
`DiagnosticEvent` and `DiagnosticSinkView` without Graphics or Vulkan.
Graphics' translation header is a private source implementation detail and is
not installed or exported as a public Graphics diagnostics API.

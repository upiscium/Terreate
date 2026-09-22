#ifndef TERREATE_CORE_DIAGNOSTICS_HPP
#define TERREATE_CORE_DIAGNOSTICS_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace terreate {

/// The severity assigned by the producer of a diagnostic.
enum class DiagnosticSeverity : std::uint8_t {
  verbose,
  info,
  warning,
  error,
};

/// A machine-readable diagnostic code owned by the event.
struct DiagnosticCode {
  std::string name{};
  std::int64_t value = 0;
};

/// An object referenced by a diagnostic.
///
/// `type` is a stable backend-neutral spelling supplied by the translating
/// backend (for example, `VkInstance`).  Core deliberately does not depend on
/// a backend object enum.
struct DiagnosticObject {
  std::string type{};
  std::uint64_t handle = 0;
  std::string name{};
};

/// A labelled execution context associated with a diagnostic.
struct DiagnosticLabel {
  std::string name{};
  std::array<float, 4> color{};
};

/// A structured, backend-neutral diagnostic event.
///
/// Every field in an event is an owning value.  In particular, an event can be
/// copied, moved, or retained by a sink after the producer callback returns;
/// it contains no spans, string views, callback user data, or other borrowed
/// producer state.  Category names are backend-neutral strings supplied by the
/// producer; Core does not prescribe a category vocabulary or ordering.
struct DiagnosticEvent {
  DiagnosticSeverity severity = DiagnosticSeverity::info;
  std::vector<std::string> categories{};
  std::string source{};
  std::string operation{};
  std::optional<DiagnosticCode> code{};
  std::string message{};
  std::string context{};
  std::vector<DiagnosticObject> objects{};
  std::vector<DiagnosticLabel> queue_labels{};
  std::vector<DiagnosticLabel> command_buffer_labels{};
};

/// A small, non-owning synchronous diagnostic destination.
///
/// The bound target is borrowed.  It must outlive this view and every call to
/// emit().  Copying a view copies only its state pointer and callback; it never
/// copies or owns the target.  Binding performs no allocation and is available
/// only for an object lvalue target whose event call is non-throwing.  Delivery
/// occurs immediately on the calling thread, and reentrant emissions are
/// ordinary recursive calls.
class DiagnosticSinkView {
public:
  using Callback = void (*)(const DiagnosticEvent &, void *) noexcept;

  constexpr DiagnosticSinkView() noexcept = default;

  constexpr explicit DiagnosticSinkView(Callback callback, void *state = nullptr) noexcept
      : callback_(callback), state_(state) {}

  constexpr DiagnosticSinkView(void *state, Callback callback) noexcept
      : callback_(callback), state_(state) {}

  [[nodiscard]] static constexpr DiagnosticSinkView noop() noexcept { return {}; }

  /// Bind a borrowed callable without allocating or taking ownership of it.
  template <typename Target>
    requires std::is_object_v<Target> &&
             std::is_nothrow_invocable_r_v<void, Target &, const DiagnosticEvent &>
  [[nodiscard]] static constexpr DiagnosticSinkView bind(Target &target) noexcept {
    auto *untyped_target = const_cast<std::remove_cv_t<Target> *>(&target);
    return DiagnosticSinkView{&invoke<Target>, static_cast<void *>(untyped_target)};
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return callback_ != nullptr; }

  [[nodiscard]] constexpr bool connected() const noexcept { return static_cast<bool>(*this); }

  void emit(const DiagnosticEvent &event) const noexcept {
    if (callback_ != nullptr) {
      callback_(event, state_);
    }
  }

  void operator()(const DiagnosticEvent &event) const noexcept { emit(event); }

private:
  template <typename Target>
  static void invoke(const DiagnosticEvent &event, void *state) noexcept {
    std::invoke(*static_cast<Target *>(state), event);
  }

  Callback callback_ = nullptr;
  void *state_ = nullptr;
};

} // namespace terreate

#endif // TERREATE_CORE_DIAGNOSTICS_HPP

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Opaque Xlib Display. Xlib's headers define macros (Bool/True/False/None)
// that leak into every translation unit, so only the .cpp includes them.
struct _XDisplay;

namespace layerc::platform {

struct X11Extension {
  bool present{};
  int major{};
  int minor{};
  // Extension opcode; XI2 events arrive as GenericEvents tagged with it.
  int opcode{};
  int event_base{};
  int error_base{};
};

// One probe pass at connect time. Every field carries the reason an adapter
// is unavailable so the daemon can publish it instead of failing silently.
struct X11Probe {
  bool connected{};
  X11Extension xi2;
  X11Extension shape;
  X11Extension randr;
  X11Extension composite;
  X11Extension damage;
  X11Extension xfixes;
  bool ewmh{};

  [[nodiscard]] std::vector<std::string> unavailable() const;
};

// Typed open failure so capability reporting switches on a code, never on
// a prose message from the display layer.
enum class X11OpenError : std::uint8_t {
  no_display_variable,
  cannot_open_display,
};

struct X11OpenResult;

class X11Display {
public:
  X11Display() = default;
  X11Display(const X11Display &) = delete;
  X11Display &operator=(const X11Display &) = delete;
  X11Display(X11Display &&other) noexcept;
  X11Display &operator=(X11Display &&other) noexcept;
  ~X11Display();

  // Uses $DISPLAY; an empty error means success.
  [[nodiscard]] static std::optional<X11Display> open(std::string &error);
  [[nodiscard]] static X11OpenResult open_result();

  [[nodiscard]] bool valid() const noexcept { return display_ != nullptr; }
  [[nodiscard]] int fd() const noexcept;
  [[nodiscard]] std::uint64_t root() const noexcept;
  [[nodiscard]] const X11Probe &probe() const noexcept { return probe_; }

  // 0 (None) when the atom is not defined; never creates atoms.
  [[nodiscard]] std::uint64_t atom(std::string_view name) const noexcept;
  [[nodiscard]] std::string atom_name(std::uint64_t atom) const;

  // Discards every event already queued on the connection. Registered on the
  // event loop so a readable socket cannot busy-poll; later phases dispatch
  // instead of discarding.
  [[nodiscard]] int drain_events() noexcept;
  // Client-side buffered event count (XPending): socket readiness alone
  // cannot prove the queue is empty because synchronous Xlib queries and
  // XSync(False) move bytes from the socket into this queue without waking
  // poll. The host checks this before blocking so buffered work never
  // sleeps until a second external event arrives.
  [[nodiscard]] int pending_events() noexcept;

  [[nodiscard]] struct _XDisplay *raw() const noexcept { return display_; }

private:
  explicit X11Display(struct _XDisplay *display, X11Probe probe) noexcept;
  void close() noexcept;

  struct _XDisplay *display_{};
  X11Probe probe_{};
};

struct X11OpenResult {
  std::optional<X11Display> display;
  std::optional<X11OpenError> error;
  std::string message;
};

} // namespace layerc::platform

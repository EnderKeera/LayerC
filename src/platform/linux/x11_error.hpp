#pragma once

struct _XDisplay;

namespace layerc::platform {

// Xlib's default error handler aborts the process on a protocol error, and
// windows disappear between a scan and its queries. One handler records the
// code instead; every X call runs inside a display-scoped synchronous trap.
void install_x11_error_handler() noexcept;

class X11ErrorTrap {
public:
  explicit X11ErrorTrap(_XDisplay *display) noexcept;
  X11ErrorTrap(const X11ErrorTrap &) = delete;
  X11ErrorTrap &operator=(const X11ErrorTrap &) = delete;
  ~X11ErrorTrap();

  [[nodiscard]] bool ok() noexcept;
  static void record(_XDisplay *display, unsigned long serial,
                     int error) noexcept;

private:
  void synchronize() noexcept;

  _XDisplay *display_{};
  unsigned long first_serial_{};
  int error_{};
  X11ErrorTrap *previous_{};
  static thread_local X11ErrorTrap *active_;
};

} // namespace layerc::platform

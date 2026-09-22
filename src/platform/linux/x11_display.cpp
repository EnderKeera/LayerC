// Xlib connection + one extension probe pass (V2.6.1). Xlib is confined to
// this translation unit: every X call runs under an error trap because Xlib's
// default handler aborts the process on a protocol error (windows vanish
// between a scan and its queries).
#include "x11_display.hpp"
#include "x11_error.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

#include <cstdlib>
#include <utility>

namespace layerc::platform {
namespace {

// Xlib aborts on protocol errors by default; record and continue instead.
int error_handler(Display *display, XErrorEvent *event) noexcept {
  X11ErrorTrap::record(display, event->serial, event->error_code);
  return 0;
}

} // namespace

thread_local X11ErrorTrap *X11ErrorTrap::active_{};

void install_x11_error_handler() noexcept {
  (void)XSetErrorHandler(&error_handler);
}

X11ErrorTrap::X11ErrorTrap(_XDisplay *display) noexcept : display_(display) {
  if (display_ != nullptr)
    XSync(reinterpret_cast<Display *>(display_), False);
  first_serial_ = display_ != nullptr
                      ? NextRequest(reinterpret_cast<Display *>(display_))
                      : 0;
  previous_ = active_;
  active_ = this;
}

X11ErrorTrap::~X11ErrorTrap() {
  synchronize();
  if (active_ == this) active_ = previous_;
}

bool X11ErrorTrap::ok() noexcept {
  synchronize();
  return error_ == 0;
}

void X11ErrorTrap::record(_XDisplay *display, const unsigned long serial,
                          const int error) noexcept {
  if (active_ != nullptr && active_->display_ == display &&
      serial >= active_->first_serial_ && active_->error_ == 0)
    active_->error_ = error;
}

void X11ErrorTrap::synchronize() noexcept {
  if (display_ != nullptr)
    XSync(reinterpret_cast<Display *>(display_), False);
}

namespace {

X11Extension query_xi2(Display *display) noexcept {
  int opcode{};
  int event{};
  int error{};
  if (XQueryExtension(display, "XInputExtension", &opcode, &event, &error) ==
      False)
    return {};
  int major = 2;
  int minor = 0;
  X11ErrorTrap trap{display};
  if (XIQueryVersion(display, &major, &minor) != Success || !trap.ok())
    return {};
  return {true, major, minor, opcode, event, error};
}

X11Extension query_shape(Display *display) noexcept {
  int major{};
  int minor{};
  int opcode{};
  int event{};
  int error{};
  if (XQueryExtension(display, "SHAPE", &opcode, &event, &error) == False)
    return {};
  X11ErrorTrap trap{display};
  if (XShapeQueryVersion(display, &major, &minor) == False || !trap.ok())
    return {};
  return {true, major, minor, opcode, event, error};
}

X11Extension query_randr(Display *display) noexcept {
  int major{};
  int minor{};
  int opcode{};
  int event{};
  int error{};
  if (XQueryExtension(display, "RANDR", &opcode, &event, &error) == False)
    return {};
  X11ErrorTrap trap{display};
  if (XRRQueryVersion(display, &major, &minor) == False || !trap.ok())
    return {};
  return {true, major, minor, opcode, event, error};
}

X11Extension query_composite(Display *display) noexcept {
  int event{};
  int error{};
  if (XCompositeQueryExtension(display, &event, &error) == False) return {};
  int major{};
  int minor{};
  X11ErrorTrap trap{display};
  if (XCompositeQueryVersion(display, &major, &minor) == False || !trap.ok())
    return {};
  return {true, major, minor, 0, event, error};
}

X11Extension query_damage(Display *display) noexcept {
  int event{};
  int error{};
  if (XDamageQueryExtension(display, &event, &error) == False) return {};
  int major{};
  int minor{};
  X11ErrorTrap trap{display};
  if (XDamageQueryVersion(display, &major, &minor) == False || !trap.ok())
    return {};
  return {true, major, minor, 0, event, error};
}

X11Extension query_xfixes(Display *display) noexcept {
  int event{};
  int error{};
  if (XFixesQueryExtension(display, &event, &error) == False) return {};
  int major{};
  int minor{};
  X11ErrorTrap trap{display};
  if (XFixesQueryVersion(display, &major, &minor) == False || !trap.ok())
    return {};
  return {true, major, minor, 0, event, error};
}

// EWMH is a convention, not an extension: a compliant WM advertises itself on
// the root window. Without it, window tracking degrades to the child scan.
bool query_ewmh(Display *display, ::Window root) noexcept {
  const Atom supporting =
      XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", True);
  const Atom client_list = XInternAtom(display, "_NET_CLIENT_LIST", True);
  if (supporting == None || client_list == None) return false;
  X11ErrorTrap trap{display};
  Atom type{};
  int format{};
  unsigned long count{};
  unsigned long remaining{};
  unsigned char *data{};
  const Status status =
      XGetWindowProperty(display, root, supporting, 0, 1, False, XA_WINDOW,
                         &type, &format, &count, &remaining, &data);
  if (status != Success || !trap.ok() || type != XA_WINDOW || count != 1) {
    if (data != nullptr) XFree(data);
    return false;
  }
  const auto owner = *reinterpret_cast<const ::Window *>(data);
  XFree(data);
  return owner != None;
}

X11Probe probe_display(Display *display, ::Window root) noexcept {
  X11Probe probe;
  probe.connected = true;
  probe.xi2 = query_xi2(display);
  probe.shape = query_shape(display);
  probe.randr = query_randr(display);
  probe.composite = query_composite(display);
  probe.damage = query_damage(display);
  probe.xfixes = query_xfixes(display);
  probe.ewmh = query_ewmh(display, root);
  return probe;
}

bool version_at_least(const X11Extension &extension, const int major,
                      const int minor) noexcept {
  return extension.major > major ||
         (extension.major == major && extension.minor >= minor);
}

std::string extension_reason(std::string_view name,
                             const X11Extension &extension,
                             const int required_major,
                             const int required_minor) {
  if (!extension.present)
    return std::string{name} + ": extension not advertised by the X server";
  if (!version_at_least(extension, required_major, required_minor))
    return std::string{name} + ": server advertises " +
           std::to_string(extension.major) + "." +
           std::to_string(extension.minor) + ", " +
           std::to_string(required_major) + "." +
           std::to_string(required_minor) + " required";
  return {};
}

} // namespace

std::vector<std::string> X11Probe::unavailable() const {
  std::vector<std::string> reasons;
  if (!connected) {
    reasons.push_back("x11: no display available ($DISPLAY unset or unreachable)");
    return reasons;
  }
  if (auto reason = extension_reason("xi2", xi2, 2, 0); !reason.empty())
    reasons.push_back(reason);
  if (auto reason = extension_reason("shape", shape, 1, 0); !reason.empty())
    reasons.push_back(reason);
  if (auto reason = extension_reason("randr", randr, 1, 2); !reason.empty())
    reasons.push_back(reason);
  if (auto reason = extension_reason("composite", composite, 0, 2);
      !reason.empty())
    reasons.push_back(reason);
  if (auto reason = extension_reason("damage", damage, 1, 0);
      !reason.empty())
    reasons.push_back(reason);
  if (auto reason = extension_reason("xfixes", xfixes, 2, 0);
      !reason.empty())
    reasons.push_back(reason);
  if (!ewmh)
    reasons.push_back(
        "ewmh: no _NET_SUPPORTING_WM_CHECK on the root window (window "
        "tracking falls back to a bounded child scan)");
  return reasons;
}

X11Display::X11Display(struct _XDisplay *display, X11Probe probe) noexcept
    : display_(display), probe_(probe) {}

X11Display::X11Display(X11Display &&other) noexcept
    : display_(other.display_), probe_(other.probe_) {
  other.display_ = nullptr;
}

X11Display &X11Display::operator=(X11Display &&other) noexcept {
  if (this != &other) {
    close();
    display_ = other.display_;
    probe_ = other.probe_;
    other.display_ = nullptr;
  }
  return *this;
}

X11Display::~X11Display() { close(); }

void X11Display::close() noexcept {
  if (display_ != nullptr) {
    XCloseDisplay(reinterpret_cast<Display *>(display_));
    display_ = nullptr;
  }
}

std::optional<X11Display> X11Display::open(std::string &error) {
  auto result = open_result();
  error = std::move(result.message);
  return std::move(result.display);
}

X11OpenResult X11Display::open_result() {
  if (std::getenv("DISPLAY") == nullptr) {
    return {.display = std::nullopt,
            .error = X11OpenError::no_display_variable,
            .message = "x11: DISPLAY is not set"};
  }
  install_x11_error_handler();
  Display *display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    return {.display = std::nullopt,
            .error = X11OpenError::cannot_open_display,
            .message = "x11: cannot open display"};
  }
  X11Probe probe = probe_display(display, DefaultRootWindow(display));
  return {.display = X11Display{reinterpret_cast<struct _XDisplay *>(display),
                                probe},
          .error = std::nullopt,
          .message = {}};
}

int X11Display::fd() const noexcept {
  if (display_ == nullptr) return -1;
  return ConnectionNumber(reinterpret_cast<Display *>(display_));
}

std::uint64_t X11Display::root() const noexcept {
  if (display_ == nullptr) return 0;
  return static_cast<std::uint64_t>(DefaultRootWindow(
      reinterpret_cast<Display *>(display_)));
}

std::uint64_t X11Display::atom(std::string_view name) const noexcept {
  if (display_ == nullptr || name.empty()) return 0;
  const std::string owned{name};
  X11ErrorTrap trap{display_};
  const Atom atom = XInternAtom(reinterpret_cast<Display *>(display_),
                                owned.c_str(), True);
  if (!trap.ok()) return 0;
  return static_cast<std::uint64_t>(atom);
}

std::string X11Display::atom_name(std::uint64_t atom) const {
  if (display_ == nullptr || atom == 0) return {};
  X11ErrorTrap trap{display_};
  char *name =
      XGetAtomName(reinterpret_cast<Display *>(display_),
                   static_cast<Atom>(atom));
  if (name == nullptr || !trap.ok()) return {};
  std::string result{name};
  XFree(name);
  return result;
}

int X11Display::drain_events() noexcept {
  if (display_ == nullptr) return 0;
  auto *raw = reinterpret_cast<Display *>(display_);
  int drained = 0;
  X11ErrorTrap trap{raw};
  while (XPending(raw) > 0) {
    XEvent event{};
    XNextEvent(raw, &event);
    ++drained;
    if (!trap.ok()) break;
  }
  return drained;
}

int X11Display::pending_events() noexcept {
  if (display_ == nullptr) return 0;
  auto *raw = reinterpret_cast<Display *>(display_);
  X11ErrorTrap trap{raw};
  const int pending = XPending(raw);
  return trap.ok() && pending > 0 ? pending : 0;
}

} // namespace layerc::platform

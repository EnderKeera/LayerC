// Xlib connection + one extension probe pass (V2.6.1). Xlib is confined to
// this translation unit: every X call runs under an error trap because Xlib's
// default handler aborts the process on a protocol error (windows vanish
// between a scan and its queries).
#include "x11_display.hpp"
#include "x11_error.hpp"
#include "x11_property.hpp"

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

// Signature shared by the extension and version queries below (Xlib's Bool
// and Status are both int).
using IntPairQuery = int (*)(Display *, int *, int *);

// Extension located by name: XQueryExtension also reports its opcode.
X11Extension query_named(Display *display, const char *name,
                         const IntPairQuery query_version) noexcept {
  int opcode{};
  int event{};
  int error{};
  if (XQueryExtension(display, name, &opcode, &event, &error) == False)
    return {};
  int major{};
  int minor{};
  X11ErrorTrap trap{display};
  if (query_version(display, &major, &minor) == False || !trap.ok())
    return {};
  return {true, major, minor, opcode, event, error};
}

// Extension located through its client library: event/error bases only.
X11Extension query_library(Display *display, const IntPairQuery query_extension,
                           const IntPairQuery query_version) noexcept {
  int event{};
  int error{};
  if (query_extension(display, &event, &error) == False) return {};
  int major{};
  int minor{};
  X11ErrorTrap trap{display};
  if (query_version(display, &major, &minor) == False || !trap.ok())
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
  const auto owners =
      x11_property::read_window_list(display, root, supporting);
  return trap.ok() && !owners.empty() && owners.front() != None;
}

X11Probe probe_display(Display *display, ::Window root) noexcept {
  X11Probe probe;
  probe.connected = true;
  probe.xi2 = query_xi2(display);
  probe.shape = query_named(display, "SHAPE", &XShapeQueryVersion);
  probe.randr = query_named(display, "RANDR", &XRRQueryVersion);
  probe.composite = query_library(display, &XCompositeQueryExtension,
                                  &XCompositeQueryVersion);
  probe.damage =
      query_library(display, &XDamageQueryExtension, &XDamageQueryVersion);
  probe.xfixes =
      query_library(display, &XFixesQueryExtension, &XFixesQueryVersion);
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
  struct Requirement {
    std::string_view name;
    const X11Extension &extension;
    int major;
    int minor;
  };
  const Requirement requirements[] = {
      {"xi2", xi2, 2, 0},           {"shape", shape, 1, 0},
      {"randr", randr, 1, 2},       {"composite", composite, 0, 2},
      {"damage", damage, 1, 0},     {"xfixes", xfixes, 2, 0},
  };
  for (const auto &[name, extension, major, minor] : requirements)
    if (auto reason = extension_reason(name, extension, major, minor);
        !reason.empty())
      reasons.push_back(std::move(reason));
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

int X11Display::pending_events() noexcept {
  if (display_ == nullptr) return 0;
  auto *raw = reinterpret_cast<Display *>(display_);
  X11ErrorTrap trap{raw};
  const int pending = XPending(raw);
  return trap.ok() && pending > 0 ? pending : 0;
}

} // namespace layerc::platform

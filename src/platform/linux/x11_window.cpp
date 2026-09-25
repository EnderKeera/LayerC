// EWMH window tracking + executable resolution (V2.6.1). Xlib stays in this
// translation unit; every query is guarded because windows disappear between
// the scan and the per-window property reads.
#include "x11_window.hpp"

#include "layerc/core/game_match.hpp"
#include "x11_error.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace layerc::platform {
namespace {

constexpr unsigned long no_length = ~0UL;

struct RawProperty {
  Atom type{None};
  int format{};
  unsigned long count{};
  unsigned char *data{};

  ~RawProperty() {
    if (data != nullptr) XFree(data);
  }
  RawProperty() = default;
  RawProperty(const RawProperty &) = delete;
  RawProperty &operator=(const RawProperty &) = delete;
  RawProperty(RawProperty &&other) noexcept
      : type(other.type), format(other.format), count(other.count),
        data(other.data) {
    other.data = nullptr;
  }
  RawProperty &operator=(RawProperty &&) = delete;
};

[[nodiscard]] RawProperty read_property(Display *display, ::Window window,
                                       Atom atom, Atom type) noexcept {
  RawProperty property;
  unsigned long remaining{};
  if (atom == None) return property;
  if (XGetWindowProperty(display, window, atom, 0, no_length, False, type,
                         &property.type, &property.format, &property.count,
                         &remaining, &property.data) != Success)
    property.data = nullptr;
  return property;
}

[[nodiscard]] std::optional<std::string> read_text_property(Display *display,
                                                           ::Window window,
                                                           Atom atom) noexcept {
  RawProperty property = read_property(display, window, atom, AnyPropertyType);
  if (property.data == nullptr || property.format != 8 || property.count == 0)
    return std::nullopt;
  return std::string(reinterpret_cast<const char *>(property.data),
                     static_cast<std::size_t>(property.count));
}

[[nodiscard]] std::optional<std::uint32_t>
read_cardinal(Display *display, ::Window window, Atom atom) noexcept {
  RawProperty property = read_property(display, window, atom, XA_CARDINAL);
  if (property.data == nullptr || property.format != 32 || property.count == 0)
    return std::nullopt;
  const auto *values = reinterpret_cast<const unsigned long *>(property.data);
  return static_cast<std::uint32_t>(values[0]);
}

[[nodiscard]] std::vector<std::uint64_t>
read_atom_list(Display *display, ::Window window, Atom atom) noexcept {
  std::vector<std::uint64_t> values;
  RawProperty property = read_property(display, window, atom, XA_ATOM);
  if (property.data == nullptr || property.format != 32) return values;
  const auto *raw = reinterpret_cast<const Atom *>(property.data);
  values.assign(raw, raw + property.count);
  return values;
}

[[nodiscard]] std::vector<::Window>
read_window_list(Display *display, ::Window window, Atom atom) noexcept {
  std::vector<::Window> values;
  RawProperty property = read_property(display, window, atom, XA_WINDOW);
  if (property.data == nullptr || property.format != 32) return values;
  const auto *raw = reinterpret_cast<const ::Window *>(property.data);
  values.assign(raw, raw + property.count);
  return values;
}

// WM_NAME is either UTF8_STRING or compound text; Xlib decodes both.
[[nodiscard]] std::string read_legacy_title(Display *display,
                                            ::Window window) noexcept {
  XTextProperty text{};
  if (XGetWMName(display, window, &text) == 0 || text.value == nullptr)
    return {};
  char **list{};
  int count{};
  std::string title;
  if (Xutf8TextPropertyToTextList(display, &text, &list, &count) >= 0 &&
      count > 0 && list != nullptr && list[0] != nullptr)
    title = list[0];
  if (list != nullptr) XFreeStringList(list);
  XFree(text.value);
  return title;
}

[[nodiscard]] std::string path_basename(std::string_view path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string_view::npos ? std::string{path}
                                         : std::string{path.substr(slash + 1)};
}

struct Atoms {
  Atom net_wm_name{};
  Atom net_wm_pid{};
  Atom net_wm_state{};
  Atom state_fullscreen{};
  Atom state_hidden{};
  Atom net_client_list{};
  Atom net_active_window{};
};

[[nodiscard]] Atoms intern_atoms(Display *display) noexcept {
  Atoms atoms;
  atoms.net_wm_name = XInternAtom(display, "_NET_WM_NAME", True);
  atoms.net_wm_pid = XInternAtom(display, "_NET_WM_PID", True);
  atoms.net_wm_state = XInternAtom(display, "_NET_WM_STATE", True);
  atoms.state_fullscreen =
      XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", True);
  atoms.state_hidden = XInternAtom(display, "_NET_WM_STATE_HIDDEN", True);
  atoms.net_client_list = XInternAtom(display, "_NET_CLIENT_LIST", True);
  atoms.net_active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
  return atoms;
}

} // namespace

std::string process_executable_path(std::uint32_t pid) {
  if (pid == 0) return {};
  char directory[64];
  std::snprintf(directory, sizeof(directory), "/proc/%u", pid);
  struct stat info {};
  if (::lstat(directory, &info) != 0) return {};
  // Only same-UID PIDs (root may resolve any); everything else degrades to
  // title/class matching instead of leaking another user's executable path.
  const auto self = ::geteuid();
  if (info.st_uid != self && self != 0) return {};
  char link[64 + 5];
  std::snprintf(link, sizeof(link), "/proc/%u/exe", pid);
  char target[4096];
  const ssize_t length = ::readlink(link, target, sizeof(target) - 1);
  if (length <= 0) return {};
  target[length] = '\0';
  return std::string{target, static_cast<std::size_t>(length)};
}

X11WindowTracker::X11WindowTracker(const X11Display &display) noexcept
    : display_(&display) {
  auto *raw = reinterpret_cast<Display *>(display.raw());
  if (raw == nullptr) return;
  X11ErrorTrap trap{raw};
  XSelectInput(raw, static_cast<::Window>(display.root()), PropertyChangeMask);
  (void)trap.ok();
}

X11WindowScan X11WindowTracker::scan(const std::uint32_t limit) const {
  X11WindowScan scan;
  if (display_ == nullptr || !display_->valid()) {
    scan.warning = "x11: no display";
    return scan;
  }
  auto *display = reinterpret_cast<Display *>(display_->raw());
  const auto root = static_cast<::Window>(display_->root());
  const Atoms atoms = intern_atoms(display);

  std::vector<::Window> candidates;
  if (display_->probe().ewmh) {
    candidates = read_window_list(display, root, atoms.net_client_list);
    scan.ewmh = !candidates.empty();
    if (candidates.empty())
      scan.warning =
          "ewmh: _NET_CLIENT_LIST is empty; scanning root children instead";
  } else {
    scan.warning = "ewmh: window manager does not advertise EWMH";
  }

  if (candidates.empty()) {
    // Bounded fallback: direct children of the root window, viewable only.
    ::Window tree_root{};
    ::Window parent{};
    ::Window *children{};
    unsigned int count{};
    if (XQueryTree(display, root, &tree_root, &parent, &children, &count) == 0 ||
        children == nullptr) {
      scan.warning = "x11: XQueryTree failed";
      return scan;
    }
    candidates.assign(children, children + count);
    XFree(children);
  }

  for (const ::Window window : candidates) {
    if (scan.windows.size() >= limit) {
      scan.truncated = static_cast<std::uint32_t>(candidates.size()) -
                       static_cast<std::uint32_t>(scan.windows.size());
      break;
    }
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0) continue;
    if (attributes.map_state != IsViewable) continue;

    X11WindowSnapshot snapshot;
    snapshot.window = static_cast<std::uint64_t>(window);

    if (auto title = read_text_property(display, window, atoms.net_wm_name);
        title.has_value() && !title->empty())
      snapshot.title = *title;
    else
      snapshot.title = read_legacy_title(display, window);

    XClassHint hint{};
    if (XGetClassHint(display, window, &hint) != 0) {
      if (hint.res_class != nullptr) {
        snapshot.class_name = hint.res_class;
        XFree(hint.res_class);
      }
      if (hint.res_name != nullptr) {
        snapshot.instance_name = hint.res_name;
        XFree(hint.res_name);
      }
    }

    snapshot.pid = read_cardinal(display, window, atoms.net_wm_pid);
    if (snapshot.pid.has_value()) {
      snapshot.executable_path = process_executable_path(*snapshot.pid);
      snapshot.executable_name = path_basename(snapshot.executable_path);
    }

    snapshot.width = attributes.width;
    snapshot.height = attributes.height;
    ::Window child{};
    int root_x{};
    int root_y{};
    if (XTranslateCoordinates(display, window, root, 0, 0, &root_x, &root_y,
                              &child) == True) {
      snapshot.x = root_x;
      snapshot.y = root_y;
    } else {
      snapshot.x = attributes.x;
      snapshot.y = attributes.y;
    }

    const auto states = read_atom_list(display, window, atoms.net_wm_state);
    for (const auto state : states) {
      if (state == static_cast<std::uint64_t>(atoms.state_fullscreen))
        snapshot.fullscreen = true;
      if (state == static_cast<std::uint64_t>(atoms.state_hidden))
        snapshot.hidden = true;
    }

    scan.windows.push_back(std::move(snapshot));
  }

  if (atoms.net_active_window != None) {
    const auto active =
        read_window_list(display, root, atoms.net_active_window);
    if (!active.empty() && active[0] != None) {
      scan.active = static_cast<std::uint64_t>(active[0]);
      for (auto &snapshot : scan.windows)
        snapshot.active = snapshot.window == *scan.active;
    }
  }
  return scan;
}

bool X11WindowTracker::poll_changes() noexcept {
  if (display_ == nullptr || !display_->valid()) return false;
  auto *raw = reinterpret_cast<Display *>(display_->raw());
  const auto root = static_cast<::Window>(display_->root());
  bool changed = false;
  X11ErrorTrap trap{raw};
  while (XPending(raw) > 0) {
    XEvent event{};
    XNextEvent(raw, &event);
    if (!trap.ok()) break;
    if (event.type == PropertyNotify && event.xproperty.window == root)
      changed = true;
  }
  return changed;
}

std::optional<X11WindowSnapshot>
X11WindowTracker::find(const std::uint64_t window) const {
  if (window == 0) return std::nullopt;
  const X11WindowScan scan = this->scan();
  for (const auto &snapshot : scan.windows)
    if (snapshot.window == window) return snapshot;
  return std::nullopt;
}

const X11WindowSnapshot *match_window(const X11WindowScan &scan,
                                      const layerc::core::GameMatch &match,
                                      const std::uint64_t previous_window) {
  if (scan.windows.empty()) return nullptr;
  if (match.global) {
    if (scan.active.has_value()) {
      for (const auto &snapshot : scan.windows)
        if (snapshot.window == *scan.active) return &snapshot;
    }
    return &scan.windows.front();
  }

  // Enumeration stays here; identity precedence and the
  // foreground-preference rule live in core::resolve_game_match. POSIX
  // filesystems compare case-sensitively, so the executable path stays
  // byte-exact (exact_path=true) while every other field uses the shared
  // fold.
  std::vector<layerc::core::MatchCandidate> candidates;
  candidates.reserve(scan.windows.size());
  for (const auto &snapshot : scan.windows) {
    candidates.push_back({.executable_name = snapshot.executable_name,
                          .executable_path = snapshot.executable_path,
                          .class_name = snapshot.class_name,
                          .title = snapshot.title,
                          .active = scan.active.has_value() &&
                                  snapshot.window == *scan.active});
  }
  // The core resolver keeps the first candidate on an identity/foreground
  // tie. Put the previous target first without changing either policy.
  std::size_t previous_index{};
  for (std::size_t index{}; index < scan.windows.size(); ++index)
    if (scan.windows[index].window == previous_window) previous_index = index;
  const auto previous = candidates.begin() +
                        static_cast<std::ptrdiff_t>(previous_index);
  std::rotate(candidates.begin(), previous, previous + 1);
  const auto resolved = layerc::core::resolve_game_match(
      candidates, match, /*exact_path=*/true);
  if (!resolved.has_value()) return nullptr;
  std::size_t index = *resolved;
  if (*resolved == 0)
    index = previous_index;
  else if (*resolved <= previous_index)
    index = *resolved - 1;
  return &scan.windows[index];
}

} // namespace layerc::platform

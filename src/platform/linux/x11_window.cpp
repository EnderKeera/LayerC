// EWMH window tracking + executable resolution (V2.6.1). Xlib stays in this
// translation unit; every query is guarded because windows disappear between
// the scan and the per-window property reads.
#include "x11_window.hpp"

#include "layerc/core/game_match.hpp"
#include "x11_error.hpp"
#include "x11_property.hpp"

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

using namespace x11_property;

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

constexpr long client_events = PropertyChangeMask | StructureNotifyMask;
constexpr long root_events = client_events | SubstructureNotifyMask;

void restore_selection(Display *display, ::Window window, long previous,
                       long owned) noexcept {
  XWindowAttributes attributes{};
  if (XGetWindowAttributes(display, window, &attributes) != 0)
    XSelectInput(display, window,
                 (attributes.your_event_mask & ~owned) | (previous & owned));
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
  XWindowAttributes attributes{};
  if (XGetWindowAttributes(raw, static_cast<::Window>(display.root()), &attributes)) {
    root_previous_mask_ = attributes.your_event_mask;
    XSelectInput(raw, static_cast<::Window>(display.root()),
                 root_previous_mask_ | root_events);
    root_selected_ = trap.ok();
  }
  const char *names[] = {"_NET_WM_NAME", "WM_NAME", "WM_CLASS", "_NET_WM_PID",
                         "_NET_WM_STATE"};
  for (std::size_t index = 0; index < identity_atoms_.size(); ++index)
    identity_atoms_[index] = XInternAtom(raw, names[index], False);
}

X11WindowTracker::~X11WindowTracker() {
  if (display_ == nullptr || !display_->valid()) return;
  auto *raw = display_->raw();
  X11ErrorTrap trap{raw};
  for (const auto &watch : watched_)
    restore_selection(raw, static_cast<::Window>(watch.window),
                       watch.previous_mask, client_events);
  if (root_selected_)
    restore_selection(raw, static_cast<::Window>(display_->root()),
                       root_previous_mask_, root_events);
  (void)trap.ok();
}

void X11WindowTracker::update_watches(
    const std::vector<std::uint64_t> &windows) const {
  auto *raw = display_->raw();
  X11ErrorTrap trap{raw};
  for (auto watch = watched_.begin(); watch != watched_.end();) {
    if (std::find(windows.begin(), windows.end(), watch->window) != windows.end()) {
      ++watch;
      continue;
    }
    restore_selection(raw, static_cast<::Window>(watch->window),
                       watch->previous_mask, client_events);
    watch = watched_.erase(watch);
  }
  for (const auto window : windows) {
    if (std::any_of(watched_.begin(), watched_.end(),
                    [&](const Watch &watch) { return watch.window == window; }))
      continue;
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(raw, static_cast<::Window>(window), &attributes) == 0)
      continue;
    XSelectInput(raw, static_cast<::Window>(window),
                 attributes.your_event_mask | client_events);
    watched_.push_back({window, attributes.your_event_mask});
  }
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

  // Keep a bounded set of listed (including unmapped) clients watched, plus
  // every visible client admitted below. Root structure events discover new
  // fallback children even before they have a title or a matching profile.
  std::vector<std::uint64_t> watched;
  for (std::size_t index = 0; index < candidates.size() && index < limit; ++index)
    watched.push_back(static_cast<std::uint64_t>(candidates[index]));
  X11ErrorTrap scan_trap{display};
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

    if (std::find(watched.begin(), watched.end(), snapshot.window) == watched.end())
      watched.push_back(snapshot.window);
    scan.windows.push_back(std::move(snapshot));
  }
  update_watches(watched);

  if (atoms.net_active_window != None) {
    const auto active =
        read_window_list(display, root, atoms.net_active_window);
    if (!active.empty() && active[0] != None) {
      scan.active = static_cast<std::uint64_t>(active[0]);
      for (auto &snapshot : scan.windows)
        snapshot.active = snapshot.window == *scan.active;
    }
  }
  if (!scan_trap.ok()) scan.warning = "x11: a window changed during the scan";
  return scan;
}

bool X11WindowTracker::observe(const _XEvent &event) const noexcept {
  if (display_ == nullptr) return false;
  const auto root = static_cast<::Window>(display_->root());
  const auto tracked = [&](::Window window) {
    return std::any_of(watched_.begin(), watched_.end(),
                       [&](const Watch &watch) { return watch.window == window; });
  };
  switch (event.type) {
  case PropertyNotify:
    if (event.xproperty.window == root) return true;
    return tracked(event.xproperty.window) &&
           std::find(identity_atoms_.begin(), identity_atoms_.end(),
                     event.xproperty.atom) != identity_atoms_.end();
  case CreateNotify: return event.xcreatewindow.parent == root;
  case MapNotify: return event.xmap.event == root || tracked(event.xmap.window);
  case UnmapNotify: return event.xunmap.event == root || tracked(event.xunmap.window);
  case DestroyNotify:
    return event.xdestroywindow.event == root || tracked(event.xdestroywindow.window);
  case ConfigureNotify:
    return event.xconfigure.event == root || event.xconfigure.window == root ||
           tracked(event.xconfigure.window);
  case ReparentNotify:
    return event.xreparent.event == root || tracked(event.xreparent.window);
  default: return false;
  }
}

bool X11WindowTracker::poll_changes() noexcept {
  if (display_ == nullptr || !display_->valid()) return false;
  bool changed = false;
  X11Display::dispatch_events(display_->raw(), [&](XEvent &event) {
    changed = observe(event) || changed;
  });
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
  // Enumeration stays here; the matching rule and the foreground/previous
  // preference live in core::select_game_candidate. POSIX filesystems
  // compare case-sensitively, so the executable path stays byte-exact.
  std::vector<layerc::core::MatchCandidate> candidates;
  candidates.reserve(scan.windows.size());
  std::optional<std::size_t> preferred;
  for (std::size_t index{}; index < scan.windows.size(); ++index) {
    const auto &snapshot = scan.windows[index];
    candidates.push_back({.executable_name = snapshot.executable_name,
                          .executable_path = snapshot.executable_path,
                          .class_name = snapshot.class_name,
                          .title = snapshot.title,
                          .active = scan.active.has_value() &&
                                  snapshot.window == *scan.active});
    if (previous_window != 0 && snapshot.window == previous_window)
      preferred = index;
  }
  const auto resolved = layerc::core::select_game_candidate(
      candidates, match, {.exact_path = true, .preferred = preferred});
  return resolved.has_value() ? &scan.windows[*resolved] : nullptr;
}

} // namespace layerc::platform

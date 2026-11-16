// ATK bridge lifetime and GLib readiness integrated into FLTK's event loop.
#include "editor_accessibility.hpp"
#include "accessibility_tree.hpp"

#include <FL/Fl.H>
#include <atk-bridge.h>
#include <atspi/atspi.h>
#include <link.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace layerc::editor {
namespace {
// ATK, its adaptor and libatspi ship together from at-spi2-core. Releases
// before 2.62 crash in the adaptor's cache cleanup (GNOME at-spi2-core
// c4ddb104, 8db3f580), so every loaded copy must be one coherent >=2.62 set.
std::string bridge_closure_fault() {
  struct Loaded {
    std::string atk, bridge, atspi;
    int copies{};
  } loaded;
  dl_iterate_phdr(
      [](dl_phdr_info *info, std::size_t, void *data) {
        auto &found = *static_cast<Loaded *>(data);
        const std::string_view path = info->dlpi_name ? info->dlpi_name : "";
        const auto name = path.substr(path.rfind('/') + 1);
        if (name.starts_with("libatk-1.0.so")) found.atk = path, ++found.copies;
        else if (name.starts_with("libatk-bridge-2.0.so")) found.bridge = path, ++found.copies;
        else if (name.starts_with("libatspi.so")) found.atspi = path, ++found.copies;
        return 0;
      },
      &loaded);
  if (loaded.atk.empty() || loaded.bridge.empty() || loaded.atspi.empty())
    return "the ATK/AT-SPI libraries are not all loaded";
  if (loaded.copies != 3) return "more than one copy of an ATK/AT-SPI library is loaded";
  const auto directory = [](const std::string &path) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    return error ? std::string() : canonical.parent_path().string();
  };
  const auto home = directory(loaded.atk);
  if (home.empty() || directory(loaded.bridge) != home || directory(loaded.atspi) != home)
    return "the ATK/AT-SPI libraries come from different installations";
  gint major{}, minor{}, micro{};
  atspi_get_version(&major, &minor, &micro);
  const auto old = [](guint a, guint b) { return a < 2 || (a == 2 && b < 62); };
  if (old(atk_get_major_version(), atk_get_minor_version()) ||
      old(static_cast<guint>(major), static_cast<guint>(minor)))
    return "ATK/AT-SPI 2.62 or later is required";
  return {};
}

} // namespace

struct EditorAccessibility::Impl {
  accessibility::AccessibilityTree tree;
  std::vector<GPollFD> descriptors;
  std::string fault_reason;
  bool initialized{};
  bool pumping{};
  bool gtk_chooser{};
  GMainContext *context{g_main_context_default()};

  explicit Impl(EditorApp &) {
    if (!tree.initialize(fault_reason)) return;
    if (const auto *disabled = g_getenv("NO_AT_BRIDGE"); disabled && std::string_view(disabled) == "1") {
      fault_reason = "disabled by NO_AT_BRIDGE=1";
      return;
    }
    if (auto closure = bridge_closure_fault(); !closure.empty()) {
      fault_reason = std::move(closure);
      return;
    }
    // The adaptor's init succeeds without a bus and exports nothing; check
    // first so the window shows why. The adaptor reuses this connection.
    if (!atspi_get_a11y_bus()) {
      fault_reason = "no accessibility bus is reachable";
      return;
    }
    // The whole tree exists before the adaptor registers its listeners.
    tree.reconcile(true);
    if (atk_bridge_adaptor_init(nullptr, nullptr) != 0) {
      fault_reason = "no accessibility bus is reachable";
      return;
    }
    initialized = true;
    // GTK's file chooser would install a second ATK implementation in this
    // process; use FLTK's own chooser, whose widgets this bridge exports.
    gtk_chooser = Fl::option(Fl::OPTION_FNFC_USES_GTK);
    Fl::option(Fl::OPTION_FNFC_USES_GTK, false);
    atk_bridge_set_event_context(context);
    Fl::add_check(check, this);
    pump();
  }
  ~Impl() {
    // An instance refused because another owns the bridge holds no state.
    if (!tree.owns_root()) return;
    accessibility::cancel_deferred();
    Fl::remove_check(check, this);
    Fl::remove_timeout(wake, this);
    remove_descriptors();
    if (initialized) Fl::option(Fl::OPTION_FNFC_USES_GTK, gtk_chooser);
    // Keep accessibles alive while the adaptor retires its leases and weak
    // references. Detach the tree first so its cleanup sees an empty root.
    tree.detach();
    if (initialized) atk_bridge_adaptor_cleanup();
    tree.release();
    accessibility::clear_root();
  }
  void remove_descriptors() {
    for (const auto &fd : descriptors) Fl::remove_fd(fd.fd, FL_READ | FL_WRITE | FL_EXCEPT);
    descriptors.clear();
  }
  static void ready(int, void *self) { static_cast<Impl *>(self)->pump(); }
  static void wake(void *self) { static_cast<Impl *>(self)->pump(); }
  static void check(void *self) { static_cast<Impl *>(self)->pump(); }
  void pump() {
    if (pumping || !initialized || !g_main_context_acquire(context)) return;
    pumping = true;
    {
      // One pass spans the syncs and the adaptor's dispatch: requests only read
      // widgets or defer callbacks, so no menu changes until pump returns.
      accessibility::TreePass pass;
      tree.sync();
      // Integrate GLib's readiness descriptors/deadlines into FLTK's blocking
      // loop: no fixed-rate timer, busy idle callback or auxiliary UI thread.
      bool dispatched = false;
      for (unsigned i = 0; i < 64 && g_main_context_pending(context); ++i)
        dispatched = g_main_context_iteration(context, FALSE) || dispatched;
      if (dispatched) tree.sync();
    }
    remove_descriptors();
    gint priority = 0, timeout = -1;
    g_main_context_prepare(context, &priority);
    const gint count = g_main_context_query(context, priority, &timeout, nullptr, 0);
    descriptors.resize(static_cast<std::size_t>(std::max(count, 0)));
    g_main_context_query(context, priority, &timeout, descriptors.data(), count);
    for (const auto &fd : descriptors) {
      int events = 0;
      if (fd.events & G_IO_IN) events |= FL_READ;
      if (fd.events & G_IO_OUT) events |= FL_WRITE;
      if (fd.events & (G_IO_ERR | G_IO_HUP)) events |= FL_EXCEPT;
      if (events) Fl::add_fd(fd.fd, events, ready, this);
    }
    Fl::remove_timeout(wake, this);
    if (timeout >= 0) Fl::add_timeout(static_cast<double>(timeout) / 1000.0, wake, this);
    pumping = false;
    g_main_context_release(context);
  }
};

EditorAccessibility::EditorAccessibility(EditorApp &app) : impl_(std::make_unique<Impl>(app)) {}
EditorAccessibility::~EditorAccessibility() = default;
bool EditorAccessibility::available() const { return impl_->initialized; }
const std::string &EditorAccessibility::fault() const { return impl_->fault_reason; }
void EditorAccessibility::announce(Fl_Widget *source, const std::string &text) {
  if (!impl_->initialized || text.empty()) return;
  // pump() syncs the tree first, so the widget the caller just wrote is
  // already exported; while a pass runs it returns without nesting one.
  impl_->pump();
  impl_->tree.announce(source, text);
}
} // namespace layerc::editor

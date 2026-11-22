#include "platform/win32/window_chrome.hpp"

#include "editor_style.hpp"
#include "platform/editor_options.hpp"
#include "platform/window_chrome.hpp"
#include "platform/win32/window_settings.hpp"

#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/window_query.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>

namespace layerc::editor {
namespace {

WindowsWindowDecorator *decorator{};
bool capture_exclusion{};

HWND root_of(const Fl_Window &window) {
  return GetAncestor(fl_xid(&window), GA_ROOT);
}

} // namespace

WindowsWindowDecorator::WindowsWindowDecorator() {
  decorator = this;
  Fl::add_system_handler(before_paint, this);
}

WindowsWindowDecorator::~WindowsWindowDecorator() {
  set_editor_capture_exclusion(false);
  Fl::remove_system_handler(before_paint);
  decorator = nullptr;
}

void WindowsWindowDecorator::prune() {
  std::erase_if(decorated_, [](const auto &entry) { return IsWindow(entry.first) == FALSE; });
}

int WindowsWindowDecorator::before_paint(void *event, void *data) {
  const auto &message = *static_cast<const MSG *>(event);
  if (message.message == WM_PAINT) {
    const HWND root = GetAncestor(message.hwnd, GA_ROOT);
    if (auto *window = fl_find(root); root != nullptr && window) {
      restore_windows_screen_text_scale(window->screen_num());
      auto &self = *static_cast<WindowsWindowDecorator *>(data);
      self.prune();
      self.decorate(root);
    }
  }
  return 0;
}

void WindowsWindowDecorator::decorate(const HWND root) {
  if (root == nullptr) return;
  auto &last = decorated_[root];
  if (last == generation_) return;
  last = generation_;
  if ((GetWindowLongPtrW(root, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) {
    const auto toolbar = current_editor_toolbar_colors();
    platform::set_dark_title_bar(root);
    platform::set_window_frame_colors(root, platform::native_color_ref(toolbar.fill),
                                      platform::native_color_ref(current_editor_tokens().divider),
                                      platform::native_color_ref(toolbar.text));
  }
  if (capture_exclusion) {
    DWORD affinity{};
    if (!GetWindowDisplayAffinity(root, &affinity) ||
        affinity != platform::capture_exclusion_affinity)
      (void)platform::apply_capture_exclusion(root, true);
  }
}

void WindowsWindowDecorator::refresh() {
  ++generation_;
  prune();
  for (auto *window = Fl::first_window(); window; window = Fl::next_window(window))
    decorate(root_of(*window));
}

void WindowsWindowDecorator::clear_capture_exclusion() {
  for (auto *window = Fl::first_window(); window; window = Fl::next_window(window))
    if (const auto root = root_of(*window))
      (void)platform::apply_capture_exclusion(root, false);
}

void refresh_editor_window_chrome() {
  if (decorator) decorator->refresh();
}

bool editor_capture_exclusion_enabled() noexcept { return capture_exclusion; }

void set_editor_capture_exclusion(const bool enabled) {
  if (capture_exclusion == enabled) return;
  capture_exclusion = enabled;
  if (decorator && !enabled) decorator->clear_capture_exclusion();
  refresh_editor_window_chrome();
}

} // namespace layerc::editor

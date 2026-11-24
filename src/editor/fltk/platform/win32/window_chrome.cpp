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

#include <algorithm>
#include <atomic>
#include <utility>

namespace layerc::editor {
namespace {

WindowsWindowDecorator *decorator{};
bool capture_requested{};
std::atomic<UINT_PTR> lifetime_counter{};

UINT_PTR next_lifetime_token() {
  UINT_PTR token{};
  while (!token) token = lifetime_counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return token;
}

LPCWSTR lifetime_property(const ATOM atom) { return MAKEINTATOM(atom); }

HWND root_of(const Fl_Window &window) {
  return GetAncestor(fl_xid(&window), GA_ROOT);
}

} // namespace

WindowsWindowDecorator::WindowsWindowDecorator()
    : WindowsWindowDecorator(platform::apply_capture_exclusion) {}

WindowsWindowDecorator::WindowsWindowDecorator(SetAffinity set_affinity, ReadAffinity read_affinity,
                                               SetFrameColors set_frame_colors)
    : set_affinity_(set_affinity), read_affinity_(read_affinity),
      set_frame_colors_(set_frame_colors ? set_frame_colors : platform::set_window_frame_colors),
      lifetime_atom_(GlobalAddAtomW(L"LayerC.Editor.CaptureLifetime")) {
  decorator = this;
  Fl::add_system_handler(before_paint, this);
  Fl::add_check(before_wait, this);
}

WindowsWindowDecorator::~WindowsWindowDecorator() {
  Fl::remove_check(before_wait, this);
  Fl::remove_system_handler(before_paint);
  prune();
  for (const auto &[root, state] : decorated_) {
    if (state.affinity != Affinity::untouched) (void)set_affinity_(root, false);
    RemovePropW(root, lifetime_property(lifetime_atom_));
  }
  // The property is integer data and the window manager drops it with the
  // window. A crash leaks one reference to the same named atom.
  if (lifetime_atom_) GlobalDeleteAtom(lifetime_atom_);
  capture_requested = false;
  decorator = nullptr;
}

void WindowsWindowDecorator::prune() {
  std::erase_if(decorated_, [this](const auto &entry) {
    const auto &[root, state] = entry;
    return !IsWindow(root) || GetWindowThreadProcessId(root, nullptr) != GetCurrentThreadId() ||
           !state.token || reinterpret_cast<UINT_PTR>(GetPropW(root, lifetime_property(lifetime_atom_))) !=
                               state.token;
  });
}

int WindowsWindowDecorator::before_paint(void *event, void *data) {
  const auto &message = *static_cast<const MSG *>(event);
  if (message.message == WM_PAINT) {
    const HWND root = GetAncestor(message.hwnd, GA_ROOT);
    if (auto *window = fl_find(root); root != nullptr && window) {
      restore_windows_screen_text_scale(window->screen_num());
      auto &self = *static_cast<WindowsWindowDecorator *>(data);
      self.prune();
      (void)self.find_or_enroll(root, false);
      self.update_warning();
    }
  }
  return 0;
}

void WindowsWindowDecorator::before_wait(void *data) {
  auto &self = *static_cast<WindowsWindowDecorator *>(data);
  if (!capture_requested && self.warning_.empty()) return;
  self.prune();
  // Cover new windows even when FLTK draws them during a flush rather than
  // dispatching WM_PAINT. An event-loop check does not wake an idle editor.
  for (auto *window = Fl::first_window(); window; window = Fl::next_window(window)) {
    const auto root = root_of(*window);
    if (root == nullptr) continue;
    if (!self.decorated_.contains(root)) (void)self.enroll(root);
  }
  self.update_warning();
}

WindowsWindowDecorator::WindowState &
WindowsWindowDecorator::find_or_enroll(const HWND root, const bool recheck_affinity) {
  const auto entry = decorated_.find(root);
  if (entry == decorated_.end()) return enroll(root);
  auto &state = entry->second;
  decorate(root, state);
  if (recheck_affinity || state.affinity != Affinity::verified) reconcile_affinity(root, state);
  return state;
}

WindowsWindowDecorator::WindowState &WindowsWindowDecorator::enroll(const HWND root) {
  auto &state = decorated_.try_emplace(root).first->second;
  const auto token = next_lifetime_token();
  if (!lifetime_atom_ || GetWindowThreadProcessId(root, nullptr) != GetCurrentThreadId() ||
      !SetPropW(root, lifetime_property(lifetime_atom_), reinterpret_cast<HANDLE>(token))) {
    if (capture_requested) state.affinity = Affinity::failed;
  } else {
    state.token = token;
    reconcile_affinity(root, state);
  }
  decorate(root, state);
  return state;
}

void WindowsWindowDecorator::decorate(const HWND root, WindowState &state) {
  if (state.caption_generation != generation_) {
    state.caption_generation = generation_;
    if ((GetWindowLongPtrW(root, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) {
      const auto toolbar = current_editor_toolbar_colors();
      platform::set_dark_title_bar(root);
      set_frame_colors_(root, platform::native_color_ref(toolbar.fill),
                        platform::native_color_ref(current_editor_tokens().divider),
                        platform::native_color_ref(toolbar.text));
    }
  }
}

void WindowsWindowDecorator::reconcile_affinity(const HWND root, WindowState &state) {
  if (!state.token) return;
  if (!capture_requested && state.affinity == Affinity::untouched) return;
  const DWORD desired = capture_requested ? platform::capture_exclusion_affinity : WDA_NONE;
  const auto matches = [this, root, desired] {
    DWORD affinity{};
    return read_affinity_(root, &affinity) && affinity == desired;
  };
  if (matches() || (set_affinity_(root, capture_requested) && matches()))
    state.affinity = capture_requested ? Affinity::verified : Affinity::untouched;
  else
    state.affinity = Affinity::failed;
}

void WindowsWindowDecorator::update_warning() {
  const bool failed = std::ranges::any_of(decorated_, [](const auto &entry) {
    return entry.second.affinity == Affinity::failed;
  });
  std::string_view next;
  if (failed) next = capture_requested ? capture_exclusion_incomplete : capture_exclusion_not_cleared;
  if (next == warning_) return;
  warning_ = next;
  if (report_) report_(warning_);
}

void WindowsWindowDecorator::set_warning_reporter(WarningReporter report) {
  report_ = std::move(report);
  if (report_ && !warning_.empty()) report_(warning_);
}

void WindowsWindowDecorator::refresh() {
  ++generation_;
  prune();
  for (auto *window = Fl::first_window(); window; window = Fl::next_window(window))
    if (const auto root = root_of(*window)) (void)find_or_enroll(root, true);
  update_warning();
}

bool WindowsWindowDecorator::verify(Fl_Window &window) {
  prune();
  const auto root = root_of(window);
  const bool verified = root && find_or_enroll(root, true).affinity == Affinity::verified;
  update_warning();
  return verified;
}

void WindowsWindowDecorator::adopt_for_test(const HWND root, const bool failed,
                                             const bool token_matches) {
  const auto token = next_lifetime_token();
  if (token_matches)
    SetPropW(root, lifetime_property(lifetime_atom_), reinterpret_cast<HANDLE>(token));
  decorated_.insert_or_assign(root, WindowState{token, generation_,
      failed ? Affinity::failed : Affinity::verified});
  update_warning();
}

bool verify_editor_window_capture(Fl_Window &window) {
  return decorator && decorator->verify(window);
}

void refresh_editor_window_chrome() {
  if (decorator) decorator->refresh();
}

bool editor_capture_exclusion_requested() noexcept { return capture_requested; }

std::string_view editor_capture_exclusion_warning() noexcept {
  return decorator ? decorator->capture_warning() : std::string_view{};
}

void set_editor_capture_exclusion(const bool enabled) {
  capture_requested = enabled;
  refresh_editor_window_chrome();
}

} // namespace layerc::editor

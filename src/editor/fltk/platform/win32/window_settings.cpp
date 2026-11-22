#include "window_settings.hpp"

#include "settings_store.hpp"
#include "layerc/platform/window_query.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace layerc::editor {
namespace {

int applied_text_scale = text_scale_choices.front();
bool capture_exclusion{};
const WindowsTextScaleShortcuts::Reporter *scale_reporter{};

void apply_text_scale(const int percent) {
  if (percent == applied_text_scale) return;
  const float ratio = static_cast<float>(percent) / static_cast<float>(applied_text_scale);
  applied_text_scale = percent;
  multiply_windows_screen_scales(ratio);
}

int text_scale_shortcut(const int event) {
  if (event != FL_SHORTCUT || !Fl::event_command() || Fl::grab()) return 0;
  const auto current = std::ranges::find(text_scale_choices, applied_text_scale);
  const bool simple_plus = Fl::option(Fl::OPTION_SIMPLE_ZOOM_SHORTCUT) &&
      (Fl::event_state() & (FL_META | FL_ALT | FL_CTRL | FL_SHIFT)) == FL_COMMAND &&
      Fl::event_key() == '=';
  auto next = current;
  if (Fl::test_shortcut(FL_COMMAND | '+') || simple_plus) {
    if (std::next(current) != text_scale_choices.end()) ++next;
  } else if (Fl::test_shortcut(FL_COMMAND | '-')) {
    if (current != text_scale_choices.begin()) --next;
  } else if (Fl::test_shortcut(FL_COMMAND | '0')) {
    next = text_scale_choices.begin();
  } else {
    return 0;
  }

  if (next != current) {
    const auto written = platform::write_editor_text_scale(static_cast<std::uint32_t>(*next));
    apply_text_scale(*next);
    if (written)
      (*scale_reporter)(StatusKind::success, "Text size " + std::to_string(*next) + " %.");
    else
      (*scale_reporter)(StatusKind::warning, "Could not save the text size.");
  }
  return 1;
}

int capture_before_paint(void *event, void *) {
  const auto &message = *static_cast<const MSG *>(event);
  if (message.message == WM_PAINT && fl_find(message.hwnd)) {
    const HWND root = GetAncestor(message.hwnd, GA_ROOT);
    DWORD affinity{};
    if (root != nullptr &&
        (!GetWindowDisplayAffinity(root, &affinity) ||
         affinity != platform::capture_exclusion_affinity)) {
      (void)platform::apply_capture_exclusion(root, true);
    }
  }
  return 0;
}

} // namespace

void multiply_windows_screen_scales(const float ratio) {
  // On equal-DPI monitors FLTK's setter changes every screen at once.
  const int count = Fl::screen_count();
  std::vector<float> target_scales;
  target_scales.reserve(static_cast<std::size_t>(count));
  for (int screen = 0; screen < count; ++screen)
    target_scales.push_back(Fl::screen_scale(screen) * ratio);
  for (int screen = 0; screen < count; ++screen) {
    const float target = target_scales[static_cast<std::size_t>(screen)];
    if (Fl::screen_scale(screen) != target) Fl::screen_scale(screen, target);
  }
}

void load_startup_text_scale() { apply_text_scale(load_editor_text_scale()); }

int current_editor_text_scale() noexcept { return applied_text_scale; }

WindowsTextScaleShortcuts::WindowsTextScaleShortcuts(Reporter report)
    : report_(std::move(report)) {
  scale_reporter = &report_;
  Fl::add_handler(text_scale_shortcut);
}

WindowsTextScaleShortcuts::~WindowsTextScaleShortcuts() {
  Fl::remove_handler(text_scale_shortcut);
  scale_reporter = nullptr;
}

bool editor_capture_exclusion_enabled() noexcept { return capture_exclusion; }

void set_editor_capture_exclusion(const bool enabled) {
  if (capture_exclusion == enabled) return;
  capture_exclusion = enabled;
  if (enabled) Fl::add_system_handler(capture_before_paint, nullptr);
  else Fl::remove_system_handler(capture_before_paint);
  for (auto *window = Fl::first_window(); window != nullptr; window = Fl::next_window(window))
    (void)platform::apply_capture_exclusion(GetAncestor(fl_xid(window), GA_ROOT), enabled);
}

void apply_editor_options_live(const EditorOptions &options) {
  set_editor_capture_exclusion(options.editor_capture_exclusion);
  apply_text_scale(normalize_editor_text_scale(static_cast<std::uint32_t>(options.text_scale)));
}

} // namespace layerc::editor

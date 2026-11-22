#include "window_settings.hpp"

#include "settings_store.hpp"
#include "layerc/platform/settings_store.hpp"

#include <FL/Fl.H>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

namespace layerc::editor {
namespace {

int applied_text_scale = text_scale_choices.front();
float desktop_text_scale = 1.0F;
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

  select_editor_text_scale(*next);
  return 1;
}

} // namespace

void multiply_windows_screen_scales(const float ratio, const int target_screen) {
  if (target_screen >= 0) {
    Fl::screen_scale(target_screen, Fl::screen_scale(target_screen) * ratio);
    return;
  }
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

void set_windows_desktop_text_scale(const float ratio) {
  multiply_windows_screen_scales(ratio / desktop_text_scale);
  desktop_text_scale = ratio;
}

void restore_windows_screen_text_scale(const int screen) {
  float dpi{}, dpi_y{};
  Fl::screen_dpi(dpi, dpi_y, screen);
  const float raw = dpi / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
  const float ratio = desktop_text_scale * static_cast<float>(applied_text_scale) / 100.0F;
  // FLTK writes DPI/96 on a same-screen change; a cross-screen move keeps
  // the destination scale. Only restore a DPI-only scale, never compound it.
  if (dpi > 0 && ratio != 1.0F && std::abs(Fl::screen_scale(screen) - raw) < 1e-4F)
    multiply_windows_screen_scales(ratio, screen);
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

void select_editor_text_scale(const int percent) {
  if (percent == applied_text_scale) return;
  const auto written = platform::write_editor_text_scale(static_cast<std::uint32_t>(percent));
  apply_text_scale(percent);
  if (scale_reporter) {
    if (written)
      (*scale_reporter)(StatusKind::success, "Text size " + std::to_string(percent) + " %.");
    else
      (*scale_reporter)(StatusKind::warning, "Could not save the text size.");
  }
}

void apply_editor_options_live(const EditorOptions &options) {
  set_editor_capture_exclusion(options.editor_capture_exclusion);
  apply_text_scale(normalize_editor_text_scale(static_cast<std::uint32_t>(options.text_scale)));
}

} // namespace layerc::editor

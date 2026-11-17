#include "editor_layout_rules.hpp"
#include "a11y/uia/bridge.hpp"
#include "layerc/platform/native_theme.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/platform.H>

#include <objbase.h>

#include <algorithm>
#include <string_view>

namespace {
Fl_Color fltk_color(const COLORREF color) {
  return fl_rgb_color(GetRValue(color), GetGValue(color), GetBValue(color));
}
void close_smoke_window(void *data) {
  static_cast<Fl_Double_Window *>(data)->hide();
}
} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR command_line,
                    _In_ int) {
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(apartment)) return 1;
  struct Apartment {
    ~Apartment() { CoUninitialize(); }
  } apartment_lifetime;
  // An empty, non-shipping host, not the future main-window layout. Its size
  // follows the work area and reuses the existing editor's width floor.
  Fl_Double_Window window(
      std::min(Fl::w(), layerc::editor::main_client_min_width), Fl::h() / 2,
      "LayerE — FLTK trial");
  const layerc::platform::EditorAppearanceSettings appearance{};
  const auto colors = layerc::platform::native_theme_colors(
      appearance, layerc::platform::EditorControlRole::window);
  window.color(fltk_color(colors.fill));
  window.end();
  window.resizable(window);
  window.show();
  layerc::editor::a11y::uia::Bridge accessibility(window);
  if (accessibility.error()) return 1;
  layerc::platform::set_window_frame_colors(
      fl_xid(&window), colors.fill, colors.border, colors.text);
  const bool smoke = command_line != nullptr &&
                     std::wstring_view(command_line) == L"--smoke";
  if (smoke)
    Fl::add_timeout(0.2, close_smoke_window, &window);
  const int result = Fl::run();
  if (smoke)
    Fl::remove_timeout(close_smoke_window, &window);
  return result;
}

#include "platform/win32/desktop_listener.hpp"

#include <FL/Fl.H>

#include <utility>

namespace layerc::editor {
namespace {

constexpr wchar_t listener_class[] = L"LayerC.LayerE.FLTK.Desktop";

} // namespace

WindowsDesktopListener::WindowsDesktopListener(const HINSTANCE instance,
                                               std::function<void()> reapply)
    : instance_(instance), reapply_(std::move(reapply)) {
  WNDCLASSW definition{};
  definition.lpfnWndProc = procedure;
  definition.hInstance = instance_;
  definition.lpszClassName = listener_class;
  window_class_ = RegisterClassW(&definition);
  if (window_class_)
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, listener_class, nullptr, WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr, instance_, this);
}

WindowsDesktopListener::~WindowsDesktopListener() {
  Fl::remove_timeout(refresh, this);
  if (window_) DestroyWindow(window_);
  if (window_class_) UnregisterClassW(listener_class, instance_);
}

LRESULT CALLBACK WindowsDesktopListener::procedure(HWND window, UINT message, WPARAM wparam,
                                                   LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  } else if (message == WM_SYSCOLORCHANGE || message == WM_THEMECHANGED ||
             (message == WM_SETTINGCHANGE && wparam == SPI_SETHIGHCONTRAST)) {
    auto *self = reinterpret_cast<WindowsDesktopListener *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && !self->pending_) {
      self->pending_ = true;
      Fl::add_timeout(0.0, refresh, self);
    }
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void WindowsDesktopListener::refresh(void *data) {
  auto &self = *static_cast<WindowsDesktopListener *>(data);
  self.pending_ = false;
  self.reapply_();
}

} // namespace layerc::editor

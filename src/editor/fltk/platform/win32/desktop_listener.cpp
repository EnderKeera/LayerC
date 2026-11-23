#include "platform/win32/desktop_listener.hpp"

#include <FL/Fl.H>

#include <cstddef>
#include <utility>

namespace layerc::editor {
namespace {

constexpr wchar_t listener_class[] = L"LayerC.LayerE.FLTK.Desktop";
HINSTANCE listener_instance{};
std::size_t listener_count{};

} // namespace

WindowsDesktopListener::WindowsDesktopListener(const HINSTANCE instance,
                                               std::function<void()> reapply)
    : reapply_(std::move(reapply)) {
  if (listener_count == 0) {
    WNDCLASSW definition{};
    definition.lpfnWndProc = procedure;
    definition.hInstance = instance;
    definition.lpszClassName = listener_class;
    if (!RegisterClassW(&definition)) return;
    listener_instance = instance;
  }
  ++listener_count;
  registered_ = true;
  window_ = CreateWindowExW(WS_EX_TOOLWINDOW, listener_class, nullptr, WS_POPUP,
                            0, 0, 0, 0, nullptr, nullptr, listener_instance, this);
}

WindowsDesktopListener::~WindowsDesktopListener() {
  Fl::remove_timeout(refresh, this);
  if (window_) DestroyWindow(window_);
  if (registered_ && --listener_count == 0) {
    UnregisterClassW(listener_class, listener_instance);
    listener_instance = nullptr;
  }
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

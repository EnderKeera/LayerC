#pragma once

#include <windows.h>

#include <functional>

template <typename State>
State *bind_dialog_state(const HWND window, const UINT message,
                         const LPARAM lparam) noexcept {
  auto *state = reinterpret_cast<State *>(
      GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<State *>(lparam);
    if (state != nullptr)
      SetWindowLongPtrW(window, DWLP_USER,
                        reinterpret_cast<LONG_PTR>(state));
  }
  return state;
}
template <typename State, auto Initialize, auto Handle>
INT_PTR CALLBACK dispatch_member_dialog(const HWND window, const UINT message,
                                        const WPARAM wparam,
                                        const LPARAM lparam) {
  auto *state = reinterpret_cast<State *>(
      GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<State *>(lparam);
    if (state == nullptr)
      return FALSE;
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    std::invoke(Initialize, *state, window);
    return TRUE;
  }
  return state == nullptr
             ? FALSE
             : static_cast<INT_PTR>(
                   std::invoke(Handle, *state, window, message, wparam, lparam));
}
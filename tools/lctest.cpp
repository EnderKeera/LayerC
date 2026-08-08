#include <windows.h>

namespace {

bool active = false;
bool shift_down = false;
bool e_down = false;

void update_active(const HWND window) {
  const bool next = shift_down && e_down;
  if (next == active) return;
  active = next;
  InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK window_proc(const HWND window, const UINT message,
                             const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
  case WM_KEYDOWN:
    if (wparam == 'E') e_down = true;
    if (wparam == VK_SHIFT) shift_down = true;
    update_active(window);
    return 0;
  case WM_KEYUP:
    if (wparam == 'E') e_down = false;
    if (wparam == VK_SHIFT) shift_down = false;
    update_active(window);
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const HBRUSH background =
        CreateSolidBrush(active ? RGB(37, 95, 104) : RGB(30, 35, 41));
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(228, 232, 236));
    DrawTextW(dc, L"LC Test\n\nPress Shift+E to trigger Dash", -1, &client,
              DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    EndPaint(window, &paint);
    return 0;
  }
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(window, message, wparam, lparam);
  }
}

} // namespace

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR,
                    const int show_command) {
  const WNDCLASSW definition{
      .lpfnWndProc = window_proc,
      .hInstance = instance,
      .hCursor = LoadCursorW(nullptr, IDC_ARROW),
      .lpszClassName = L"LCTestWindow",
  };
  if (RegisterClassW(&definition) == 0) {
    return 1;
  }
  const HWND window =
      CreateWindowExW(0, definition.lpszClassName, L"LC Test",
                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000,
                      700, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) {
    return 1;
  }
  ShowWindow(window, show_command);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

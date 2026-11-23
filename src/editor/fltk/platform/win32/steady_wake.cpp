#include "steady_wake.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace layerc::editor {
namespace {

struct Wake {
  Fl_Timeout_Handler handler;
  void *data;
};

std::map<UINT_PTR, Wake> wakes;

auto find_wake(Fl_Timeout_Handler handler, void *data) {
  return std::find_if(wakes.begin(), wakes.end(), [=](const auto &entry) {
    return entry.second.handler == handler && entry.second.data == data;
  });
}

void CALLBACK on_timer(HWND, UINT, const UINT_PTR id, DWORD) {
  const auto it = wakes.find(id);
  if (it == wakes.end()) return;
  KillTimer(nullptr, id);
  const auto wake = it->second;
  wakes.erase(it);
  // Native modal loops can dispatch WM_TIMER; only FLTK runs the handler.
  Fl::add_timeout(0.0, wake.handler, wake.data);
}

} // namespace

void steady_wake_add(const double seconds, Fl_Timeout_Handler handler, void *data) {
  steady_wake_remove(handler, data);
  if (!(seconds > 0.0)) {
    Fl::add_timeout(0.0, handler, data);
    return;
  }
  const auto ms = static_cast<UINT>(std::clamp(std::ceil(seconds * 1000.0),
      static_cast<double>(USER_TIMER_MINIMUM), static_cast<double>(USER_TIMER_MAXIMUM)));
  const auto id = SetTimer(nullptr, 0, ms, on_timer);
  if (id != 0) {
    wakes.emplace(id, Wake{handler, data});
    return;
  }
  static const auto warned = std::fputs("LayerE: thread timer unavailable; using the FLTK timer.\n", stderr);
  (void)warned;
  Fl::add_timeout(seconds, handler, data);
}

void steady_wake_remove(Fl_Timeout_Handler handler, void *data) {
  const auto it = find_wake(handler, data);
  if (it != wakes.end()) {
    KillTimer(nullptr, it->first);
    wakes.erase(it);
  }
  Fl::remove_timeout(handler, data);
}

bool steady_wake_armed(Fl_Timeout_Handler handler, void *data) {
  return find_wake(handler, data) != wakes.end();
}

} // namespace layerc::editor

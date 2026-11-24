#include "steady_wake.hpp"
#include "fltk_fd.hpp"

#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>

namespace layerc::editor {
namespace {

struct Wake {
  Fl_Timeout_Handler handler;
  void *data;
};

std::map<int, Wake> wakes;
SteadyWakeFault fault_for_test{};

auto find_wake(Fl_Timeout_Handler handler, void *data) {
  return std::find_if(wakes.begin(), wakes.end(), [=](const auto &entry) {
    return entry.second.handler == handler && entry.second.data == data;
  });
}

void remove_fd(const std::map<int, Wake>::iterator it) {
  Fl::remove_fd(it->first);
  close(it->first);
  wakes.erase(it);
}

void on_timer(const int fd, void *) {
  const auto it = wakes.find(fd);
  if (it == wakes.end()) return;
  std::uint64_t expirations{};
  if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations)) return;
  const auto wake = it->second;
  remove_fd(it);
  Fl::add_timeout(0.0, wake.handler, wake.data);
}

} // namespace

void steady_wake_fault_for_test(const SteadyWakeFault fault) noexcept {
  fault_for_test = fault;
}

bool steady_wake_add(const double seconds, Fl_Timeout_Handler handler, void *data) {
  steady_wake_remove(handler, data);
  if (!(seconds > 0.0)) {
    Fl::add_timeout(0.0, handler, data);
    return true;
  }

  const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0) return false;
  if (fault_for_test == SteadyWakeFault::create || !fltk_fd_watchable(fd)) {
    close(fd);
    return false;
  }

  using namespace std::chrono;
  using Seconds = duration<double>;
  const auto max_delay = nanoseconds::max();
  const auto delay = seconds < Seconds(max_delay).count()
      ? ceil<nanoseconds>(Seconds(seconds)) : max_delay;
  const auto whole = duration_cast<std::chrono::seconds>(delay);
  itimerspec timer{};
  timer.it_value.tv_sec = static_cast<time_t>(whole.count());
  timer.it_value.tv_nsec = static_cast<long>((delay - whole).count());
  if (timerfd_settime(fd, 0, &timer, nullptr) != 0 || fault_for_test == SteadyWakeFault::arm) {
    close(fd);
    return false;
  }
  wakes.emplace(fd, Wake{handler, data});
  Fl::add_fd(fd, FL_READ, on_timer, nullptr);
  return true;
}

void steady_wake_remove(Fl_Timeout_Handler handler, void *data) {
  const auto it = find_wake(handler, data);
  if (it != wakes.end()) remove_fd(it);
  Fl::remove_timeout(handler, data);
}

bool steady_wake_armed(Fl_Timeout_Handler handler, void *data) {
  return find_wake(handler, data) != wakes.end();
}

} // namespace layerc::editor

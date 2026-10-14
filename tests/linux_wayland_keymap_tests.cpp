// The seat keymap used to translate opted-in evdev keycodes: wl_seat binding,
// wl_keyboard.keymap acceptance bounds, capability loss and seat removal,
// against a disposable in-process protocol server.
#include "wayland_connection.hpp"
#include "wayland_keymap.hpp"
#include "test_support.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using layerc::test::expect;
using layerc::test::ScopedEnvironment;
using layerc::platform::WaylandKeymap;

// Threads belong only to this fixture; the product reads its connection on
// the daemon's single EventLoop.
class SeatServer {
public:
  SeatServer() {
    int sockets[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) return;
    display_ = wl_display_create();
    if (!display_ || !wl_client_create(display_, sockets[0])) {
      ::close(sockets[0]);
      ::close(sockets[1]);
      return;
    }
    client_ = sockets[1];
    seat_ = wl_global_create(display_, &wl_seat_interface, 5, this, bind_seat);
    if (!expect(seat_ != nullptr, "create the fixture seat")) return;
    ready_ = true;
    thread_ = std::jthread([this](const std::stop_token stop) {
      while (!stop.stop_requested()) {
        std::vector<std::function<void()>> work;
        {
          const std::lock_guard lock(mutex_);
          work.swap(pending_);
        }
        for (auto &task : work) task();
        (void)wl_event_loop_dispatch(wl_display_get_event_loop(display_), 5);
        wl_display_flush_clients(display_);
      }
    });
  }
  ~SeatServer() {
    if (thread_.joinable()) { thread_.request_stop(); thread_.join(); }
    if (display_) {
      wl_display_destroy_clients(display_);
      wl_display_destroy(display_);
    }
    if (client_ >= 0) ::close(client_);
  }
  SeatServer(const SeatServer &) = delete;
  SeatServer &operator=(const SeatServer &) = delete;

  [[nodiscard]] int take_fd() noexcept { return ready_ ? std::exchange(client_, -1) : -1; }

  // Sends to every bound keyboard (and future ones) from the server thread.
  void send_keymap(std::string text, const std::uint32_t format = WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                   const std::size_t size = 0) {
    post([this, text = std::move(text), format, size] {
      keymap_ = {text, format, size};
      for (auto *keyboard : keyboards_) send(keyboard);
    });
  }
  void set_keyboard(const bool present) {
    post([this, present] {
      keyboard_ = present;
      for (auto *seat : seats_)
        wl_seat_send_capabilities(seat, present ? WL_SEAT_CAPABILITY_KEYBOARD : 0);
    });
  }
  void remove_seat() {
    post([this] {
      if (seat_) wl_global_destroy(std::exchange(seat_, nullptr));
    });
  }

private:
  struct Keymap {
    std::string text;
    std::uint32_t format{};
    std::size_t size{};
  };

  void post(std::function<void()> task) {
    const std::lock_guard lock(mutex_);
    pending_.push_back(std::move(task));
  }
  void send(wl_resource *keyboard) const {
    const int fd = ::memfd_create("layerc-keymap", MFD_CLOEXEC);
    if (fd < 0) return;
    const std::size_t size = keymap_.size ? keymap_.size : keymap_.text.size() + 1;
    if (::ftruncate(fd, static_cast<off_t>(size)) == 0 &&
        ::pwrite(fd, keymap_.text.c_str(), std::min(size, keymap_.text.size() + 1), 0) >= 0)
      wl_keyboard_send_keymap(keyboard, keymap_.format, fd, static_cast<std::uint32_t>(size));
    ::close(fd);
  }

  static void bind_seat(wl_client *client, void *data, std::uint32_t version, std::uint32_t id) {
    auto &self = *static_cast<SeatServer *>(data);
    auto *seat = wl_resource_create(client, &wl_seat_interface, static_cast<int>(version), id);
    if (!seat) return;
    static const struct wl_seat_interface implementation{
        [](wl_client *, wl_resource *, std::uint32_t) {},
        [](wl_client *requester, wl_resource *resource, std::uint32_t keyboard_id) {
          auto &server = *static_cast<SeatServer *>(wl_resource_get_user_data(resource));
          auto *keyboard = wl_resource_create(requester, &wl_keyboard_interface,
                                              wl_resource_get_version(resource), keyboard_id);
          if (!keyboard) return;
          static const struct wl_keyboard_interface keyboard_implementation{
              [](wl_client *, wl_resource *released) { wl_resource_destroy(released); }};
          wl_resource_set_implementation(keyboard, &keyboard_implementation, &server,
              [](wl_resource *gone) {
                auto &owner = *static_cast<SeatServer *>(wl_resource_get_user_data(gone));
                std::erase(owner.keyboards_, gone);
              });
          server.keyboards_.push_back(keyboard);
          server.send(keyboard);
        },
        [](wl_client *, wl_resource *, std::uint32_t) {},
        [](wl_client *, wl_resource *released) { wl_resource_destroy(released); }};
    wl_resource_set_implementation(seat, &implementation, &self, [](wl_resource *gone) {
      auto &owner = *static_cast<SeatServer *>(wl_resource_get_user_data(gone));
      std::erase(owner.seats_, gone);
    });
    self.seats_.push_back(seat);
    wl_seat_send_capabilities(seat, self.keyboard_ ? WL_SEAT_CAPABILITY_KEYBOARD : 0);
  }

  wl_display *display_{};
  wl_global *seat_{};
  int client_{-1};
  bool ready_{};
  bool keyboard_{true};
  Keymap keymap_{"xkb_keymap { fixture 1 };", WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, 0};
  std::vector<wl_resource *> seats_;
  std::vector<wl_resource *> keyboards_;
  std::mutex mutex_;
  std::vector<std::function<void()>> pending_;
  std::jthread thread_;
};

template <class Predicate> bool eventually(Predicate predicate) {
  const auto deadline = layerc::core::MonotonicClock::now() + std::chrono::seconds{2};
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  } while (layerc::core::MonotonicClock::now() < deadline);
  return false;
}

// Lets queued server work and its events arrive without matching anything.
void settle(layerc::platform::WaylandConnection &connection) {
  const auto until = layerc::core::MonotonicClock::now() + std::chrono::milliseconds{100};
  while (layerc::core::MonotonicClock::now() < until) {
    (void)connection.dispatch();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
}

void seat_keymap_lifecycle() {
  SeatServer server;
  const int fd = server.take_fd();
  if (!expect(fd >= 0, "create private Wayland server")) return;
  ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(fd));
  layerc::platform::WaylandConnection connection;
  if (!expect(connection.open() && connection.status().connected, "connect to the fixture")) return;
  WaylandKeymap keymap;
  expect(!keymap.text() && !keymap.take_changed(), "no keymap before a seat is bound");
  keymap.sync(connection);
  connection.flush();
  const auto text_is = [&](const char *expected) {
    (void)connection.dispatch();
    return keymap.text() && *keymap.text() == expected;
  };
  expect(eventually([&] { return text_is("xkb_keymap { fixture 1 };"); }) && keymap.take_changed() &&
             !keymap.take_changed(),
         "the seat keyboard's XKB keymap arrives once");

  server.send_keymap("ignored", WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP);
  settle(connection);
  server.send_keymap("too large", WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, WaylandKeymap::text_limit + 1);
  settle(connection);
  expect(text_is("xkb_keymap { fixture 1 };") && !keymap.take_changed(),
         "non-XKB and oversized keymaps keep the previous map");

  server.send_keymap("xkb_keymap { fixture 2 };");
  expect(eventually([&] { return text_is("xkb_keymap { fixture 2 };"); }) && keymap.take_changed(),
         "a layout change replaces the keymap");

  server.set_keyboard(false);
  expect(eventually([&] { (void)connection.dispatch(); return !keymap.text(); }) && keymap.take_changed(),
         "losing the keyboard capability drops the keymap");
  server.set_keyboard(true);
  expect(eventually([&] { return text_is("xkb_keymap { fixture 2 };"); }) && keymap.take_changed(),
         "a returning keyboard sends its keymap again");

  server.remove_seat();
  expect(eventually([&] {
           (void)connection.dispatch();
           return !connection.status().globals.contains("wl_seat");
         }),
         "seat removal reaches the registry");
  keymap.sync(connection);
  expect(!keymap.text() && keymap.take_changed(), "a removed seat releases its keymap");
  keymap.release();
  connection.flush();
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(seat_keymap_lifecycle))

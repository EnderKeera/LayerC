#include "wayland_keymap.hpp"

#include "wayland_connection.hpp"

#include <wayland-client.h>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace layerc::platform {
namespace {

constexpr std::uint32_t seat_version = 5;

// Pointer-stable listener data for the seat and its keyboard.
struct SeatKeymap {
  wl_seat *seat{};
  std::uint32_t seat_id{};
  std::uint32_t version{};
  wl_keyboard *keyboard{};
  std::optional<std::string> text;
  bool changed{};

  SeatKeymap() = default;
  SeatKeymap(const SeatKeymap &) = delete;
  SeatKeymap &operator=(const SeatKeymap &) = delete;
  ~SeatKeymap() { release_proxies(); }

  void release_keyboard() noexcept {
    if (!keyboard) return;
    if (version >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
      wl_keyboard_release(std::exchange(keyboard, nullptr));
    else
      wl_keyboard_destroy(std::exchange(keyboard, nullptr));
  }
  void forget() noexcept {
    if (text) changed = true;
    text.reset();
  }
  void release() noexcept {
    release_proxies();
    forget();
  }
  void release_proxies() noexcept {
    release_keyboard();
    if (seat) {
      if (version >= WL_SEAT_RELEASE_SINCE_VERSION)
        wl_seat_release(std::exchange(seat, nullptr));
      else
        wl_seat_destroy(std::exchange(seat, nullptr));
    }
    seat_id = 0;
  }

  // The descriptor is always closed. Only an XKB v1 map within the bound is
  // accepted; anything else keeps the previous map.
  void keymap(const std::uint32_t format, const int fd, const std::uint32_t size) {
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0 &&
        size <= WaylandKeymap::text_limit) {
      void *mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (mapped != MAP_FAILED) {
        const auto *chars = static_cast<const char *>(mapped);
        std::string next(chars, ::strnlen(chars, size));
        ::munmap(mapped, size);
        if (!next.empty() && (!text || *text != next)) {
          text = std::move(next);
          changed = true;
        }
      }
    }
    if (fd >= 0) ::close(fd);
  }
};

const wl_keyboard_listener keyboard_listener{
    [](void *data, wl_keyboard *, std::uint32_t format, std::int32_t fd,
       std::uint32_t size) {
      static_cast<SeatKeymap *>(data)->keymap(format, fd, size);
    },
    [](void *, wl_keyboard *, std::uint32_t, wl_surface *, wl_array *) {},
    [](void *, wl_keyboard *, std::uint32_t, wl_surface *) {},
    [](void *, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t,
       std::uint32_t) {},
    [](void *, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t,
       std::uint32_t, std::uint32_t) {},
    [](void *, wl_keyboard *, std::int32_t, std::int32_t) {},
};

const wl_seat_listener seat_listener{
    [](void *data, wl_seat *seat, std::uint32_t capabilities) {
      auto &self = *static_cast<SeatKeymap *>(data);
      const bool has_keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
      if (has_keyboard && !self.keyboard) {
        self.keyboard = wl_seat_get_keyboard(seat);
        if (self.keyboard) wl_keyboard_add_listener(self.keyboard, &keyboard_listener, &self);
      } else if (!has_keyboard && self.keyboard) {
        self.release_keyboard();
        self.forget();
      }
    },
    [](void *, wl_seat *, const char *) {},
};

} // namespace

struct WaylandKeymap::Impl : SeatKeymap {};

WaylandKeymap::WaylandKeymap() : impl_(std::make_unique<Impl>()) {}
WaylandKeymap::~WaylandKeymap() = default;

void WaylandKeymap::sync(const WaylandConnection &connection) {
  auto &self = *impl_;
  auto *registry = connection.registry();
  const auto globals = connection.globals();
  const auto present = [&](const std::uint32_t id) {
    return std::ranges::any_of(globals, [&](const WaylandGlobal &global) {
      return global.name == id && global.interface == "wl_seat";
    });
  };
  if (self.seat && (!registry || !present(self.seat_id))) self.release();
  if (self.seat || !registry) return;
  // The lowest-named seat is the default seat; multi-seat setups keep it.
  const WaylandGlobal *chosen{};
  for (const auto &global : globals)
    if (global.interface == "wl_seat" && (!chosen || global.name < chosen->name))
      chosen = &global;
  if (!chosen) return;
  self.version = std::min(chosen->version, seat_version);
  self.seat = static_cast<wl_seat *>(
      wl_registry_bind(registry, chosen->name, &wl_seat_interface, self.version));
  if (!self.seat) return;
  self.seat_id = chosen->name;
  wl_seat_add_listener(self.seat, &seat_listener, static_cast<SeatKeymap *>(impl_.get()));
}

void WaylandKeymap::release() noexcept { impl_->release(); }

bool WaylandKeymap::take_changed() noexcept { return std::exchange(impl_->changed, false); }

const std::optional<std::string> &WaylandKeymap::text() const noexcept { return impl_->text; }

} // namespace layerc::platform
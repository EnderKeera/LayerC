#include "wayland_test_client.hpp"

#include "layerc/core/types.hpp"

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <wayland-client.h>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>
#include <utility>

namespace layerc::test {
namespace {

using Clock = layerc::core::MonotonicClock;

struct Shm {
  wl_buffer *buffer{};
  void *data{};
  std::size_t size{};
};

void destroy(Shm &shm) noexcept {
  if (shm.buffer) wl_buffer_destroy(std::exchange(shm.buffer, nullptr));
  if (shm.data) munmap(std::exchange(shm.data, nullptr), shm.size);
}

bool allocate(wl_shm *shm, int width, int height, int stride, std::uint32_t format,
              Shm &result) {
  // Screencopy sizes come from the compositor; bound them like the product.
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || stride < width * 4 ||
      stride > 8192 * 4)
    return false;
  const auto size = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
  const int fd = memfd_create("layerc-test", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
    if (fd >= 0) close(fd);
    return false;
  }
  void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return false;
  }
  auto *pool = wl_shm_create_pool(shm, fd, static_cast<std::int32_t>(size));
  close(fd);
  result.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
  wl_shm_pool_destroy(pool);
  result.data = data;
  result.size = size;
  return result.buffer != nullptr;
}

struct NamedOutput {
  wl_output *proxy{};
  std::string name;
};

} // namespace

struct WaylandTestClient::Impl {
  wl_display *display{};
  wl_registry *registry{};
  wl_compositor *compositor{};
  wl_shm *shm{};
  xdg_wm_base *wm{};
  wl_seat *seat{};
  wl_pointer *pointer{};
  zwlr_virtual_pointer_manager_v1 *pointer_manager{};
  zwlr_virtual_pointer_v1 *virtual_pointer{};
  zwlr_screencopy_manager_v1 *screencopy{};
  std::map<std::uint32_t, NamedOutput> outputs;

  wl_surface *surface{};
  xdg_surface *xdg{};
  xdg_toplevel *toplevel{};
  Shm window_buffer;
  std::uint32_t window_rgb{};
  bool mapped{};

  wl_surface *focus{};
  double pointer_x{};
  double pointer_y{};
  std::optional<WaylandPointerPress> press;
  std::uint32_t time_ms{};

  ~Impl() {
    close_window();
    if (virtual_pointer) zwlr_virtual_pointer_v1_destroy(virtual_pointer);
    if (pointer_manager) zwlr_virtual_pointer_manager_v1_destroy(pointer_manager);
    if (pointer) wl_pointer_destroy(pointer);
    if (seat) wl_seat_destroy(seat);
    if (screencopy) zwlr_screencopy_manager_v1_destroy(screencopy);
    for (auto &[id, output] : outputs) wl_output_destroy(output.proxy);
    if (wm) xdg_wm_base_destroy(wm);
    if (shm) wl_shm_destroy(shm);
    if (compositor) wl_compositor_destroy(compositor);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);
  }

  void close_window() {
    if (toplevel) xdg_toplevel_destroy(std::exchange(toplevel, nullptr));
    if (xdg) xdg_surface_destroy(std::exchange(xdg, nullptr));
    if (surface) wl_surface_destroy(std::exchange(surface, nullptr));
    destroy(window_buffer);
    mapped = false;
  }

  void pump(int timeout_ms) {
    if (!display) return;
    while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
    wl_display_flush(display);
    pollfd watch{wl_display_get_fd(display), POLLIN, 0};
    if (::poll(&watch, 1, timeout_ms) > 0 && (watch.revents & POLLIN) != 0)
      wl_display_read_events(display);
    else
      wl_display_cancel_read(display);
    wl_display_dispatch_pending(display);
  }

  bool pump_until(const std::function<bool()> &predicate, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds{timeout_ms};
    while (!predicate()) {
      if (Clock::now() >= deadline) return false;
      pump(10);
    }
    return true;
  }

  wl_output *output(const std::string &name) const {
    for (const auto &[id, output] : outputs)
      if (output.name == name) return output.proxy;
    return nullptr;
  }

  void paint_window(int width, int height) {
    destroy(window_buffer);
    if (!allocate(shm, width, height, width * 4, WL_SHM_FORMAT_XRGB8888, window_buffer)) return;
    auto *pixels = static_cast<std::uint32_t *>(window_buffer.data);
    std::fill(pixels, pixels + static_cast<std::size_t>(width) * height, window_rgb);
    wl_surface_attach(surface, window_buffer.buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);
    mapped = true;
  }
};

namespace {

const wl_output_listener output_listener{
    [](void *, wl_output *, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
       std::int32_t, const char *, const char *, std::int32_t) {},
    [](void *, wl_output *, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {},
    [](void *, wl_output *) {},
    [](void *, wl_output *, std::int32_t) {},
    [](void *data, wl_output *, const char *name) {
      static_cast<NamedOutput *>(data)->name = name ? name : "";
    },
    [](void *, wl_output *, const char *) {},
};

const xdg_wm_base_listener wm_listener{
    [](void *, xdg_wm_base *wm, std::uint32_t serial) { xdg_wm_base_pong(wm, serial); }};

const wl_pointer_listener pointer_listener{
    [](void *data, wl_pointer *, std::uint32_t, wl_surface *surface, wl_fixed_t x,
       wl_fixed_t y) {
      auto &self = *static_cast<WaylandTestClient::Impl *>(data);
      self.focus = surface;
      self.pointer_x = wl_fixed_to_double(x);
      self.pointer_y = wl_fixed_to_double(y);
    },
    [](void *data, wl_pointer *, std::uint32_t, wl_surface *) {
      static_cast<WaylandTestClient::Impl *>(data)->focus = nullptr;
    },
    [](void *data, wl_pointer *, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
      auto &self = *static_cast<WaylandTestClient::Impl *>(data);
      self.pointer_x = wl_fixed_to_double(x);
      self.pointer_y = wl_fixed_to_double(y);
    },
    [](void *data, wl_pointer *, std::uint32_t, std::uint32_t, std::uint32_t button,
       std::uint32_t state) {
      auto &self = *static_cast<WaylandTestClient::Impl *>(data);
      if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED) return;
      self.press = WaylandPointerPress{self.focus != nullptr && self.focus == self.surface,
                                       self.pointer_x, self.pointer_y};
    },
    [](void *, wl_pointer *, std::uint32_t, std::uint32_t, wl_fixed_t) {},
    [](void *, wl_pointer *) {},
    [](void *, wl_pointer *, std::uint32_t) {},
    [](void *, wl_pointer *, std::uint32_t, std::uint32_t) {},
    [](void *, wl_pointer *, std::uint32_t, std::int32_t) {},
    [](void *, wl_pointer *, std::uint32_t, std::int32_t) {},
    [](void *, wl_pointer *, std::uint32_t, std::uint32_t) {},
};

const wl_seat_listener seat_listener{
    [](void *data, wl_seat *seat, std::uint32_t capabilities) {
      auto &self = *static_cast<WaylandTestClient::Impl *>(data);
      if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && !self.pointer) {
        self.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(self.pointer, &pointer_listener, &self);
      }
    },
    [](void *, wl_seat *, const char *) {},
};

const wl_registry_listener registry_listener{
    [](void *data, wl_registry *registry, std::uint32_t name, const char *interface,
       std::uint32_t version) {
      auto &self = *static_cast<WaylandTestClient::Impl *>(data);
      const std::string_view id{interface};
      const auto bind = [&](const wl_interface *type, std::uint32_t wanted) {
        return wl_registry_bind(registry, name, type, std::min(version, wanted));
      };
      if (id == "wl_compositor") {
        self.compositor = static_cast<wl_compositor *>(bind(&wl_compositor_interface, 4));
      } else if (id == "wl_shm") {
        self.shm = static_cast<wl_shm *>(bind(&wl_shm_interface, 1));
      } else if (id == "xdg_wm_base") {
        self.wm = static_cast<xdg_wm_base *>(bind(&xdg_wm_base_interface, 2));
        xdg_wm_base_add_listener(self.wm, &wm_listener, &self);
      } else if (id == "wl_seat" && !self.seat) {
        self.seat = static_cast<wl_seat *>(bind(&wl_seat_interface, 5));
        wl_seat_add_listener(self.seat, &seat_listener, &self);
      } else if (id == "wl_output" && version >= 4) {
        auto &output = self.outputs[name];
        output.proxy = static_cast<wl_output *>(bind(&wl_output_interface, 4));
        wl_output_add_listener(output.proxy, &output_listener, &output);
      } else if (id == "zwlr_virtual_pointer_manager_v1") {
        self.pointer_manager = static_cast<zwlr_virtual_pointer_manager_v1 *>(
            bind(&zwlr_virtual_pointer_manager_v1_interface, 1));
      } else if (id == "zwlr_screencopy_manager_v1") {
        self.screencopy = static_cast<zwlr_screencopy_manager_v1 *>(
            bind(&zwlr_screencopy_manager_v1_interface, 1));
      }
    },
    [](void *, wl_registry *, std::uint32_t) {},
};

struct ToplevelSize {
  WaylandTestClient::Impl *self{};
  int width{};
  int height{};
};

} // namespace

WaylandTestClient::WaylandTestClient() : impl_(std::make_unique<Impl>()) {}
WaylandTestClient::~WaylandTestClient() = default;

bool WaylandTestClient::connect(std::string &error) {
  auto &self = *impl_;
  self.display = wl_display_connect(nullptr);
  if (!self.display) {
    error = "cannot connect to the disposable compositor";
    return false;
  }
  self.registry = wl_display_get_registry(self.display);
  wl_registry_add_listener(self.registry, &registry_listener, &self);
  wl_display_roundtrip(self.display);
  wl_display_roundtrip(self.display);
  if (!self.compositor || !self.shm || !self.wm || !self.seat || !self.pointer_manager ||
      !self.screencopy || self.outputs.empty()) {
    error = "fixture globals missing (compositor/shm/xdg_wm_base/seat/virtual "
            "pointer/screencopy/outputs)";
    return false;
  }
  // The headless seat has no devices; the virtual pointer gives it one.
  self.virtual_pointer =
      zwlr_virtual_pointer_manager_v1_create_virtual_pointer(self.pointer_manager, self.seat);
  if (!self.pump_until([&] { return self.pointer != nullptr; }, 3000)) {
    error = "virtual pointer did not add a seat pointer";
    return false;
  }
  return true;
}

bool WaylandTestClient::show_window(const std::string &title, const std::string &app_id,
                                    const std::string &output_name, const std::uint32_t rgb) {
  auto &self = *impl_;
  auto *output = self.output(output_name);
  if (!output) return false;
  self.close_window();
  self.window_rgb = rgb;
  self.surface = wl_compositor_create_surface(self.compositor);
  self.xdg = xdg_wm_base_get_xdg_surface(self.wm, self.surface);
  self.toplevel = xdg_surface_get_toplevel(self.xdg);
  static ToplevelSize size;
  size = {&self, 0, 0};
  static const xdg_toplevel_listener toplevel_listener{
      [](void *data, xdg_toplevel *, std::int32_t width, std::int32_t height, wl_array *) {
        auto &pending = *static_cast<ToplevelSize *>(data);
        pending.width = width;
        pending.height = height;
      },
      [](void *, xdg_toplevel *) {},
      [](void *, xdg_toplevel *, std::int32_t, std::int32_t) {},
      [](void *, xdg_toplevel *, wl_array *) {},
  };
  static const xdg_surface_listener surface_listener{
      [](void *data, xdg_surface *surface, std::uint32_t serial) {
        xdg_surface_ack_configure(surface, serial);
        auto &pending = *static_cast<ToplevelSize *>(data);
        pending.self->paint_window(pending.width > 0 ? pending.width : 320,
                                   pending.height > 0 ? pending.height : 240);
      }};
  xdg_surface_add_listener(self.xdg, &surface_listener, &size);
  xdg_toplevel_add_listener(self.toplevel, &toplevel_listener, &size);
  xdg_toplevel_set_title(self.toplevel, title.c_str());
  xdg_toplevel_set_app_id(self.toplevel, app_id.c_str());
  xdg_toplevel_set_fullscreen(self.toplevel, output);
  wl_surface_commit(self.surface);
  return self.pump_until([&] { return self.mapped; }, 3000);
}

void WaylandTestClient::close_window() {
  impl_->close_window();
  if (impl_->display) wl_display_flush(impl_->display);
}

void WaylandTestClient::pump() { impl_->pump(0); }

bool WaylandTestClient::window_mapped() const noexcept { return impl_->mapped; }

std::optional<WaylandPixels> WaylandTestClient::capture(const std::string &output_name) {
  auto &self = *impl_;
  auto *output = self.output(output_name);
  if (!output) return std::nullopt;
  struct Capture {
    WaylandTestClient::Impl *self{};
    Shm shm;
    std::uint32_t format{};
    int width{};
    int height{};
    int stride{};
    bool y_invert{};
    bool ready{};
    bool failed{};
  } capture;
  capture.self = &self;
  static const zwlr_screencopy_frame_v1_listener frame_listener{
      [](void *data, zwlr_screencopy_frame_v1 *frame, std::uint32_t format,
         std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
        auto &state = *static_cast<Capture *>(data);
        state.format = format;
        state.width = static_cast<int>(width);
        state.height = static_cast<int>(height);
        state.stride = static_cast<int>(stride);
        if (!allocate(state.self->shm, state.width, state.height, state.stride, format,
                      state.shm)) {
          state.failed = true;
          return;
        }
        zwlr_screencopy_frame_v1_copy(frame, state.shm.buffer);
      },
      [](void *data, zwlr_screencopy_frame_v1 *, std::uint32_t flags) {
        static_cast<Capture *>(data)->y_invert =
            (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
      },
      [](void *data, zwlr_screencopy_frame_v1 *, std::uint32_t, std::uint32_t, std::uint32_t) {
        static_cast<Capture *>(data)->ready = true;
      },
      [](void *data, zwlr_screencopy_frame_v1 *) { static_cast<Capture *>(data)->failed = true; },
      [](void *, zwlr_screencopy_frame_v1 *, std::uint32_t, std::uint32_t, std::uint32_t,
         std::uint32_t) {},
      [](void *, zwlr_screencopy_frame_v1 *, std::uint32_t, std::uint32_t, std::uint32_t) {},
      [](void *, zwlr_screencopy_frame_v1 *) {},
  };
  auto *frame = zwlr_screencopy_manager_v1_capture_output(self.screencopy, 0, output);
  zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, &capture);
  const bool done = self.pump_until([&] { return capture.ready || capture.failed; }, 3000);
  zwlr_screencopy_frame_v1_destroy(frame);
  std::optional<WaylandPixels> result;
  if (done && capture.ready) {
    WaylandPixels pixels{capture.width, capture.height, {}};
    pixels.rgb.reserve(static_cast<std::size_t>(capture.width) * capture.height);
    const bool bgr = capture.format == WL_SHM_FORMAT_XBGR8888 ||
                     capture.format == WL_SHM_FORMAT_ABGR8888;
    for (int y = 0; y < capture.height; ++y) {
      const int row = capture.y_invert ? capture.height - 1 - y : y;
      const auto *line = static_cast<const unsigned char *>(capture.shm.data) +
                         static_cast<std::size_t>(row) * capture.stride;
      for (int x = 0; x < capture.width; ++x) {
        std::uint32_t value{};
        std::memcpy(&value, line + static_cast<std::size_t>(x) * 4, sizeof(value));
        if (bgr)
          value = ((value & 0xFF) << 16) | (value & 0xFF00) | ((value >> 16) & 0xFF);
        pixels.rgb.push_back(value & 0xFFFFFF);
      }
    }
    result = std::move(pixels);
  }
  destroy(capture.shm);
  return result;
}

std::optional<WaylandPointerPress> WaylandTestClient::click(const int x, const int y,
                                                            const int layout_width,
                                                            const int layout_height) {
  auto &self = *impl_;
  if (!self.virtual_pointer) return std::nullopt;
  self.press.reset();
  const auto time = [&] { return self.time_ms += 16; };
  zwlr_virtual_pointer_v1_motion_absolute(self.virtual_pointer, time(),
                                          static_cast<std::uint32_t>(x),
                                          static_cast<std::uint32_t>(y),
                                          static_cast<std::uint32_t>(layout_width),
                                          static_cast<std::uint32_t>(layout_height));
  zwlr_virtual_pointer_v1_frame(self.virtual_pointer);
  (void)self.pump_until([&] { return false; }, 100);
  zwlr_virtual_pointer_v1_button(self.virtual_pointer, time(), BTN_LEFT,
                                 WL_POINTER_BUTTON_STATE_PRESSED);
  zwlr_virtual_pointer_v1_frame(self.virtual_pointer);
  (void)self.pump_until([&] { return self.press.has_value(); }, 1000);
  zwlr_virtual_pointer_v1_button(self.virtual_pointer, time(), BTN_LEFT,
                                 WL_POINTER_BUTTON_STATE_RELEASED);
  zwlr_virtual_pointer_v1_frame(self.virtual_pointer);
  self.pump(10);
  return self.press;
}

bool wait_until(const std::function<void()> &pump, const std::function<bool()> &predicate,
                const int timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds{timeout_ms};
  while (!predicate()) {
    if (Clock::now() >= deadline) return false;
    pump();
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  return true;
}

} // namespace layerc::test

#include "wayland_monitors.hpp"

#include "wayland_connection.hpp"
#include "xdg-output-unstable-v1-client-protocol.h"

#include "layerc/core/utf8.hpp"

#include <wayland-client.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace layerc::platform {
namespace {

constexpr std::uint32_t output_version = 4;
constexpr std::uint32_t xdg_output_manager_version = 3;
constexpr std::size_t name_limit = 128;

struct Output {
  bool *changed{};
  std::uint32_t id{};
  std::uint32_t version{};
  wl_output *proxy{};
  zxdg_output_v1 *xdg{};
  std::string name;
  int geometry_x{};
  int geometry_y{};
  int transform{};
  int mode_width{};
  int mode_height{};
  int scale{1};
  int logical_x{};
  int logical_y{};
  int logical_width{};
  int logical_height{};
  bool logical{};
  bool done{};

  void touch() const noexcept { *changed = true; }
  void set_name(const char *value) {
    const std::string_view text{value ? value : ""};
    if (!text.empty() && text.size() <= name_limit && core::is_valid_utf8(text))
      name = text;
    touch();
  }
};

const wl_output_listener output_listener{
    [](void *data, wl_output *, std::int32_t x, std::int32_t y, std::int32_t,
       std::int32_t, std::int32_t, const char *, const char *,
       std::int32_t transform) {
      auto &output = *static_cast<Output *>(data);
      output.geometry_x = x;
      output.geometry_y = y;
      output.transform = transform;
      output.touch();
    },
    [](void *data, wl_output *, std::uint32_t flags, std::int32_t width,
       std::int32_t height, std::int32_t) {
      auto &output = *static_cast<Output *>(data);
      if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) return;
      output.mode_width = width;
      output.mode_height = height;
      // Version 1 outputs have no done event; their mode completes them.
      if (output.version < 2) output.done = true;
      output.touch();
    },
    [](void *data, wl_output *) {
      auto &output = *static_cast<Output *>(data);
      output.done = true;
      output.touch();
    },
    [](void *data, wl_output *, std::int32_t factor) {
      auto &output = *static_cast<Output *>(data);
      output.scale = std::clamp(factor, 1, 8);
      output.touch();
    },
    [](void *data, wl_output *, const char *name) {
      static_cast<Output *>(data)->set_name(name);
    },
    [](void *, wl_output *, const char *) {},
};

const zxdg_output_v1_listener xdg_output_listener{
    [](void *data, zxdg_output_v1 *, std::int32_t x, std::int32_t y) {
      auto &output = *static_cast<Output *>(data);
      output.logical_x = x;
      output.logical_y = y;
      output.touch();
    },
    [](void *data, zxdg_output_v1 *, std::int32_t width, std::int32_t height) {
      auto &output = *static_cast<Output *>(data);
      output.logical_width = width;
      output.logical_height = height;
      output.logical = width > 0 && height > 0;
      output.touch();
    },
    [](void *data, zxdg_output_v1 *) { static_cast<Output *>(data)->touch(); },
    [](void *data, zxdg_output_v1 *, const char *name) {
      auto &output = *static_cast<Output *>(data);
      // wl_output.name (v4) is authoritative when both exist.
      if (output.version < 4) output.set_name(name);
    },
    [](void *, zxdg_output_v1 *, const char *) {},
};

void destroy(Output &output) noexcept {
  if (output.xdg) zxdg_output_v1_destroy(std::exchange(output.xdg, nullptr));
  if (output.proxy) {
    if (output.version >= WL_OUTPUT_RELEASE_SINCE_VERSION)
      wl_output_release(std::exchange(output.proxy, nullptr));
    else
      wl_output_destroy(std::exchange(output.proxy, nullptr));
  }
}

} // namespace

struct WaylandMonitors::Impl {
  // Pointer-stable entries: each is listener data for its proxies.
  std::map<std::uint32_t, std::unique_ptr<Output>> outputs;
  zxdg_output_manager_v1 *manager{};
  std::uint32_t manager_id{};
  bool changed{};
  std::vector<const wl_output *> removed;

  ~Impl() { release(); }
  void release() noexcept {
    for (auto &[id, output] : outputs) {
      removed.push_back(output->proxy);
      destroy(*output);
    }
    outputs.clear();
    if (manager) zxdg_output_manager_v1_destroy(std::exchange(manager, nullptr));
    manager_id = 0;
  }
  void attach_xdg(Output &output) {
    if (!manager || output.xdg) return;
    output.xdg = zxdg_output_manager_v1_get_xdg_output(manager, output.proxy);
    if (output.xdg) zxdg_output_v1_add_listener(output.xdg, &xdg_output_listener, &output);
  }
  [[nodiscard]] Monitor make_monitor(const Output &output) const {
    Monitor monitor;
    monitor.name = output.name.empty() ? "wl_output-" + std::to_string(output.id)
                                       : output.name;
    monitor.buffer_scale = output.scale;
    if (output.logical) {
      monitor.x = output.logical_x;
      monitor.y = output.logical_y;
      monitor.width = output.logical_width;
      monitor.height = output.logical_height;
    } else {
      // Without xdg-output: compositor position plus the mode in logical
      // units (rotated outputs swap their axes).
      const bool rotated = (output.transform & 1) != 0;
      monitor.x = output.geometry_x;
      monitor.y = output.geometry_y;
      monitor.width = (rotated ? output.mode_height : output.mode_width) / output.scale;
      monitor.height = (rotated ? output.mode_width : output.mode_height) / output.scale;
    }
    return monitor;
  }
};

WaylandMonitors::WaylandMonitors() : impl_(std::make_unique<Impl>()) {}
WaylandMonitors::~WaylandMonitors() = default;

void WaylandMonitors::sync(const WaylandConnection &connection) {
  auto &self = *impl_;
  auto *registry = connection.registry();
  const auto globals = connection.globals();
  if (!registry || globals.empty()) {
    if (!self.outputs.empty() || self.manager) self.changed = true;
    self.release();
    return;
  }
  const auto present = [&](std::uint32_t id, std::string_view interface) {
    return std::ranges::any_of(globals, [&](const WaylandGlobal &global) {
      return global.name == id && global.interface == interface;
    });
  };
  for (auto it = self.outputs.begin(); it != self.outputs.end();) {
    if (present(it->first, "wl_output")) { ++it; continue; }
    self.removed.push_back(it->second->proxy);
    destroy(*it->second);
    it = self.outputs.erase(it);
    self.changed = true;
  }
  if (self.manager && !present(self.manager_id, "zxdg_output_manager_v1")) {
    for (auto &[id, output] : self.outputs)
      if (output->xdg) zxdg_output_v1_destroy(std::exchange(output->xdg, nullptr));
    zxdg_output_manager_v1_destroy(std::exchange(self.manager, nullptr));
    self.manager_id = 0;
    self.changed = true;
  }
  for (const auto &global : globals) {
    if (global.interface == "zxdg_output_manager_v1" && !self.manager) {
      self.manager = static_cast<zxdg_output_manager_v1 *>(wl_registry_bind(
          registry, global.name, &zxdg_output_manager_v1_interface,
          std::min(global.version, xdg_output_manager_version)));
      if (self.manager) self.manager_id = global.name;
    }
  }
  for (const auto &global : globals) {
    if (global.interface != "wl_output" || self.outputs.contains(global.name)) continue;
    auto output = std::make_unique<Output>();
    output->changed = &self.changed;
    output->id = global.name;
    output->version = std::min(global.version, output_version);
    output->proxy = static_cast<wl_output *>(wl_registry_bind(
        registry, global.name, &wl_output_interface, output->version));
    if (!output->proxy) continue;
    wl_output_add_listener(output->proxy, &output_listener, output.get());
    self.outputs.emplace(global.name, std::move(output));
    self.changed = true;
  }
  for (auto &[id, output] : self.outputs) self.attach_xdg(*output);
}

void WaylandMonitors::release() noexcept { impl_->release(); }

bool WaylandMonitors::take_changed() noexcept {
  return std::exchange(impl_->changed, false);
}

std::vector<const wl_output *> WaylandMonitors::take_removed() {
  return std::exchange(impl_->removed, {});
}

MonitorScan WaylandMonitors::scan() const {
  MonitorScan result;
  for (const auto &[id, output] : impl_->outputs) {
    if (!output->done) continue;
    auto monitor = impl_->make_monitor(*output);
    if (monitor.width <= 0 || monitor.height <= 0) continue;
    result.monitors.push_back(std::move(monitor));
  }
  result.available = !result.monitors.empty();
  if (!result.available)
    result.warning = "no_wayland_outputs";
  else if (!impl_->manager)
    result.warning = "xdg_output_missing";
  return result;
}

wl_output *WaylandMonitors::output(const std::string_view name) const noexcept {
  for (const auto &[id, output] : impl_->outputs)
    if (output->done && impl_->make_monitor(*output).name == name) return output->proxy;
  return nullptr;
}

std::optional<Monitor> WaylandMonitors::monitor(const wl_output *proxy) const {
  for (const auto &[id, output] : impl_->outputs)
    if (output->proxy == proxy && output->done) return impl_->make_monitor(*output);
  return std::nullopt;
}

} // namespace layerc::platform

#include "wayland_toplevels.hpp"

#include "wayland_connection.hpp"
#include "wayland_monitors.hpp"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include "layerc/core/utf8.hpp"

#include <wayland-client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace layerc::platform {
namespace {

constexpr std::uint32_t manager_version = 3;
constexpr std::size_t text_limit = 512;

struct State {
  std::string title;
  std::string app_id;
  bool active{};
  bool fullscreen{};
  bool hidden{};

  bool operator==(const State &) const = default;
};

struct Toplevel {
  struct Owner *owner{};
  zwlr_foreign_toplevel_handle_v1 *proxy{};
  std::uint64_t id{};
  // Protocol state is double-buffered: events fill `pending`, done applies.
  State pending;
  State current;
  std::vector<wl_output *> outputs;
  bool ready{};
};

struct Owner {
  zwlr_foreign_toplevel_manager_v1 *manager{};
  std::uint32_t manager_id{};
  std::list<Toplevel> toplevels;
  std::uint64_t next_id{1};
  bool changed{};
};

void assign(std::string &target, const char *value) {
  const std::string_view text{value ? value : ""};
  // Oversized or invalid identity text is dropped rather than truncated
  // mid-character; matching then falls back to other fields.
  target = text.size() <= text_limit && core::is_valid_utf8(text) ? std::string{text} : std::string{};
}

void remove(Owner &owner, Toplevel &toplevel) noexcept {
  if (toplevel.proxy) zwlr_foreign_toplevel_handle_v1_destroy(toplevel.proxy);
  const bool visible = toplevel.ready;
  owner.toplevels.remove_if([&](const Toplevel &entry) { return &entry == &toplevel; });
  if (visible) owner.changed = true;
}

const zwlr_foreign_toplevel_handle_v1_listener handle_listener{
    [](void *data, zwlr_foreign_toplevel_handle_v1 *, const char *title) {
      assign(static_cast<Toplevel *>(data)->pending.title, title);
    },
    [](void *data, zwlr_foreign_toplevel_handle_v1 *, const char *app_id) {
      assign(static_cast<Toplevel *>(data)->pending.app_id, app_id);
    },
    [](void *data, zwlr_foreign_toplevel_handle_v1 *, wl_output *output) {
      auto &toplevel = *static_cast<Toplevel *>(data);
      if (std::ranges::find(toplevel.outputs, output) == toplevel.outputs.end())
        toplevel.outputs.push_back(output);
      toplevel.owner->changed = toplevel.ready || toplevel.owner->changed;
    },
    [](void *data, zwlr_foreign_toplevel_handle_v1 *, wl_output *output) {
      auto &toplevel = *static_cast<Toplevel *>(data);
      std::erase(toplevel.outputs, output);
      toplevel.owner->changed = toplevel.ready || toplevel.owner->changed;
    },
    [](void *data, zwlr_foreign_toplevel_handle_v1 *, wl_array *states) {
      auto &pending = static_cast<Toplevel *>(data)->pending;
      pending.active = pending.fullscreen = pending.hidden = false;
      const auto count = states->size / sizeof(std::uint32_t);
      for (std::size_t index = 0; index < count; ++index) {
        std::uint32_t state{};
        std::memcpy(&state, static_cast<const char *>(states->data) + index * sizeof(state),
                    sizeof(state));
        if (state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) pending.active = true;
        if (state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) pending.fullscreen = true;
        if (state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) pending.hidden = true;
      }
    },
    [](void *data, zwlr_foreign_toplevel_handle_v1 *) {
      auto &toplevel = *static_cast<Toplevel *>(data);
      if (!toplevel.ready || toplevel.current != toplevel.pending) toplevel.owner->changed = true;
      toplevel.current = toplevel.pending;
      toplevel.ready = true;
    },
    [](void *data, zwlr_foreign_toplevel_handle_v1 *) {
      auto &toplevel = *static_cast<Toplevel *>(data);
      remove(*toplevel.owner, toplevel);
    },
    [](void *, zwlr_foreign_toplevel_handle_v1 *, zwlr_foreign_toplevel_handle_v1 *) {},
};

const zwlr_foreign_toplevel_manager_v1_listener manager_listener{
    [](void *data, zwlr_foreign_toplevel_manager_v1 *,
       zwlr_foreign_toplevel_handle_v1 *handle) {
      auto &owner = *static_cast<Owner *>(data);
      auto &toplevel = owner.toplevels.emplace_back();
      toplevel.owner = &owner;
      toplevel.proxy = handle;
      toplevel.id = owner.next_id++;
      zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, &toplevel);
    },
    [](void *data, zwlr_foreign_toplevel_manager_v1 *manager) {
      // The compositor stopped sending toplevels; the proxy is now inert.
      auto &owner = *static_cast<Owner *>(data);
      zwlr_foreign_toplevel_manager_v1_destroy(manager);
      if (owner.manager == manager) owner.manager = nullptr;
      owner.manager_id = 0;
      owner.changed = true;
    },
};

} // namespace

struct WaylandToplevels::Impl : Owner {
  ~Impl() { release(); }
  void release() noexcept {
    for (auto &toplevel : toplevels)
      if (toplevel.proxy) zwlr_foreign_toplevel_handle_v1_destroy(toplevel.proxy);
    if (!toplevels.empty()) changed = true;
    toplevels.clear();
    if (manager) {
      zwlr_foreign_toplevel_manager_v1_stop(manager);
      zwlr_foreign_toplevel_manager_v1_destroy(std::exchange(manager, nullptr));
      changed = true;
    }
    manager_id = 0;
  }
};

WaylandToplevels::WaylandToplevels() : impl_(std::make_unique<Impl>()) {}
WaylandToplevels::~WaylandToplevels() = default;

void WaylandToplevels::sync(const WaylandConnection &connection) {
  auto &self = *impl_;
  auto *registry = connection.registry();
  const auto globals = connection.globals();
  const auto advertised = std::ranges::find_if(globals, [](const WaylandGlobal &global) {
    return global.interface == "zwlr_foreign_toplevel_manager_v1";
  });
  if (!registry || advertised == globals.end()) {
    self.release();
    return;
  }
  if (self.manager && self.manager_id == advertised->name) return;
  self.release();
  self.manager = static_cast<zwlr_foreign_toplevel_manager_v1 *>(wl_registry_bind(
      registry, advertised->name, &zwlr_foreign_toplevel_manager_v1_interface,
      std::min(advertised->version, manager_version)));
  if (!self.manager) return;
  self.manager_id = advertised->name;
  zwlr_foreign_toplevel_manager_v1_add_listener(self.manager, &manager_listener,
                                                static_cast<Owner *>(&self));
  self.changed = true;
}

void WaylandToplevels::release() noexcept { impl_->release(); }

void WaylandToplevels::forget_outputs(const std::vector<const wl_output *> &outputs) {
  if (outputs.empty()) return;
  for (auto &toplevel : impl_->toplevels)
    std::erase_if(toplevel.outputs, [&](const wl_output *output) {
      return std::ranges::find(outputs, output) != outputs.end();
    });
  impl_->changed = true;
}

bool WaylandToplevels::take_changed() noexcept {
  return std::exchange(impl_->changed, false);
}

bool WaylandToplevels::available() const noexcept { return impl_->manager != nullptr; }

WindowScan WaylandToplevels::scan(const WaylandMonitors &monitors) const {
  WindowScan result;
  // Compositors may omit output_enter (sway 1.10 does). With exactly one
  // monitor the output is unambiguous; otherwise geometry stays unknown
  // rather than guessing a monitor.
  const auto layout = monitors.scan();
  const Monitor *only = layout.monitors.size() == 1 ? &layout.monitors.front() : nullptr;
  result.authoritative = impl_->manager != nullptr;
  if (!result.authoritative) result.warning = "foreign_toplevel_missing";
  for (const auto &toplevel : impl_->toplevels) {
    if (!toplevel.ready) continue;
    if (result.windows.size() >= window_scan_limit) {
      ++result.truncated;
      continue;
    }
    WindowSnapshot window;
    window.window = toplevel.id;
    window.title = toplevel.current.title;
    window.class_name = window.instance_name = toplevel.current.app_id;
    window.active = toplevel.current.active;
    window.fullscreen = toplevel.current.fullscreen;
    window.hidden = toplevel.current.hidden;
    std::optional<Monitor> placed;
    for (const auto *output : toplevel.outputs)
      if ((placed = monitors.monitor(output))) break;
    if (!placed && only) placed = *only;
    if (placed) {
      window.x = placed->x;
      window.y = placed->y;
      window.width = placed->width;
      window.height = placed->height;
    }
    if (window.active && !result.active) result.active = window.window;
    result.windows.push_back(std::move(window));
  }
  return result;
}

} // namespace layerc::platform

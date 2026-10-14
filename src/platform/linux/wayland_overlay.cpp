#include "wayland_overlay.hpp"

#include "wayland_connection.hpp"

#include <cairo.h>
#include <wayland-client.h>

// The protocol names a request argument `namespace`, a C++ keyword.
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace layerc::platform {
namespace {

constexpr std::uint32_t compositor_version = 4;
constexpr std::uint32_t layer_shell_version = 4;
// A panel is a few hundred logical pixels; this bounds the shm allocation.
constexpr int max_buffer_side = 4096;

struct Buffer {
  wl_buffer *proxy{};
  void *data{};
  std::size_t size{};
  int width{};
  int height{};
  bool busy{};
};

void destroy(Buffer &buffer) noexcept {
  if (buffer.proxy) wl_buffer_destroy(std::exchange(buffer.proxy, nullptr));
  if (buffer.data) munmap(std::exchange(buffer.data, nullptr), buffer.size);
  buffer = {};
}

const wl_buffer_listener buffer_listener{
    [](void *data, wl_buffer *) { static_cast<Buffer *>(data)->busy = false; }};

struct Surface {
  bool *changed{};
  wl_surface *surface{};
  zwlr_layer_surface_v1 *layer{};
  wl_output *output{};
  std::array<Buffer, 2> buffers;
  OverlayDisplayList list;
  int scale{1};
  int margin_left{};
  int margin_top{};
  // Geometry requested at least once (the first commit needs it).
  bool requested{};
  // A configure arrived and was acked since the last size request. Any
  // configure counts (wlroots sends the requested size); painting always
  // uses the requested size, never a compositor-chosen one.
  bool configured{};
  bool closed{};
  bool mapped{};
  bool dirty{true};
};

void destroy(Surface &surface) noexcept {
  if (surface.layer) zwlr_layer_surface_v1_destroy(std::exchange(surface.layer, nullptr));
  if (surface.surface) wl_surface_destroy(std::exchange(surface.surface, nullptr));
  for (auto &buffer : surface.buffers) destroy(buffer);
}

const zwlr_layer_surface_v1_listener layer_listener{
    [](void *data, zwlr_layer_surface_v1 *layer, std::uint32_t serial, std::uint32_t,
       std::uint32_t) {
      auto &surface = *static_cast<Surface *>(data);
      zwlr_layer_surface_v1_ack_configure(layer, serial);
      surface.configured = true;
      surface.dirty = true;
      *surface.changed = true;
    },
    [](void *data, zwlr_layer_surface_v1 *) {
      // The compositor dropped the surface; the next present recreates it.
      auto &surface = *static_cast<Surface *>(data);
      surface.closed = true;
      surface.mapped = false;
      *surface.changed = true;
    },
};

void set_rgb(cairo_t *cairo, const std::uint32_t rgb) {
  cairo_set_source_rgb(cairo, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                       (rgb & 0xFF) / 255.0);
}

} // namespace

void paint_overlay_display_list(const OverlayDisplayList &list, unsigned char *pixels,
                                const int width, const int height, const int stride,
                                const int scale) {
  // CAIRO_FORMAT_RGB24 is native-endian 32-bit xRGB, which is
  // WL_SHM_FORMAT_XRGB8888 on the little-endian hosts LayerC supports.
  auto *target = cairo_image_surface_create_for_data(pixels, CAIRO_FORMAT_RGB24, width,
                                                     height, stride);
  auto *cairo = cairo_create(target);
  cairo_set_source_rgb(cairo, 0, 0, 0);
  cairo_paint(cairo);
  cairo_scale(cairo, scale, scale);
  // One device pixel in logical units: outlines stay one device pixel wide
  // and inside their rect at every scale, like XDrawRectangle on X11.
  const double pixel = 1.0 / scale;
  for (const auto &op : list.ops) {
    set_rgb(cairo, op.rgb);
    const double x = op.rect.x;
    const double y = op.rect.y;
    const double w = op.rect.width;
    const double h = op.rect.height;
    switch (op.kind) {
    case OverlayOpKind::fill_rect:
      cairo_rectangle(cairo, x, y, w, h);
      cairo_fill(cairo);
      break;
    case OverlayOpKind::stroke_rect:
      if (w <= 0 || h <= 0) break;
      cairo_set_line_width(cairo, pixel);
      cairo_rectangle(cairo, x + pixel / 2, y + pixel / 2, std::max(0.0, w - pixel),
                      std::max(0.0, h - pixel));
      cairo_stroke(cairo);
      break;
    case OverlayOpKind::text: {
      if (op.text.empty() || op.font_px <= 0) break;
      cairo_save(cairo);
      cairo_rectangle(cairo, x, y, w, h);
      cairo_clip(cairo);
      cairo_select_font_face(cairo, "monospace", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cairo, op.font_px);
      cairo_text_extents_t extents{};
      cairo_text_extents(cairo, op.text.c_str(), &extents);
      cairo_font_extents_t font{};
      cairo_font_extents(cairo, &font);
      cairo_move_to(cairo, x + (w - extents.x_advance) / 2,
                    y + (h - (font.ascent + font.descent)) / 2 + font.ascent);
      cairo_show_text(cairo, op.text.c_str());
      cairo_restore(cairo);
      break;
    }
    }
  }
  cairo_destroy(cairo);
  cairo_surface_flush(target);
  cairo_surface_destroy(target);
}

struct WaylandOverlay::Impl {
  wl_compositor *compositor{};
  std::uint32_t compositor_id{};
  std::uint32_t compositor_bound{};
  wl_shm *shm{};
  std::uint32_t shm_id{};
  zwlr_layer_shell_v1 *shell{};
  std::uint32_t shell_id{};
  // Pointer-stable entries: each is listener data for its proxies.
  std::map<std::string, std::unique_ptr<Surface>, std::less<>> surfaces;
  std::string fault;
  bool changed{};

  ~Impl() { release(); }
  void release() noexcept {
    clear();
    if (shell) zwlr_layer_shell_v1_destroy(std::exchange(shell, nullptr));
    if (shm) wl_shm_destroy(std::exchange(shm, nullptr));
    if (compositor) wl_compositor_destroy(std::exchange(compositor, nullptr));
    compositor_id = shm_id = shell_id = 0;
  }
  void clear() noexcept {
    for (auto &[key, surface] : surfaces) destroy(*surface);
    if (!surfaces.empty()) changed = true;
    surfaces.clear();
  }

  [[nodiscard]] bool create(Surface &surface, wl_output *output) {
    surface.surface = wl_compositor_create_surface(compositor);
    auto *region = surface.surface ? wl_compositor_create_region(compositor) : nullptr;
    if (!region) {
      // Without an explicit empty input region the surface would take
      // input, so it must never be mapped.
      fault = "overlay_input_region_failed";
      destroy(surface);
      return false;
    }
    wl_surface_set_input_region(surface.surface, region);
    wl_region_destroy(region);
    surface.layer = zwlr_layer_shell_v1_get_layer_surface(
        shell, surface.surface, output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "layerc-overlay");
    if (!surface.layer) {
      fault = "overlay_layer_surface_failed";
      destroy(surface);
      return false;
    }
    zwlr_layer_surface_v1_add_listener(surface.layer, &layer_listener, &surface);
    zwlr_layer_surface_v1_set_anchor(surface.layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_exclusive_zone(surface.layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        surface.layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    surface.output = output;
    return true;
  }

  [[nodiscard]] Buffer *buffer(Surface &surface, const int width, const int height) {
    for (auto &candidate : surface.buffers) {
      if (candidate.busy) continue;
      if (candidate.proxy && candidate.width == width && candidate.height == height)
        return &candidate;
    }
    auto free = std::ranges::find_if(surface.buffers,
                                     [](const Buffer &candidate) { return !candidate.busy; });
    if (free == surface.buffers.end()) return nullptr;
    destroy(*free);
    const int stride = width * 4;
    const auto size = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    const int fd = memfd_create("layerc-overlay", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
      if (fd >= 0) close(fd);
      fault = "overlay_shm_failed";
      return nullptr;
    }
    (void)fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
      close(fd);
      fault = "overlay_shm_failed";
      return nullptr;
    }
    auto *pool = wl_shm_create_pool(shm, fd, static_cast<std::int32_t>(size));
    close(fd);
    auto *proxy = pool ? wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                   WL_SHM_FORMAT_XRGB8888)
                       : nullptr;
    if (pool) wl_shm_pool_destroy(pool);
    if (!proxy) {
      munmap(data, size);
      fault = "overlay_shm_failed";
      return nullptr;
    }
    *free = {proxy, data, size, width, height, false};
    wl_buffer_add_listener(proxy, &buffer_listener, &*free);
    return &*free;
  }

  void paint(Surface &surface) {
    if (!surface.configured || !surface.dirty) return;
    // Panels always request an explicit size; paint exactly that, never a
    // compositor-chosen one (which would also size the shm allocation).
    const int width = surface.list.bounds.width;
    const int height = surface.list.bounds.height;
    // Bounded before multiplying, so the device size cannot overflow.
    if (width <= 0 || height <= 0 || width > max_buffer_side / surface.scale ||
        height > max_buffer_side / surface.scale)
      return;
    const int device_width = width * surface.scale;
    const int device_height = height * surface.scale;
    auto *target = buffer(surface, device_width, device_height);
    if (!target) return; // Both busy: the next present repaints.
    paint_overlay_display_list(surface.list, static_cast<unsigned char *>(target->data),
                               device_width, device_height, device_width * 4, surface.scale);
    wl_surface_set_buffer_scale(surface.surface, surface.scale);
    wl_surface_attach(surface.surface, target->proxy, 0, 0);
    if (compositor_bound >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
      wl_surface_damage_buffer(surface.surface, 0, 0, device_width, device_height);
    else
      wl_surface_damage(surface.surface, 0, 0, width, height);
    wl_surface_commit(surface.surface);
    target->busy = true;
    surface.dirty = false;
    // A transient memfd/mmap failure is not sticky once a frame succeeds.
    fault.clear();
    if (!surface.mapped) changed = true;
    surface.mapped = true;
  }
};

WaylandOverlay::WaylandOverlay() : impl_(std::make_unique<Impl>()) {}
WaylandOverlay::~WaylandOverlay() = default;

void WaylandOverlay::sync(const WaylandConnection &connection) {
  auto &self = *impl_;
  auto *registry = connection.registry();
  const auto globals = connection.globals();
  const auto find = [&](std::string_view interface) -> const WaylandGlobal * {
    const auto it = std::ranges::find_if(
        globals, [&](const WaylandGlobal &global) { return global.interface == interface; });
    return it == globals.end() ? nullptr : &*it;
  };
  const auto *compositor = find("wl_compositor");
  const auto *shm = find("wl_shm");
  const auto *shell = find("zwlr_layer_shell_v1");
  const bool same = registry && compositor && shm && shell &&
                    self.compositor_id == compositor->name && self.shm_id == shm->name &&
                    self.shell_id == shell->name;
  if (same) return;
  self.release();
  if (!registry) return;
  if (compositor) {
    self.compositor_bound = std::min(compositor->version, compositor_version);
    self.compositor = static_cast<wl_compositor *>(wl_registry_bind(
        registry, compositor->name, &wl_compositor_interface, self.compositor_bound));
    if (self.compositor) self.compositor_id = compositor->name;
  }
  if (shm) {
    self.shm = static_cast<wl_shm *>(
        wl_registry_bind(registry, shm->name, &wl_shm_interface, 1));
    if (self.shm) self.shm_id = shm->name;
  }
  if (shell) {
    self.shell = static_cast<zwlr_layer_shell_v1 *>(wl_registry_bind(
        registry, shell->name, &zwlr_layer_shell_v1_interface,
        std::min(shell->version, layer_shell_version)));
    if (self.shell) self.shell_id = shell->name;
  }
  self.changed = true;
}

void WaylandOverlay::release() noexcept { impl_->release(); }

bool WaylandOverlay::take_changed() noexcept { return std::exchange(impl_->changed, false); }

std::vector<std::string> WaylandOverlay::unavailable() const {
  std::vector<std::string> reasons;
  if (!impl_->shell) reasons.emplace_back("layer_shell_missing");
  if (!impl_->compositor) reasons.emplace_back("wl_compositor_missing");
  if (!impl_->shm) reasons.emplace_back("wl_shm_missing");
  return reasons;
}

bool WaylandOverlay::present(const std::string_view key, const OverlayDisplayList &list,
                             wl_output *output, const Monitor &monitor) {
  auto &self = *impl_;
  if (!self.shell || !self.compositor || !self.shm || !output || list.bounds.width <= 0 ||
      list.bounds.height <= 0 || list.bounds.width > max_buffer_side ||
      list.bounds.height > max_buffer_side)
    return false;
  const auto left64 = static_cast<std::int64_t>(list.bounds.x) - monitor.x;
  const auto top64 = static_cast<std::int64_t>(list.bounds.y) - monitor.y;
  if (left64 < -max_buffer_side || left64 > std::numeric_limits<std::int32_t>::max() ||
      top64 < -max_buffer_side || top64 > std::numeric_limits<std::int32_t>::max())
    return false;
  auto it = self.surfaces.find(key);
  if (it != self.surfaces.end() && (it->second->closed || it->second->output != output)) {
    destroy(*it->second);
    self.surfaces.erase(it);
    it = self.surfaces.end();
  }
  if (it == self.surfaces.end()) {
    auto surface = std::make_unique<Surface>();
    surface->changed = &self.changed;
    if (!self.create(*surface, output)) return false;
    it = self.surfaces.emplace(std::string{key}, std::move(surface)).first;
  }
  auto &surface = *it->second;
  const int scale = std::clamp(monitor.buffer_scale, 1, 8);
  const int left = static_cast<int>(left64);
  const int top = static_cast<int>(top64);
  const bool resized = surface.list.bounds.width != list.bounds.width ||
                       surface.list.bounds.height != list.bounds.height;
  const bool moved = surface.margin_left != left || surface.margin_top != top;
  if (!surface.requested || resized || moved) {
    zwlr_layer_surface_v1_set_margin(surface.layer, top, 0, 0, left);
    zwlr_layer_surface_v1_set_size(surface.layer,
                                   static_cast<std::uint32_t>(list.bounds.width),
                                   static_cast<std::uint32_t>(list.bounds.height));
    surface.margin_left = left;
    surface.margin_top = top;
    // A new size is painted only after the configure that carries it.
    if (!surface.requested || resized) surface.configured = false;
    surface.requested = true;
    surface.dirty = true;
    wl_surface_commit(surface.surface);
  }
  if (surface.scale != scale || surface.list.ops != list.ops) surface.dirty = true;
  surface.scale = scale;
  surface.list = list;
  self.paint(surface);
  return true;
}

void WaylandOverlay::repaint() {
  for (auto &[key, surface] : impl_->surfaces)
    if (!surface->closed) impl_->paint(*surface);
}

void WaylandOverlay::remove(const std::string_view key) noexcept {
  const auto it = impl_->surfaces.find(key);
  if (it == impl_->surfaces.end()) return;
  destroy(*it->second);
  impl_->surfaces.erase(it);
  impl_->changed = true;
}

void WaylandOverlay::retain(const std::span<const std::string> keys) noexcept {
  for (auto it = impl_->surfaces.begin(); it != impl_->surfaces.end();) {
    if (std::ranges::find(keys, it->first) != keys.end()) {
      ++it;
      continue;
    }
    destroy(*it->second);
    it = impl_->surfaces.erase(it);
    impl_->changed = true;
  }
}

void WaylandOverlay::clear() noexcept { impl_->clear(); }

std::size_t WaylandOverlay::surface_count() const noexcept { return impl_->surfaces.size(); }

bool WaylandOverlay::mapped(const std::string_view key) const noexcept {
  const auto it = impl_->surfaces.find(key);
  return it != impl_->surfaces.end() && it->second->mapped && !it->second->closed;
}

const std::string &WaylandOverlay::fault() const noexcept { return impl_->fault; }

} // namespace layerc::platform

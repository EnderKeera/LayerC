// X11 Composite/Damage capture (V2.6.2). Sessions only name pixmaps
// redirected by another client; LayerC never redirects application windows.
#include "x11_capture.hpp"
#include "x11_error.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace layerc::platform {
namespace {

constexpr std::uint64_t max_source_pixels = 4'096ULL * 4'096ULL;
constexpr auto direct_capture_interval = std::chrono::milliseconds{100};

[[nodiscard]] bool version_at_least(const X11Extension &extension,
                                    const int major,
                                    const int minor) noexcept {
  return extension.present &&
         (extension.major > major ||
          (extension.major == major && extension.minor >= minor));
}

void append_diagnostic(std::string &target, const std::string &part) {
  if (part.empty()) return;
  if (!target.empty()) target += "; ";
  target += part;
}

[[nodiscard]] bool query_geometry(const X11Display *display,
                                  const core::WindowId window,
                                  std::uint32_t &width,
                                  std::uint32_t &height) noexcept {
  if (display == nullptr || !display->valid() || window.opaque == 0) return false;
  auto *raw = reinterpret_cast<Display *>(display->raw());
  ::Window root{};
  int x{};
  int y{};
  unsigned int native_width{};
  unsigned int native_height{};
  unsigned int border{};
  unsigned int depth{};
  X11ErrorTrap trap{raw};
  const Status status =
      XGetGeometry(raw, static_cast<Drawable>(window.opaque), &root, &x, &y,
                   &native_width, &native_height, &border, &depth);
  if (status == 0 || !trap.ok() || native_width == 0 || native_height == 0)
    return false;
  width = native_width;
  height = native_height;
  return true;
}

struct DamageEventMatch {
  int type{};
  Damage damage{};
};

Bool matches_damage(Display *, XEvent *event, XPointer argument) noexcept {
  const auto *match = reinterpret_cast<const DamageEventMatch *>(argument);
  if (match == nullptr || event->type != match->type) return False;
  const auto *damage_event = reinterpret_cast<const XDamageNotifyEvent *>(event);
  return damage_event->damage == match->damage ? True : False;
}

struct XImageDeleter {
  void operator()(XImage *image) const noexcept {
    if (image != nullptr) (void)XDestroyImage(image);
  }
};

[[nodiscard]] std::uint8_t component(const unsigned long pixel,
                                     const unsigned long mask) noexcept {
  if (mask == 0) return 0;
  unsigned long shifted_mask = mask;
  unsigned int shift{};
  while ((shifted_mask & 1UL) == 0UL) {
    shifted_mask >>= 1U;
    ++shift;
  }
  const unsigned long value = (pixel >> shift) & shifted_mask;
  return static_cast<std::uint8_t>(
      (value * 255UL + shifted_mask / 2UL) / shifted_mask);
}

[[nodiscard]] std::uint32_t sampled_coordinate(const std::uint32_t index,
                                               const std::uint32_t source,
                                               const std::uint32_t output) {
  return std::min<std::uint32_t>(
      source - 1U,
      static_cast<std::uint32_t>(
          (static_cast<std::uint64_t>(index) * source + source / 2U) /
          output));
}

} // namespace

X11CaptureSession::X11CaptureSession(const X11Display &display,
                                     const core::WindowId window,
                                     const core::CaptureQuality quality) noexcept
    : display_(&display), window_(window), quality_(quality) {}

X11CaptureSession::~X11CaptureSession() { stop(); }

bool X11CaptureSession::start() {
  stop();
  if (display_ == nullptr || !display_->valid() || window_.opaque == 0) {
    error_ = "x11 capture: invalid display or target window";
    return false;
  }
  if (!query_geometry(display_, window_, source_width_, source_height_)) {
    error_ = "x11 capture: target window is unavailable";
    return false;
  }

  started_ = true;
  dirty_ = true;
  refresh_named_pixmap();
  start_damage();
  return true;
}

void X11CaptureSession::stop() noexcept {
  release_damage();
  release_named_pixmap();
  started_ = false;
  dirty_ = false;
  source_width_ = 0;
  source_height_ = 0;
  backend_ = core::CaptureBackendKind::none;
  frame_.reset();
  health_.reset();
  next_capture_at_ = {};
  fallback_reason_.clear();
  damage_reason_.clear();
  error_.clear();
}

void X11CaptureSession::release_named_pixmap() noexcept {
  const auto pixmap = std::exchange(pixmap_, 0);
  if (pixmap == 0 || display_ == nullptr || !display_->valid()) return;
  auto *raw = reinterpret_cast<Display *>(display_->raw());
  X11ErrorTrap trap{raw}; // synchronizes on scope exit
  XFreePixmap(raw, static_cast<Pixmap>(pixmap));
}

void X11CaptureSession::release_damage() noexcept {
  const auto damage = std::exchange(damage_, 0);
  const auto region = std::exchange(damage_region_, 0);
  damage_event_base_ = 0;
  if (display_ == nullptr || !display_->valid()) return;
  auto *raw = reinterpret_cast<Display *>(display_->raw());
  X11ErrorTrap trap{raw}; // synchronizes on scope exit
  if (damage != 0) XDamageDestroy(raw, static_cast<Damage>(damage));
  if (region != 0)
    XFixesDestroyRegion(raw, static_cast<XserverRegion>(region));
}

void X11CaptureSession::refresh_named_pixmap() {
  release_named_pixmap();
  backend_ = core::CaptureBackendKind::x11_full_frame;
  const auto &composite = display_->probe().composite;
  if (!version_at_least(composite, 0, 2)) {
    fallback_reason_ =
        "x11 composite: version 0.2 unavailable; using bounded direct capture";
    return;
  }

  auto *raw = reinterpret_cast<Display *>(display_->raw());
  X11ErrorTrap trap{raw};
  const Pixmap pixmap = XCompositeNameWindowPixmap(
      raw, static_cast<::Window>(window_.opaque));
  if (pixmap == None || !trap.ok()) {
    fallback_reason_ =
        "x11 composite: window is not redirected; using bounded direct capture";
    return;
  }

  pixmap_ = static_cast<std::uint64_t>(pixmap);
  backend_ = core::CaptureBackendKind::x11_composite;
  fallback_reason_.clear();
}

void X11CaptureSession::start_damage() {
  release_damage();
  damage_reason_.clear();
  const auto &probe = display_->probe();
  if (!version_at_least(probe.damage, 1, 0)) {
    damage_reason_ =
        "x11 damage: version 1.0 unavailable; capture changes are polled";
    return;
  }
  if (!version_at_least(probe.xfixes, 2, 0)) {
    damage_reason_ =
        "x11 xfixes: version 2.0 unavailable; capture changes are polled";
    return;
  }

  auto *raw = reinterpret_cast<Display *>(display_->raw());
  X11ErrorTrap trap{raw};
  const XserverRegion region = XFixesCreateRegion(raw, nullptr, 0);
  const Damage damage = XDamageCreate(
      raw, static_cast<Drawable>(window_.opaque), XDamageReportNonEmpty);
  damage_region_ = static_cast<std::uint64_t>(region);
  damage_ = static_cast<std::uint64_t>(damage);
  damage_event_base_ = probe.damage.event_base;
  if (region == None || damage == None || !trap.ok()) {
    release_damage();
    damage_reason_ = "x11 damage: tracking setup failed; capture changes are polled";
  }
}

bool X11CaptureSession::collect_damage() {
  if (damage_ == 0 || damage_region_ == 0) return false;
  auto *raw = reinterpret_cast<Display *>(display_->raw());
  const auto damage = static_cast<Damage>(damage_);
  const auto region = static_cast<XserverRegion>(damage_region_);

  X11ErrorTrap trap{raw};
  XDamageSubtract(raw, damage, None, region);
  int rectangle_count{};
  XRectangle *rectangles = XFixesFetchRegion(raw, region, &rectangle_count);
  if (rectangles != nullptr) XFree(rectangles);

  bool event_seen = false;
  DamageEventMatch match{damage_event_base_ + XDamageNotify, damage};
  XEvent event{};
  while (XCheckIfEvent(raw, &event, &matches_damage,
                       reinterpret_cast<XPointer>(&match)) != False)
    event_seen = true;

  if (!trap.ok()) {
    release_damage();
    damage_reason_ =
        "x11 damage: tracking failed; capture changes are polled";
    return false;
  }
  return rectangle_count > 0 || event_seen;
}

std::shared_ptr<const capture::CaptureFrame>
X11CaptureSession::capture_frame(const core::TimePoint now,
                                 std::string &failure) {
  failure.clear();
  const std::uint64_t source_pixels =
      static_cast<std::uint64_t>(source_width_) * source_height_;
  if (source_pixels == 0 || source_pixels > max_source_pixels) {
    failure = "x11 capture: source exceeds the 4096x4096 pixel limit";
    return {};
  }

  auto *raw = reinterpret_cast<Display *>(display_->raw());
  const Drawable drawable = pixmap_ != 0
                                ? static_cast<Drawable>(pixmap_)
                                : static_cast<Drawable>(window_.opaque);
  X11ErrorTrap trap{raw};
  std::unique_ptr<XImage, XImageDeleter> image{
      XGetImage(raw, drawable, 0, 0, source_width_, source_height_, AllPlanes,
                ZPixmap)};
  if (!image || !trap.ok()) {
    failure = "x11 capture: reading the target drawable failed";
    return {};
  }

  unsigned long red_mask = image->red_mask;
  unsigned long green_mask = image->green_mask;
  unsigned long blue_mask = image->blue_mask;
  if (red_mask == 0 || green_mask == 0 || blue_mask == 0) {
    XWindowAttributes attributes{};
    X11ErrorTrap attributes_trap{raw};
    if (XGetWindowAttributes(raw, static_cast<::Window>(window_.opaque),
                             &attributes) == 0 ||
        !attributes_trap.ok() || attributes.visual == nullptr ||
        attributes.visual->c_class != TrueColor) {
      failure = "x11 capture: target TrueColor masks are unavailable";
      return {};
    }
    red_mask = attributes.visual->red_mask;
    green_mask = attributes.visual->green_mask;
    blue_mask = attributes.visual->blue_mask;
  }

  const auto [width, height] = capture::capture_sample_dimensions(
      source_width_, source_height_, quality_);
  if (width == 0 || height == 0) {
    failure = "x11 capture: sampled dimensions are empty";
    return {};
  }

  auto frame = std::make_shared<capture::CaptureFrame>();
  frame->width = width;
  frame->height = height;
  frame->captured_at = now;
  frame->pixels.resize(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto source_y = sampled_coordinate(y, source_height_, height);
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto source_x = sampled_coordinate(x, source_width_, width);
      const unsigned long pixel = XGetPixel(image.get(), source_x, source_y);
      frame->pixels[static_cast<std::size_t>(y) * width + x] = {
          component(pixel, red_mask), component(pixel, green_mask),
          component(pixel, blue_mask)};
    }
  }
  return frame;
}

capture::CapturePoll X11CaptureSession::current_result() {
  capture::CapturePollResult result;
  result.backend = backend_;
  result.diagnostic = warning();
  result.frame = frame_;
  result.availability = frame_ ? core::CaptureAvailability::available
                               : core::CaptureAvailability::starting;
  return capture::normalize_capture_poll(
      window_, health_.observe(std::move(result)));
}

capture::CapturePoll
X11CaptureSession::unavailable(std::string diagnostic,
                               const core::CaptureBackendKind backend) const {
  capture::CapturePollResult result;
  result.availability = core::CaptureAvailability::unavailable;
  result.backend = backend;
  result.diagnostic = std::move(diagnostic);
  return capture::normalize_capture_poll(window_, std::move(result));
}

std::string X11CaptureSession::warning() const {
  std::string result = fallback_reason_;
  append_diagnostic(result, damage_reason_);
  return result;
}

capture::CapturePoll X11CaptureSession::poll(const core::TimePoint now) {
  if (!started_) {
    capture::CapturePollResult result;
    result.availability = core::CaptureAvailability::unavailable;
    result.backend = backend_;
    result.diagnostic = error_.empty() ? "capture is not started" : error_;
    return capture::normalize_capture_poll(window_, std::move(result));
  }

  std::uint32_t width{};
  std::uint32_t height{};
  if (!query_geometry(display_, window_, width, height)) {
    frame_.reset();
    health_.reset();
    dirty_ = true;
    source_width_ = 0;
    source_height_ = 0;
    release_damage();
    release_named_pixmap();
    error_ = "x11 capture: target window is unavailable";
    std::string diagnostic = warning();
    append_diagnostic(diagnostic, error_);
    return unavailable(std::move(diagnostic), backend_);
  }

  if (width != source_width_ || height != source_height_) {
    source_width_ = width;
    source_height_ = height;
    frame_.reset();
    health_.reset();
    dirty_ = true;
    next_capture_at_ = {};
    refresh_named_pixmap();
  }

  if (damage_ != 0)
    dirty_ = collect_damage() || dirty_;
  else
    dirty_ = true;

  if (!dirty_) return current_result();
  if (backend_ == core::CaptureBackendKind::x11_full_frame && frame_ &&
      now < next_capture_at_)
    return current_result();

  std::string failure;
  auto captured = capture_frame(now, failure);
  if (!captured) {
    error_ = std::move(failure);
    std::string diagnostic = warning();
    append_diagnostic(diagnostic, error_);
    return unavailable(std::move(diagnostic), backend_);
  }

  frame_ = std::move(captured);
  dirty_ = false;
  error_.clear();
  if (backend_ == core::CaptureBackendKind::x11_full_frame)
    next_capture_at_ = now + direct_capture_interval;
  return current_result();
}

void X11CaptureSession::set_quality(const core::CaptureQuality quality) noexcept {
  if (quality_ == quality) return;
  quality_ = quality;
  frame_.reset();
  health_.reset();
  dirty_ = true;
  next_capture_at_ = {};
}

runtime::RuntimeResourceStats X11CaptureSession::resources() const noexcept {
  runtime::RuntimeResourceStats result;
  result.capture_sessions = started_ ? 1U : 0U;
  result.objects = static_cast<std::uint32_t>((pixmap_ != 0 ? 1U : 0U) +
                                              (damage_ != 0 ? 1U : 0U) +
                                              (damage_region_ != 0 ? 1U : 0U));
  if (frame_)
    result.cache_bytes = static_cast<std::uint64_t>(frame_->pixels.size()) *
                         sizeof(core::RgbPixel);
  return result;
}

} // namespace layerc::platform

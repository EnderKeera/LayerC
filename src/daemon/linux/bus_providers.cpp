#include "bus_providers.hpp"
#include "display_selection.hpp"

#include "dbus_shell_service.hpp"
#include "gnome_bridge.hpp"
#include "kwin_script.hpp"
#include "portal_capture_status.hpp"
#include "portal_screencast.hpp"
#ifdef LAYERC_HAVE_PIPEWIRE
#include "pipewire_capture.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <utility>

namespace layerc::daemon {
namespace {
namespace platform = layerc::platform;
namespace core = layerc::core;
namespace runtime = layerc::runtime;
using State = runtime::DisplayProviderState;
using Compositor = runtime::DisplayCompositor;
using Source = platform::WindowReportSource;
}

struct BusProviders::Impl {
  struct Session;
  platform::EventLoop *loop;
  const DisplaySource *window_source{};
  bool own_windows{};
  platform::BusConnection bus;
  platform::ShellService shell{bus};
  platform::KwinScript kwin{bus};
  platform::GnomeBridge bridge{bus};
  platform::ScreencastTokenStore tokens{platform::screencast_token_path()};
  std::set<Session *> captures;
  std::map<std::string, platform::GnomePanel, std::less<>> panels;
  // Panels whose proposal exceeded the frame bounds; reported, never sent.
  std::set<std::string, std::less<>> rejected;
  std::string desktop, reported_identity;
  bool tracking{}, overlay_wanted{}, attempted{}, stopped{}, dirty{};
  std::optional<core::TimePoint> close_at;

  explicit Impl(platform::EventLoop &event_loop) : loop(&event_loop) {}

  [[nodiscard]] Compositor compositor() const {
    return detect_compositor(desktop, bus.owner(platform::kwin_bus_name).has_value(),
        bus.owner(platform::gnome_shell_bus_name).has_value(), false, false);
  }
  [[nodiscard]] std::optional<Source> source() const {
    if (compositor() == Compositor::kwin) return Source::kwin;
    if (compositor() == Compositor::gnome) return Source::gnome;
    return std::nullopt;
  }
  [[nodiscard]] std::optional<platform::WindowScan> scan() const {
    const auto selected = source();
    return selected ? shell.windows(*selected) : std::nullopt;
  }
  [[nodiscard]] platform::WindowScan windows() const {
    if (auto reported = scan()) return std::move(*reported);
    return {};
  }
  [[nodiscard]] std::optional<platform::WindowSnapshot> window(std::uint64_t id) const {
    const auto all = window_source ? window_source->windows() : platform::WindowScan{};
    const auto found = std::ranges::find(all.windows, id, &platform::WindowSnapshot::window);
    return found == all.windows.end() ? std::nullopt : std::optional{*found};
  }
  [[nodiscard]] std::string capture_identity() const {
    return own_windows ? identity() : "foreign_toplevel";
  }
  [[nodiscard]] std::string identity() const {
    const auto selected = source();
    if (!selected) return "foreign_toplevel";
    return std::string{platform::window_report_source_name(*selected)} + ":" +
        bus.owner(platform::window_report_bus_name(*selected)).value_or("");
  }
  void sync_providers() {
    const auto selected = compositor();
    kwin.set_wanted(tracking && shell.name_state() == platform::ShellService::NameState::owned &&
                    selected == Compositor::kwin);
    kwin.sync();
    bridge.set_wanted(tracking && overlay_wanted && selected == Compositor::gnome);
    bridge.sync();
  }
  void shutdown() noexcept;
};

struct BusProviders::Impl::Session final : VisualCaptureSession {
  // Capture ownership can outlive the provider wrapper during quiescence;
  // the shared owner keeps the bus valid until every portal has closed.
  std::shared_ptr<Impl> owner;
  platform::PortalScreencast portal;
#ifdef LAYERC_HAVE_PIPEWIRE
  platform::PipeWireCapture pipewire;
#endif
  core::WindowId window;
  std::string profile, identity, fault;
  bool started{};
  std::optional<runtime::DisplayProviderStatus> previous;

  Session(std::shared_ptr<Impl> host, std::string_view id, core::WindowId target,
          core::CaptureQuality quality, std::uint8_t rate)
      : owner(std::move(host)), portal(owner->bus, owner->tokens),
#ifdef LAYERC_HAVE_PIPEWIRE
        pipewire(*owner->loop, target, quality),
#endif
        window(target), profile(id) {
#ifndef LAYERC_HAVE_PIPEWIRE
    (void)quality; (void)rate;
#else
    pipewire.set_sample_rate(rate);
#endif
    owner->captures.insert(this);
  }
  ~Session() override { stop(); owner->captures.erase(this); }

  bool start() override {
    if (started) return fault.empty();
    started = true;
    fault.clear();
#ifndef LAYERC_HAVE_PIPEWIRE
    fault = "pipewire_not_built";
#else
    if (owner->stopped || !owner->tracking || !owner->bus.connected()) {
      fault = owner->bus.reason().empty() ? "bus_unavailable" : owner->bus.reason();
    } else if (!owner->window(window.opaque)) {
      fault = "window_unavailable";
    } else {
      identity = owner->capture_identity();
      if (!portal.start(profile)) fault = portal.reason();
    }
#endif
    return fault.empty();
  }
  void stop() noexcept override {
#ifdef LAYERC_HAVE_PIPEWIRE
    pipewire.stop();
#endif
    portal.stop();
    started = false;
    fault.clear();
    previous.reset();
  }
  void invalidate(std::string reason) {
#ifdef LAYERC_HAVE_PIPEWIRE
    pipewire.stop();
#endif
    portal.stop();
    fault = std::move(reason); // sticky until configuration or demand changes
  }
  void set_quality(core::CaptureQuality quality) noexcept override {
#ifdef LAYERC_HAVE_PIPEWIRE
    pipewire.set_quality(quality);
#else
    (void)quality;
#endif
  }
  void set_sample_rate(std::uint8_t rate) noexcept override {
#ifdef LAYERC_HAVE_PIPEWIRE
    pipewire.set_sample_rate(rate);
#else
    (void)rate;
#endif
  }
  bool dispatch(std::span<const int> ready) {
    if (!started) return false;
#ifdef LAYERC_HAVE_PIPEWIRE
    if (fault.empty()) {
      const auto target = owner->window(window.opaque);
      if (owner->stopped || !owner->bus.connected()) {
        invalidate(owner->bus.reason().empty() ? "bus_unavailable" : owner->bus.reason());
      } else if (!target || identity != owner->capture_identity()) {
        invalidate("window_provider_changed");
      } else {
        pipewire.set_window({target->x, target->y, target->width, target->height});
        portal.sync();
        if (!portal.ready() && portal.reason() != "portal_pending") {
          invalidate(portal.reason().empty() ? "portal_unavailable" : portal.reason());
        } else if (auto remote = portal.take_remote()) {
          if (!pipewire.start(std::move(*remote))) invalidate(pipewire.reason());
        }
        const bool active = pipewire.active();
        pipewire.dispatch(ready);
        if (active && !pipewire.active()) invalidate(pipewire.reason());
      }
    }
#else
    (void)ready;
#endif
    const auto current = status();
    const bool changed = previous != current;
    previous = current;
    return changed;
  }
  layerc::capture::CapturePoll poll(core::TimePoint now) override {
    if (!fault.empty()) return layerc::capture::normalize_capture_poll(window,
        {core::CaptureAvailability::unavailable, core::CaptureBackendKind::pipewire_portal, fault});
#ifdef LAYERC_HAVE_PIPEWIRE
    auto result = pipewire.poll(now);
    if (!portal.reason().empty() && portal.reason() != "portal_pending") {
      result = layerc::capture::normalize_capture_poll(window,
          {core::CaptureAvailability::unavailable, core::CaptureBackendKind::pipewire_portal,
           portal.reason()});
    }
    return result;
#else
    (void)now;
    return layerc::capture::normalize_capture_poll(window,
        {core::CaptureAvailability::unavailable, core::CaptureBackendKind::pipewire_portal,
         "pipewire_not_built"});
#endif
  }
  [[nodiscard]] runtime::DisplayProviderStatus status() {
    return portal_capture_status(
        started, poll(core::MonotonicClock::now()).result);
  }
  runtime::RuntimeResourceStats resources() const noexcept override {
    runtime::RuntimeResourceStats result;
    if (started) result.objects = 1; // consent-session owner (bus is shared)
#ifdef LAYERC_HAVE_PIPEWIRE
    result += pipewire.resources();
#endif
    return result;
  }
};

void BusProviders::Impl::shutdown() noexcept {
  try {
    for (auto *session : captures) session->stop();
    panels.clear();
    rejected.clear();
    bridge.set_wanted(false);
    kwin.set_wanted(false);
    shell.stop();
    // Drain only queued writes, not reply timeouts, on the blocking host loop.
    // Never sd_bus_flush here: it can block on a congested socket.
    if (bus.outgoing_pending()) close_at = core::MonotonicClock::now() + std::chrono::seconds{5};
    else { bus.close(); close_at.reset(); }
  } catch (...) { bus.close(); close_at.reset(); }
}

BusProviders::BusProviders(platform::EventLoop &loop)
    : impl_(std::make_shared<Impl>(loop)) {}
BusProviders::~BusProviders() { stop(); impl_->bus.close(); }

void BusProviders::configure(bool tracking, std::string_view desktop, bool configuration_changed) {
  auto &host = *impl_;
  host.desktop = desktop;
  if (tracking == host.tracking && !configuration_changed) return;
  host.stopped = false;
  if (!tracking) {
    host.tracking = host.overlay_wanted = host.attempted = false;
    host.shutdown();
    return;
  }
  if (configuration_changed) {
    host.kwin.set_wanted(false);
    for (auto *session : host.captures) session->stop();
  }
  host.tracking = true;
  host.close_at.reset();
  if (!host.attempted || configuration_changed) {
    host.attempted = true;
    if (host.bus.open(*host.loop)) host.shell.start();
  }
  host.sync_providers();
}
void BusProviders::use_window_source(const DisplaySource *source) {
  if (impl_->window_source == source) return;
  for (auto *session : impl_->captures)
    if (session->started) session->invalidate("window_provider_changed");
  impl_->window_source = source;
  impl_->own_windows = source == this;
}
void BusProviders::want_overlay(bool wanted) {
  impl_->overlay_wanted = wanted;
  impl_->sync_providers();
}
bool BusProviders::dispatch(std::span<const int> ready) {
  auto &host = *impl_;
  (void)host.bus.dispatch();
  if (host.close_at && (!host.bus.outgoing_pending() || core::MonotonicClock::now() >= *host.close_at)) {
    host.bus.close(); host.close_at.reset();
  }
  if (host.tracking && !host.stopped) host.sync_providers();
  bool changed = host.bus.take_owners_changed();
  changed = host.shell.take_changed() || changed;
  changed = host.bridge.take_changed() || changed;
  const auto identity = host.identity();
  if (host.reported_identity != identity) {
    host.panels.clear();
    host.rejected.clear();
    host.reported_identity = identity;
    changed = true;
  }
  for (auto *session : host.captures) changed = session->dispatch(ready) || changed;
  return std::exchange(host.dirty, false) || changed;
}
void BusProviders::present() {
  auto &host = *impl_;
  if (!host.bridge.available()) return;
  std::vector<platform::GnomePanel> panels;
  panels.reserve(host.panels.size());
  for (const auto &[key, panel] : host.panels) { (void)key; panels.push_back(panel); }
  (void)host.bridge.present(panels);
}
void BusProviders::stop() noexcept {
  impl_->stopped = true;
  impl_->tracking = impl_->overlay_wanted = false;
  impl_->shutdown();
}
std::optional<core::TimePoint> BusProviders::next_deadline() const noexcept {
  return core::earliest_deadline(impl_->close_at, impl_->bus.next_deadline());
}
bool BusProviders::connected() const noexcept { return impl_->bus.connected(); }
bool BusProviders::kwin_owner() const { return impl_->bus.owner(platform::kwin_bus_name).has_value(); }
bool BusProviders::gnome_owner() const { return impl_->bus.owner(platform::gnome_shell_bus_name).has_value(); }

std::optional<runtime::DisplayProviderStatus> BusProviders::window_provider() const {
  const auto selected = impl_->source();
  if (!selected) return std::nullopt;
  const char *name = *selected == Source::kwin ? "kwin_dbus" : "gnome_shell";
  if (!impl_->tracking) return runtime::DisplayProviderStatus{name, State::inactive, {"not_demanded"}};
  std::string reason = impl_->bus.reason();
  if (reason.empty()) reason = impl_->shell.reason();
  if (reason.empty()) reason = impl_->shell.report_error(*selected);
  const auto reported = impl_->scan();
  if (reason.empty() && !reported) {
    reason = *selected == Source::kwin && !impl_->kwin.reason().empty()
        ? impl_->kwin.reason() : "window_reports_pending";
  }
  return runtime::DisplayProviderStatus{name,
      reported ? reason.empty() ? State::active : State::degraded : State::unavailable,
      reason.empty() ? std::vector<std::string>{} : std::vector<std::string>{reason}};
}
std::optional<runtime::DisplayProviderStatus> BusProviders::overlay_provider() const {
  if (impl_->compositor() != Compositor::gnome) return std::nullopt;
  if (!impl_->overlay_wanted)
    return runtime::DisplayProviderStatus{"gnome_shell", State::inactive, {"not_demanded"}};
  if (!impl_->bridge.available())
    return runtime::DisplayProviderStatus{"gnome_shell", State::unavailable, unavailable()};
  // Rejected panels degrade the overlay without disabling the healthy transport.
  std::string reason = impl_->rejected.empty() ? impl_->bridge.reason()
                                               : std::string{"gnome_frame_too_large"};
  if (reason.empty()) return runtime::DisplayProviderStatus{"gnome_shell", State::active, {}};
  return runtime::DisplayProviderStatus{"gnome_shell", State::degraded, {std::move(reason)}};
}
bool BusProviders::capture_demanded() const noexcept {
  return std::ranges::any_of(impl_->captures, [](const auto *session) { return session->started; });
}
runtime::DisplayProviderStatus BusProviders::capture_provider() const {
#ifndef LAYERC_HAVE_PIPEWIRE
  return {"portal_screencast", State::not_built, {"pipewire_not_built"}};
#else
  runtime::DisplayProviderStatus result{"portal_screencast", State::inactive, {"not_demanded"}};
  bool demanded = false;
  for (auto *session : impl_->captures) {
    if (!session->started) continue;
    const auto current = session->status();
    if (!demanded) { result = current; demanded = true; continue; }
    // Retain every failure instead of masking it with another working stream.
    if (current.state == State::unavailable) result.state = State::unavailable;
    else if (current.state == State::degraded && result.state == State::active) result.state = State::degraded;
    for (const auto &reason : current.reasons)
      if (std::ranges::find(result.reasons, reason) == result.reasons.end()) result.reasons.push_back(reason);
  }
  return result;
#endif
}
bool BusProviders::has_windows() const {
  return impl_->scan().has_value();
}
platform::WindowScan BusProviders::windows() const { return impl_->windows(); }
bool BusProviders::has_monitors() const { return false; }
platform::MonitorScan BusProviders::monitors() const { return {}; }
OverlayBackend *BusProviders::overlay() {
  return impl_->compositor() == Compositor::gnome && impl_->bridge.available() ? this : nullptr;
}
bool BusProviders::show(std::string_view key, const platform::OverlayFrame &frame) {
  if (!impl_->bridge.available()) return false;
  // Enforce whole-frame bounds before committing the replacement panel.
  auto panels = impl_->panels;
  panels.insert_or_assign(std::string{key}, platform::GnomePanel{std::string{key},
                          platform::build_overlay_display_list(frame)});
  std::vector<platform::GnomePanel> proposed;
  for (const auto &[id, panel] : panels) { (void)id; proposed.push_back(panel); }
  if (!platform::build_gnome_overlay_frame(proposed)) {
    // Reject atomically: the accepted panels and the transport stay intact,
    // and the failure stays visible until this panel fits or goes away.
    if (impl_->rejected.insert(std::string{key}).second) impl_->dirty = true;
    return false;
  }
  impl_->panels = std::move(panels);
  if (const auto found = impl_->rejected.find(key); found != impl_->rejected.end()) {
    impl_->rejected.erase(found);
    impl_->dirty = true;
  }
  return true;
}
void BusProviders::hide(std::string_view key) noexcept {
  try {
    impl_->panels.erase(std::string{key});
    if (const auto found = impl_->rejected.find(key); found != impl_->rejected.end()) {
      impl_->rejected.erase(found);
      impl_->dirty = true;
    }
  } catch (...) {}
}
void BusProviders::retain(std::span<const std::string> keys) noexcept {
  const auto gone = [&](const std::string &key) {
    return std::ranges::find(keys, key) == keys.end();
  };
  std::erase_if(impl_->panels, [&](const auto &entry) { return gone(entry.first); });
  if (std::erase_if(impl_->rejected, gone) != 0) impl_->dirty = true;
}
std::vector<std::string> BusProviders::unavailable() const {
  if (!impl_->bus.reason().empty()) return {impl_->bus.reason()};
  return impl_->bridge.reason().empty() ? std::vector<std::string>{}
                                      : std::vector<std::string>{impl_->bridge.reason()};
}
std::optional<std::uint64_t> BusProviders::x11_window(std::uint64_t id) const {
  const auto target = impl_->window(id);
  return target ? target->x11_window : std::nullopt;
}
std::unique_ptr<VisualCaptureSession> BusProviders::create_capture(
    std::string_view profile_id, core::WindowId window, core::CaptureQuality quality,
    std::uint8_t sample_rate) {
  return std::make_unique<Impl::Session>(impl_, profile_id, window, quality, sample_rate);
}

} // namespace layerc::daemon

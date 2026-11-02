// Display-free regression for the shared visual evaluator's native seam.
// No compositor, portal, production profile store, or worker thread is used.
#include "visual_service.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {
namespace core = layerc::core;
namespace daemon = layerc::daemon;
namespace capture = layerc::capture;
namespace runtime = layerc::runtime;
using layerc::test::expect;
using namespace std::chrono_literals;

struct CaptureState {
  unsigned created{};
  unsigned live{};
  unsigned polls{};
  bool failed{};
  core::CaptureQuality quality{};
  std::uint8_t sample_rate{};
};

class FakeCapture final : public daemon::VisualCaptureSession {
public:
  FakeCapture(CaptureState &state, core::WindowId window) : state_(&state), window_(window) {
    ++state_->created;
    ++state_->live;
  }
  ~FakeCapture() override { --state_->live; }
  bool start() override { running_ = true; return true; }
  void stop() noexcept override { running_ = false; }
  void set_quality(core::CaptureQuality quality) noexcept override { state_->quality = quality; }
  void set_sample_rate(std::uint8_t rate) noexcept override { state_->sample_rate = rate; }
  capture::CapturePoll poll(core::TimePoint now) override {
    ++state_->polls;
    if (!running_ || state_->failed)
      return capture::normalize_capture_poll(window_, {
          core::CaptureAvailability::unavailable, core::CaptureBackendKind::pipewire_portal,
          "portal_cancelled", nullptr});
    auto frame = std::make_shared<capture::CaptureFrame>();
    frame->width = frame->height = 2;
    frame->captured_at = now;
    frame->pixels.assign(4, core::RgbPixel{0, 136, 0});
    return capture::normalize_capture_poll(window_, {
        core::CaptureAvailability::available, core::CaptureBackendKind::pipewire_portal,
        {}, std::move(frame)});
  }
  runtime::RuntimeResourceStats resources() const noexcept override {
    runtime::RuntimeResourceStats result;
    if (running_) result.capture_sessions = 1;
    return result;
  }
private:
  CaptureState *state_;
  core::WindowId window_;
  bool running_{};
};

class FakeFactory final : public daemon::CaptureFactory {
public:
  CaptureState state;
  bool xwayland{};
  std::optional<std::uint64_t> x11_window(std::uint64_t) const override {
    return xwayland ? std::optional<std::uint64_t>{23} : std::nullopt;
  }
  std::unique_ptr<daemon::VisualCaptureSession>
  create_capture(std::string_view, core::WindowId window,
                 core::CaptureQuality quality, std::uint8_t rate) override {
    state.quality = quality;
    state.sample_rate = rate;
    return std::make_unique<FakeCapture>(state, window);
  }
};

daemon::ConfiguredProfile profile(const std::filesystem::path &path, const char *id,
                                  core::CaptureQuality quality, std::uint8_t rate) {
  daemon::ConfiguredProfile result;
  result.source = {path, {}};
  result.source.profile.id = id;
  result.source.profile.name = id;
  result.source.profile.cooldowns = {{.id = id, .name = id, .duration = 60s}};
  auto &module = result.modules.get<core::VisualModule>();
  module.state = layerc::platform::ModuleSidecarState::valid;
  core::VisualProfile visual;
  visual.profile_id = id;
  visual.quality = quality;
  visual.sample_rate_hz = rate;
  visual.observations.push_back(core::PixelRule{
      .id = "green", .name = "Green", .region = {0, 0, 1, 1},
      .predicate = core::AverageColorPredicate{0x008800, 0}, .stable_samples = 1});
  visual.triggers = {{.cooldown_id = id, .observation_id = "green"}};
  module.profile = std::move(visual);
  return result;
}

runtime::CapabilityDemand demand_for(const std::vector<daemon::InputSession> &sessions) {
  std::vector<runtime::SessionCapabilityRequest> requests;
  for (const auto &session : sessions)
    requests.push_back({.window = {session.window}, .active = true, .visual = true,
                        .profile_id = session.automation.profile().id});
  return runtime::calculate_capability_demand(requests, false);
}

void native_visual_shares_evaluator_and_releases_demand() {
  layerc::test::TempDirectory temp{"layerc-native-visual"};
  layerc::platform::AutomationPersistence persistence;
  FakeFactory factory;
  daemon::VisualService visual;
  visual.use_route({.native = &factory, .native_windows = true});
  std::vector<daemon::InputSession> sessions;
  sessions.emplace_back(profile(temp.path() / "a.json", "a", core::CaptureQuality::low, 10),
                        55, persistence);
  sessions.emplace_back(profile(temp.path() / "b.json", "b", core::CaptureQuality::exact, 30),
                        55, persistence);
  for (auto &session : sessions) session.active = true;
  const auto demand = demand_for(sessions);
  visual.refresh(sessions, demand, true);
  expect(visual.reconcile(demand).status == runtime::ServiceReconcileStatus::ok &&
             factory.state.created == 1 && factory.state.live == 1 && factory.state.quality == core::CaptureQuality::exact &&
             factory.state.sample_rate == 30,
         "one native capture uses the highest consumer quality and sample rate");
  const auto now = core::MonotonicClock::now();
  const auto triggered = visual.poll(sessions, now);
  expect(std::ranges::find(triggered, "a") != triggered.end() &&
             std::ranges::find(triggered, "b") != triggered.end() && factory.state.polls == 1,
         "the shared evaluator feeds both cooldown sessions from one native frame");
  const auto work = visual.work_stats();
  const auto deadline = visual.next_deadline(now);
  visual.refresh(sessions, demand);
  expect(visual.reconcile(demand).status == runtime::ServiceReconcileStatus::ok &&
             factory.state.created == 1 && visual.work_stats() == work && visual.next_deadline(now) == deadline,
         "unchanged native scans retain the capture, evaluation and sample deadline");
  sessions.front().visual_profile()->sample_rate_hz = 40;
  visual.refresh(sessions, demand);
  expect(factory.state.created == 1 && factory.state.sample_rate == 40,
         "a retained native capture receives a changed maximum sample rate");
  factory.state.failed = true;
  expect(visual.poll(sessions, now + 200ms).empty(), "a denied native frame cannot trigger");
  const auto unavailable = visual.unavailable();
  expect(std::ranges::find(unavailable, "portal_cancelled") != unavailable.end(),
         "portal fault reasons remain visible through the shared visual service");
  visual.refresh(sessions, demand, true);
  expect(factory.state.created == 2 && factory.state.live == 1,
         "an explicit reload recreates the native consent lifetime without leaking the old one");
  visual.refresh(sessions, {});
  expect(factory.state.live == 0 && visual.resources().capture_sessions == 0 &&
             visual.resources().timers == 0 && !visual.next_deadline(now),
         "lost visual demand releases capture and the sampling deadline");
  visual.use_route({});
}

void xwayland_identity_does_not_request_portal_consent() {
  layerc::test::TempDirectory temp{"layerc-native-xwayland"};
  layerc::platform::AutomationPersistence persistence;
  FakeFactory factory;
  factory.xwayland = true;
  daemon::VisualService visual;
  visual.use_route({.native = &factory, .native_windows = true});
  std::vector<daemon::InputSession> sessions;
  sessions.emplace_back(profile(temp.path() / "a.json", "a", core::CaptureQuality::low, 10),
                        55, persistence);
  const auto demand = demand_for(sessions);
  visual.refresh(sessions, demand, true);
  (void)visual.reconcile(demand);
  expect(factory.state.created == 0,
         "an explicit XWayland XID uses X11 capture, never the native portal route");
  visual.quiesce();
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(native_visual_shares_evaluator_and_releases_demand),
                 LAYERC_TEST_CASE(xwayland_identity_does_not_request_portal_consent))

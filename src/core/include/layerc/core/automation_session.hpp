#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/automation_checkpoint.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/pixel_engine.hpp"

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::core {

// Platform-neutral output: native adapters publish traces, diagnostics and
// durable checkpoints without making the session depend on a profile store.
class AutomationSessionSink {
public:
  virtual ~AutomationSessionSink() = default;
  virtual void on_result(const AutomationEvaluationResult &) {}
  virtual void on_persistent_change(const AutomationPersistentValues &,
                                    TimePoint) {}
  virtual void on_stopping(TimePoint) {}
};

// Current runtime faults of one session's automation: the last evaluation
// error, and at most one fault per automation that faulted during an event.
// Both daemons report these lines; they never block unrelated automations.
struct AutomationRuntimeHealth {
  std::optional<std::string> runtime_error;
  std::map<std::string, std::string, std::less<>> runtime_faults;

  void record(const AutomationEvaluationResult &result);
  // One line per fault, prefixed with the session's profile source.
  [[nodiscard]] std::vector<std::string> lines(std::string_view source) const;
};

struct SessionFeedResult {
  std::vector<std::string> triggered;
  bool changed{};
  bool matched{};
};

// One owner for a matched profile's cooldown/automation/visual state. Native
// sessions compose it; platform glue never participates in its lifecycle.
// Every call evaluates against the session's own `profile`.
class AutomationSession {
public:
  explicit AutomationSession(GameProfile matched);
  AutomationSession(AutomationSession &&) = default;
  AutomationSession &operator=(AutomationSession &&) = default;
  AutomationSession(const AutomationSession &) = delete;
  AutomationSession &operator=(const AutomationSession &) = delete;
  ~AutomationSession() = default;

  void set_invalid_visual_sidecar(bool invalid) noexcept {
    invalid_visual_sidecar_ = invalid;
  }
  [[nodiscard]] bool blocks_all_triggers() const noexcept {
    return invalid_visual_sidecar_;
  }
  [[nodiscard]] static std::uint64_t next_origin() noexcept;
  [[nodiscard]] static AutomationEvent event(AutomationEventKind kind,
                                             std::string source, TimePoint now,
                                             std::optional<std::uint64_t> origin = std::nullopt);

  [[nodiscard]] bool start(AutomationProfile automation,
                           AutomationPersistentValues values, TimePoint now,
                           AutomationSessionSink *sink = nullptr,
                           bool trace_enabled = false,
                           AutomationRuntime::ClockReader clock = {});
  [[nodiscard]] bool stop(TimePoint now, AutomationSessionSink *sink = nullptr);
  [[nodiscard]] bool process(const AutomationEvent &value,
                             AutomationSessionSink *sink = nullptr);
  [[nodiscard]] bool poll_ready(TimePoint now, bool emit,
                                AutomationSessionSink *sink = nullptr);
  [[nodiscard]] SessionFeedResult trigger(std::string_view id, TimePoint now,
                                           AutomationSessionSink *sink = nullptr);
  [[nodiscard]] SessionFeedResult feed_input(InputMatcher &matcher,
                                              const InputEvent &input,
                                              TimePoint now,
                                              AutomationSessionSink *sink = nullptr);
  // Continuous input (controller axes, HID values): applies the automation
  // profile's demand check, deadzone, rate limit and change threshold, and
  // returns the input_value_changed event to process, if any. A momentary
  // (relative) sample reports from 0 on every accepted sample.
  [[nodiscard]] std::optional<AutomationEvent>
  input_value_event(std::string source, double value, TimePoint now,
                    bool momentary = false);
  [[nodiscard]] SessionFeedResult feed_measurements(
      std::span<const ObservationMeasurement> measurements, TimePoint now,
      AutomationSessionSink *sink = nullptr);
  [[nodiscard]] bool poll(TimePoint now, AutomationSessionSink *sink = nullptr);
  [[nodiscard]] std::optional<TimePoint> next_deadline(
      TimePoint now, std::optional<TimePoint> checkpoint = std::nullopt);
  // Call only for sessions of the same ProfileSource; the owning registry
  // chooses siblings and persists the source's winning value once.
  [[nodiscard]] bool sync_persistent_to(AutomationSession &sibling) const;

  // Native reconciliation supplies the cooldown reset set; the session keeps
  // definitions, engine and reusable snapshot storage consistent.
  void reconfigure_profile(GameProfile matched,
                           std::span<const std::string> reset_cooldown_ids,
                           TimePoint now);
  void configure_visual(const std::optional<VisualProfile> &visual,
                        bool enabled = true);
  // Used when a sidecar/capability disappears, without synthesizing lifecycle
  // events. Normal retirement uses stop() and publishes through the sink.
  void discard_runtime() noexcept { automation_runtime_.reset(); }
  [[nodiscard]] std::span<const CooldownState> snapshot(TimePoint now);

  [[nodiscard]] const GameProfile &profile() const noexcept { return profile_; }
  [[nodiscard]] const AutomationRuntimeHealth &health() const noexcept { return automation_health_; }
  [[nodiscard]] CooldownEngine &cooldowns() noexcept { return cooldowns_; }
  [[nodiscard]] const CooldownEngine &cooldowns() const noexcept { return cooldowns_; }
  [[nodiscard]] VisualTriggerEngine *visual_triggers() noexcept { return visual_triggers_.get(); }
  [[nodiscard]] const VisualTriggerEngine *visual_triggers() const noexcept { return visual_triggers_.get(); }
  [[nodiscard]] AutomationRuntime *runtime() noexcept { return automation_runtime_.get(); }
  [[nodiscard]] const AutomationRuntime *runtime() const noexcept { return automation_runtime_.get(); }
  [[nodiscard]] const std::vector<CooldownState> &snapshot_buffer() const noexcept { return snapshot_buffer_; }

private:
  GameProfile profile_;
  AutomationRuntimeHealth automation_health_;
  CooldownEngine cooldowns_;
  std::unique_ptr<VisualTriggerEngine> visual_triggers_;
  std::unique_ptr<AutomationRuntime> automation_runtime_;
  std::vector<CooldownState> snapshot_buffer_;

  [[nodiscard]] SessionFeedResult trigger_requests(
      std::span<const VisualTriggerRequest> requests, TimePoint now,
      AutomationSessionSink *sink);
  bool invalid_visual_sidecar_{};
};

} // namespace layerc::core

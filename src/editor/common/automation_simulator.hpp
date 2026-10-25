#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::editor {

// What the last processed event actually did: the committed effects and
// derived events the runtime returns, plus the trace entries that call added.
struct SimulatorStepReport {
  layerc::core::AutomationEvent root;
  std::size_t timer_events{};
  // Entries added during the step; the root provenance phase is excluded.
  std::vector<layerc::core::AutomationTraceEntry> trace;
  std::vector<layerc::core::AutomationEffect> effects;
  std::vector<layerc::core::AutomationEvent> derived;
  std::optional<std::string> rejection;
};

// The editors' deterministic automation simulator: a runtime compiled from the
// edited graph replays an editable list of root events one step at a time,
// firing due timers and expiring overrides first, and pauses on a rejected
// root or a breakpoint rule. It reuses the core evaluator and never touches a
// daemon; frontends own only the controls and the playback timer.
class AutomationSimulator final {
public:
  struct Reset {
    // Shown instead of the state when the graph does not compile.
    std::optional<std::string> error;
    // The sessionStarted root seeded for a fresh session, never a loaded trace.
    std::optional<layerc::core::AutomationEvent> seeded;
  };

  AutomationSimulator();
  ~AutomationSimulator();
  AutomationSimulator(const AutomationSimulator &) = delete;
  AutomationSimulator &operator=(const AutomationSimulator &) = delete;

  // Compiles the graph JSON against its base profile and rewinds to the
  // first event. A failed compile leaves no runtime.
  [[nodiscard]] Reset reset(std::string_view graph_json,
                            const layerc::core::GameProfile &profile);
  // Processes the next event. Returns whether playback may continue: false
  // when nothing is compiled or left, the root was rejected, the breakpoint
  // rule ran, or the last event was just processed.
  [[nodiscard]] bool step(std::string_view breakpoint_rule);
  // Appends a synthetic root 100 ms after the previous one. fields is a
  // ';'-separated list of name=JSON, or name:type=JSON for a field the event
  // kind does not define. Returns the event or the reason it was refused.
  [[nodiscard]] std::variant<layerc::core::AutomationEvent, std::string>
  add_event(layerc::core::AutomationEventKind kind, std::string source_id,
            std::string_view fields);
  // Replaces the event list exactly, including an explicitly empty trace.
  // Subsequent resets compile/rewind without adding starter roots.
  void load(std::vector<layerc::core::AutomationEvent> events);
  // Starts a fresh session; its first successful reset may seed sessionStarted.
  void clear();

  [[nodiscard]] bool compiled() const noexcept { return runtime_ != nullptr; }
  [[nodiscard]] std::size_t position() const noexcept { return index_; }
  [[nodiscard]] const std::vector<layerc::core::AutomationEvent> &
  events() const noexcept {
    return events_;
  }
  [[nodiscard]] const layerc::core::AutomationRuntime *runtime() const noexcept {
    return runtime_.get();
  }
  // The report of the last step(), cleared by reset/load/clear.
  [[nodiscard]] const std::optional<SimulatorStepReport> &last_step() const noexcept {
    return last_step_;
  }
  // Progress, timers, variables and, when assertions ("id=JSON; ...") are
  // given, whether every declared variable currently equals its value.
  // Lines end with '\n'.
  [[nodiscard]] std::string describe(std::string_view assertions) const;

private:
  std::unique_ptr<layerc::core::AutomationRuntime> runtime_;
  std::vector<layerc::core::AutomationEvent> events_;
  std::size_t index_{};
  bool seed_empty_{true};
  std::optional<SimulatorStepReport> last_step_;
};

// Playback interval for the slow/normal/fast speed choices (0, 1, 2).
[[nodiscard]] std::chrono::milliseconds simulator_interval(int speed) noexcept;

// Editor import allocation policy, distinct from the 4MiB trace format bound.
// Resource refusal never changes the file or the current editor trace/profile.
inline constexpr std::size_t max_trace_import_json_values = 131072;

// .layerctrace files: a bounded read that parses the root events.
struct LoadedTrace {
  std::string text;
  std::vector<layerc::core::AutomationEvent> events;
};
struct TraceLoadError {
  std::string message;
  // The file was read but is not a valid trace (versus unreadable/too big).
  bool invalid{};
};
[[nodiscard]] std::variant<LoadedTrace, TraceLoadError>
read_trace_file(const std::filesystem::path &path);
[[nodiscard]] bool write_trace_file(const std::filesystem::path &path,
                                    std::string_view text);
// Exports events as replay roots, numbered from 1.
[[nodiscard]] std::string
trace_from_events(std::span<const layerc::core::AutomationEvent> events);

// The bounded record of live LayerD trace packets an editor keeps.
class LiveTraceLog final {
public:
  static constexpr std::size_t max_events = layerc::core::max_automation_trace_entries;
  // Appends a packet's events (keeping the newest max_events) and returns
  // the status text, or the rejection text for a malformed packet.
  std::string append_packet(std::string_view packet);
  void clear();
  [[nodiscard]] const std::vector<layerc::core::AutomationEvent> &
  events() const noexcept {
    return events_;
  }
  // Simulator or synthetic roots recorded beside the live events.
  void record(const layerc::core::AutomationEvent &event);
  void assign(std::vector<layerc::core::AutomationEvent> events,
              std::string text);
  // What a trace save writes: the recorded events, else the last packet.
  [[nodiscard]] std::string export_text() const;
  [[nodiscard]] bool empty() const noexcept {
    return events_.empty() && text_.empty();
  }

private:
  std::vector<layerc::core::AutomationEvent> events_;
  std::string text_;
};

} // namespace layerc::editor

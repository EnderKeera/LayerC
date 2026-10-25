#include "automation_simulator.hpp"

#include "automation_graph.hpp"

#include "layerc/core/cooldown_groups.hpp"
#include "layerc/core/json.hpp"

#include <algorithm>
#include <fstream>
#include <ranges>
#include <system_error>
#include <utility>

namespace layerc::editor {
namespace {
using namespace std::chrono_literals;
namespace core = layerc::core;

// Visits each ';'-separated item until visit returns false. Spaces after a
// separator are skipped and a trailing separator yields no empty item.
template <typename Visit>
void for_each_list_item(const std::string_view text, Visit &&visit) {
  std::size_t cursor{};
  while (cursor < text.size()) {
    const auto end = text.find(';', cursor);
    const auto item = text.substr(
        cursor, end == std::string_view::npos ? std::string_view::npos
                                              : end - cursor);
    if (!visit(item) || end == std::string_view::npos)
      return;
    cursor = end + 1U;
    while (cursor < text.size() && text[cursor] == ' ')
      ++cursor;
  }
}

constexpr char field_syntax_message[] =
    "Fields use name=JSON, or name:type=JSON for fields this event does not "
    "define; for example value=0.5";
} // namespace

AutomationSimulator::AutomationSimulator() = default;
AutomationSimulator::~AutomationSimulator() = default;

AutomationSimulator::Reset
AutomationSimulator::reset(const std::string_view graph_json,
                           const core::GameProfile &profile) {
  runtime_.reset();
  index_ = 0;
  last_step_.reset();
  const auto parsed = core::parse_automation_profile_json(graph_json, profile);
  const auto *graph = std::get_if<core::AutomationProfile>(&parsed);
  if (graph == nullptr)
    return {.error = "Simulator reset failed: " +
                     std::get<core::AutomationError>(parsed).describe(),
            .seeded = std::nullopt};
  runtime_ = std::make_unique<core::AutomationRuntime>(
      *graph, core::AutomationPersistentValues{},
      core::CooldownGroupTopology(profile.cooldown_groups), [] { return core::TimePoint{}; });
  runtime_->set_trace_enabled(true);
  Reset result;
  if (seed_empty_ && events_.empty()) {
    core::AutomationEvent event;
    event.kind = core::AutomationEventKind::session_started;
    event.timestamp = core::TimePoint{};
    event.origin_token = 1;
    events_.push_back(event);
    result.seeded = std::move(event);
  }
  return result;
}

bool AutomationSimulator::step(const std::string_view breakpoint_rule) {
  if (!runtime_ || index_ >= events_.size())
    return false;
  const auto &event = events_[index_++];
  last_step_.emplace();
  auto &report = *last_step_;
  report.root = event;
  const auto evaluate = [&](const core::AutomationEvent &root) {
    const auto before = runtime_->trace().empty()
                            ? std::uint64_t{}
                            : runtime_->trace().back().sequence;
    auto result = runtime_->process(root);
    for (const auto &entry : runtime_->trace())
      if (entry.sequence > before &&
          entry.phase != core::automation_trace_root_phase)
        report.trace.push_back(entry);
    if (result.committed) {
      report.effects.insert(report.effects.end(), result.effects.begin(),
                            result.effects.end());
      report.derived.insert(report.derived.end(), result.derived_events.begin(),
                            result.derived_events.end());
    }
    return result;
  };
  (void)runtime_->expire_overlay_overrides(event.timestamp);
  for (const auto &timer_event : runtime_->due_timer_events(event.timestamp)) {
    ++report.timer_events;
    (void)evaluate(timer_event);
  }
  const auto result = evaluate(event);
  bool keep_playing = result.committed;
  if (!result.committed)
    report.rejection = result.error
                           ? result.error->describe()
                           : std::string("The root event was rejected.");
  if (!breakpoint_rule.empty() &&
      std::any_of(report.trace.begin(), report.trace.end(), [&](const auto &entry) {
        return entry.rule_id == breakpoint_rule;
      }))
    keep_playing = false;
  return keep_playing && index_ < events_.size();
}

std::variant<core::AutomationEvent, std::string>
AutomationSimulator::add_event(const core::AutomationEventKind kind,
                               std::string source_id,
                               const std::string_view fields) {
  if (events_.size() >= core::max_automation_trace_entries)
    return std::string("Simulator event limit reached. Load a smaller trace.");
  core::AutomationEvent event;
  event.kind = kind;
  event.source_id = std::move(source_id);
  const auto next_time = events_.empty() ? std::optional{core::TimePoint{}}
      : core::automation_time_after(events_.back().timestamp, 100ms);
  if (!next_time) return std::string("Simulator event timestamp limit reached.");
  event.timestamp = *next_time;
  event.origin_token = events_.size() + 1U;
  std::optional<std::string> refusal;
  for_each_list_item(fields, [&](const std::string_view item) {
    // name=JSON for a field the event kind types; name:type=JSON otherwise.
    const auto equal = item.find('=');
    const auto colon = item.substr(0, equal).find(':');
    const auto name = item.substr(0, std::min(colon, equal));
    const auto *spec = core::find_automation_event_field(event.kind, name);
    const bool defined_type = spec != nullptr && spec->type.has_value();
    auto type = defined_type ? *spec->type : core::AutomationValueType::boolean;
    if (equal == std::string_view::npos || name.empty() ||
        (colon == std::string_view::npos && !defined_type)) {
      refusal = field_syntax_message;
      return false;
    }
    if (colon != std::string_view::npos) {
      const auto declared = core::parse_automation_value_type_name(
          item.substr(colon + 1U, equal - colon - 1U));
      if (!declared.has_value() || (defined_type && type != *declared)) {
        refusal = "Unsupported synthetic field type for this event.";
        return false;
      }
      type = *declared;
    }
    const auto parsed =
        core::parse_automation_value_json(item.substr(equal + 1U), type);
    const auto *value = std::get_if<core::AutomationValue>(&parsed);
    if (value == nullptr) {
      refusal = std::get<core::AutomationError>(parsed).describe();
      return false;
    }
    event.fields.emplace(std::string(name), *value);
    return true;
  });
  if (refusal)
    return *refusal;
  events_.push_back(event);
  return event;
}

void AutomationSimulator::load(std::vector<core::AutomationEvent> events) {
  events_ = std::move(events);
  seed_empty_ = false;
  index_ = 0;
  runtime_.reset();
  last_step_.reset();
}

void AutomationSimulator::clear() {
  load({});
  seed_empty_ = true;
}

namespace {

// The value report of the last step: committed effects, derived events, the
// non-root trace entries and the rejection, bounded to 32 detail lines.
void append_step_report(std::string &output, const SimulatorStepReport &report) {
  output += "\nLast step: " + std::string(core::automation_event_name(report.root.kind));
  if (!report.root.source_id.empty()) output += " " + report.root.source_id;
  if (report.timer_events != 0)
    output += " (+" + std::to_string(report.timer_events) + " timer events)";
  std::size_t count{};
  const auto detail = [&](const std::string &line) {
    if (count < 32U) output += "\n  " + line;
    ++count;
  };
  if (report.rejection) detail("Rejected: " + *report.rejection);
  for (const auto &entry : report.trace) {
    if (entry.phase == core::automation_trace_root_phase) continue;
    detail(entry.automation_id + "/" + entry.rule_id + ": " + entry.phase + " " +
           entry.detail);
  }
  for (const auto &effect : report.effects) {
    std::string line = "Effect: " + std::string(action_label(effect.kind));
    if (!effect.target.empty()) line += " " + effect.target;
    if (!effect.property.empty()) line += "." + effect.property;
    const auto *spec = core::automation_action_spec(effect.kind);
    if (spec != nullptr && spec->accepts_value())
      line += " = " + core::automation_value_json(effect.value);
    detail(line);
  }
  for (const auto &event : report.derived) {
    std::string line = "Derived: " + std::string(core::automation_event_name(event.kind));
    if (!event.source_id.empty()) line += " " + event.source_id;
    detail(line);
  }
  if (report.trace.empty() && report.effects.empty() && report.derived.empty() &&
      !report.rejection)
    detail("Nothing matched this event.");
  if (count > 32U) output += "\n  … " + std::to_string(count - 32U) + " more";
}

} // namespace

std::string
AutomationSimulator::describe(const std::string_view assertions) const {
  std::string output = "Events: " + std::to_string(index_) + "/" +
                       std::to_string(events_.size());
  if (!runtime_)
    return output + "\nReset the simulator to compile the current graph.";
  output += "    Timers: " + std::to_string(runtime_->snapshot().timers.size()) +
            "\nVariables:";
  for (const auto &[id, value] : runtime_->snapshot().variables)
    output += " " + id + "=" + core::automation_value_json(value) + ";";
  if (last_step_) append_step_report(output, *last_step_);
  if (assertions.empty())
    return output;
  bool all_passed = true;
  for_each_list_item(assertions, [&](const std::string_view item) {
    const auto equal = item.find('=');
    if (equal == std::string_view::npos || equal == 0) {
      all_passed = false;
      return false;
    }
    const auto id = std::string(item.substr(0, equal));
    const auto &variables = runtime_->profile().variables;
    const auto declaration =
        std::ranges::find(variables, id, &core::AutomationVariable::id);
    if (declaration == variables.end()) {
      all_passed = false;
      return false;
    }
    const auto expected = core::parse_automation_value_json(
        item.substr(equal + 1U), declaration->type);
    const auto *value = std::get_if<core::AutomationValue>(&expected);
    const auto actual = runtime_->snapshot().variables.find(id);
    if (value == nullptr || actual == runtime_->snapshot().variables.end() ||
        actual->second != *value)
      all_passed = false;
    return true;
  });
  return output + (all_passed ? "\nAssertions: PASS" : "\nAssertions: FAIL");
}

std::chrono::milliseconds simulator_interval(const int speed) noexcept {
  switch (speed) {
  case 0:
    return 400ms;
  case 2:
    return 25ms;
  default:
    return 100ms;
  }
}

std::variant<LoadedTrace, TraceLoadError>
read_trace_file(const std::filesystem::path &path) {
  const TraceLoadError unreadable{
      .message = "The trace could not be read or exceeds 4 MiB.",
      .invalid = false};
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > core::max_automation_trace_bytes)
    return unreadable;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return unreadable;
  std::string text(static_cast<std::size_t>(size), '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (input.gcount() != static_cast<std::streamsize>(text.size()))
    return unreadable;
  auto parsed = core::parse_layerc_trace(text, max_trace_import_json_values);
  if (auto *events = std::get_if<std::vector<core::AutomationEvent>>(&parsed))
    return LoadedTrace{.text = std::move(text), .events = std::move(*events)};
  const auto &parse_error = std::get<core::AutomationError>(parsed);
  if (parse_error.message == core::json::value_count_limit_error)
    return TraceLoadError{
        .message = "Trace import resource limit exceeded (131072 JSON values); "
                   "the current trace and profile were not changed.",
        .invalid = false};
  return TraceLoadError{.message = parse_error.describe(), .invalid = true};
}

bool write_trace_file(const std::filesystem::path &path,
                      const std::string_view text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.flush();
  return static_cast<bool>(output);
}

std::string
trace_from_events(const std::span<const core::AutomationEvent> events) {
  std::vector<core::AutomationTraceEntry> entries;
  entries.reserve(events.size());
  for (std::size_t index{}; index < events.size(); ++index)
    entries.push_back(
        core::make_automation_root_trace_entry(events[index], index + 1U));
  return core::export_layerc_trace(entries);
}

std::string LiveTraceLog::append_packet(const std::string_view packet) {
  const auto parsed = core::parse_layerc_trace(packet);
  const auto *events = std::get_if<std::vector<core::AutomationEvent>>(&parsed);
  if (events == nullptr)
    return "Rejected malformed LayerD trace: " +
           std::get<core::AutomationError>(parsed).describe();
  const std::size_t incoming = std::min(events->size(), max_events);
  const std::size_t retained = max_events - incoming;
  if (events_.size() > retained)
    events_.erase(events_.begin(),
                  events_.end() - static_cast<std::ptrdiff_t>(retained));
  events_.insert(events_.end(),
                 events->end() - static_cast<std::ptrdiff_t>(incoming),
                 events->end());
  text_ = std::string(packet);
  return "Live trace events: " + std::to_string(events_.size()) +
         "\nLast authenticated packet: " + std::to_string(packet.size()) +
         " bytes";
}

void LiveTraceLog::clear() {
  events_.clear();
  text_.clear();
}

void LiveTraceLog::record(const core::AutomationEvent &event) {
  // The caller may pass an event already in this log. Copy before eviction.
  auto copy = event;
  if (events_.size() >= max_events)
    events_.erase(events_.begin(),
                  events_.end() - static_cast<std::ptrdiff_t>(max_events - 1U));
  events_.push_back(std::move(copy));
}

void LiveTraceLog::assign(std::vector<core::AutomationEvent> events,
                          std::string text) {
  if (events.size() > max_events)
    events.erase(events.begin(),
                 events.end() - static_cast<std::ptrdiff_t>(max_events));
  events_ = std::move(events);
  text_ = std::move(text);
}

std::string LiveTraceLog::export_text() const {
  return events_.empty() ? text_ : trace_from_events(events_);
}

} // namespace layerc::editor

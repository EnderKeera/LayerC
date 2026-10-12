#include "automation_graph.hpp"
#include "automation_simulator.hpp"
#include "layerc/core/automation_trace_control.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <variant>

namespace {
using namespace layerc::core;
using namespace layerc::editor;
using layerc::test::expect;

GameProfile base_profile() {
  auto profile = make_new_profile();
  profile.game.executable_name = "fixture.exe";
  return profile;
}

template <typename Value, typename Variant>
const Value *as(const Variant &result) {
  return std::get_if<Value>(&result);
}

void presets_and_palette() {
  auto empty = base_profile();
  empty.cooldowns.clear();
  empty.panels.clear();
  expect(preset_unavailable(empty, AutomationPreset::input_cooldown).has_value() &&
             !preset_unavailable(empty, AutomationPreset::blank).has_value(),
         "non-blank presets need a cooldown");

  const auto profile = base_profile();
  std::optional<AutomationProfile> graph;
  const auto first =
      add_automation_preset(profile, graph, AutomationPreset::input_cooldown);
  expect(as<AutomationSelection>(first) != nullptr && graph &&
             graph->profile_id == profile.id &&
             graph->automations[0].id == "automation-1" &&
             *as<AutomationSelection>(first) == AutomationSelection{0, 0, -1},
         "a preset creates the module and selects its first rule");
  expect(validate_automation_graph(*graph, profile).empty(),
         "the input preset is a valid graph");
  const auto second =
      add_automation_preset(profile, graph, AutomationPreset::blank);
  expect(as<AutomationSelection>(second) != nullptr &&
             graph->automations[1].id == "automation-2" &&
             as<AutomationSelection>(second)->rule == -1,
         "a blank preset has no rule to select");

  const auto &action_entry = automation_palette[2];
  expect(palette_unavailable(profile, graph, {1, -1, -1}, action_entry) ==
             std::optional<std::string>("Select a rule before adding an action."),
         "actions need a selected rule");
  const auto rule = add_palette_node(profile, graph, {1, -1, -1},
                                     automation_palette[1]);
  expect(as<AutomationSelection>(rule) != nullptr &&
             *as<AutomationSelection>(rule) == AutomationSelection{1, 0, -1} &&
             graph->automations[1].rules[0].id == "rule-1",
         "a rule node is added and selected");
  const auto action =
      add_palette_node(profile, graph, {1, 0, -1}, action_entry);
  expect(as<AutomationSelection>(action) != nullptr &&
             *as<AutomationSelection>(action) == AutomationSelection{1, 0, 1},
         "an action node is appended to the selected rule");
  const auto variable = add_palette_node(profile, graph, {1, 0, 1},
                                         automation_palette[5]);
  expect(as<AutomationSelection>(variable) != nullptr &&
             graph->variables.size() == 1U && graph->variables[0].id == "flag-1",
         "a variable keeps the selection");
  expect(automation_validation_summary(graph, profile) == "Valid — no errors",
         "palette nodes keep the graph valid");

  const auto rows = automation_node_rows(graph->automations[1]);
  expect(rows.size() == 3U && rows[0].action == -1 && rows[2].action == 1 &&
             rows[0].label.starts_with("Rule: rule-1 — Timer fired [pulse]"),
         "node rows list the rule then its actions");
  expect(event_label(AutomationEventKind::cooldown_ready) == "Cooldown ready" &&
             automation_label({.id = "a", .name = "", .enabled = false}) ==
                 "a (disabled)",
         "labels name events and disabled automations");
}

void node_fields_and_removal() {
  const auto profile = base_profile();
  std::optional<AutomationProfile> graph;
  (void)add_automation_preset(profile, graph,
                              AutomationPreset::repeating_session_timer);
  const AutomationSelection timer{0, 0, 0};
  auto fields = automation_node_fields(*graph, timer);
  expect(fields.rule_id == "start-timer" && fields.repeating &&
             fields.priority == "0" && !fields.advanced_value &&
             !fields.value.empty(),
         "fields show the selected rule and action");

  const auto before = *graph;
  fields.priority = "soon";
  expect(apply_automation_node_fields(*graph, timer, fields, false) ==
                 std::optional<std::string>("Priority must be a whole number.") &&
             *graph == before,
         "a non-numeric priority is refused without changing the graph");
  fields.priority = "5";
  fields.value = "\"not a duration\"";
  expect(apply_automation_node_fields(*graph, timer, fields, false).has_value() &&
             *graph == before,
         "a mistyped value is refused without changing the graph");
  fields.value = "2500";
  fields.rule_id = "begin";
  expect(!apply_automation_node_fields(*graph, timer, fields, false) &&
             graph->automations[0].rules[0].id == "begin" &&
             graph->automations[0].rules[0].priority == 5,
         "valid fields replace the rule and action");

  auto selection = delete_automation_node(graph, {0, 1, 0});
  expect(graph && graph->automations[0].rules.size() == 1U &&
             selection == AutomationSelection{0, -1, -1},
         "deleting a rule's last action deletes the rule");
  selection = delete_automation_node(graph, {0, 0, -1});
  expect(!graph && selection == AutomationSelection{},
         "deleting the last rule removes the automation and the empty module");
}

void json_error_location() {
  const std::string source = "{\n  \"naïve\": tru\n}";
  const auto parsed = parse_automation_profile_json(source, base_profile());
  const auto *error = std::get_if<AutomationError>(&parsed);
  expect(error != nullptr, "the fixture is invalid JSON");
  if (error == nullptr)
    return;
  const auto location = locate_json_error(source, *error);
  expect(!error->path.starts_with("$@") ||
             (location.line == 2U && location.begin.has_value() &&
              location.message.starts_with("Line 2, column ")),
         "a parse error names its line and column");

  const AutomationError synthetic{.path = "$@14", .message = "bad"};
  const auto mapped = locate_json_error(source, synthetic);
  expect(mapped.line == 2U && mapped.column == 12U && mapped.begin == 14U &&
             mapped.end == 15U,
         "columns count characters, not UTF-8 bytes");
  const auto plain = locate_json_error(source, {.path = "$.x", .message = "m"});
  expect(!plain.begin && plain.message == "$.x: m",
         "a schema error keeps its description");
}

AutomationProfile flag_graph(const GameProfile &profile) {
  AutomationProfile graph;
  graph.profile_id = profile.id;
  graph.variables.push_back(make_palette_variable("flag-1"));
  AutomationRule rule;
  rule.id = "set-flag";
  rule.event = AutomationEventKind::session_started;
  AutomationAction set;
  set.kind = AutomationActionKind::set_variable;
  set.target = "flag-1";
  set.value = expr::literal(AutomationValue(true));
  rule.actions.push_back(std::move(set));
  graph.automations.push_back({.id = "flags", .name = "Flags", .rules = {rule}});
  return graph;
}

void simulator_replay() {
  const auto profile = base_profile();
  const auto json = automation_graph_json(flag_graph(profile));
  AutomationSimulator simulator;
  expect(!simulator.step("") && simulator.describe("").ends_with(
                                    "Reset the simulator to compile the current graph."),
         "nothing steps before a reset");
  const auto broken = simulator.reset("{", profile);
  expect(broken.error.has_value() && !simulator.compiled(),
         "an invalid graph does not compile");

  const auto reset = simulator.reset(json, profile);
  expect(!reset.error && reset.seeded &&
             reset.seeded->kind == AutomationEventKind::session_started &&
             simulator.events().size() == 1U,
         "a reset seeds sessionStarted into an empty list");
  const auto refused = simulator.add_event(AutomationEventKind::input_pressed,
                                           "kbd", "bogus");
  expect(as<std::string>(refused) != nullptr && simulator.events().size() == 1U,
         "malformed fields are refused");
  const auto typed = simulator.add_event(AutomationEventKind::input_pressed,
                                         "kbd", "extra:int=3; ");
  expect(as<AutomationEvent>(typed) != nullptr &&
             as<AutomationEvent>(typed)->timestamp ==
                 TimePoint(std::chrono::milliseconds(100)) &&
             as<AutomationEvent>(typed)->fields.contains("extra"),
         "typed extra fields are accepted 100 ms after the previous root");

  expect(!simulator.step("set-flag"), "a breakpoint rule pauses playback");
  expect(simulator.describe("flag-1=true").ends_with("Assertions: PASS") &&
             simulator.describe("flag-1=false").ends_with("Assertions: FAIL") &&
             simulator.describe("missing=1").ends_with("Assertions: FAIL") &&
             simulator.describe("").starts_with("Events: 1/2"),
         "assertions compare declared variables");
  expect(!simulator.step("") && simulator.position() == 2U && !simulator.step(""),
         "the last event ends playback");
  (void)simulator.reset(json, profile);
  expect(simulator.position() == 0U && simulator.events().size() == 2U &&
             simulator.step(""),
         "a reset rewinds without reseeding");
}

void simulator_timer_ordering() {
  using namespace std::chrono_literals;
  const auto profile = base_profile();
  auto graph = flag_graph(profile);
  auto &rules = graph.automations[0].rules;
  AutomationAction timer;
  timer.kind = AutomationActionKind::create_timer;
  timer.target = "tick";
  timer.value = expr::literal(AutomationValue(DurationValue{50ms}));
  AutomationAction overlay;
  overlay.kind = AutomationActionKind::set_overlay_override;
  overlay.target = profile.panels[0].id;
  overlay.name = "pulse";
  overlay.property = "visible";
  overlay.value = expr::literal(AutomationValue(true));
  overlay.secondary = expr::literal(AutomationValue(DurationValue{50ms}));
  const auto set_flag = rules[0].actions[0];
  rules[0].actions = {timer, overlay};
  AutomationRule tick;
  tick.id = "on-tick";
  tick.event = AutomationEventKind::timer_fired;
  tick.source_filter = "tick";
  tick.actions = {set_flag};
  rules.push_back(tick);
  AutomationRule root;
  root.id = "after-timer";
  root.event = AutomationEventKind::input_pressed;
  root.condition = expr::variable("flag-1");
  auto clear_flag = set_flag;
  clear_flag.value = expr::literal(AutomationValue(false));
  root.actions = {clear_flag};
  rules.push_back(root);
  AutomationSimulator simulator;
  const auto reset = simulator.reset(automation_graph_json(graph), profile);
  expect(!reset.error, "the timer graph compiles");
  if (reset.error) return;
  (void)simulator.add_event(AutomationEventKind::input_pressed, "kbd", "");
  const bool keep_playing = simulator.step("");
  std::string detail;
  for (const auto &entry : simulator.runtime()->trace())
    detail += entry.phase + ": " + entry.detail + "; ";
  expect(keep_playing && simulator.runtime()->snapshot().timers.size() == 1U &&
             simulator.runtime()->snapshot().overlay_overrides.size() == 1U,
         "session start creates a timer and a temporary overlay override: " + detail);
  (void)simulator.step("");
  expect(simulator.runtime()->snapshot().timers.empty() &&
             simulator.runtime()->snapshot().overlay_overrides.empty() &&
             simulator.describe("flag-1=false").ends_with("Assertions: PASS"),
         "overrides expire and due timers fire before the next root reads variables");
  expect(simulator_interval(0) == 400ms && simulator_interval(1) == 100ms &&
             simulator_interval(2) == 25ms,
         "playback uses the shared slow, normal and fast intervals");
}

void trace_files() {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("layerc-trace-test-" + std::to_string(
                              std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()));
  std::filesystem::create_directories(directory);
  AutomationSimulator simulator;
  (void)simulator.reset(automation_graph_json(flag_graph(base_profile())),
                        base_profile());
  (void)simulator.add_event(AutomationEventKind::input_pressed, "kbd", "");
  const auto text = trace_from_events(simulator.events());
  expect(write_trace_file(directory / "ok.layerctrace", text),
         "the trace file is written");
  const auto loaded = read_trace_file(directory / "ok.layerctrace");
  expect(as<LoadedTrace>(loaded) != nullptr &&
             as<LoadedTrace>(loaded)->events.size() == 2U,
         "a written trace reads back its roots");
  (void)layerc::test::write_file(directory / "bad.layerctrace", "not a trace");
  const auto bad = read_trace_file(directory / "bad.layerctrace");
  expect(as<TraceLoadError>(bad) != nullptr && as<TraceLoadError>(bad)->invalid,
         "a malformed trace is reported as invalid");
  const auto missing = read_trace_file(directory / "missing.layerctrace");
  expect(as<TraceLoadError>(missing) != nullptr &&
             !as<TraceLoadError>(missing)->invalid,
         "an unreadable trace is reported as unreadable");

  LiveTraceLog log;
  expect(log.append_packet("garbage").starts_with("Rejected malformed LayerD trace:") &&
             log.empty(),
         "a malformed packet is rejected and not kept");
  expect(log.append_packet(text).starts_with("Live trace events: 2") &&
             log.events().size() == 2U &&
             std::holds_alternative<std::vector<AutomationEvent>>(
                 parse_layerc_trace(log.export_text())),
         "a packet's events are kept and exported");
  for (std::size_t i = 0; i < LiveTraceLog::max_events; ++i)
    log.record(log.events().front());
  expect(log.events().size() == LiveTraceLog::max_events,
         "synthetic recording stays bounded, including a reference into the log");
  auto many = std::vector<AutomationEvent>(LiveTraceLog::max_events + 2U);
  for (std::size_t i = 0; i < many.size(); ++i)
    many[i].origin_token = i + 1U;
  log.assign(std::move(many), {});
  expect(log.events().size() == LiveTraceLog::max_events &&
             log.events().front().origin_token == 3U,
         "loading keeps only the newest bounded trace events");
  (void)log.append_packet(text);
  expect(log.events().size() == LiveTraceLog::max_events,
         "the live log keeps only the newest events");
  std::filesystem::remove_all(directory);
}

void trace_control_codec_contracts() {
  const layerc::core::TraceControlRequest request{
      layerc::core::TraceControlOperation::poll, "profile-Δ", "fixture-42", 17, 1234};
  expect(layerc::core::parse_trace_control_request(
             layerc::core::serialize_trace_control_request(request)) == request,
         "typed UTF-8 trace requests roundtrip byte-stably");
  for (const auto invalid : {
           R"({"traceVersion":2,"operation":"poll","profileId":"p","clientId":"c","generation":1,"cursor":0})",
           R"({"traceVersion":1,"operation":"execute","profileId":"p","clientId":"c","generation":1,"cursor":0})",
           R"({"traceVersion":1,"operation":"poll","profileId":"p","clientId":"c","generation":-1,"cursor":0})",
           R"({"traceVersion":1,"operation":"poll","profileId":"p","clientId":"c","generation":1,"cursor":0.5})",
           R"({"traceVersion":1,"operation":"poll","profileId":"p","clientId":"../c","generation":1,"cursor":0})",
           R"({"traceVersion":1,"operation":"poll","profileId":"p","clientId":"c","generation":1,"cursor":0,"extra":1})"})
    expect(!layerc::core::parse_trace_control_request(invalid),
           "version, operation, integers, ownership token and unknown fields are strict");
  auto bad = request;
  bad.client_id.assign(layerc::core::trace_control_client_bytes + 1, 'x');
  expect(!layerc::core::trace_control_request_valid(bad), "client identity is bounded");
  bad = request; bad.profile_id = std::string(1, static_cast<char>(0xff));
  expect(!layerc::core::trace_control_request_valid(bad), "invalid UTF-8 profile IDs are refused");
  const layerc::core::TraceControlReply reply{17, 1234, false, true, "{\"entries\":[]}"};
  expect(layerc::core::parse_trace_control_reply(
             layerc::core::serialize_trace_control_reply(reply)) == reply,
         "typed trace metadata and escaped packet roundtrip");
  auto oversized = reply;
  oversized.packet.assign(layerc::core::trace_control_packet_bytes + 1, 'x');
  expect(!layerc::core::parse_trace_control_reply(
             layerc::core::serialize_trace_control_reply(oversized)), "trace packets are bounded");
}

void simulator_event_bounds() {
  AutomationSimulator simulator;
  for (std::size_t i = 0; i < max_automation_trace_entries; ++i)
    if (!expect(as<AutomationEvent>(simulator.add_event(
            AutomationEventKind::input_pressed, {}, {})) != nullptr, "bounded synthetic event is accepted"))
      return;
  expect(as<std::string>(simulator.add_event(AutomationEventKind::input_pressed, {}, {})) &&
             simulator.events().size() == max_automation_trace_entries,
         "one more event is refused without growth");
  AutomationEvent last;
  last.timestamp = TimePoint::max();
  simulator.load({last});
  expect(as<std::string>(simulator.add_event(AutomationEventKind::input_pressed, {}, {})) &&
             simulator.events().size() == 1,
         "timestamp overflow is refused before appending a synthetic root");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(presets_and_palette),
                 LAYERC_TEST_CASE(node_fields_and_removal),
                 LAYERC_TEST_CASE(json_error_location),
                 LAYERC_TEST_CASE(simulator_replay),
                 LAYERC_TEST_CASE(simulator_timer_ordering),
                 LAYERC_TEST_CASE(trace_files),
                 LAYERC_TEST_CASE(trace_control_codec_contracts),
                 LAYERC_TEST_CASE(simulator_event_bounds))

#include "automation_graph.hpp"
#include "automation_simulator.hpp"
#include "layerc/core/automation_trace_control.hpp"
#include "layerc/core/live_draft_control.hpp"
#include "layerc/core/json.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include <algorithm>
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

void simulator_empty_loaded_trace() {
  const auto profile = base_profile();
  const auto json = automation_graph_json(flag_graph(profile));
  AutomationSimulator simulator;
  expect(simulator.reset(json, profile).seeded.has_value(),
         "a fresh simulator still offers its starter event");
  simulator.load({});
  const auto invalid = simulator.reset("{", profile);
  expect(invalid.error.has_value() && !simulator.compiled(),
         "invalid compilation leaves an explicitly empty trace uncompiled");
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto reset = simulator.reset(json, profile);
    expect(!reset.error && !reset.seeded && simulator.compiled() &&
               simulator.events().empty() && simulator.position() == 0U,
           "reset preserves an explicitly loaded empty root list");
    expect(simulator.describe("flag-1=false").starts_with("Events: 0/0") &&
               simulator.describe("flag-1=false").ends_with("Assertions: PASS") &&
               !simulator.step(""),
           "an empty trace executes no invented sessionStarted rule");
  }
  const auto added = simulator.add_event(AutomationEventKind::input_pressed, "kbd", "");
  expect(as<AutomationEvent>(added) &&
             as<AutomationEvent>(added)->timestamp == TimePoint{} &&
             simulator.events().size() == 1U,
         "a synthetic event may be added to the preserved empty trace");
  const auto reset = simulator.reset(json, profile);
  expect(!reset.error && !reset.seeded && simulator.events().size() == 1U &&
             simulator.events().front().kind == AutomationEventKind::input_pressed,
         "nonempty imported or edited roots are not reseeded");
  simulator.clear();
  expect(!simulator.compiled() && simulator.events().empty() && simulator.position() == 0U,
         "clearing a session removes its compiled state and roots");
  const auto fresh = simulator.reset(json, profile);
  expect(!fresh.error && fresh.seeded && simulator.events().size() == 1U &&
             fresh.seeded->kind == AutomationEventKind::session_started,
         "a fresh session after a loaded trace restores only the starter root");
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
  const auto &timed = simulator.last_step();
  expect(simulator.runtime()->snapshot().timers.empty() &&
             simulator.runtime()->snapshot().overlay_overrides.empty() &&
             simulator.describe("flag-1=false").ends_with("Assertions: PASS"),
         "overrides expire and due timers fire before the next root reads variables");
  expect(timed && timed->timer_events == 1U && timed->effects.size() == 2U &&
             timed->effects.front().kind == AutomationActionKind::set_variable &&
             timed->effects.front().value == AutomationValue(true) &&
             timed->effects.back().kind == AutomationActionKind::set_variable &&
             timed->effects.back().value == AutomationValue(false),
         "the step report counts the due timer and keeps timer-before-root effect order");
  bool saw_tick{};
  bool saw_after{};
  if (timed)
    for (const auto &entry : timed->trace) {
      saw_tick = saw_tick || entry.rule_id == "on-tick";
      saw_after = saw_after || entry.rule_id == "after-timer";
    }
  expect(saw_tick && saw_after,
         "the step report trace includes the timer rule and the root rule");
  expect(simulator_interval(0) == 400ms && simulator_interval(1) == 100ms &&
             simulator_interval(2) == 25ms,
         "playback uses the shared slow, normal and fast intervals");
}

void trace_files() {
  namespace core = layerc::core;
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

  const auto trace_with_opaque_values = [](const std::size_t count) {
    std::string trace =
        R"({"schemaVersion":1,"kind":"layerctrace","entries":[],"opaque":[)";
    trace.reserve(trace.size() + count * 2U + 2U);
    for (std::size_t index{}; index < count; ++index) {
      if (index != 0) trace.push_back(',');
      trace.push_back('0');
    }
    trace += "]}";
    return trace;
  };
  const auto excessive_trace =
      trace_with_opaque_values(max_trace_import_json_values);
  expect(excessive_trace.size() < core::max_automation_trace_bytes &&
             std::holds_alternative<std::vector<AutomationEvent>>(
                 core::parse_layerc_trace(excessive_trace)),
         "the default trace parser accepts the valid under-4-MiB opaque trace");
  const auto excessive_path = directory / "many-values.layerctrace";
  expect(write_trace_file(excessive_path, excessive_trace),
         "the bounded-allocation trace fixture is written");
  const auto bytes_before = layerc::test::read_file(excessive_path);
  expect(bytes_before.has_value(), "the original trace bytes can be captured");
  const auto simulator_before = simulator.events();
  const auto log_before = log.export_text();
  const auto refused = read_trace_file(excessive_path);
  const auto *resource_error = as<TraceLoadError>(refused);
  expect(resource_error != nullptr && !resource_error->invalid &&
             resource_error->message ==
                 "Trace import resource limit exceeded (131072 JSON values); "
                 "the current trace and profile were not changed.",
         "editor trace import refuses excessive allocation as a resource error");
  const auto bytes_after = layerc::test::read_file(excessive_path);
  expect(bytes_before.has_value() && bytes_after == bytes_before,
         "resource refusal preserves the authored trace bytes");
  const auto &events_after = simulator.events();
  const bool events_unchanged =
      events_after.size() == simulator_before.size() &&
      std::equal(events_after.begin(), events_after.end(), simulator_before.begin(),
                 [](const AutomationEvent &left, const AutomationEvent &right) {
                   return left.kind == right.kind &&
                          left.timestamp == right.timestamp &&
                          left.source_id == right.source_id &&
                          left.origin_token == right.origin_token &&
                          left.fields == right.fields;
                 });
  expect(events_unchanged && log.export_text() == log_before,
         "resource refusal preserves simulator events and live trace log");
  const auto recovered = read_trace_file(directory / "ok.layerctrace");
  expect(as<LoadedTrace>(recovered) != nullptr &&
             as<LoadedTrace>(recovered)->events.size() == 2U,
         "a valid trace loads after the resource refusal");

  const auto exact_limit_trace = trace_with_opaque_values(
      max_trace_import_json_values - 5U);
  const auto exact_limit = core::parse_layerc_trace(
      exact_limit_trace, max_trace_import_json_values);
  expect(exact_limit_trace.size() < core::max_automation_trace_bytes &&
             std::holds_alternative<std::vector<AutomationEvent>>(exact_limit),
         "the caller-bounded trace parser accepts exactly 131072 JSON values");
  const auto over_limit_trace = trace_with_opaque_values(
      max_trace_import_json_values - 4U);
  const auto over_limit = core::parse_layerc_trace(
      over_limit_trace, max_trace_import_json_values);
  const auto *value_limit_error = std::get_if<core::AutomationError>(&over_limit);
  expect(value_limit_error != nullptr &&
             value_limit_error->message == core::json::value_count_limit_error,
         "the caller-bounded trace parser refuses the 131073rd JSON value");

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

// The typed failures are the machine contract: one wire name and one text per
// enumerator, and a lapse predicate only for the two client-recoverable codes.
void control_failure_contracts() {
  namespace core = layerc::core;
  for (const auto &entry : core::trace_control_error_wire) {
    const auto name = core::enum_wire_name(core::trace_control_error_wire, entry.value);
    const auto parsed = core::parse_enum_wire(core::trace_control_error_wire, entry.name);
    const auto failure = core::trace_control_failure(entry.value);
    expect(name.has_value() && parsed == entry.value &&
               failure.code == entry.value && !failure.message.empty(),
           "every trace control error has one wire name and a text");
    expect(core::trace_control_lapsed(entry.value) ==
               (entry.value == core::TraceControlError::expired ||
                entry.value == core::TraceControlError::invalid_cursor),
           "only a lapsed lease or cursor is recoverable as a gap");
  }
  for (const auto &entry : core::live_draft_error_wire) {
    const auto name = core::enum_wire_name(core::live_draft_error_wire, entry.value);
    const auto parsed = core::parse_enum_wire(core::live_draft_error_wire, entry.name);
    const auto failure = core::live_draft_failure(entry.value);
    expect(name.has_value() && parsed == entry.value &&
               failure.code == entry.value && !failure.message.empty(),
           "every live draft error has one wire name and a text");
  }
  const core::TraceControlRequest request{
      core::TraceControlOperation::poll, "profile-Δ", "fixture-42", 17, 1234};
  const auto request_bytes = core::serialize_trace_control_request(request);
  const auto request_tree = core::json::parse(request_bytes);
  const auto *request_value = std::get_if<core::json::Value>(&request_tree);
  expect(request_value &&
             core::parse_trace_control_request(*request_value) ==
                 core::parse_trace_control_request(request_bytes),
         "the value overload agrees with the string overload for a valid request");
  const core::TraceControlReply reply{17, 1234, false, true, "{\"entries\":[]}"};
  const auto reply_bytes = core::serialize_trace_control_reply(reply);
  const auto reply_tree = core::json::parse(reply_bytes);
  const auto *reply_value = std::get_if<core::json::Value>(&reply_tree);
  expect(reply_value && core::parse_trace_control_reply(*reply_value) ==
                            core::parse_trace_control_reply(reply_bytes),
         "the value overload agrees with the string overload for a valid reply");
  for (const auto invalid : {
           R"({"traceVersion":2,"operation":"poll","profileId":"p","clientId":"c","generation":1,"cursor":0})",
           R"({"traceVersion":1,"operation":"poll","profileId":"p","clientId":"c","generation":1})",
           R"({"traceVersion":1,"operation":"poll","profileId":"p","clientId":"../c","generation":1,"cursor":0})",
           R"({"traceVersion":1,"operation":"poll","profileId":"p","clientId":"c","generation":1,"cursor":-4})"}) {
    const auto tree = core::json::parse(invalid);
    const auto *value = std::get_if<core::json::Value>(&tree);
    expect(value && !core::parse_trace_control_request(*value),
           "the value overload refuses the same malformed trace requests");
  }
  const core::LiveDraftRequest draft{core::LiveDraftOperation::publish, "profile-1",
                                     "fixture-42", 7, "{\"name\":\"Draft\"}"};
  const auto draft_bytes = core::serialize_live_draft_request(draft);
  const auto draft_tree = core::json::parse(draft_bytes);
  const auto *draft_value = std::get_if<core::json::Value>(&draft_tree);
  expect(draft_value && core::parse_live_draft_request(*draft_value) ==
                            core::parse_live_draft_request(draft_bytes),
         "the value overload agrees with the string overload for a valid draft");
  const core::LiveDraftReply draft_reply{7, core::LiveDraftState::applied};
  const auto draft_reply_bytes = core::serialize_live_draft_reply(draft_reply);
  const auto draft_reply_tree = core::json::parse(draft_reply_bytes);
  const auto *draft_reply_value = std::get_if<core::json::Value>(&draft_reply_tree);
  expect(draft_reply_value &&
             core::parse_live_draft_reply(*draft_reply_value) ==
                 core::parse_live_draft_reply(draft_reply_bytes),
         "the value overload agrees with the string overload for a draft reply");
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

// The step report is a value of what the last step actually did: committed
// effects, derived events, the non-root trace and the rejection.
void simulator_step_report_contracts() {
  // 1. A matching input triggers a cooldown; a non-matching source does not.
  {
    const auto profile = base_profile();
    std::optional<AutomationProfile> preset;
    (void)add_automation_preset(profile, preset, AutomationPreset::input_cooldown);
    if (!expect(preset.has_value() && !preset->automations.empty() &&
                    !preset->automations.front().rules.empty(),
                "the input preset graph has one rule"))
      return;
    preset->automations.front().rules.front().source_filter = "fixture-match";
    const auto graph = *preset;
    AutomationSimulator simulator;
    if (!expect(!simulator.reset(automation_graph_json(graph), profile).error,
                "the input preset graph compiles"))
      return;
    AutomationEvent matching;
    matching.kind = AutomationEventKind::input_pressed;
    matching.source_id = "fixture-match";
    matching.origin_token = 1;
    AutomationEvent other = matching;
    other.source_id = "fixture-other";
    other.origin_token = 2;
    // Load the two explicit roots and recompile so no seeded root is stepped.
    simulator.load({matching, other});
    if (!expect(!simulator.reset(automation_graph_json(graph), profile).error,
                "the loaded input graph compiles"))
      return;
    (void)simulator.step("");
    const auto first = simulator.last_step();
    if (!expect(first.has_value(), "the first step records a report")) return;
    const auto &target = profile.cooldowns.front().id;
    bool condition_true{};
    for (const auto &entry : first->trace)
      condition_true = condition_true || (entry.phase == "condition" &&
                                          entry.detail == "true");
    expect(condition_true && first->effects.size() == 1U &&
               first->effects.front().kind == AutomationActionKind::trigger_cooldown &&
               first->effects.front().target == target,
           "a matching input reports its true condition and committed effect");
    const auto described = simulator.describe("");
    expect(described.find("Effect: Trigger cooldown " + target) != std::string::npos &&
               described.find(target + " = false") == std::string::npos,
           "the description renders the effect without an unused action value");
    (void)simulator.step("");
    const auto second = simulator.last_step();
    expect(second && second->effects.empty() && second->derived.empty() &&
               second->trace.empty() &&
               simulator.describe("").find("Nothing matched this event.") !=
                   std::string::npos,
           "a non-matching source reports that nothing matched");
  }
  // 2. A false condition is visible and commits nothing.
  {
    const auto profile = base_profile();
    auto graph = flag_graph(profile);
    graph.automations.front().rules.front().condition =
        expr::literal(AutomationValue(false));
    AutomationSimulator simulator;
    if (!expect(!simulator.reset(automation_graph_json(graph), profile).error,
                "the false-condition graph compiles"))
      return;
    (void)simulator.step("");
    const auto report = simulator.last_step();
    bool condition_false{};
    for (const auto &entry : report ? report->trace : std::vector<AutomationTraceEntry>{})
      condition_false = condition_false || (entry.phase == "condition" &&
                                             entry.detail == "false");
    expect(report && report->effects.empty() && condition_false &&
               simulator.describe("").find("condition false") != std::string::npos,
           "a false condition is reported with no committed effect");
  }
  // 5. Reset, load and clear drop the report and its section.
  {
    const auto profile = base_profile();
    const auto json = automation_graph_json(flag_graph(profile));
    AutomationSimulator simulator;
    if (!expect(!simulator.reset(json, profile).error, "the reset fixture compiles"))
      return;
    (void)simulator.step("");
    expect(simulator.last_step().has_value() &&
               simulator.describe("").find("Last step:") != std::string::npos,
           "a step section is visible before reset");
    (void)simulator.reset(json, profile);
    expect(!simulator.last_step() &&
               simulator.describe("").find("Last step:") == std::string::npos,
           "reset clears the report and its section");
    (void)simulator.step("");
    simulator.load({});
    expect(!simulator.last_step() &&
               simulator.describe("").find("Last step:") == std::string::npos,
           "load clears the report");
    (void)simulator.reset(json, profile);
    (void)simulator.step("");
    simulator.clear();
    expect(!simulator.last_step() &&
               simulator.describe("").find("Last step:") == std::string::npos,
           "clear clears the report");
  }
  // 6. Report details precede the assertions line, which stays last.
  {
    const auto profile = base_profile();
    const auto json = automation_graph_json(flag_graph(profile));
    AutomationSimulator simulator;
    if (!expect(!simulator.reset(json, profile).error, "the ordering fixture compiles"))
      return;
    (void)simulator.step("");
    const auto described = simulator.describe("flag-1=true");
    const auto step_at = described.find("Last step:");
    const auto assertions_at = described.rfind("Assertions: PASS");
    expect(step_at != std::string::npos && assertions_at != std::string::npos &&
               step_at < assertions_at && described.ends_with("Assertions: PASS"),
           "step details precede the assertions line, which stays last");
  }
  // 3/7. A runtime node-limit rejection is reported first and stays bounded.
  // Fifteen always-false 63-node conditions never fire, so the runtime
  // re-evaluates them for each of nine derived cooldown events: 15 x 63 x 9
  // exceeds max_automation_node_evaluations while the graph JSON stays small.
  {
    auto profile = base_profile();
    for (int index = 1; index <= 8; ++index) {
      auto stress = profile.cooldowns.front();
      stress.id = "stress-" + std::to_string(index);
      stress.name = "Stress " + std::to_string(index);
      profile.cooldowns.push_back(std::move(stress));
    }
    const auto rule_base = flag_graph(profile).automations.front().rules.front();
    const auto false_tree = [](const auto &self, const std::size_t depth)
        -> AutomationExpression {
      if (depth == 0) return expr::literal(AutomationValue(false));
      return expr::binary("or", self(self, depth - 1U), self(self, depth - 1U));
    };
    AutomationProfile nodes;
    nodes.profile_id = profile.id;
    AutomationRule fanout = rule_base;
    fanout.id = "fanout";
    fanout.event = AutomationEventKind::session_started;
    fanout.source_filter = "stress";
    fanout.condition = expr::literal(AutomationValue(true));
    fanout.actions.clear();
    for (const auto &cooldown : profile.cooldowns) {
      AutomationAction trigger;
      trigger.kind = AutomationActionKind::trigger_cooldown;
      trigger.target = cooldown.id;
      fanout.actions.push_back(std::move(trigger));
    }
    nodes.automations.push_back(
        {.id = "fanout", .name = "Fanout", .rules = {std::move(fanout)}});
    for (std::size_t index{}; index < 15U; ++index) {
      AutomationRule scan = rule_base;
      scan.id = "scan";
      scan.event = AutomationEventKind::cooldown_triggered;
      scan.source_filter.clear();
      scan.condition = false_tree(false_tree, 5U);
      AutomationAction reset;
      reset.kind = AutomationActionKind::reset_cooldown;
      reset.target = profile.cooldowns.front().id;
      scan.actions = {reset};
      nodes.automations.push_back({.id = "s" + std::to_string(index),
                                   .name = "Scan",
                                   .rules = {std::move(scan)}});
    }
    if (!expect(validate_automation_profile(nodes, profile).message.empty() &&
                    automation_graph_json(nodes).size() < max_automation_json_bytes,
                "the node-limit fixture is a valid graph within the JSON cap"))
      return;
    AutomationSimulator simulator;
    const auto compiled_nodes = simulator.reset(automation_graph_json(nodes), profile);
    if (!expect(!compiled_nodes.error,
                "the node-limit graph compiles: " +
                    compiled_nodes.error.value_or("")))
      return;
    simulator.load({AutomationEvent{.kind = AutomationEventKind::session_started,
                                    .source_id = "stress",
                                    .origin_token = 1}});
    (void)simulator.reset(automation_graph_json(nodes), profile);
    (void)simulator.step("");
    const auto report = simulator.last_step();
    const auto described = simulator.describe("");
    expect(report && report->rejection &&
               report->rejection->find(
                   std::to_string(max_automation_node_evaluations)) !=
                   std::string::npos &&
               report->effects.empty() &&
               described.find("Rejected:") != std::string::npos,
           "a node-limit rejection is reported with no committed effect");
    std::size_t details{};
    std::size_t omissions{};
    std::size_t at = described.find('\n');
    while (at != std::string::npos) {
      const auto next = described.find('\n', at + 1);
      const auto line = described.substr(at + 1, next - at - 1);
      if (line.starts_with("  \u2026 "))
        ++omissions;
      else if (line.starts_with("  "))
        ++details;
      at = next;
    }
    expect(details <= 32U && omissions == 1U,
           "the step detail list is capped with exactly one omission line");
  }
}

void simulator_item_previews() {
  using namespace std::chrono_literals;
  auto profile = base_profile();
  auto &cooldown = profile.cooldowns.front();
  cooldown.duration = 5s;
  cooldown.active_duration = 2s;
  auto &panel = profile.panels.front();
  panel.cooldown_ids.clear();
  panel.items = std::vector<OverlayItem>{
      {.id = "cooldown-row", .content = CooldownItem{cooldown.id}},
      {.id = "active-row", .content = CooldownActiveTimerItem{cooldown.id}},
      {.id = "timer-row", .content = AutomationTimerItem{"tick"}},
      {.id = "counter-row", .content = CounterItem{"stacks", 8, CounterDisplay::pips}},
      {.id = "indicator-row", .content = IndicatorItem{"flag-1", 2}}};
  auto graph = flag_graph(profile);
  graph.variables.push_back({.id = "stacks", .type = AutomationValueType::integer,
                            .default_value = AutomationValue(std::int64_t{3})});
  AutomationAction trigger;
  trigger.kind = AutomationActionKind::trigger_cooldown;
  trigger.target = cooldown.id;
  AutomationAction timer;
  timer.kind = AutomationActionKind::create_timer;
  timer.target = "tick";
  timer.value = expr::literal(AutomationValue(DurationValue{2s}));
  AutomationAction label;
  label.kind = AutomationActionKind::set_overlay_override;
  label.target = panel.id;
  label.item_id = "counter-row";
  label.name = "live-label";
  label.property = "text";
  label.value = expr::literal(AutomationValue(std::string("Live count")));
  graph.automations.front().rules.front().actions.insert(
      graph.automations.front().rules.front().actions.end(), {trigger, timer, label});

  AutomationSimulator simulator;
  expect(!simulator.panel_preview(panel.id), "an uncompiled simulator has no panel preview");
  simulator.load({
      {.kind = AutomationEventKind::session_started, .timestamp = TimePoint{}, .origin_token = 1},
      {.kind = AutomationEventKind::input_pressed, .timestamp = TimePoint(250ms), .origin_token = 2},
      {.kind = AutomationEventKind::input_pressed, .timestamp = TimePoint(2s), .origin_token = 3},
      {.kind = AutomationEventKind::input_pressed, .timestamp = TimePoint(5s), .origin_token = 4}});
  const auto reset = simulator.reset(automation_graph_json(graph), profile);
  expect(!reset.error, "the item simulator graph compiles");
  if (reset.error) return;
  const auto initial = simulator.panel_preview(panel.id);
  expect(initial && initial->presentation.items.size() == 5 &&
             initial->presentation.items[0].ready &&
             !initial->presentation.items[1].active &&
             initial->presentation.items[3].filled_pips == 3,
         "reset projects ready cooldowns and declared default counter values");
  expect(!simulator.panel_preview("unknown-panel"), "unknown panel IDs have no preview");
  (void)simulator.step("");
  const auto started = simulator.panel_preview(panel.id);
  expect(started && !started->presentation.items[0].ready &&
             started->presentation.items[1].active &&
             started->presentation.items[2].active &&
             started->presentation.items[3].label == "Live count" &&
             started->presentation.items[4].active &&
             started->presentation.items[4].flash_on,
         "committed cooldown effects, timers, item overrides and variables feed the preview");
  (void)simulator.step("");
  const auto flashing = simulator.panel_preview(panel.id);
  expect(flashing && flashing->presentation.items[1].active &&
             flashing->presentation.items[1].remaining < 1.0 &&
             !flashing->presentation.items[4].flash_on,
         "progress and flash phase use replay time, not a wall clock");
  (void)simulator.step("");
  const auto expired = simulator.panel_preview(panel.id);
  expect(expired && !expired->presentation.items[1].active &&
             !expired->presentation.items[2].active &&
             !expired->presentation.items[0].ready,
         "active duration and automation timer end before the cooldown is ready");
  (void)simulator.step("");
  const auto ready = simulator.panel_preview(panel.id);
  expect(ready && ready->presentation.items[0].ready,
         "later replay roots advance the cooldown engine to ready");

  AutomationAction hidden = label;
  hidden.item_id.clear();
  hidden.name = "hide-panel";
  hidden.property = "visible";
  hidden.value = expr::literal(AutomationValue(false));
  graph.automations.front().rules.front().actions.push_back(hidden);
  expect(!simulator.reset(automation_graph_json(graph), profile).error,
         "reset accepts a panel visibility override");
  (void)simulator.step("");
  const auto invisible = simulator.panel_preview(panel.id);
  expect(invisible && !invisible->presentation.visible,
         "panel override resolution is shared with the live overlay");
  expect(simulator.reset("{", profile).error && !simulator.panel_preview(panel.id),
         "a failed reset clears stale simulated panels");

  AutomationProfile empty;
  empty.profile_id = profile.id;
  simulator.load({
      {.kind = AutomationEventKind::cooldown_triggered, .timestamp = TimePoint{},
       .source_id = cooldown.id, .origin_token = 1},
      {.kind = AutomationEventKind::input_pressed, .timestamp = TimePoint(2s), .origin_token = 2}});
  expect(!simulator.reset(automation_graph_json(empty), profile).error,
         "standalone active duration does not need an automation rule");
  (void)simulator.step("");
  const auto standalone = simulator.panel_preview(panel.id);
  expect(standalone && standalone->presentation.items[1].active,
         "accepted cooldown trace facts arm standalone active-duration timers");
  (void)simulator.step("");
  const auto ended = simulator.panel_preview(panel.id);
  expect(ended && !ended->presentation.items[1].active,
         "standalone active-duration preview expires monotonically");
  simulator.clear();
  expect(!simulator.panel_preview(panel.id), "clear removes every stale preview value");
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
                 LAYERC_TEST_CASE(simulator_empty_loaded_trace),
                 LAYERC_TEST_CASE(simulator_timer_ordering),
                 LAYERC_TEST_CASE(simulator_item_previews),
                 LAYERC_TEST_CASE(simulator_step_report_contracts),
                 LAYERC_TEST_CASE(trace_files),
                 LAYERC_TEST_CASE(trace_control_codec_contracts),
                 LAYERC_TEST_CASE(control_failure_contracts),
                 LAYERC_TEST_CASE(simulator_event_bounds))

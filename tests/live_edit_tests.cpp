#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/core/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <variant>

namespace layerc::test {
namespace {

layerc::core::GameProfile live_profile() {
  using namespace std::chrono_literals;
  layerc::core::GameProfile profile;
  profile.id = "game";
  profile.name = "Game";
  profile.game.executable_name = "game.exe";
  profile.cooldowns.push_back({.id = "dash",
                               .name = "Dash",
                               .duration = 5s,
                               .max_charges = 1});
  profile.panels.push_back({.id = "dash-bar",
                            .name = "Dash",
                            .style = layerc::core::PanelStyle::bar,
                            .anchor_x = 0.5,
                            .anchor_y = 0.75,
                            .cooldown_ids = {"dash"}});
  profile.bindings.push_back(
      {.cooldown_id = "dash",
       .chord = {{{layerc::core::InputDevice::keyboard, 0x20, 0}}}});
  return profile;
}

} // namespace

void test_live_profile_reconciliation() {
  using namespace std::chrono_literals;
  using namespace layerc::core;
  const auto before = live_profile();
  auto cosmetic = before;
  cosmetic.name = "Renamed";
  cosmetic.cooldowns.front().name = "Dash renamed";
  cosmetic.panels.front().anchor_x = 0.2;
  cosmetic.bindings.front().chord.tokens.front().code = 0x21;
  const auto plan = plan_session_reconciliation(before, cosmetic);
  expect(!plan.recreate_instances && plan.rebuild_input_matcher &&
             plan.rebuild_overlays && plan.reset_cooldown_ids.empty() &&
             plan.preserved_cooldown_ids.size() == 1,
         "cosmetic, placement, and binding edits should preserve cooldown state");

  auto changed_definition = cosmetic;
  changed_definition.cooldowns.front().duration = 9s;
  const auto reset_plan =
      plan_session_reconciliation(cosmetic, changed_definition);
  expect(reset_plan.rebuild_overlays &&
             reset_plan.reset_cooldown_ids == std::vector<std::string>{"dash"},
         "runtime cooldown definition edits should reset only that cooldown");

  auto recovery_changed = cosmetic;
  recovery_changed.cooldowns.front().recovery = RecoveryMode::parallel;
  const auto recovery_plan =
      plan_session_reconciliation(cosmetic, recovery_changed);
  expect(
      recovery_plan.reset_cooldown_ids == std::vector<std::string>{"dash"},
      "recovery-mode edits (single to parallel) should reset the cooldown");

  auto visuals_only = before;
  visuals_only.cooldowns.front().name = "Dash visual";
  const auto visuals_plan = plan_session_reconciliation(before, visuals_only);
  expect(!visuals_plan.recreate_instances &&
             !visuals_plan.rebuild_input_matcher &&
             visuals_plan.rebuild_overlays &&
             visuals_plan.reset_cooldown_ids.empty(),
         "visual-only edits should rebuild overlays without resetting state");

  auto enablement_only = before;
  enablement_only.enabled = false;
  const auto enablement_plan =
      plan_session_reconciliation(before, enablement_only);
  expect(enablement_plan.recreate_instances &&
             !enablement_plan.rebuild_input_matcher &&
             !enablement_plan.rebuild_overlays,
         "enablement-only edits should recreate instance membership only");

  auto hid_before = before;
  hid_before.hid_bindings.push_back({.id = "stick",
                                     .device = {.vendor_id = 1,
                                                .product_id = 2,
                                                .usage_page = 1,
                                                .usage = 4,
                                                .descriptor_hash =
                                                    std::string(64, 'a')},
                                     .usage_page = 1,
                                     .usage = 0x30,
                                     .activation =
                                         HidActivationMode::axis_positive});
  hid_before.bindings.front().chord.tokens = {
      {InputDevice::generic_hid, 1, 0}};
  auto hid_after = hid_before;
  hid_after.hid_bindings.front().usage = 0x31;
  const auto hid_plan = plan_session_reconciliation(hid_before, hid_after);
  expect(!hid_plan.recreate_instances && hid_plan.rebuild_input_matcher,
         "HID descriptor recapture should rebuild baselines even when compact tokens are unchanged");

  auto isolated = before;
  isolated.visual_runtime = VisualRuntime::isolated;
  expect(plan_session_reconciliation(before, isolated).recreate_instances,
         "changing visual runtime ownership should recreate affected sessions");
  auto module_changed = before;
  module_changed.modules.push_back(
      {.kind = ProfileModuleKind::visual, .schema_version = 1});
  expect(plan_session_reconciliation(before, module_changed)
             .recreate_instances,
         "changing visual module identity should recreate affected sessions");

  auto added = before;
  added.cooldowns.push_back({.id = "smoke",
                             .name = "Smoke",
                             .duration = 3s});
  const auto added_plan = plan_session_reconciliation(before, added);
  expect(added_plan.added_cooldown_ids == std::vector<std::string>{"smoke"} &&
             added_plan.rebuild_input_matcher && added_plan.rebuild_overlays,
         "added cooldowns should rebuild dependent input and overlay state");

  auto removed = added;
  removed.cooldowns.erase(removed.cooldowns.begin());
  const auto removed_plan = plan_session_reconciliation(added, removed);
  expect(removed_plan.removed_cooldown_ids ==
                 std::vector<std::string>{"dash"} &&
             removed_plan.rebuild_input_matcher &&
             removed_plan.rebuild_overlays,
         "removed cooldowns should rebuild dependent input and overlay state");

  auto changed_matcher = changed_definition;
  changed_matcher.game.executable_name = "other.exe";
  expect(plan_session_reconciliation(changed_definition, changed_matcher)
             .recreate_instances,
         "matcher edits should recreate only affected runtime instances");

  CooldownEngine engine(before.cooldowns);
  const TimePoint start{};
  expect(engine.trigger("dash", start),
         "reconciliation test cooldown should start");
  engine.reconfigure(cosmetic.cooldowns, {}, start + 1s);
  expect(engine.remaining("dash", start + 1s) == 4s,
         "cosmetic reconfiguration should retain the active timeline");
  const std::vector<std::string> reset{"dash"};
  engine.reconfigure(changed_definition.cooldowns, reset, start + 1s);
  expect(engine.remaining("dash", start + 1s) == Milliseconds::zero(),
         "changed cooldown definitions should become ready");
}

void test_shared_pool_reconciliation() {
  using namespace std::chrono_literals;
  using namespace layerc::core;
  GameProfile before;
  before.cooldowns = {{.id = "a", .name = "A", .duration = 1s},
                      {.id = "b", .name = "B", .duration = 10s}};
  before.cooldown_groups = {{.id = "pool",
                            .name = "Pool",
                            .mode = CooldownGroupMode::shared_pool,
                            .cooldown_ids = {"a", "b"},
                            .duration = 5s,
                            .max_charges = 2,
                            .recovery = RecoveryMode::parallel}};
  const TimePoint start{};
  const std::vector<std::string> aliases{"a", "b"};
  const auto check_edit = [&](const GameProfile &after) {
    const auto delta = diff_profiles(before, after);
    const auto plan = plan_session_reconciliation(before, after);
    expect(!delta.empty() && plan.reset_cooldown_ids == aliases &&
               plan.preserved_cooldown_ids.empty() && plan.rebuild_overlays,
           "CM-01: effective pool edits reset every public alias");
    CooldownEngine engine(before.cooldowns, before.cooldown_groups);
    expect(engine.trigger("a", start), "pool edit fixture consumes a charge");
    engine.reconfigure(after.cooldowns, plan.reset_cooldown_ids, start + 100ms,
                       after.cooldown_groups);
    expect(engine.remaining("a", start + 100ms) == 0ms &&
               engine.remaining("b", start + 100ms) == 0ms,
           "CM-01: a group edit cannot retain the old 4900 ms deadline");
  };
  auto after = before;
  after.cooldown_groups.front().duration = 7s;
  check_edit(after);
  after = before;
  after.cooldown_groups.front().max_charges = 3;
  check_edit(after);
  after = before;
  after.cooldown_groups.front().recovery = RecoveryMode::sequential;
  check_edit(after);
  after = before;
  after.cooldown_groups.front().retrigger = RetriggerPolicy::restart;
  check_edit(after);
  after = before;
  after.cooldown_groups.front().id = "new-pool";
  check_edit(after);
  after = before;
  after.cooldown_groups.front().mode = CooldownGroupMode::trigger_propagation;
  check_edit(after);

  after = before;
  after.cooldowns.front().duration = 9s;
  after.cooldowns.front().max_charges = 4;
  after.cooldowns.front().recovery = RecoveryMode::single;
  after.cooldowns.front().retrigger = RetriggerPolicy::restart;
  after.cooldown_groups.front().name = "Renamed pool";
  const auto dormant_plan = plan_session_reconciliation(before, after);
  expect(!diff_profiles(before, after).empty() &&
             dormant_plan.reset_cooldown_ids.empty() &&
             dormant_plan.preserved_cooldown_ids == aliases,
         "CM-01: dormant member settings and pool labels are not runtime owners");
  CooldownEngine preserved(before.cooldowns, before.cooldown_groups);
  expect(preserved.trigger_with_propagation("a", start, 91).size() == 1,
         "preservation fixture consumes one owner charge");
  std::ranges::reverse(after.cooldowns);
  std::ranges::reverse(after.cooldown_groups.front().cooldown_ids);
  preserved.reconfigure(after.cooldowns, {}, start + 100ms,
                        after.cooldown_groups);
  expect(preserved.remaining("a", start + 100ms) == 4900ms &&
             preserved.remaining("b", start + 100ms) == 4900ms &&
             preserved.trigger_with_propagation("b", start + 100ms, 91).empty(),
         "CM-01: reorder and cosmetic edits preserve one owner and its origin");

  for (const auto &reset_id : aliases) {
    for (const bool reversed : {false, true}) {
      after = before;
      if (reversed) {
        std::ranges::reverse(after.cooldowns);
        std::ranges::reverse(after.cooldown_groups.front().cooldown_ids);
      }
      CooldownEngine engine(before.cooldowns, before.cooldown_groups);
      expect(engine.trigger("a", start), "member reset fixture consumes a charge");
      const std::vector<std::string> reset{reset_id};
      engine.reconfigure(after.cooldowns, reset, start + 100ms,
                         after.cooldown_groups);
      const auto states = engine.snapshot(start + 100ms);
      expect(std::ranges::all_of(states, [](const auto &state) {
               return state.available_charges == 2 && !state.next_charge_at &&
                      state.recovery_deadlines.empty();
             }),
             "CM-01: no alias or iteration order can defeat an owner reset");
    }
  }

  after = before;
  after.cooldown_groups.front().duration = 7s;
  CooldownEngine direct(before.cooldowns, before.cooldown_groups);
  expect(direct.trigger("a", start), "direct reconfigure fixture consumes a charge");
  direct.reconfigure(after.cooldowns, {}, start + 100ms, after.cooldown_groups);
  expect(direct.remaining("a", start + 100ms) == 0ms,
         "CM-01: reconfigure itself compares effective owner definitions");

  CooldownEngine removed(before.cooldowns, before.cooldown_groups);
  expect(removed.trigger("a", start), "removed alias fixture consumes a charge");
  after = before;
  after.cooldowns.erase(after.cooldowns.begin());
  after.cooldown_groups.front().cooldown_ids = {"b"};
  removed.reconfigure(after.cooldowns, {}, start + 100ms, after.cooldown_groups);
  expect(removed.remaining("b", start + 100ms) == 4900ms,
         "CM-01: removing the first alias preserves the unchanged pool owner");
  after.cooldowns.push_back({.id = "c", .name = "C", .duration = 2s});
  after.cooldown_groups.front().cooldown_ids.push_back("c");
  removed.reconfigure(after.cooldowns, {}, start + 100ms, after.cooldown_groups);
  expect(removed.remaining("c", start + 100ms) == 4900ms,
         "CM-01: a new alias joins the existing owner, not dormant member state");

  auto propagation = before;
  propagation.cooldown_groups.front().mode = CooldownGroupMode::trigger_propagation;
  auto topology = propagation;
  topology.cooldown_groups.front().cooldown_ids = {"a"};
  expect(!diff_profiles(propagation, topology).empty() &&
             plan_session_reconciliation(propagation, topology)
                 .reset_cooldown_ids.empty(),
         "CM-01: propagation edits are visible without changing timer owners");
}

void test_live_protocol_framing() {
  using namespace layerc::core;
  LiveMessage source{
      .type = LiveMessageType::begin_edit,
      .revision = 9'007'199'254'740'993ULL,
      .edit_session = 42,
      .target_window = 0xFEDCBA9876543210ULL,
      .profile_key = "C:/LayerC/profiles/gam\xC3\xA9.json",
      .panel_id = "dash-bar",
      .profile_json = R"({"schemaVersion":1})",
      .anchor_x = 0.125,
      .anchor_y = 0.875,
  };
  const auto encoded = encode_live_message_json(source);
  const auto decoded = decode_live_message_json(encoded);
  const auto *message = std::get_if<LiveMessage>(&decoded);
  expect(message != nullptr && message->revision == source.revision &&
             message->target_window == source.target_window &&
             message->profile_key == source.profile_key &&
             message->profile_json == source.profile_json &&
             message->anchor_y == 0.5 &&
             encoded.find("anchorY") == std::string::npos,
         "live JSON should preserve UTF-8, profile bytes, and 64-bit identifiers exactly");

  const auto initial_tree = json::parse(encoded);
  expect(std::holds_alternative<json::Value>(initial_tree) &&
             std::get<json::Value>(initial_tree).find("profile")->is_object() &&
             encoded.find("profileBase64") == std::string::npos,
         "live profiles must be nested objects, never encoded strings");
  for (const auto &payload : {std::string{}, std::string{"f"},
                              std::string{"[]"}, std::string{"\0\xff", 2}}) {
    auto invalid = source;
    invalid.profile_json = payload;
    expect(std::holds_alternative<LiveProtocolError>(encode_live_frame(invalid)),
           "invalid local nested documents must produce a typed framing error");
  }
  for (const auto packet : {
           R"({"protocolVersion":4,"type":"committedProfile","revision":"0","profileKey":"x","profileBase64":"e30="})",
           R"({"protocolVersion":5,"type":"committedProfile","revision":"0","profileKey":"x","profile":"{}"})",
           R"({"protocolVersion":5,"type":"committedProfile","revision":"0","profileKey":"x","profile":[]})",
           R"({"protocolVersion":5,"type":"committedProfile","revision":"0","profileKey":"x","profile":null})"}) {
    const auto rejected = decode_live_message_json(packet);
    const auto *error = std::get_if<LiveProtocolError>(&rejected);
    expect(error != nullptr && !error->message.empty(),
           "old peers and nonobject nested profiles must fail visibly");
  }

  const LiveMessage status{.type = LiveMessageType::runtime_status,
                           .text = "Visual recognition: WGC active"};
  const auto decoded_status =
      decode_live_message_json(encode_live_message_json(status));
  const auto *status_message = std::get_if<LiveMessage>(&decoded_status);
  expect(status_message != nullptr &&
             status_message->type == LiveMessageType::runtime_status &&
             status_message->text == status.text,
         "runtime capture status should round-trip over the authenticated channel");

  for (const auto type : {LiveMessageType::diagnostics_request,
                          LiveMessageType::diagnostics_reply,
                          LiveMessageType::retry_visual}) {
    const LiveMessage diagnostic{
        .type = type,
        .revision = 7,
        .text = type == LiveMessageType::diagnostics_reply
                    ? "{\"workingSet\":1}" : ""};
    const auto round_trip =
        decode_live_message_json(encode_live_message_json(diagnostic));
    const auto *decoded_diagnostic = std::get_if<LiveMessage>(&round_trip);
    expect(decoded_diagnostic != nullptr &&
               decoded_diagnostic->type == type &&
               decoded_diagnostic->text == diagnostic.text,
           "diagnostics messages should round-trip over the current live protocol");
  }

  // Exercise every descriptor. Its emitted fields are all required, and
  // unrelated fields are rejected rather than silently ignored.
  for (const auto type : {
           LiveMessageType::handshake, LiveMessageType::committed_profile,
           LiveMessageType::begin_edit, LiveMessageType::live_profile,
           LiveMessageType::select_panel, LiveMessageType::anchor_changed,
           LiveMessageType::set_direct_drag, LiveMessageType::end_edit,
           LiveMessageType::heartbeat, LiveMessageType::runtime_status,
           LiveMessageType::diagnostics_request,
           LiveMessageType::diagnostics_reply, LiveMessageType::retry_visual,
           LiveMessageType::automation_trace_subscribe,
           LiveMessageType::automation_trace_unsubscribe,
           LiveMessageType::automation_trace_event,
           LiveMessageType::retry_automation,
           LiveMessageType::acknowledgement, LiveMessageType::error}) {
    auto example = source;
    example.type = type;
    example.text = "payload";
    const auto wire = encode_live_message_json(example);
    const auto round_trip = decode_live_message_json(wire);
    const auto *received = std::get_if<LiveMessage>(&round_trip);
    expect(received != nullptr && received->type == type &&
               encode_live_message_json(*received) == wire,
           "every descriptor must round-trip exactly its own fields");
    auto unexpected = wire;
    unexpected.insert(unexpected.size() - 1U, ",\"unexpected\":true");
    expect(std::holds_alternative<LiveProtocolError>(
               decode_live_message_json(unexpected)),
           "every descriptor must reject unknown fields");
    const auto tree = json::parse(wire);
    if (const auto *object = std::get_if<json::Value>(&tree)) {
      for (const auto &[key, ignored] : object->as_object()) {
        (void)ignored;
        auto missing = object->as_object();
        missing.erase(key);
        std::string incomplete;
        json::append_value(incomplete, json::Value(std::move(missing)));
        expect(std::holds_alternative<LiveProtocolError>(
                   decode_live_message_json(incomplete)),
               "every emitted descriptor field must be mandatory");
      }
    }
  }

  const auto encoded_frame = encode_live_frame(source);
  const auto *frame = std::get_if<std::vector<std::byte>>(&encoded_frame);
  expect(frame != nullptr, "a bounded live message should produce a frame");
  LiveFrameDecoder decoder;
  if (frame != nullptr) {
    expect(decoder.push(std::span(*frame).first(2)).empty(),
           "partial frame headers should remain buffered");
    const auto results = decoder.push(std::span(*frame).subspan(2));
    expect(results.size() == 1 &&
               std::holds_alternative<LiveMessage>(results.front()),
           "split live frames should decode once complete");
  }

  auto wrong_version = encoded;
  const auto version = wrong_version.find("\"protocolVersion\":5");
  if (version != std::string::npos)
    wrong_version.replace(version, 19, "\"protocolVersion\":1");
  expect(std::holds_alternative<LiveProtocolError>(
             decode_live_message_json(wrong_version)),
         "protocol version mismatches should be rejected");
  expect(revision_decision(10, 9) == RevisionDecision::accept &&
             revision_decision(9, 9) == RevisionDecision::duplicate &&
             revision_decision(8, 9) == RevisionDecision::stale,
         "revision decisions should reject duplicate and stale state");
  expect(revision_decision(1, 0) == RevisionDecision::accept &&
             revision_decision(1, 100) == RevisionDecision::stale,
         "peer-origin revisions must be compared with their peer stream, not a local send counter");

  expect(std::holds_alternative<LiveProtocolError>(decode_live_message_json(
             R"({"protocolVersion":5,"type":"anchorChanged","revision":"1","editSession":"2","panelId":"main","anchorX":0.5,"final":true})")),
         "anchor messages missing a mandatory coordinate should be rejected");

  LiveMessage too_large{.type = LiveMessageType::live_profile,
                        .revision = 1,
                        .edit_session = 1,
                        .profile_json =
                            std::string(max_live_message_bytes, 'x')};
  expect(std::holds_alternative<LiveProtocolError>(
             encode_live_frame(too_large)),
         "oversized outbound messages should return an explicit protocol error");

  const std::byte oversized_header[]{std::byte{0x01}, std::byte{0x00},
                                     std::byte{0x08}, std::byte{0x00}};
  const auto oversized = decoder.push(oversized_header);
  expect(oversized.size() == 1 &&
             std::holds_alternative<LiveProtocolError>(oversized.front()),
         "oversized live frames should fail before allocating their payload");

  std::vector<std::byte> excessive_buffer(max_live_buffer_bytes + 1U,
                                          std::byte{});
  const auto excessive = decoder.push(excessive_buffer);
  expect(excessive.size() == 1 &&
             std::holds_alternative<LiveProtocolError>(excessive.front()) &&
             decoder.buffered_bytes() == 0 && decoder.reserved_bytes() == 0,
         "incomplete or hostile streams should not grow the decoder buffer without bound");

  std::vector<std::byte> partial(64U * 1024U, std::byte{});
  const auto partial_length = static_cast<std::uint32_t>(max_live_message_bytes);
  partial[0] = static_cast<std::byte>(partial_length & 0xFFU);
  partial[1] = static_cast<std::byte>((partial_length >> 8U) & 0xFFU);
  partial[2] = static_cast<std::byte>((partial_length >> 16U) & 0xFFU);
  partial[3] = static_cast<std::byte>((partial_length >> 24U) & 0xFFU);
  expect(decoder.push(partial).empty() && decoder.buffered_bytes() != 0 &&
             decoder.reserved_bytes() != 0,
         "an incomplete valid frame should reserve decoder storage");
  decoder.clear();
  expect(decoder.buffered_bytes() == 0 && decoder.reserved_bytes() == 0,
         "disconnect cleanup should release decoder capacity");

  FrameDecoder framing(4, 12);
  const std::byte small_frame[]{std::byte{2}, std::byte{}, std::byte{},
                               std::byte{}, std::byte{'o'}, std::byte{'k'}};
  framing.feed(small_frame);
  framing.feed(std::span(small_frame).first(4));
  expect(framing.next_frame() == "ok" && !framing.next_frame(),
         "the shared decoder drains complete frames before a partial tail");
  framing.feed(std::span(small_frame).subspan(4));
  expect(framing.next_frame() == "ok" && framing.buffered_bytes() == 0,
         "a compacted partial tail must complete without retained consumed bytes");
  const std::byte invalid_length[]{std::byte{5}, std::byte{}, std::byte{}, std::byte{}};
  framing.feed(invalid_length);
  expect(!framing.next_frame() &&
             framing.fault() == FrameDecoder::Fault::frame_too_large &&
             framing.reserved_bytes() == 0,
         "oversized frames release storage and fault before reading payloads");
  framing.feed(small_frame);
  expect(!framing.next_frame(), "framing faults remain sticky until reset");
  framing.clear();
  const std::byte flood[13]{};
  framing.feed(flood);
  expect(framing.fault() == FrameDecoder::Fault::buffer_too_large &&
             framing.reserved_bytes() == 0,
         "a buffer flood is rejected before allocating storage");
}

void test_live_placement_math() {
  using namespace layerc::core;
  const PixelRect target{100, 200, 1100, 700};
  const auto anchor = normalized_anchor_for_drag(610, 460, 10, 10, target);
  expect(anchor == NormalizedAnchor{0.5, 0.5},
         "drag placement should preserve the mouse grab offset");
  expect(normalized_anchor_for_drag(-100, 9999, 0, 0, target) ==
             NormalizedAnchor{0.0, 1.0},
         "drag placement should clamp normalized anchors");
  expect(overlay_rectangle(anchor, target, 200, 40) ==
             PixelRect{500, 430, 700, 470},
         "overlay rectangles should center on normalized anchors");
}

} // namespace layerc::test

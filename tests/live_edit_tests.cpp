#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/live_edit.hpp"

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
  expect(reset_plan.reset_cooldown_ids == std::vector<std::string>{"dash"},
         "runtime cooldown definition edits should reset only that cooldown");

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
             message->anchor_y == source.anchor_y,
         "live JSON should preserve UTF-8 and 64-bit identifiers exactly");

  const auto frame = encode_live_frame(source);
  LiveFrameDecoder decoder;
  expect(decoder.push(std::span(frame).first(2)).empty(),
         "partial frame headers should remain buffered");
  const auto results = decoder.push(std::span(frame).subspan(2));
  expect(results.size() == 1 &&
             std::holds_alternative<LiveMessage>(results.front()),
         "split live frames should decode once complete");

  auto wrong_version = encoded;
  const auto version = wrong_version.find("\"protocolVersion\":1");
  if (version != std::string::npos)
    wrong_version.replace(version, 19, "\"protocolVersion\":2");
  expect(std::holds_alternative<LiveProtocolError>(
             decode_live_message_json(wrong_version)),
         "protocol version mismatches should be rejected");
  expect(revision_decision(10, 9) == RevisionDecision::accept &&
             revision_decision(9, 9) == RevisionDecision::duplicate &&
             revision_decision(8, 9) == RevisionDecision::stale,
         "revision decisions should reject duplicate and stale state");

  const std::byte oversized_header[]{std::byte{0x01}, std::byte{0x00},
                                     std::byte{0x08}, std::byte{0x00}};
  const auto oversized = decoder.push(oversized_header);
  expect(oversized.size() == 1 &&
             std::holds_alternative<LiveProtocolError>(oversized.front()),
         "oversized live frames should fail before allocating their payload");
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

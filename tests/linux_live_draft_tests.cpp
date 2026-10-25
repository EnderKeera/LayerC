// Live draft payloads and LayerD's in-memory draft lease, without a display.
#include "daemon_host.hpp"
#include "live_draft.hpp"
#include "control_channel.hpp"

#include "layerc/core/live_draft_control.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include "test_support.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace core = layerc::core;
using layerc::test::expect;
using layerc::platform::StoredProfile;

// A complete schema-v4 profile: one cooldown, panel and binding.
core::GameProfile profile(const std::string &id, const std::string &name) {
  const auto json = R"({"schemaVersion":4,"id":")" + id + R"(","name":")" + name +
      R"(","enabled":true,"modules":[],"hidBindings":[],"cooldownGroups":[],)"
      R"("game":{"executableName":"fixture.exe"},)"
      R"("cooldowns":[{"id":"ability","name":"Ability","durationMs":8000,"maxCharges":1,)"
      R"("retrigger":"ignore","recovery":"single"}],)"
      R"("panels":[{"id":"main","name":"Main","layout":"row","style":"bar",)"
      R"("bar":{"width":360,"height":30,"fillColor":"#50BAC8","backgroundColor":"#202A31",)"
      R"("borderColor":"#50BAC8","textColor":"#F1F5F7","hideWhenReady":false},)"
      R"("anchorX":0.25,"anchorY":0.75,"cooldowns":["ability"]}],)"
      R"("bindings":[{"cooldownId":"ability","chord":[{"device":"keyboard","code":69}]}]})";
  auto parsed = core::parse_profile_json(json);
  expect(std::holds_alternative<core::GameProfile>(parsed), "the fixture profile is valid");
  return std::get<core::GameProfile>(std::move(parsed));
}

core::LiveDraftRequest publish(std::uint64_t revision, const core::GameProfile &draft,
                               std::string client = "editor-1") {
  return {core::LiveDraftOperation::publish, draft.id, std::move(client), revision,
          core::serialize_profile_json(draft)};
}

void codec_contracts() {
  const auto draft = profile("fixture", "Draft name");
  const auto request = publish(3, draft);
  expect(core::parse_live_draft_request(core::serialize_live_draft_request(request)) == request,
         "publish requests round-trip");
  const core::LiveDraftRequest renew{core::LiveDraftOperation::renew, "fixture", "editor-1", 3, {}};
  expect(core::parse_live_draft_request(core::serialize_live_draft_request(renew)) == renew,
         "renew requests round-trip without a body");
  const core::LiveDraftRequest end{core::LiveDraftOperation::end, "fixture", "editor-1", 0, {}};
  expect(core::live_draft_request_valid(end), "end needs no revision");
  auto invalid = request;
  invalid.profile_json.clear();
  expect(!core::live_draft_request_valid(invalid), "publish requires a profile body");
  invalid = renew;
  invalid.profile_json = "{}";
  expect(!core::live_draft_request_valid(invalid), "renew carries no body");
  invalid = request;
  invalid.revision = 0;
  expect(!core::live_draft_request_valid(invalid), "publish requires a nonzero revision");
  invalid = request;
  invalid.client_id = "bad client";
  expect(!core::live_draft_request_valid(invalid), "client ids are bounded tokens");
  invalid = request;
  invalid.profile_json.assign(core::live_draft_profile_bytes + 1, 'x');
  expect(!core::live_draft_request_valid(invalid), "profile bodies are bounded");
  auto wire = core::serialize_live_draft_request(renew);
  wire.replace(wire.find("\"draftVersion\":1"), 16, "\"draftVersion\":2");
  expect(!core::parse_live_draft_request(wire), "another payload version is refused");
  wire = core::serialize_live_draft_request(renew);
  wire.insert(wire.size() - 1, ",\"extra\":true");
  expect(!core::parse_live_draft_request(wire), "unknown fields are refused");
  const core::LiveDraftReply reply{7, core::LiveDraftState::renewed};
  expect(core::parse_live_draft_reply(core::serialize_live_draft_reply(reply)) == reply,
         "replies round-trip");
  expect(!core::parse_live_draft_reply("{\"draftVersion\":1,\"revision\":1,\"state\":\"lost\"}"),
         "unknown reply states are refused");
}

namespace {

std::optional<core::LiveDraftError> draft_failure(const layerc::daemon::LiveDraftResult &result) {
  if (const auto *failure = std::get_if<core::LiveDraftFailure>(&result)) return failure->code;
  return std::nullopt;
}

} // namespace

void host_lease_and_revisions() {
  const std::vector<StoredProfile> stored{{"fixture.json", profile("fixture", "Committed")},
                                          {"other.json", profile("other", "Other")}};
  layerc::daemon::LiveDraftHost host;
  const auto now = core::MonotonicClock::now();
  bool changed{};
  const auto first = host.handle(publish(1, profile("fixture", "Draft one")), stored, now, changed);
  expect(std::holds_alternative<core::LiveDraftReply>(first) && changed && host.active() &&
             std::get<core::LiveDraftReply>(first).state == core::LiveDraftState::applied,
         std::string("a stored profile's draft applies: ") +
             (draft_failure(first) ? "refused" : "ok"));
  const auto effective = host.effective(stored);
  expect(effective[0].profile.name == "Draft one" && effective[0].path == "fixture.json" &&
             effective[1].profile.name == "Other" && stored[0].profile.name == "Committed",
         "only the drafted base profile is replaced, in memory");
  const auto duplicate = host.handle(publish(1, profile("fixture", "Draft one")), stored, now, changed);
  expect(!changed && std::get<core::LiveDraftReply>(duplicate).state == core::LiveDraftState::duplicate,
         "a repeated revision is a duplicate, not a reapply");
  (void)host.handle(publish(3, profile("fixture", "Draft three")), stored, now, changed);
  expect(changed && host.describe() == "fixture r3", "a newer revision replaces the draft");
  expect(draft_failure(host.handle(publish(2, profile("fixture", "Old")), stored, now, changed)) ==
             core::LiveDraftError::stale_revision &&
             !changed && host.effective(stored)[0].profile.name == "Draft three",
         "a stale revision is refused and leaves the draft intact");
  expect(draft_failure(host.handle(publish(4, profile("fixture", "Other editor"), "editor-2"),
                                    stored, now, changed)) ==
             core::LiveDraftError::owned_elsewhere,
         "another editor cannot replace an owned draft");
  expect(draft_failure(host.handle(
             {core::LiveDraftOperation::renew, "fixture", "editor-1", 2, {}}, stored, now, changed)) ==
             core::LiveDraftError::revision_mismatch,
         "a renewal must name the current revision");
  const auto later = now + std::chrono::milliseconds(2000);
  const auto renewed = host.handle({core::LiveDraftOperation::renew, "fixture", "editor-1", 3, {}},
                                   stored, later, changed);
  expect(std::get<core::LiveDraftReply>(renewed).state == core::LiveDraftState::renewed &&
             host.next_deadline() == later + core::live_draft_lease,
         "renewal extends the monotonic lease");
  expect(!host.expire(later + std::chrono::milliseconds(2400), stored) && host.active(),
         "a renewed lease outlives the original deadline");
  expect(host.expire(later + core::live_draft_lease, stored) && !host.active() &&
             host.effective(stored)[0].profile.name == "Committed" && !host.next_deadline(),
         "an expired lease restores the committed profile and stops waking LayerD");
}

void expiry_during_failing_request() {
  const std::vector<StoredProfile> stored{{"fixture.json", profile("fixture", "Committed")}};
  layerc::daemon::LiveDraftHost host;
  const auto now = core::MonotonicClock::now();
  bool changed{};
  (void)host.handle(publish(1, profile("fixture", "Draft")), stored, now, changed);
  // The renewal arrives exactly at the deadline: the lease has already lapsed.
  const auto late = host.handle({core::LiveDraftOperation::renew, "fixture", "editor-1", 1, {}},
                                stored, now + core::live_draft_lease, changed);
  expect(draft_failure(late) == core::LiveDraftError::expired && changed && !host.active() &&
             !host.next_deadline() && host.effective(stored)[0].profile.name == "Committed",
         "a refused late renewal still reports the expiry that restored the committed profile");
  (void)host.handle(publish(2, profile("fixture", "Draft two")), stored, now, changed);
  auto broken = publish(3, profile("fixture", "Draft"));
  broken.profile_json = "{\"id\":\"fixture\"}";
  const auto refused = host.handle(broken, stored, now + 2 * core::live_draft_lease, changed);
  expect(draft_failure(refused) == core::LiveDraftError::invalid_profile && changed &&
             !host.active(),
         "an invalid publish after expiry still reports the restoring change");
}

void host_refusals_and_end() {
  const std::vector<StoredProfile> stored{{"fixture.json", profile("fixture", "Committed")}};
  layerc::daemon::LiveDraftHost host;
  const auto now = core::MonotonicClock::now();
  bool changed{};
  expect(draft_failure(host.handle(publish(1, profile("unsaved", "New")), stored, now, changed)) ==
             core::LiveDraftError::unknown_profile && !host.active(),
         "a draft must name a stored profile");
  auto mismatch = publish(1, profile("fixture", "Draft"));
  mismatch.profile_id = "other";
  expect(draft_failure(host.handle(mismatch, stored, now, changed)) ==
             core::LiveDraftError::id_mismatch,
         "the draft body must carry the named profile id");
  auto broken = publish(1, profile("fixture", "Draft"));
  broken.profile_json = "{\"id\":\"fixture\"}";
  expect(draft_failure(host.handle(broken, stored, now, changed)) ==
             core::LiveDraftError::invalid_profile && !host.active(),
         "an invalid profile body is refused before replacing anything");
  (void)host.handle(publish(1, profile("fixture", "Draft")), stored, now, changed);
  const auto foreign = host.handle({core::LiveDraftOperation::end, "fixture", "editor-2", 0, {}},
                                   stored, now, changed);
  expect(std::holds_alternative<core::LiveDraftReply>(foreign) && !changed && host.active(),
         "ending another editor's draft changes nothing");
  const auto ended = host.handle({core::LiveDraftOperation::end, "fixture", "editor-1", 0, {}},
                                 stored, now, changed);
  expect(std::get<core::LiveDraftReply>(ended).state == core::LiveDraftState::ended && changed &&
             !host.active(), "the owner ends its draft");
  (void)host.handle(publish(1, profile("fixture", "Draft")), stored, now, changed);
  expect(host.expire(now, {}) && !host.active(),
         "a draft ends when its stored profile disappears");
}

void control_dispatch() {
  layerc::runtime::DaemonKernel kernel;
  const auto request = layerc::platform::encode_control_request(
      layerc::platform::ControlCommand::draft,
      core::serialize_live_draft_request(publish(1, profile("fixture", "Draft"))));
  const auto unavailable = layerc::daemon::handle_control_request(request, kernel);
  expect(unavailable.response.find("live drafts unavailable") != std::string::npos,
         "a daemon without a draft host refuses explicitly");
  core::LiveDraftRequest seen;
  const auto reply = layerc::daemon::handle_control_request(request, kernel, {}, {},
      [&](const core::LiveDraftRequest &incoming) -> layerc::daemon::DraftControlResult {
        seen = incoming;
        return core::LiveDraftReply{1, core::LiveDraftState::applied};
      });
  expect(seen.profile_id == "fixture" && seen.revision == 1 &&
             reply.response.find("\"ok\":true") != std::string::npos &&
             reply.response.find("applied") != std::string::npos && !reply.reload_requested,
         "draft payloads dispatch to the typed handler without a store reload");
  const auto invalid = layerc::daemon::handle_control_request(
      layerc::platform::encode_control_request(layerc::platform::ControlCommand::draft, "{}"), kernel);
  expect(invalid.response.find("invalid live draft request") != std::string::npos &&
             invalid.response.find("\"code\":\"invalid-request\"") != std::string::npos,
         "malformed draft payloads are refused with a machine code");
  const auto trace_expired = layerc::daemon::handle_control_request(
      layerc::platform::encode_control_request(layerc::platform::ControlCommand::trace,
                                               R"({"traceVersion":1,"operation":"poll","profileId":"fixture","clientId":"client-a","generation":1,"cursor":0})"),
      kernel, {},
      [](const core::TraceControlRequest &) -> layerc::daemon::TraceControlResult {
        return core::TraceControlFailure{core::TraceControlError::expired,
                                         "renamed presentation"};
      });
  expect(trace_expired.response.find("\"code\":\"expired\"") != std::string::npos &&
             trace_expired.response.find("renamed presentation") != std::string::npos,
         "a trace failure carries its machine code independently of renamed prose");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(codec_contracts),
                 LAYERC_TEST_CASE(host_lease_and_revisions),
                 LAYERC_TEST_CASE(expiry_during_failing_request),
                 LAYERC_TEST_CASE(host_refusals_and_end),
                 LAYERC_TEST_CASE(control_dispatch))

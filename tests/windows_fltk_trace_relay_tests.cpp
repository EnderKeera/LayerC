#include "platform/win32/trace_relay.hpp"
#include "layerc/core/automation_trace_control.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace core = layerc::core;
namespace editor = layerc::editor;

core::AutomationEvent make_event(const std::uint64_t id) {
  core::AutomationEvent event;
  event.kind = core::AutomationEventKind::session_started;
  event.timestamp = core::TimePoint{} + core::Milliseconds{
      static_cast<std::int64_t>(id)};
  event.source_id = "event-" + std::to_string(id);
  event.origin_token = id;
  event.fields.emplace("id", core::AutomationValue{
                                  static_cast<std::int64_t>(id)});
  return event;
}

core::AutomationEvent make_large_event(const std::uint64_t id,
                                       const std::size_t maps) {
  auto event = make_event(id);
  for (std::size_t map_index{}; map_index < maps; ++map_index) {
    core::AutomationMap nested;
    for (std::size_t entry_index{};
         entry_index < core::max_automation_collection_entries; ++entry_index) {
      std::string key = "key-" + std::to_string(entry_index);
      key.resize(core::max_automation_string_bytes, 'k');
      nested.emplace(std::move(key),
                     core::AutomationValue{std::string(
                         core::max_automation_string_bytes, 'v')});
    }
    event.fields.emplace("map-" + std::to_string(map_index),
                         core::AutomationValue{std::move(nested)});
  }
  return event;
}

std::string trace_packet(const std::vector<core::AutomationEvent> &events) {
  std::vector<core::AutomationTraceEntry> entries;
  entries.reserve(events.size());
  for (std::size_t index{}; index < events.size(); ++index)
    entries.push_back(
        core::make_automation_root_trace_entry(events[index], index + 1U));
  return core::export_layerc_trace(entries);
}

std::vector<core::AutomationEvent>
parsed_events(const core::TraceControlReply &reply) {
  const auto parsed = core::parse_layerc_trace(reply.packet);
  const auto *events =
      std::get_if<std::vector<core::AutomationEvent>>(&parsed);
  if (!layerc::test::expect(events != nullptr,
                            "the delivered packet parses as a LayerC trace"))
    return {};
  return *events;
}

void fresh_subscribe_resets_once() {
  editor::TraceRelay relay;
  const auto id = relay.subscribe();
  layerc::test::expect(id != 0 && !relay.take_update(id).has_value(),
                       "a fresh subscription waits for LayerD's acknowledgement");
  relay.attempt_succeeded(id);
  const auto reset = relay.take_update(id);
  layerc::test::expect(reset.has_value() && reset->reply.has_value() &&
                           reset->error.empty() && reset->reply->reset &&
                           reset->reply->packet.empty() &&
                           reset->reply->generation == id,
                       "the first success produces one empty reset for its id");
  layerc::test::expect(!relay.take_update(id).has_value(),
                       "the reset is delivered only once");
}

void appended_roots_are_delivered_in_order() {
  editor::TraceRelay relay;
  const auto id = relay.subscribe();
  relay.attempt_succeeded(id);
  (void)relay.take_update(id);
  const std::vector<core::AutomationEvent> expected{
      make_event(1), make_event(2), make_event(3)};
  relay.append_packet(trace_packet(expected));
  const auto update = relay.take_update(id);
  if (!layerc::test::expect(update.has_value() && update->reply.has_value(),
                            "an appended packet yields a reply"))
    return;
  const auto &reply = *update->reply;
  const auto actual = parsed_events(reply);
  bool same = actual.size() == expected.size();
  for (std::size_t index{}; same && index < expected.size(); ++index) {
    same = actual[index].kind == expected[index].kind &&
           actual[index].timestamp == expected[index].timestamp &&
           actual[index].source_id == expected[index].source_id &&
           actual[index].origin_token == expected[index].origin_token &&
           actual[index].fields == expected[index].fields;
  }
  layerc::test::expect(same && reply.cursor == expected.size() &&
                           !reply.reset && !reply.gap,
                       "the packet round-trips the same ordered roots and cursor");
}

void overflow_keeps_the_newest_roots_and_reports_a_gap() {
  editor::TraceRelay relay;
  const auto id = relay.subscribe();
  relay.attempt_succeeded(id);
  (void)relay.take_update(id);

  std::vector<core::AutomationEvent> first;
  first.reserve(editor::TraceRelay::max_buffered_events);
  for (std::size_t index{};
       index < editor::TraceRelay::max_buffered_events; ++index)
    first.push_back(make_event(index));
  relay.append_packet(trace_packet(first));
  relay.append_packet(trace_packet({make_event(first.size())}));

  std::vector<std::string> delivered;
  bool first_reply = true;
  std::uint64_t cursor{};
  while (const auto update = relay.take_update(id)) {
    if (!layerc::test::expect(update->reply.has_value(),
                              "overflow returns a trace reply"))
      return;
    const auto &reply = *update->reply;
    if (first_reply)
      layerc::test::expect(reply.gap && !reply.reset,
                           "the dropped oldest root is reported as a gap");
    for (const auto &event : parsed_events(reply))
      delivered.push_back(event.source_id);
    cursor = reply.cursor;
    first_reply = false;
  }
  layerc::test::expect(delivered.size() == editor::TraceRelay::max_buffered_events &&
                           delivered.front() == "event-1" &&
                           delivered.back() ==
                               "event-" + std::to_string(
                                              editor::TraceRelay::max_buffered_events) &&
                           cursor == editor::TraceRelay::max_buffered_events,
                       "the relay keeps and delivers exactly the newest bounded roots");
}

void packet_budget_splits_large_roots_but_sends_one_oversized_root() {
  editor::TraceRelay relay;
  const auto id = relay.subscribe();
  relay.attempt_succeeded(id);
  (void)relay.take_update(id);
  const std::vector<core::AutomationEvent> large{
      make_large_event(1, 1), make_large_event(2, 1)};
  relay.append_packet(trace_packet(large));

  for (const auto expected_id : {"event-1", "event-2"}) {
    const auto update = relay.take_update(id);
    if (!layerc::test::expect(update.has_value() && update->reply.has_value(),
                              "a large root is returned in its own bounded take"))
      return;
    const auto &reply = *update->reply;
    const auto events = parsed_events(reply);
    layerc::test::expect(reply.packet.size() <= core::trace_control_packet_bytes &&
                             events.size() == 1 &&
                             events.front().source_id == expected_id,
                         "large roots split across takes within the packet budget");
  }
  layerc::test::expect(!relay.take_update(id).has_value(),
                       "all split roots have been delivered");

  const std::vector<core::AutomationEvent> oversized{make_large_event(3, 2)};
  const auto input = trace_packet(oversized);
  layerc::test::expect(input.size() > core::trace_control_packet_bytes,
                       "the single-root fixture exceeds the packet budget");
  relay.append_packet(input);
  const auto update = relay.take_update(id);
  if (!layerc::test::expect(update.has_value() && update->reply.has_value(),
                            "an oversized root is still returned alone"))
    return;
  const auto events = parsed_events(*update->reply);
  layerc::test::expect(events.size() == 1 &&
                           events.front().source_id == "event-3" &&
                           update->reply->cursor == 3,
                       "a single oversized root is not dropped or split");
}

void reconnect_reports_a_gap_not_a_reset() {
  editor::TraceRelay relay;
  const auto id = relay.subscribe();
  relay.attempt_succeeded(id);
  (void)relay.take_update(id);
  relay.connection_lost();
  layerc::test::expect(relay.begin_attempt(id),
                       "a lapsed subscription can be retried");
  relay.attempt_succeeded(id);
  const auto update = relay.take_update(id);
  layerc::test::expect(update.has_value() && update->reply.has_value() &&
                           update->reply->gap && !update->reply->reset,
                       "reconnection reports the trace gap without a fresh reset");
}

void three_failures_end_the_subscription_with_the_last_reason() {
  editor::TraceRelay relay;
  const auto id = relay.subscribe();
  relay.attempt_failed(id, "first failure");
  relay.attempt_failed(id, "second failure");
  layerc::test::expect(!relay.take_update(id).has_value(),
                       "two failures do not end the subscription");
  layerc::test::expect(relay.begin_attempt(id),
                       "the two-failure subscription remains retryable");
  relay.attempt_succeeded(id);
  const auto reset = relay.take_update(id);
  layerc::test::expect(reset && reset->reply && reset->reply->reset,
                       "success after two failures resets the trace");

  const auto failing_id = relay.subscribe();
  relay.attempt_failed(failing_id, "failure one");
  relay.attempt_failed(failing_id, "failure two");
  relay.attempt_failed(failing_id, "last failure");
  const auto failure = relay.take_update(failing_id);
  layerc::test::expect(failure.has_value() && !failure->reply.has_value() &&
                           failure->error == "last failure",
                       "the third consecutive failure returns its reason");
  layerc::test::expect(!relay.take_update(failing_id).has_value() &&
                           !relay.begin_attempt(failing_id),
                       "a failed subscription cannot be taken or retried again");
}

void invalid_packet_fails_the_subscription() {
  editor::TraceRelay relay;
  const auto id = relay.subscribe();
  relay.attempt_succeeded(id);
  (void)relay.take_update(id);
  relay.append_packet("not a LayerC trace");
  const auto update = relay.take_update(id);
  layerc::test::expect(update.has_value() && !update->reply.has_value() &&
                           update->error ==
                               "LayerD returned an invalid trace packet.",
                       "malformed input returns the defined trace packet error");
}

void newer_subscription_replaces_the_old_and_ignores_early_packets() {
  editor::TraceRelay relay;
  const std::vector<core::AutomationEvent> events{make_event(7)};
  relay.append_packet(trace_packet(events));
  const auto old_id = relay.subscribe();
  const auto id = relay.subscribe();
  layerc::test::expect(id != old_id &&
                           !relay.take_update(old_id).has_value() &&
                           !relay.begin_attempt(old_id) &&
                           !relay.unsubscribe(old_id),
                       "the replaced id cannot take, retry, or unsubscribe");
  relay.append_packet(trace_packet(events));
  relay.attempt_succeeded(id);
  const auto reset = relay.take_update(id);
  layerc::test::expect(reset.has_value() && reset->reply.has_value() &&
                           reset->reply->reset && reset->reply->packet.empty(),
                       "packets before a held subscription are ignored");
}

void late_acknowledgement_of_a_replaced_subscription_can_be_cleaned_up() {
  editor::TraceRelay relay;
  const auto previous = relay.subscribe();
  const auto current = relay.subscribe();
  relay.attempt_succeeded(previous);
  layerc::test::expect(!relay.take_update(current),
                       "a superseded acknowledgement does not start the new trace");
  for (int attempt = 0; attempt < editor::TraceRelay::max_consecutive_failures;
       ++attempt) {
    if (attempt != 0) (void)relay.begin_attempt(current);
    relay.attempt_failed(current, "subscription refused");
  }
  layerc::test::expect(relay.unsubscribe(current),
                       "stopping the failed replacement cleans up the older wire subscription");
}

void unsubscribe_reports_whether_layerd_may_still_hold_it() {
  editor::TraceRelay relay;
  const auto attempting = relay.subscribe();
  layerc::test::expect(relay.unsubscribe(attempting),
                       "unsubscribe during an attempt tells LayerD to stop");

  const auto held = relay.subscribe();
  relay.attempt_succeeded(held);
  (void)relay.take_update(held);
  layerc::test::expect(relay.unsubscribe(held),
                       "unsubscribe while held tells LayerD to stop");
  relay.unsubscribe_succeeded();
  const auto refused = relay.subscribe();
  relay.attempt_failed(refused, "subscription refused");
  layerc::test::expect(!relay.unsubscribe(refused),
                       "an acknowledged stop left no wire subscription to clean up");

  const auto lapsed = relay.subscribe();
  relay.attempt_succeeded(lapsed);
  (void)relay.take_update(lapsed);
  relay.connection_lost();
  layerc::test::expect(!relay.unsubscribe(lapsed),
                       "unsubscribe after connection loss needs no pipe message");
}

} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(fresh_subscribe_resets_once),
    LAYERC_TEST_CASE(appended_roots_are_delivered_in_order),
    LAYERC_TEST_CASE(overflow_keeps_the_newest_roots_and_reports_a_gap),
    LAYERC_TEST_CASE(packet_budget_splits_large_roots_but_sends_one_oversized_root),
    LAYERC_TEST_CASE(reconnect_reports_a_gap_not_a_reset),
    LAYERC_TEST_CASE(three_failures_end_the_subscription_with_the_last_reason),
    LAYERC_TEST_CASE(invalid_packet_fails_the_subscription),
    LAYERC_TEST_CASE(newer_subscription_replaces_the_old_and_ignores_early_packets),
    LAYERC_TEST_CASE(late_acknowledgement_of_a_replaced_subscription_can_be_cleaned_up),
    LAYERC_TEST_CASE(unsubscribe_reports_whether_layerd_may_still_hold_it))

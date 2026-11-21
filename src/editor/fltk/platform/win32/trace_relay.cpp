#include "platform/win32/trace_relay.hpp"

#include "layerc/core/automation_trace_control.hpp"

#include <utility>
#include <variant>
#include <vector>

namespace layerc::editor {
namespace core = layerc::core;

void TraceRelay::clear_subscription() noexcept {
  failures_ = 0;
  attempting_ = false;
  subscribed_ = false;
  lapsed_ = false;
  reset_pending_ = false;
  gap_pending_ = false;
  finished_ = false;
  error_.reset();
  events_.clear();
  delivered_ = 0;
}

std::uint64_t TraceRelay::subscribe() {
  clear_subscription();
  subscriber_ = ++next_id_;
  attempting_ = true;
  return subscriber_;
}

bool TraceRelay::unsubscribe(const std::uint64_t id) noexcept {
  if (!current(id)) return false;
  const bool told = held_ || attempting_;
  clear_subscription();
  subscriber_ = 0;
  return told;
}

bool TraceRelay::begin_attempt(const std::uint64_t id) noexcept {
  if (!current(id) || attempting_ || subscribed_ || finished_) return false;
  attempting_ = true;
  return true;
}

void TraceRelay::attempt_succeeded(const std::uint64_t id) noexcept {
  // Even a superseded acknowledgement confirms a live subscription on the
  // pipe. The current id must be able to stop it if its own attempt fails.
  held_ = true;
  if (!current(id) || finished_) return;
  attempting_ = false;
  subscribed_ = true;
  failures_ = 0;
  // LayerD starts each subscription from an empty trace. The first one
  // starts a fresh log; a resubscription keeps it and reports the gap.
  if (std::exchange(lapsed_, false))
    gap_pending_ = true;
  else
    reset_pending_ = true;
}

void TraceRelay::attempt_failed(const std::uint64_t id, std::string reason) {
  if (!current(id) || finished_) return;
  attempting_ = false;
  if (++failures_ >= max_consecutive_failures) fail(std::move(reason));
}

void TraceRelay::unsubscribe_succeeded() noexcept { held_ = false; }

void TraceRelay::connection_lost() noexcept {
  held_ = false;
  if (!subscribed_) return;
  subscribed_ = false;
  lapsed_ = true;
}

void TraceRelay::fail(std::string reason) {
  if (subscriber_ == 0 || finished_) return;
  finished_ = true;
  attempting_ = false;
  subscribed_ = false;
  events_.clear();
  error_ = std::move(reason);
}

void TraceRelay::append_packet(const std::string_view packet) {
  if (!subscribed_ || finished_) return;
  auto parsed = core::parse_layerc_trace(packet);
  auto *events = std::get_if<std::vector<core::AutomationEvent>>(&parsed);
  if (events == nullptr) {
    fail("LayerD returned an invalid trace packet.");
    return;
  }
  for (auto &event : *events) events_.push_back(std::move(event));
  while (events_.size() > max_buffered_events) {
    events_.pop_front();
    gap_pending_ = true;
  }
}

std::optional<AutomationTraceUpdate>
TraceRelay::take_update(const std::uint64_t id) {
  if (!current(id)) return std::nullopt;
  if (error_) {
    AutomationTraceUpdate update;
    update.error = std::move(*error_);
    error_.reset();
    return update;
  }
  if (finished_ || (!reset_pending_ && !gap_pending_ && events_.empty()))
    return std::nullopt;

  core::TraceControlReply reply;
  reply.generation = subscriber_;
  reply.reset = std::exchange(reset_pending_, false);
  reply.gap = std::exchange(gap_pending_, false);
  // One reply stays within the control packet budget, taking at least one
  // root; the rest wait for the next poll.
  std::vector<core::AutomationTraceEntry> entries;
  std::string encoded;
  std::size_t bytes = 64;
  while (!events_.empty()) {
    auto entry =
        core::make_automation_root_trace_entry(events_.front(), delivered_ + 1U);
    encoded.clear();
    core::append_layerc_trace_entry(encoded, entry);
    if (!entries.empty() &&
        bytes + encoded.size() + 1U > core::trace_control_packet_bytes)
      break;
    bytes += encoded.size() + 1U;
    entries.push_back(std::move(entry));
    events_.pop_front();
    ++delivered_;
  }
  if (!entries.empty()) reply.packet = core::export_layerc_trace(entries);
  reply.cursor = delivered_;
  AutomationTraceUpdate update;
  update.reply = std::move(reply);
  return update;
}

} // namespace layerc::editor

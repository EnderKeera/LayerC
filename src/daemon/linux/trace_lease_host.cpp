#include "trace_lease_host.hpp"

#include <algorithm>

namespace layerc::daemon {
namespace core = layerc::core;

bool TraceLeaseHost::demanded(const std::map<std::string, Lease, std::less<>> &leases,
                              const std::string &profile_id) {
  return std::ranges::any_of(leases, [&](const auto &item) {
    return item.second.profile_id == profile_id;
  });
}

void TraceLeaseHost::reconcile(const std::span<InputSession> sessions) {
  for (auto &session : sessions) {
    if (auto *runtime = session.automation.runtime()) {
      runtime->set_trace_enabled(demanded(leases_, session.automation.profile().id));
    }
  }
}

bool TraceLeaseHost::expire(const core::TimePoint now, const std::span<InputSession> sessions) {
  std::erase_if(leases_, [now](const auto &item) { return item.second.expires <= now; });
  bool changed = false;
  for (auto &session : sessions) {
    if (auto *runtime = session.automation.runtime()) {
      const bool wanted = demanded(leases_, session.automation.profile().id);
      changed = changed || runtime->trace_enabled() != wanted;
    }
  }
  reconcile(sessions);
  return changed;
}

std::optional<core::TimePoint> TraceLeaseHost::next_deadline() const noexcept {
  std::optional<core::TimePoint> next;
  for (const auto &[client, lease] : leases_) {
    (void)client;
    next = core::earliest_deadline(next, lease.expires);
  }
  return next;
}

TraceControlResult TraceLeaseHost::handle(const core::TraceControlRequest &request,
                                          const std::span<InputSession> sessions,
                                          const core::TimePoint now) {
  if (!core::trace_control_request_valid(request))
    return core::trace_control_failure(core::TraceControlError::invalid_request);
  (void)expire(now, sessions);
  auto lease = leases_.find(request.client_id);
  if (lease != leases_.end() && lease->second.profile_id != request.profile_id)
    return core::trace_control_failure(core::TraceControlError::client_conflict);
  if (request.operation == core::TraceControlOperation::unsubscribe) {
    if (lease != leases_.end()) leases_.erase(lease);
    (void)expire(now, sessions);
    return core::TraceControlReply{};
  }
  const auto session = std::ranges::find_if(sessions, [&](const auto &item) {
    return item.automation.profile().id == request.profile_id && item.automation.runtime();
  });
  if (session == sessions.end())
    return core::trace_control_failure(core::TraceControlError::unavailable);
  const auto expires = core::automation_time_after(now, core::trace_control_lease);
  if (!expires) return core::trace_control_failure(core::TraceControlError::clock_limit);
  auto &runtime = *session->automation.runtime();
  if (request.operation == core::TraceControlOperation::subscribe) {
    if (lease == leases_.end() && leases_.size() >= core::trace_control_client_limit)
      return core::trace_control_failure(core::TraceControlError::subscriber_limit);
    const auto cursor = runtime.trace().empty() ? 0U : runtime.trace().back().sequence;
    leases_[request.client_id] = {request.profile_id,
        *expires, session->trace_generation, cursor, cursor};
    (void)expire(now, sessions);
    return core::TraceControlReply{session->trace_generation, cursor, true, false, {}};
  }
  if (lease == leases_.end())
    return core::trace_control_failure(core::TraceControlError::expired);
  auto &subscriber = lease->second;
  if (request.generation != session->trace_generation) {
    subscriber.expires = *expires;
    subscriber.generation = session->trace_generation;
    subscriber.cursor = subscriber.acknowledged = 0;
    return core::TraceControlReply{session->trace_generation, 0, true, false, {}};
  }
  // The last delivered cursor, or the previous one when that reply was lost
  // (the client retries); an acknowledged batch is never replayed.
  if (request.generation != subscriber.generation ||
      (request.cursor != subscriber.cursor && request.cursor != subscriber.acknowledged))
    return core::trace_control_failure(core::TraceControlError::invalid_cursor);
  subscriber.acknowledged = request.cursor;
  subscriber.expires = *expires;
  const auto &entries = runtime.trace();
  const auto begin = std::ranges::find_if(entries, [&](const auto &entry) {
    return entry.sequence > request.cursor;
  });
  const auto count = std::min(static_cast<std::size_t>(entries.end() - begin),
                              core::trace_control_batch_entries);
  core::TraceControlReply reply{session->trace_generation, request.cursor, false, false, {}};
  if (count == 0) return reply;
  reply.gap = request.cursor != 0 && begin->sequence > request.cursor + 1;
  // At most 32 entries per request; shrink the prefix if a typed root carries
  // a large value. The JSON string envelope remains below the control limit.
  auto accepted = count;
  do {
    reply.packet = core::export_layerc_trace({&*begin, accepted});
    if (reply.packet.size() <= core::trace_control_packet_bytes) break;
    accepted /= 2;
  } while (accepted != 0);
  if (accepted == 0) {
    // One oversized entry must not stall the subscription: skip it as a gap.
    reply.packet.clear();
    reply.gap = true;
    reply.cursor = subscriber.cursor = begin->sequence;
    return reply;
  }
  reply.cursor = (begin + static_cast<std::ptrdiff_t>(accepted - 1))->sequence;
  subscriber.cursor = reply.cursor;
  return reply;
}

} // namespace layerc::daemon

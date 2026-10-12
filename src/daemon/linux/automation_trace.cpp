#include "input_service.hpp"

#include <algorithm>

namespace layerc::daemon {
namespace core = layerc::core;

void X11SessionRegistry::reconcile_trace(const core::TimePoint now) {
  std::erase_if(trace_leases_, [now](const auto &item) { return item.second.expires <= now; });
  for (auto &session : sessions_) {
    if (auto *runtime = session.automation.runtime()) {
      const bool demanded = std::ranges::any_of(trace_leases_, [&](const auto &item) {
        return item.second.profile_id == session.automation.profile().id;
      });
      runtime->set_trace_enabled(demanded);
    }
  }
}

TraceControlResult X11SessionRegistry::trace_request(
    const core::TraceControlRequest &request, const core::TimePoint now) {
  if (!core::trace_control_request_valid(request)) return std::string("invalid trace request");
  reconcile_trace(now);
  auto lease = trace_leases_.find(request.client_id);
  if (lease != trace_leases_.end() && lease->second.profile_id != request.profile_id)
    return std::string("trace client belongs to another profile");
  if (request.operation == core::TraceControlOperation::unsubscribe) {
    if (lease != trace_leases_.end()) trace_leases_.erase(lease);
    reconcile_trace(now);
    return core::TraceControlReply{};
  }
  const auto session = std::ranges::find_if(sessions_, [&](const auto &item) {
    return item.automation.profile().id == request.profile_id && item.automation.runtime();
  });
  if (session == sessions_.end())
    return std::string("automation trace unavailable: no matched, enabled automation session");
  const auto expires = core::automation_time_after(now, core::trace_control_lease);
  if (!expires) return std::string("automation trace lease clock limit exceeded");
  auto &runtime = *session->automation.runtime();
  if (request.operation == core::TraceControlOperation::subscribe) {
    if (lease == trace_leases_.end() && trace_leases_.size() >= core::trace_control_client_limit)
      return std::string("automation trace subscriber limit reached");
    const auto cursor = runtime.trace().empty() ? 0U : runtime.trace().back().sequence;
    trace_leases_[request.client_id] = {request.profile_id,
        *expires, session->trace_generation, cursor, cursor};
    reconcile_trace(now);
    return core::TraceControlReply{session->trace_generation, cursor, true, false, {}};
  }
  if (lease == trace_leases_.end()) return std::string("automation trace subscription expired");
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
    return std::string("invalid automation trace cursor");
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

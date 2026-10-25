#include "automation_trace_client.hpp"
#include "control_channel.hpp"
#include "lease_worker.hpp"

#include <utility>

namespace layerc::editor {
namespace core = layerc::core;
namespace {

struct TraceExchange {
  AutomationTraceUpdate update;
  std::optional<core::TraceControlError> code;
};

TraceExchange exchange(const core::TraceControlRequest &request) {
  const auto call = platform::control_call(
      platform::ControlCommand::trace, core::serialize_trace_control_request(request),
      std::chrono::milliseconds(300));
  if (!call.ok) {
    std::optional<core::TraceControlError> code;
    if (!call.code.empty())
      code = core::parse_enum_wire(core::trace_control_error_wire, call.code);
    return {.update = {.reply = std::nullopt, .error = call.error}, .code = code};
  }
  auto reply = call.payload ? core::parse_trace_control_reply(*call.payload) : std::nullopt;
  if (!reply)
    return {.update = {.reply = std::nullopt,
                       .error = "LayerD returned an invalid trace payload"},
            .code = std::nullopt};
  return {.update = {.reply = std::move(reply), .error = {}}, .code = std::nullopt};
}

} // namespace

struct AutomationTraceClient::Impl {
  core::TraceControlRequest request;
  std::optional<AutomationTraceUpdate> update;
  bool pending{true}, busy{}, finished{};
  // Worker-only state.
  static constexpr int max_consecutive_failures = 3;
  int failures{};
  bool resubscribed{};
  std::uint64_t lapsed_generation{};
  LeaseWorker worker;

  explicit Impl(std::string profile_id)
      : request{core::TraceControlOperation::subscribe, std::move(profile_id),
                make_lease_client_id(), 0, 0} {
    worker.start([this] { run(); });
  }
  ~Impl() = default; // the worker is declared last and joins first
  void run() noexcept {
    try {
      for (;;) {
        if (!worker.wait([&] { return pending; }, std::nullopt, [&] {
              pending = false;
              busy = true;
            }))
          break;
        auto result = exchange(request);
        auto &received = result.update;
        if (!received.reply) {
          // A lapsed lease or cursor resubscribes and reports a gap; other
          // faults retry on the next UI poll before they end the session.
          const bool lapsed = request.operation == core::TraceControlOperation::poll &&
                              result.code && core::trace_control_lapsed(*result.code);
          if (lapsed) {
            lapsed_generation = request.generation;
            request = {core::TraceControlOperation::subscribe, request.profile_id,
                       request.client_id, 0, 0};
            resubscribed = true;
          }
          if (lapsed || ++failures < max_consecutive_failures) {
            (void)worker.submit([&] { busy = false; });
            continue;
          }
        } else {
          failures = 0;
          request.operation = core::TraceControlOperation::poll;
          request.generation = received.reply->generation;
          request.cursor = received.reply->cursor;
          // Same graph generation: keep collected roots and report the gap;
          // a replaced graph still resets.
          if (std::exchange(resubscribed, false) &&
              received.reply->generation == lapsed_generation) {
            received.reply->reset = false;
            received.reply->gap = true;
          }
        }
        const bool failed = !received.reply;
        if (!worker.submit([&] {
              busy = false;
              finished = failed;
              update = std::move(received);
            }))
          break;
        if (failed) break;
      }
      request.operation = core::TraceControlOperation::unsubscribe;
      (void)exchange(request); // lease expiry is the disconnected cleanup path
    } catch (...) {
      (void)worker.submit([&] {
        busy = false;
        finished = true;
        update = AutomationTraceUpdate{std::nullopt, "Automation trace client failed"};
      });
    }
  }
};

AutomationTraceClient::AutomationTraceClient(std::string profile_id)
    : impl_(std::make_unique<Impl>(std::move(profile_id))) {}
AutomationTraceClient::~AutomationTraceClient() { impl_->worker.stop(); }
void AutomationTraceClient::request_poll() {
  (void)impl_->worker.submit([&] {
    if (impl_->finished || impl_->busy || impl_->update) return;
    impl_->pending = true;
  });
}
std::optional<AutomationTraceUpdate> AutomationTraceClient::take_update() {
  return impl_->worker.locked([&] { return std::exchange(impl_->update, std::nullopt); });
}
} // namespace layerc::editor

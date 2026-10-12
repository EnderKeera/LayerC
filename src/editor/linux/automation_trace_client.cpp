#include "automation_trace_client.hpp"
#include "control_channel.hpp"
#include "layerc/core/json.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <unistd.h>

namespace layerc::editor {
namespace core = layerc::core;
namespace {
AutomationTraceUpdate exchange(const core::TraceControlRequest &request) {
  std::string error;
  const auto response = platform::control_roundtrip(
      platform::encode_control_request(platform::ControlCommand::trace,
          core::serialize_trace_control_request(request)),
      error, std::chrono::milliseconds(300));
  if (!response) return {.reply = std::nullopt, .error = std::move(error)};
  const auto tree = core::json::parse(*response);
  const auto *value = std::get_if<core::json::Value>(&tree);
  const auto *payload = value ? value->find("payload") : nullptr;
  std::string text;
  if (payload) core::json::append_value(text, *payload);
  auto reply = core::parse_trace_control_reply(text);
  if (!reply) return {.reply = std::nullopt, .error = "LayerD returned an invalid trace payload"};
  return {.reply = std::move(reply), .error = {}};
}
} // namespace

struct AutomationTraceClient::Impl {
  core::TraceControlRequest request;
  std::mutex mutex;
  std::condition_variable condition;
  std::optional<AutomationTraceUpdate> update;
  bool pending{true}, busy{}, stopping{}, finished{};
  // Worker-only state.
  static constexpr int max_consecutive_failures = 3;
  int failures{};
  bool resubscribed{};
  std::uint64_t lapsed_generation{};
  std::thread worker;

  explicit Impl(std::string profile_id)
      : request{core::TraceControlOperation::subscribe, std::move(profile_id),
          std::to_string(::getpid()) + "-" + std::to_string(
              core::MonotonicClock::now().time_since_epoch().count()), 0, 0},
        worker([this] { run(); }) {}
  ~Impl() {
    { const std::lock_guard lock(mutex); stopping = true; }
    condition.notify_one();
    if (worker.joinable()) worker.join(); // at most two 300 ms roundtrips
  }
  void run() noexcept {
    try {
      for (;;) {
        {
          std::unique_lock lock(mutex);
          condition.wait(lock, [&] { return pending || stopping; });
          if (stopping) break;
          pending = false; busy = true;
        }
        auto received = exchange(request);
        if (!received.reply) {
          // A lapsed lease or cursor resubscribes and reports a gap; other
          // faults retry on the next UI poll before they end the session.
          const bool lapsed = request.operation == core::TraceControlOperation::poll &&
              (received.error.find("subscription expired") != std::string::npos ||
               received.error.find("invalid automation trace cursor") != std::string::npos);
          if (lapsed) {
            lapsed_generation = request.generation;
            request = {core::TraceControlOperation::subscribe, request.profile_id,
                       request.client_id, 0, 0};
            resubscribed = true;
          }
          if (lapsed || ++failures < max_consecutive_failures) {
            const std::lock_guard lock(mutex);
            busy = false;
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
        {
          const std::lock_guard lock(mutex);
          busy = false; finished = failed;
          update = std::move(received);
        }
        if (failed) break;
      }
      request.operation = core::TraceControlOperation::unsubscribe;
      (void)exchange(request); // lease expiry is the disconnected cleanup path
    } catch (...) {
      const std::lock_guard lock(mutex);
      busy = false; finished = true;
      update = AutomationTraceUpdate{std::nullopt, "Automation trace client failed"};
    }
  }
};
AutomationTraceClient::AutomationTraceClient(std::string profile_id)
    : impl_(std::make_unique<Impl>(std::move(profile_id))) {}
AutomationTraceClient::~AutomationTraceClient() = default;
void AutomationTraceClient::request_poll() {
  {
    const std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->finished || impl_->busy || impl_->update) return;
    impl_->pending = true;
  }
  impl_->condition.notify_one();
}
std::optional<AutomationTraceUpdate> AutomationTraceClient::take_update() {
  const std::lock_guard lock(impl_->mutex);
  return std::exchange(impl_->update, std::nullopt);
}
} // namespace layerc::editor

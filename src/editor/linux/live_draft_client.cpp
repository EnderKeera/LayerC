#include "live_draft_client.hpp"
#include "control_channel.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/json_reader.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <sys/eventfd.h>
#include <unistd.h>

namespace layerc::editor {
namespace core = layerc::core;
namespace {
LiveDraftUpdate exchange(const core::LiveDraftRequest &request) {
  std::string error;
  const auto response = platform::control_roundtrip(
      platform::encode_control_request(platform::ControlCommand::draft,
          core::serialize_live_draft_request(request)),
      error, std::chrono::milliseconds(300));
  if (!response) return {.reply = std::nullopt, .error = std::move(error)};
  const auto tree = core::json::parse_bounded(*response, platform::control_frame_limit);
  const auto *value = std::get_if<core::json::Value>(&tree);
  const auto *payload = value ? value->find("payload") : nullptr;
  std::string text;
  if (payload) core::json::append_value(text, *payload);
  auto reply = core::parse_live_draft_reply(text);
  const bool state_ok = reply && (request.operation == core::LiveDraftOperation::end
      ? reply->state == core::LiveDraftState::ended
      : request.operation == core::LiveDraftOperation::renew
          ? reply->state == core::LiveDraftState::renewed
          : reply->state == core::LiveDraftState::applied ||
            reply->state == core::LiveDraftState::duplicate);
  if (!state_ok || (request.operation != core::LiveDraftOperation::end &&
                    reply->revision != request.revision))
    return {.reply = std::nullopt, .error = "LayerD returned an invalid live draft acknowledgement"};
  return {.reply = std::move(reply), .error = {}};
}

// Owns the notification descriptor, so a throwing constructor cannot leak it.
struct EventFd {
  int fd;
  EventFd() : fd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (fd < 0) throw std::runtime_error("cannot create the live draft notification");
  }
  ~EventFd() { ::close(fd); }
  EventFd(const EventFd &) = delete;
  EventFd &operator=(const EventFd &) = delete;
};

struct Publish {
  std::string profile_id;
  std::string profile_json;
  std::uint64_t sequence{};
};
} // namespace

struct LiveDraftClient::Impl {
  // Declared first: it outlives the worker that signals it.
  EventFd notify;
  std::string client_id{std::to_string(::getpid()) + "-" + std::to_string(
      core::MonotonicClock::now().time_since_epoch().count())};
  std::mutex mutex;
  std::condition_variable condition;
  std::optional<Publish> pending;
  std::uint64_t submitted{};
  bool end_requested{};
  std::optional<LiveDraftUpdate> update;
  bool stopping{};
  std::thread worker{[this] { run(); }};

  ~Impl() {
    { const std::lock_guard lock(mutex); stopping = true; }
    condition.notify_one();
    if (worker.joinable()) worker.join(); // at most two 300 ms roundtrips
  }
  void signal() const noexcept {
    const std::uint64_t one = 1;
    (void)!::write(notify.fd, &one, sizeof one); // a saturated counter is still readable
  }
  void run() noexcept {
    // Worker-owned: the lease this client holds, if any. The revision stays
    // monotonic for the client across profiles and ends.
    core::LiveDraftRequest lease{core::LiveDraftOperation::publish, {}, client_id, 0, {}};
    bool leased = false;
    std::uint64_t lease_sequence{};
    const auto end_lease = [&] {
      if (lease.profile_id.empty()) return;
      lease.operation = core::LiveDraftOperation::end;
      lease.profile_json.clear();
      (void)exchange(lease); // expiry covers a lost End
      lease.profile_id.clear();
      leased = false;
    };
    auto renew_at = core::MonotonicClock::now();
    try {
      for (;;) {
        std::optional<Publish> next;
        bool end_now = false;
        {
          std::unique_lock lock(mutex);
          const auto ready = [&] { return pending.has_value() || end_requested || stopping; };
          if (leased) condition.wait_until(lock, renew_at, ready);
          else condition.wait(lock, ready);
          if (stopping) break;
          end_now = std::exchange(end_requested, false);
          next = std::exchange(pending, std::nullopt);
        }
        // One daemon draft at a time: end the old lease before another
        // profile's publish, and before any publish after end().
        if (end_now || (next && next->profile_id != lease.profile_id)) {
          end_lease();
          // Shutdown during that End must not start another publish.
          const std::lock_guard lock(mutex);
          if (stopping) break;
        }
        if (!next && !leased) continue;
        if (next) {
          lease.operation = core::LiveDraftOperation::publish;
          lease.profile_id = std::move(next->profile_id);
          lease.profile_json = std::move(next->profile_json);
          lease_sequence = next->sequence;
          ++lease.revision;
        } else {
          lease.operation = core::LiveDraftOperation::renew;
          lease.profile_json.clear();
        }
        auto received = exchange(lease);
        received.sequence = lease_sequence;
        leased = received.reply.has_value();
        renew_at = core::MonotonicClock::now() + std::chrono::seconds(1);
        // A successful renewal changes nothing visible: keep it observable
        // without waking the UI once per second.
        const bool visible = !leased || lease.operation == core::LiveDraftOperation::publish;
        {
          const std::lock_guard lock(mutex);
          update = std::move(received);
        }
        if (visible) signal();
        // A failure blocks until another edit, instead of spinning or silently
        // weakening ownership. The existing lease then expires in the daemon.
      }
      end_lease();
    } catch (...) {
      {
        const std::lock_guard lock(mutex);
        update = LiveDraftUpdate{std::nullopt, "Live draft client failed", submitted};
      }
      signal();
    }
  }
};
LiveDraftClient::LiveDraftClient() : impl_(std::make_unique<Impl>()) {}
LiveDraftClient::~LiveDraftClient() = default;
std::uint64_t LiveDraftClient::publish(std::string profile_id, std::string profile_json) {
  std::uint64_t sequence{};
  {
    const std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) return 0;
    sequence = ++impl_->submitted;
    impl_->pending = Publish{std::move(profile_id), std::move(profile_json), sequence};
  }
  impl_->condition.notify_one();
  return sequence;
}
void LiveDraftClient::end() {
  {
    const std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) return;
    impl_->pending.reset();
    impl_->end_requested = true;
  }
  impl_->condition.notify_one();
}
std::optional<LiveDraftUpdate> LiveDraftClient::take_update() {
  std::uint64_t count{};
  (void)!::read(impl_->notify.fd, &count, sizeof count); // nonblocking drain
  const std::lock_guard lock(impl_->mutex);
  return std::exchange(impl_->update, std::nullopt);
}
int LiveDraftClient::notify_fd() const noexcept { return impl_->notify.fd; }
} // namespace layerc::editor

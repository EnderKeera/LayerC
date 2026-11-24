#include "live_draft_client.hpp"
#include "control_channel.hpp"
#include "lease_worker.hpp"
#include "layerc/core/json_reader.hpp"

#include <cstdint>
#include <utility>
#include <sys/eventfd.h>
#include <unistd.h>

namespace layerc::editor {
namespace core = layerc::core;
namespace {
LiveDraftUpdate exchange(const core::LiveDraftRequest &request) {
  const auto call = platform::control_call(
      platform::ControlCommand::draft, core::serialize_live_draft_request(request),
      std::chrono::milliseconds(300));
  if (!call.ok) return {.reply = std::nullopt, .error = call.error};
  auto reply = call.payload ? core::parse_live_draft_reply(*call.payload) : std::nullopt;
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
  int fd{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
  EventFd() = default;
  ~EventFd() { if (fd >= 0) ::close(fd); }
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
  std::string client_id{make_lease_client_id()};
  std::optional<Publish> pending;
  std::uint64_t submitted{};
  bool end_requested{};
  std::optional<LiveDraftUpdate> update;
  // Declared last: it joins before the state it guards.
  LeaseWorker worker;

  Impl() {
    if (notify.fd < 0) worker.stop();
    else worker.start([this] { run(); });
  }
  ~Impl() = default;
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
        if (!worker.wait(
                [&] { return pending.has_value() || end_requested; },
                leased ? std::optional<core::TimePoint>{renew_at} : std::nullopt,
                [&] {
                  next = std::exchange(pending, std::nullopt);
                  end_now = std::exchange(end_requested, false);
                }))
          break;
        // One daemon draft at a time: end the old lease before another
        // profile's publish, and before any publish after end().
        if (end_now || (next && next->profile_id != lease.profile_id)) {
          end_lease();
          // Shutdown during that End must not start another publish.
          if (!worker.submit([] {})) break;
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
        if (!worker.submit([&] { update = std::move(received); })) break;
        if (visible) signal();
        // A failure blocks until another edit, instead of spinning or silently
        // weakening ownership. The existing lease then expires in the daemon.
      }
      end_lease();
    } catch (...) {
      (void)worker.submit([&] {
        update = LiveDraftUpdate{std::nullopt, "Live draft client failed", submitted};
      });
      signal();
    }
  }
};
LiveDraftClient::LiveDraftClient() : impl_(std::make_unique<Impl>()) {}
LiveDraftClient::~LiveDraftClient() = default;
std::uint64_t LiveDraftClient::publish(std::string profile_id, std::string profile_json) {
  std::uint64_t sequence{};
  const bool accepted = impl_->worker.submit([&] {
    sequence = ++impl_->submitted;
    impl_->pending = Publish{std::move(profile_id), std::move(profile_json), sequence};
  });
  return accepted ? sequence : 0;
}
void LiveDraftClient::end() {
  (void)impl_->worker.submit([&] {
    impl_->pending.reset();
    impl_->end_requested = true;
  });
}
std::optional<LiveDraftUpdate> LiveDraftClient::take_update() {
  if (impl_->notify.fd < 0) return std::nullopt;
  std::uint64_t count{};
  (void)!::read(impl_->notify.fd, &count, sizeof count); // nonblocking drain
  return impl_->worker.locked([&] { return std::exchange(impl_->update, std::nullopt); });
}
int LiveDraftClient::notify_fd() const noexcept { return impl_->notify.fd; }
} // namespace layerc::editor

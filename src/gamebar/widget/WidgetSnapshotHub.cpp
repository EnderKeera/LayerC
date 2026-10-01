#include "pch.h"

#include "WidgetSnapshotHub.hpp"
#include "WidgetServiceSession.hpp"

namespace layerc::gamebar::widget {
namespace {

using Connection =
    winrt::Windows::ApplicationModel::AppService::AppServiceConnection;
using Task = winrt::Windows::ApplicationModel::Background::IBackgroundTaskInstance;
using RequestArgs = winrt::Windows::ApplicationModel::AppService::
    AppServiceRequestReceivedEventArgs;

struct ServiceRevocations final {
  Connection::RequestReceived_revoker request;
  Connection::ServiceClosed_revoker closed;
  Task::Canceled_revoker canceled;
};

struct ServiceResources final {
  using Revocations = ServiceRevocations;

  ServiceResources(Task task_instance, Connection app_service)
      : task(std::move(task_instance)), connection(std::move(app_service)),
        deferral(task.GetDeferral()) {}
  ~ServiceResources() {
    revocations.reset();
    try {
      connection.Close();
    } catch (...) {
    }
    try {
      deferral.Complete();
    } catch (...) {
    }
  }
  Task task;
  Connection connection;
  winrt::Windows::ApplicationModel::Background::BackgroundTaskDeferral deferral;
  std::unique_ptr<Revocations> revocations;
};

struct RequestDeferral final {
  winrt::Windows::ApplicationModel::AppService::AppServiceDeferral deferral;
  ~RequestDeferral() {
    try {
      deferral.Complete();
    } catch (...) {
    }
  }
};

} // namespace

// A task, connection, deferral and all three revocations have one owner. Close
// consumes that owner once; registrations racing with close are revoked locally.
class AppServiceSession final
    : public std::enable_shared_from_this<AppServiceSession> {
public:
  AppServiceSession(Task task, Connection connection)
      : resources_(std::make_shared<ServiceResources>(std::move(task),
                                                      std::move(connection))) {}
  ~AppServiceSession() { close(); }

  void start() {
    // Retain the entire owner, including its task deferral, until all event
    // registrations finish, even if close/replacement wins in the meantime.
    const auto resources = resources_.resources();
    if (!resources)
      return;
    const auto weak = weak_from_this();
    auto revocations = std::make_unique<ServiceRevocations>();
    revocations->request = resources->connection.RequestReceived(
        winrt::auto_revoke, [weak](const auto &, const RequestArgs &args) {
          if (const auto session = weak.lock())
            receive(session, args);
        });
    const auto terminal = [weak](const auto &, const auto &) {
      if (const auto session = weak.lock())
        SnapshotHub::instance().disconnect(session);
    };
    revocations->closed =
        resources->connection.ServiceClosed(winrt::auto_revoke, terminal);
    revocations->canceled =
        resources->task.Canceled(winrt::auto_revoke, terminal);
    resources_.install_revocations(std::move(revocations));
  }

  void close() noexcept { resources_.close(); }
  [[nodiscard]] Connection connection() const {
    const auto resources = resources_.resources();
    return resources ? resources->connection : nullptr;
  }

private:
  static winrt::fire_and_forget receive(std::shared_ptr<AppServiceSession> session,
                                       RequestArgs args) {
    try {
      const RequestDeferral deferral{args.GetDeferral()};
      winrt::Windows::Foundation::Collections::ValueSet response;
      bool accepted{};
      std::string error;
      const auto message = args.Request().Message();
      const auto kind = winrt::unbox_value_or<winrt::hstring>(
          message.TryLookup(L"kind"), L"");
      const auto payload = winrt::unbox_value_or<winrt::hstring>(
          message.TryLookup(L"json"), L"");
      if (kind != L"snapshot") {
        error = "unsupported companion message";
      } else {
        const auto utf8 = winrt::to_string(payload);
        if (utf8.size() > core::max_companion_snapshot_bytes)
          error = "companion snapshot exceeds bounded message size";
        else
          accepted = SnapshotHub::instance().accept_snapshot_json(session, utf8,
                                                                  error);
      }
      response.Insert(L"accepted", winrt::box_value(accepted));
      if (!error.empty())
        response.Insert(L"error", winrt::box_value(winrt::to_hstring(error)));
      co_await args.Request().SendResponseAsync(response);
    } catch (...) {
    }
  }

  ServiceSessionOwner<ServiceResources> resources_;
};

SnapshotHub &SnapshotHub::instance() {
  static SnapshotHub hub;
  return hub;
}

SnapshotHub::~SnapshotHub() {
  auto change = state_.replace(nullptr);
  cancel_timer(change.revision);
  if (change.retired)
    change.retired->close();
}

void SnapshotHub::connect(Task task, Connection connection) {
  const auto session =
      std::make_shared<AppServiceSession>(std::move(task), std::move(connection));
  auto change = state_.replace(session);
  cancel_timer(change.revision);
  if (change.retired)
    change.retired->close();
  change.notifications.deliver();
  try {
    session->start();
  } catch (...) {
    disconnect(session);
  }
}

void SnapshotHub::disconnect(
    const std::shared_ptr<AppServiceSession> &session) noexcept {
  auto change = state_.disconnect(session);
  if (change.retired)
    cancel_timer(change.revision);
  session->close();
  change.notifications.deliver();
}

bool SnapshotHub::accept_snapshot_json(
    const std::shared_ptr<AppServiceSession> &session, const std::string_view json,
    std::string &error) {
  const auto parsed = core::parse_companion_snapshot_json(json);
  const auto *snapshot = std::get_if<Snapshot>(&parsed);
  if (!snapshot) {
    error = std::get<core::CompanionError>(parsed).message;
    return false;
  }
  const auto publication = state_.publish(session, *snapshot, State::Clock::now());
  if (!publication) {
    error = "companion connection was retired";
    return false;
  }
  error.clear();
  arm_timer(*publication);
  publication->notifications.deliver();
  return true;
}

SnapshotHub::Subscription SnapshotHub::subscribe(Callback callback) {
  return state_.subscribe(std::move(callback));
}

std::optional<SnapshotHub::Snapshot> SnapshotHub::current() const {
  return state_.current(State::Clock::now());
}

winrt::Windows::Foundation::IAsyncOperation<bool>
SnapshotHub::save_preferences(core::CompanionProfile profile) {
  const auto session = state_.session();
  const auto connection = session ? session->connection() : nullptr;
  if (!connection)
    co_return false;
  try {
    winrt::Windows::Foundation::Collections::ValueSet message;
    message.Insert(L"kind", winrt::box_value(L"preferences"));
    message.Insert(
        L"json", winrt::box_value(winrt::to_hstring(
                     core::serialize_companion_profile_json(profile))));
    const auto response = co_await connection.SendMessageAsync(message);
    if (response.Status() != winrt::Windows::ApplicationModel::AppService::
                                 AppServiceResponseStatus::Success ||
        state_.session() != session)
      co_return false;
    co_return winrt::unbox_value_or<bool>(
        response.Message().TryLookup(L"accepted"), false);
  } catch (...) {
    co_return false;
  }
}

void SnapshotHub::arm_timer(const State::Publication &publication) {
  std::lock_guard lock(timer_mutex_);
  if (!state_.is_current(publication.revision))
    return;
  if (stale_timer_)
    stale_timer_.Cancel();
  timer_revision_ = publication.revision;
  stale_timer_ = winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
      [revision = publication.revision](const auto &) {
        SnapshotHub::instance().expire(revision);
      },
      std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
          std::max(publication.deadline - State::Clock::now(),
                   State::Clock::duration::zero())));
}

void SnapshotHub::cancel_timer(const std::uint64_t revision) noexcept {
  winrt::Windows::System::Threading::ThreadPoolTimer retired{nullptr};
  {
    std::lock_guard lock(timer_mutex_);
    if (timer_revision_ < revision)
      retired = std::exchange(stale_timer_, nullptr);
  }
  if (retired)
    retired.Cancel();
}

void SnapshotHub::expire(const std::uint64_t revision) {
  auto notifications = state_.expire(revision, State::Clock::now());
  cancel_timer(revision + 1U);
  notifications.deliver();
}

} // namespace layerc::gamebar::widget

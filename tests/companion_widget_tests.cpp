#include "../src/gamebar/widget/WidgetServiceSession.hpp"
#include "../src/gamebar/widget/WidgetSnapshotState.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using layerc::test::expect;
namespace widget = layerc::gamebar::widget;

struct ResourceCounts {
  std::atomic<int> closes{};
  std::atomic<int> completions{};
  std::atomic<int> request_revocations{};
  std::atomic<int> close_revocations{};
  std::atomic<int> cancel_revocations{};

  [[nodiscard]] bool released_once() const {
    return closes == 1 && completions == 1 && request_revocations == 1 &&
           close_revocations == 1 && cancel_revocations == 1;
  }
};

// Models the connection, retained task, deferral and all three registrations.
// Reentrant destruction probes ensure none of their cleanup runs under locks.
struct ServiceResources {
  struct Revocations {
    explicit Revocations(ResourceCounts &value,
                         std::function<void()> callback = {})
        : counts(value), on_revoke(std::move(callback)) {}
    ResourceCounts &counts;
    std::function<void()> on_revoke;
    ~Revocations() {
      ++counts.request_revocations;
      ++counts.close_revocations;
      ++counts.cancel_revocations;
      if (on_revoke)
        on_revoke();
    }
  };

  explicit ServiceResources(ResourceCounts &value) : counts(value) {}
  ~ServiceResources() {
    revocations.reset();
    ++counts.closes;
    ++counts.completions;
    if (on_close)
      on_close();
  }
  ResourceCounts &counts;
  std::unique_ptr<Revocations> revocations;
  std::function<void()> on_close;
};

struct Service {
  explicit Service(ResourceCounts &counts)
      : owner(std::make_shared<ServiceResources>(counts)) {}
  void start() {
    const auto resources = owner.resources();
    if (resources)
      owner.install_revocations(
          std::make_unique<ServiceResources::Revocations>(resources->counts));
  }
  void close() { owner.close(); }
  widget::ServiceSessionOwner<ServiceResources> owner;
};

using State = widget::SnapshotState<Service>;

[[nodiscard]] State::Snapshot snapshot(std::string key) {
  State::Snapshot value;
  value.profile_id = "fixture";
  value.session_key = std::move(key);
  return value;
}

void finish(State::Change change) {
  if (change.retired)
    change.retired->close();
  change.notifications.deliver();
}

struct Dispatcher {
  std::vector<std::function<void()>> pending;
  void run(const std::size_t index = 0) {
    auto work = std::move(pending.at(index));
    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
    work();
  }
  void drain() {
    while (!pending.empty())
      run();
  }
};

} // namespace

void test_widget_connection_ownership() {
  ResourceCounts a_counts;
  ResourceCounts b_counts;
  State state;
  const auto a = std::make_shared<Service>(a_counts);
  const auto b = std::make_shared<Service>(b_counts);
  finish(state.replace(a));
  a->start();
  const auto now = State::Clock::time_point{};
  const auto initial = state.publish(a, snapshot("A"), now);
  if (!expect(initial.has_value(), "current connection can publish"))
    return;

  // The request has already captured A before replacement, like a dispatched
  // App Service callback. It must not publish into B's hub later.
  const auto dispatched_request = [&] {
    return state.publish(a, snapshot("late A"), now);
  };
  finish(state.replace(b));
  b->start();
  const auto current = state.publish(b, snapshot("B"), now);
  expect(current.has_value(), "replacement connection remains usable");
  expect(!dispatched_request(), "already-dispatched retired request is rejected");

  std::latch ready(2);
  std::latch release(1);
  const auto terminal = [&](const std::shared_ptr<Service> &service) {
    ready.count_down();
    release.wait();
    finish(state.disconnect(service));
    service->close();
  };
  std::jthread closed([&] { terminal(a); });
  std::jthread canceled([&] { terminal(a); });
  ready.wait();
  release.count_down();
  closed.join();
  canceled.join();
  const auto latest = state.current(now);
  expect(state.session() == b && latest && latest->session_key == "B",
         "late concurrent close/cancel from A cannot retire or hide B");
  expect(a_counts.released_once(), "A resources and every revocation release once");
  expect(b_counts.closes == 0 && b_counts.completions == 0 &&
             b_counts.cancel_revocations == 0,
         "A cleanup cannot touch B resources");

  std::latch current_ready(2);
  std::latch current_release(1);
  std::atomic<int> retirements{};
  const auto current_terminal = [&] {
    current_ready.count_down();
    current_release.wait();
    auto change = state.disconnect(b);
    if (change.retired)
      ++retirements;
    finish(std::move(change));
    b->close();
  };
  std::jthread current_closed(current_terminal);
  std::jthread current_canceled(current_terminal);
  current_ready.wait();
  current_release.count_down();
  current_closed.join();
  current_canceled.join();
  expect(retirements == 1 && !state.session() && !state.current(now),
         "concurrent current terminal callbacks retire exactly one session");
  expect(b_counts.released_once(), "B terminal cleanup is idempotent");
  expect(!state.publish(b, snapshot("closed B"), now),
         "closed current connection cannot republish");
}

void test_widget_service_registration_cleanup() {
  ResourceCounts a_counts;
  ResourceCounts b_counts;
  State state;
  const auto a = std::make_shared<Service>(a_counts);
  const auto b = std::make_shared<Service>(b_counts);
  finish(state.replace(a));
  std::latch registering(1);
  std::latch resume(1);
  std::atomic<bool> revoked_outside_lock{};
  std::atomic<bool> completed_outside_lock{};
  {
    const auto resources = a->owner.resources();
    resources->on_close = [&] {
      completed_outside_lock = !a->owner.resources() && state.session() == b;
    };
  }
  std::jthread registration([&] {
    auto lease = a->owner.resources();
    registering.count_down();
    resume.wait();
    auto revocations = std::make_unique<ServiceResources::Revocations>(
        lease->counts, [&] {
          revoked_outside_lock = !a->owner.resources() && state.session() == b;
        });
    a->owner.install_revocations(std::move(revocations));
  });
  registering.wait();
  finish(state.replace(b));
  b->start();
  expect(!a->owner.resources() && a_counts.completions == 0 &&
             a_counts.closes == 0,
         "registration lease retains A's task and deferral through close");
  resume.count_down();
  registration.join();
  expect(a_counts.released_once() && revoked_outside_lock &&
             completed_outside_lock,
         "late registration revokes and completes only A, outside locks");
  expect(state.session() == b && b_counts.closes == 0,
         "late registration never installs resources into B");
  finish(state.disconnect(b));

  ResourceCounts counts;
  widget::ServiceSessionOwner<ServiceResources> owner(
      std::make_shared<ServiceResources>(counts));
  bool replacement_outside_lock{};
  owner.install_revocations(std::make_unique<ServiceResources::Revocations>(
      counts, [&] {
        replacement_outside_lock = static_cast<bool>(owner.resources());
      }));
  owner.install_revocations(
      std::make_unique<ServiceResources::Revocations>(counts));
  expect(replacement_outside_lock && counts.cancel_revocations == 1,
         "replaced registrations are revoked outside the owner mutex");
  owner.close();
  owner.close();
  expect(counts.closes == 1 && counts.completions == 1 &&
             counts.request_revocations == 2 && counts.close_revocations == 2 &&
             counts.cancel_revocations == 2,
         "all registrations are owned, with one terminal deferral completion");

  ResourceCounts failed_counts;
  const auto failed = std::make_shared<Service>(failed_counts);
  finish(state.replace(failed));
  try {
    const auto lease = failed->owner.resources();
    const auto partial =
        std::make_unique<ServiceResources::Revocations>(lease->counts);
    throw std::runtime_error("controlled registration failure");
  } catch (const std::runtime_error &) {
    finish(state.disconnect(failed));
  }
  expect(failed_counts.released_once() && !state.session(),
         "registration failure unwinds local revocations and the session owner");
}

void test_widget_latest_state_delivery() {
  ResourceCounts counts;
  State state;
  const auto service = std::make_shared<Service>(counts);
  finish(state.replace(service));
  auto now = State::Clock::time_point{};
  Dispatcher dispatcher;
  std::optional<State::Snapshot> rendered;
  auto subscription = state.subscribe(widget::latest_state_callback(
      [&] { return state.current(now); },
      [&](auto work) { dispatcher.pending.emplace_back(std::move(work)); },
      [] { return true; }, [&](auto value) { rendered = std::move(value); }));

  // Pause the initial dispatcher operation until after a subsequent update.
  auto a = state.publish(service, snapshot("A"), now);
  a->notifications.deliver();
  dispatcher.run(1);
  dispatcher.run();
  expect(rendered && rendered->session_key == "A",
         "delayed initial subscription cannot overwrite a later update");

  // Pause publisher A outside the hub lock while B is committed and delivered.
  a = state.publish(service, snapshot("A"), now);
  auto b = state.publish(service, snapshot("B"), now);
  b->notifications.deliver();
  dispatcher.drain();
  a->notifications.deliver();
  dispatcher.drain();
  expect(rendered && rendered->session_key == "B",
         "delayed A notification reads B rather than overwriting it");

  // Also reverse already-queued dispatcher work, not just publisher callbacks.
  a = state.publish(service, snapshot("queued A"), now);
  a->notifications.deliver();
  b = state.publish(service, snapshot("queued B"), now);
  b->notifications.deliver();
  dispatcher.run(1);
  dispatcher.run();
  expect(rendered && rendered->session_key == "queued B",
         "old queued dispatcher work cannot overwrite newer data");

  a = state.publish(service, snapshot("expiring A"), now);
  a->notifications.deliver();
  now = a->deadline;
  auto expired = state.expire(a->revision, now);
  expired.deliver();
  dispatcher.run(1);
  dispatcher.run();
  expect(!rendered, "expiry cannot be followed by queued snapshot resurrection");
  a->notifications.deliver();
  dispatcher.drain();
  expect(!rendered, "publisher paused past expiry cannot resurrect data either");

  // An expiry's disconnected invalidation is delayed past the next publication.
  b = state.publish(service, snapshot("fresh B"), now);
  b->notifications.deliver();
  dispatcher.drain();
  expired.deliver();
  dispatcher.drain();
  expect(rendered && rendered->session_key == "fresh B",
         "an old disconnected notification cannot hide a fresh publication");
  state.expire(a->revision, now + State::stale_after).deliver();
  expect(state.is_current(b->revision), "obsolete timer cannot expire B");

  auto copy = state.current(now);
  copy->session_key = "modified copy";
  expect(state.current(now)->session_key == "fresh B",
         "consumers receive synchronized value copies, never mutable hub state");

  b->notifications.deliver();
  now = b->deadline;
  dispatcher.drain();
  expect(!rendered,
         "dispatcher freshness check hides stale data even before timer delivery");
}

void test_widget_owned_subscriptions() {
  ResourceCounts counts;
  State state;
  const auto service = std::make_shared<Service>(counts);
  finish(state.replace(service));
  const auto now = State::Clock::time_point{};
  Dispatcher dispatcher;
  const auto baseline = state.subscriber_count();
  std::array<int, 8> renders{};
  for (int cycle = 0; cycle < 32; ++cycle) {
    std::array<State::Subscription, 8> pages;
    for (std::size_t slot = 0; slot < pages.size(); ++slot) {
      pages[slot] = state.subscribe(widget::latest_state_callback(
          [&] { return state.current(now); },
          [&](auto work) { dispatcher.pending.emplace_back(std::move(work)); },
          [] { return true; }, [&, slot](const auto &) { ++renders[slot]; }));
    }
    expect(state.subscriber_count() == baseline + pages.size(),
           "eight independently owned widget subscriptions register");
    const auto closed_renders = renders[0];
    pages[0].reset(); // Close without navigation, with initial work still queued.
    pages[0].reset(); // Unload/close/destruction cleanup is idempotent.
    expect(state.subscriber_count() == baseline + pages.size() - 1U,
           "close immediately removes just its own subscription");
    dispatcher.drain();
    expect(renders[0] == closed_renders,
           "already-queued work cannot render after widget close");
    const auto other_renders = renders[1];
    const auto publication = state.publish(service, snapshot("live"), now);
    auto captured = publication->notifications;
    captured.deliver();
    dispatcher.drain();
    expect(renders[0] == closed_renders && renders[1] == other_renders + 1,
           "closing one widget leaves the other seven usable");
    for (auto &page : pages)
      page.reset();
    captured.deliver();
    expect(dispatcher.pending.empty() && state.subscriber_count() == baseline,
           "captured notifications cannot dispatch after all widgets close");
  }
  expect(state.subscriber_count() == baseline,
         "repeated close/reopen returns the subscriber count to baseline");

  bool alive = true;
  auto page = state.subscribe(widget::latest_state_callback(
      [&] { return state.current(now); },
      [&](auto work) { dispatcher.pending.emplace_back(std::move(work)); },
      [&] { return alive; }, [&](const auto &) { ++renders[0]; }));
  const auto before_death = renders[0];
  alive = false;
  dispatcher.drain();
  const auto publication = state.publish(service, snapshot("live"), now);
  publication->notifications.deliver();
  expect(dispatcher.pending.empty() && renders[0] == before_death,
         "dead pages are checked both before dispatch and after queueing");
  page.reset();

  int healthy_calls{};
  auto broken = state.subscribe([](auto) {
    throw std::runtime_error("controlled closed-dispatcher failure");
  });
  auto healthy = state.subscribe([&](auto) {
    // These accesses would deadlock if fan-out held the hub mutex.
    (void)state.current(now);
    (void)state.subscriber_count();
    ++healthy_calls;
  });
  const auto healthy_before = healthy_calls;
  const auto next = state.publish(service, snapshot("after failure"), now);
  next->notifications.deliver();
  expect(healthy_calls == healthy_before + 1,
         "a failed dispatcher cannot interrupt out-of-lock fan-out");

  auto moved = std::move(healthy);
  healthy.reset();
  expect(state.subscriber_count() == baseline + 2U,
         "moving a subscription transfers ownership without removing it");
  moved.reset();
  broken.reset();
  expect(state.subscriber_count() == baseline,
         "subscription owners remove their registrations at destruction/close");

  bool callback_destroyed_outside_lock{};
  struct Capture {
    State &state;
    bool &destroyed;
    ~Capture() {
      (void)state.subscriber_count();
      destroyed = true;
    }
  };
  auto capture = std::make_shared<Capture>(state, callback_destroyed_outside_lock);
  auto reentrant = state.subscribe([capture](auto) {});
  capture.reset();
  reentrant.reset();
  expect(callback_destroyed_outside_lock,
         "callback captures are destroyed outside the subscription mutex");
}

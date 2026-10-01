#pragma once

#include "layerc/core/companion.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace layerc::gamebar::widget {

// Notifications only invalidate state. Consumers take a fresh value copy when
// they actually render, not when a publisher or timer captures the subscribers.
template <typename Session> class SnapshotState final {
public:
  using Snapshot = core::CompanionSessionSnapshot;
  using Clock = std::chrono::steady_clock;
  static constexpr auto stale_after = std::chrono::seconds(2);
  struct SubscriptionLifetime final {};
  using WeakLifetime = std::weak_ptr<const SubscriptionLifetime>;
  using Callback = std::function<void(WeakLifetime)>;

private:
  struct Observer {
    WeakLifetime lifetime;
    Callback callback;
  };
  struct Shared {
    std::mutex mutex;
    std::shared_ptr<Session> session;
    std::optional<Snapshot> latest;
    Clock::time_point deadline{};
    std::uint64_t revision{};
    std::uint64_t next_token{1};
    std::map<std::uint64_t, Observer> observers;
  };

public:
  class Subscription final {
  public:
    Subscription() = default;
    ~Subscription() { reset(); }
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;
    Subscription(Subscription &&other) noexcept { *this = std::move(other); }
    Subscription &operator=(Subscription &&other) noexcept {
      if (this != &other) {
        reset();
        shared_ = std::move(other.shared_);
        lifetime_ = std::move(other.lifetime_);
        token_ = std::exchange(other.token_, 0);
      }
      return *this;
    }
    [[nodiscard]] WeakLifetime lifetime() const noexcept { return lifetime_; }
    void reset() noexcept {
      lifetime_.reset();
      if (const auto shared = shared_.lock()) {
        // Destroy callback captures outside the mutex too.
        typename decltype(Shared::observers)::node_type removed;
        {
          std::lock_guard lock(shared->mutex);
          removed = shared->observers.extract(token_);
        }
      }
      shared_.reset();
      token_ = 0;
    }

  private:
    friend class SnapshotState;
    std::weak_ptr<Shared> shared_;
    std::shared_ptr<const SubscriptionLifetime> lifetime_;
    std::uint64_t token_{};
  };

  struct Notifications {
    void deliver() const noexcept {
      for (const auto &observer : observers) {
        if (observer.lifetime.expired())
          continue;
        try {
          observer.callback(observer.lifetime);
        } catch (...) {
          // One closed/failed dispatcher must not interrupt other widgets.
        }
      }
    }
    std::vector<Observer> observers;
  };
  struct Change {
    std::shared_ptr<Session> retired;
    std::uint64_t revision{};
    Notifications notifications;
  };
  struct Publication {
    std::uint64_t revision{};
    Clock::time_point deadline;
    Notifications notifications;
  };

  [[nodiscard]] Subscription subscribe(Callback callback) {
    Subscription subscription;
    subscription.shared_ = shared_;
    subscription.lifetime_ = std::make_shared<SubscriptionLifetime>();
    const Observer observer{subscription.lifetime_, std::move(callback)};
    {
      std::lock_guard lock(shared_->mutex);
      subscription.token_ = shared_->next_token++;
      shared_->observers.emplace(subscription.token_, observer);
    }
    Notifications{{observer}}.deliver();
    return subscription;
  }

  // Replacement, terminal callbacks and publication share one serialized
  // identity boundary. Retired resource owners are returned for out-of-lock
  // revocation/completion; a late callback can only retire its own session.
  [[nodiscard]] Change replace(std::shared_ptr<Session> session) {
    std::lock_guard lock(shared_->mutex);
    auto retired = std::exchange(shared_->session, std::move(session));
    shared_->latest.reset();
    return {std::move(retired), ++shared_->revision, notifications_locked()};
  }
  [[nodiscard]] Change disconnect(const std::shared_ptr<Session> &session) {
    std::lock_guard lock(shared_->mutex);
    if (!session || shared_->session != session)
      return {};
    auto retired = std::exchange(shared_->session, {});
    shared_->latest.reset();
    return {std::move(retired), ++shared_->revision, notifications_locked()};
  }
  [[nodiscard]] std::shared_ptr<Session> session() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->session;
  }
  [[nodiscard]] std::optional<Publication>
  publish(const std::shared_ptr<Session> &session, Snapshot snapshot,
          const Clock::time_point now) {
    std::lock_guard lock(shared_->mutex);
    if (!session || shared_->session != session)
      return std::nullopt;
    shared_->latest = std::move(snapshot);
    shared_->deadline = now + stale_after;
    return Publication{++shared_->revision, shared_->deadline,
                       notifications_locked()};
  }
  [[nodiscard]] Notifications expire(const std::uint64_t revision,
                                      const Clock::time_point now) {
    std::lock_guard lock(shared_->mutex);
    if (revision != shared_->revision || !shared_->latest ||
        now < shared_->deadline)
      return {};
    shared_->latest.reset();
    ++shared_->revision;
    return notifications_locked();
  }
  [[nodiscard]] std::optional<Snapshot> current(const Clock::time_point now) const {
    std::lock_guard lock(shared_->mutex);
    if (!shared_->latest || now >= shared_->deadline)
      return std::nullopt;
    return shared_->latest;
  }
  [[nodiscard]] bool is_current(const std::uint64_t revision) const {
    std::lock_guard lock(shared_->mutex);
    return revision == shared_->revision && shared_->latest.has_value();
  }
  [[nodiscard]] std::size_t subscriber_count() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->observers.size();
  }

private:
  [[nodiscard]] Notifications notifications_locked() const {
    Notifications notifications;
    notifications.observers.reserve(shared_->observers.size());
    for (const auto &[token, observer] : shared_->observers) {
      (void)token;
      notifications.observers.push_back(observer);
    }
    return notifications;
  }
  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
};

// Queue only an invalidation. The shared helper is exercised with a controlled
// dispatcher/clock in companion tests and with CoreDispatcher in the host.
// Both lifetime checks are needed: close may happen before queueing or while a
// dispatcher operation is pending. No captured snapshot is authoritative.
template <typename Read, typename Queue, typename Alive, typename Render>
[[nodiscard]] auto latest_state_callback(Read read, Queue queue, Alive alive,
                                         Render render) {
  return [read, queue, alive, render](auto lifetime) {
    if (lifetime.expired() || !alive())
      return;
    queue([read, alive, render, lifetime] {
      if (!lifetime.expired() && alive())
        render(read());
    });
  };
}

} // namespace layerc::gamebar::widget

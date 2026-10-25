#include "evdev_route.hpp"
#include "display_route.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace layerc::daemon {

std::vector<std::string> evdev_reason_tokens(const std::span<const std::string> reasons) {
  std::vector<std::string> tokens;
  for (const auto &reason : reasons) {
    const std::string_view text{reason};
    std::string token{text.substr(0, text.find(':'))};
    if (!token.empty() && std::ranges::find(tokens, token) == tokens.end())
      tokens.push_back(std::move(token));
  }
  return tokens;
}

EvdevInputRoute::~EvdevInputRoute() { close(); }

void EvdevInputRoute::configure(const InputOptions &options) {
  close();
  options_ = options;
  fault_.clear();
}

bool EvdevInputRoute::open(std::string &error) {
  error.clear();
  if (source_) return true;
  if (!enabled()) {
    error = "evdev_not_enabled";
    return false;
  }
  if (loop_ == nullptr) {
    error = "evdev_unavailable: no event loop";
    fault_ = error;
    return false;
  }
  const auto create = [&](const std::optional<std::string> &keymap, std::string &failure) {
    const layerc::platform::EvdevOptions evdev{options_.device_dir, keymap};
    return options_.backend
        ? layerc::platform::EvdevInputSource::create(evdev, failure, options_.backend)
        : layerc::platform::EvdevInputSource::create(evdev, failure);
  };
  std::string keymap_error;
  auto source = create(keymap_, error);
  if (!source && keymap_) {
    // A compositor keymap that fails to compile must not cost the input.
    keymap_error = error;
    source = create(std::nullopt, error);
  }
  if (!source) {
    if (error.empty()) error = "evdev_unavailable: source could not be created";
    fault_ = error;
    return false;
  }
  // Every device denied: nothing to read until the user joins the input
  // group, so report it and let the caller use its fallback.
  if (source->devices().empty()) {
    for (const auto &reason : source->unavailable())
      if (reason.starts_with("evdev_permission_denied")) {
        error = reason;
        fault_ = error;
        return false;
      }
  }
  source_ = std::move(source);
  fault_ = keymap_error;
  sync_registrations();
  return true;
}

void EvdevInputRoute::close() noexcept {
  if (loop_ != nullptr)
    for (const int fd : registered_) loop_->unregister_fd(fd);
  registered_.clear();
  source_.reset();
}

layerc::platform::EvdevPoll EvdevInputRoute::poll(const std::span<const int> ready) {
  if (!source_) return {};
  const bool mine = std::ranges::any_of(ready, [&](const int fd) {
    return std::ranges::find(registered_, fd) != registered_.end();
  });
  if (!mine) return {};
  auto result = source_->poll(ready);
  if (result.devices_changed) sync_registrations();
  return result;
}

void EvdevInputRoute::set_keymap(const std::optional<std::string> &text) {
  if (keymap_ == text) return;
  keymap_ = text;
  if (!source_) return;
  std::string error;
  if (source_->set_keymap(text, error)) {
    fault_.clear();
  } else {
    fault_ = error;
  }
}

std::vector<std::string>
EvdevInputRoute::unavailable(const layerc::platform::EvdevInputCapabilities needed) const {
  if (!source_) {
    if (!needed.keyboard && !needed.mouse && !needed.gamepad) return {};
    return {fault_.empty() ? std::string{"evdev_unavailable: source is not open"} : fault_};
  }
  auto reasons = source_->unavailable(needed);
  if (!fault_.empty()) reasons.push_back(fault_);
  return reasons;
}

layerc::runtime::RuntimeResourceStats EvdevInputRoute::resources() const noexcept {
  return source_ ? source_->resources() : layerc::runtime::RuntimeResourceStats{};
}

void EvdevInputRoute::sync_registrations() noexcept {
  if (loop_ == nullptr) return;
  std::vector<int> next;
  try {
    next = source_ ? source_->fds() : std::vector<int>{};
  } catch (...) {
    next.clear();
  }
  // Closed device descriptors leave the poll set before a reused number
  // could be reported as theirs.
  for (const int fd : registered_)
    if (std::ranges::find(next, fd) == next.end()) loop_->unregister_fd(fd);
  for (const int fd : next) loop_->register_fd(fd);
  registered_ = std::move(next);
}

InputRouteStatus input_route_status(const EvdevInputRoute &evdev, const PassiveInputSource *xi2,
                                    const bool prefer_evdev, const InputDeviceNeeds needed) {
  InputRouteStatus result;
  result.built = true;
  result.selection = evdev.enabled() ? InputSelection::evdev : InputSelection::automatic;
  // select_input_sources takes mutable borrowed pointers but only compares and queries them.
  auto *native = evdev.enabled() ? const_cast<EvdevInputRoute *>(&evdev) : nullptr;
  const auto sources =
      select_input_sources(native, const_cast<PassiveInputSource *>(xi2), prefer_evdev);
  result.evdev_selected = native && !sources.empty() && sources.front() == native;
  result.evdev_attached = evdev.attached();
  result.xi2_attached = xi2 && xi2->input_open();
  if (result.evdev_attached) {
    result.evdev_reasons = evdev_reason_tokens(evdev.unavailable(
        {.keyboard = needed.keyboard, .mouse = needed.mouse, .gamepad = needed.gamepad}));
  } else if (!evdev.fault().empty()) {
    const std::string fault = evdev.fault();
    result.evdev_reasons = evdev_reason_tokens(std::span{&fault, 1});
  }
  return result;
}

} // namespace layerc::daemon

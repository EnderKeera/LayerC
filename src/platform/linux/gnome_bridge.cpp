#include "gnome_bridge.hpp"

#include "dbus_shell_service.hpp"

#include <utility>

namespace layerc::platform {

GnomeBridge::~GnomeBridge() { stop(); }

bool GnomeBridge::trusted() const {
  const auto shell = bus_->owner(gnome_shell_bus_name);
  const auto extension = bus_->owner(gnome_bridge_bus_name);
  return !owner_.empty() && shell && extension && *shell == owner_ && *extension == owner_;
}

bool GnomeBridge::available() const noexcept {
  return wanted_ && verified_ && !failed_ && reason_.empty();
}

bool GnomeBridge::take_changed() noexcept { return std::exchange(changed_, false); }

void GnomeBridge::set_wanted(const bool wanted) {
  if (wanted_ == wanted) return;
  if (!wanted) {
    stop();
    return;
  }
  wanted_ = true;
  sync();
}

void GnomeBridge::stop() noexcept {
  // Release the panels even when Present is in flight. Same-connection FIFO
  // ordering places Clear after it; invalidating the guard prevents its late
  // reply from sending another Present.
  if (verified_ && trusted() && bus_->raw()) {
    try {
      (void)bus_->call(owner_.c_str(), gnome_bridge_bus_path, gnome_bridge_interface, "Clear",
                      {}, [](sd_bus_message *, const sd_bus_error *) {}, guard_);
    } catch (...) {}
  }
  ++generation_;
  bus_->release(guard_);
  wanted_ = false;
  verified_ = probing_ = pending_ = failed_ = false;
  desired_.clear();
  acknowledged_.clear();
  owner_.clear();
  reason_.clear();
  frame_error_.clear();
  changed_ = true;
}

void GnomeBridge::sync() {
  if (!wanted_) return;
  bus_->watch_name(gnome_shell_bus_name);
  bus_->watch_name(gnome_bridge_bus_name);
  const auto shell = bus_->owner(gnome_shell_bus_name);
  const auto extension = bus_->owner(gnome_bridge_bus_name);
  std::string unavailable;
  if (!bus_->connected())
    unavailable = bus_->reason().empty() ? "bus_unavailable" : bus_->reason();
  else if (!shell || !extension)
    unavailable = "gnome_extension_missing";
  else if (*shell != *extension)
    unavailable = "gnome_extension_untrusted";
  if (!unavailable.empty()) {
    if (!owner_.empty() || reason_ != unavailable) {
      ++generation_;
      bus_->release(guard_);
      owner_.clear();
      verified_ = probing_ = pending_ = failed_ = false;
      acknowledged_.clear();
      reason_ = std::move(unavailable);
      changed_ = true;
    }
    return;
  }
  if (*extension != owner_) probe(*extension);
  if (available()) send();
}

void GnomeBridge::probe(const std::string &owner) {
  owner_ = owner;
  const auto generation = ++generation_;
  bus_->release(guard_); // cancel the replaced owner's probe and frame
  guard_ = make_bus_guard();
  verified_ = pending_ = failed_ = false;
  probing_ = true;
  acknowledged_.clear();
  reason_ = "gnome_extension_checking";
  changed_ = true;
  const bool queued = bus_->get_property(owner.c_str(), gnome_bridge_bus_path,
      gnome_bridge_interface, "Version",
      [this, generation](sd_bus_message *reply, const sd_bus_error *error) {
        if (generation != generation_ || !trusted()) return;
        probing_ = false;
        std::uint32_t version{};
        if (error || sd_bus_message_enter_container(reply, 'v', "u") <= 0 ||
            sd_bus_message_read(reply, "u", &version) <= 0 || version != 1) {
          failed_ = true;
          reason_ = "gnome_extension_version";
        } else {
          verified_ = true;
          reason_.clear();
          send();
        }
        changed_ = true;
      }, guard_);
  if (!queued) {
    probing_ = false;
    failed_ = true;
    reason_ = "gnome_extension_failed";
  }
}

bool GnomeBridge::present(const std::span<const GnomePanel> panels) {
  if (!wanted_) return false;
  auto frame = build_gnome_overlay_frame(panels);
  if (!frame) {
    // Keep the transport and the last accepted frame; the next valid frame recovers.
    if (frame_error_.empty()) {
      frame_error_ = "gnome_frame_too_large";
      changed_ = true;
    }
    return false;
  }
  if (!frame_error_.empty()) {
    frame_error_.clear();
    changed_ = true;
  }
  desired_ = panels.empty() ? "" : std::move(*frame);
  sync();
  return available();
}

void GnomeBridge::clear() {
  desired_.clear();
  if (available()) send();
}

void GnomeBridge::send() {
  if (!available() || pending_ || !trusted() || desired_ == acknowledged_) return;
  pending_ = true;
  const auto generation = generation_;
  const auto sent = desired_;
  const bool queued = bus_->call(owner_.c_str(), gnome_bridge_bus_path, gnome_bridge_interface,
      sent.empty() ? "Clear" : "Present",
      [sent](sd_bus_message *call) {
        return sent.empty() ? 0 : sd_bus_message_append(call, "s", sent.c_str());
      },
      [this, generation, sent](sd_bus_message *, const sd_bus_error *error) {
        if (generation != generation_ || !trusted()) return;
        pending_ = false;
        if (error) {
          failed_ = true;
          reason_ = "gnome_extension_failed";
          changed_ = true;
          return;
        }
        acknowledged_ = sent;
        send(); // At most the single newest replacement.
      }, guard_);
  if (!queued) {
    pending_ = false;
    failed_ = true;
    reason_ = "gnome_extension_failed";
    changed_ = true;
  }
}

} // namespace layerc::platform

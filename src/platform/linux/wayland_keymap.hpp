#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace layerc::platform {

class WaylandConnection;

// The compositor's XKB keymap for the first wl_seat's keyboard, used only to
// translate passive evdev keycodes. The daemon never has keyboard focus (its
// layer surfaces take no keyboard interactivity), so no key event arrives.
class WaylandKeymap {
public:
  static constexpr std::size_t text_limit = 1024U * 1024U;

  WaylandKeymap();
  ~WaylandKeymap();
  WaylandKeymap(const WaylandKeymap &) = delete;
  WaylandKeymap &operator=(const WaylandKeymap &) = delete;

  // Binds a newly advertised seat and releases a removed one.
  void sync(const WaylandConnection &connection);
  // Destroys every proxy; the owning connection is about to disconnect.
  void release() noexcept;
  // True once after the keymap text changed or went away.
  [[nodiscard]] bool take_changed() noexcept;
  // The last valid XKB v1 keymap, if the seat has a keyboard.
  [[nodiscard]] const std::optional<std::string> &text() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::platform

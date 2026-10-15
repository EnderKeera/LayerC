#pragma once

// Loads the embedded LayerC window-report script into KWin while window
// tracking is needed (docs/Wayland.md, "KWin script"). Every step is an
// asynchronous call on the daemon's one bus connection.

#include "dbus_connection.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace layerc::platform {

inline constexpr const char *kwin_script_plugin = "layerc-windows";

// The JavaScript layerd writes for KWin 6 (with the KWin 5.27 fallback).
[[nodiscard]] std::string_view kwin_window_script() noexcept;

// $XDG_RUNTIME_DIR/layerc/kwin/layerc-windows.js; empty without a usable
// runtime directory.
[[nodiscard]] std::filesystem::path kwin_script_path();

class KwinScript {
public:
  enum class State : std::uint8_t { idle, waiting_for_kwin, loading, running, failed };

  explicit KwinScript(BusConnection &bus) : bus_(&bus) {}
  ~KwinScript();
  KwinScript(const KwinScript &) = delete;
  KwinScript &operator=(const KwinScript &) = delete;

  // Window tracking demand. Turning it off unloads the script.
  void set_wanted(bool wanted);
  // Re-evaluates after a bus dispatch: a new KWin owner reloads the script.
  void sync();
  [[nodiscard]] State state() const noexcept { return state_; }
  // kwin_unavailable, kwin_script_write_failed, kwin_script_failed, or empty.
  [[nodiscard]] const std::string &reason() const noexcept { return reason_; }

private:
  void begin(const std::string &owner);
  void run(const std::string &owner, std::int32_t id, bool legacy_path);
  void unload() noexcept;
  void fail(std::string reason);

  BusConnection *bus_;
  BusGuard guard_;
  bool wanted_{};
  bool load_requested_{};
  State state_{State::idle};
  std::string reason_;
  std::string owner_;
  std::uint64_t generation_{};
};

} // namespace layerc::platform

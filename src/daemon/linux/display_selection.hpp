#pragma once

#include "layerc/runtime/display_diagnostics.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::platform {
class EvdevDeviceBackend;
}

namespace layerc::daemon {

// The global input source the user opted into (docs/Wayland.md). evdev sees
// every keystroke on the seat, so it is never selected without an opt-in.
enum class InputSelection : std::uint8_t { automatic, evdev, invalid };

[[nodiscard]] InputSelection input_selection(std::string_view value) noexcept;
// `--input=auto|evdev` wins over LAYERC_INPUT (`environment`, may be null).
// An unknown argument or flag value is a usage error: nullopt plus `error`.
// An invalid environment value selects InputSelection::invalid (no evdev).
[[nodiscard]] std::optional<InputSelection>
parse_input_selection(std::span<const std::string_view> arguments,
                      const char *environment, std::string &error);

struct InputOptions {
  InputSelection selection{InputSelection::automatic};
  std::filesystem::path device_dir{"/dev/input"};
  // Tests only: synthetic device capabilities; null uses the kernel ioctls.
  std::shared_ptr<layerc::platform::EvdevDeviceBackend> backend;
};

// Which input route serves the session, for the display status rows.
struct InputRouteStatus {
  bool built{};
  InputSelection selection{InputSelection::automatic};
  // Opted in and preferred over XI2 for this session.
  bool evdev_selected{};
  bool evdev_attached{};
  bool xi2_attached{};
  // Stable reason tokens (no paths): device gaps or the open failure.
  std::vector<std::string> evdev_reasons;
};

// Pure selection policy; the booleans are successful connection probes, not
// environment hints. Forced backends never silently fall back to another.
[[nodiscard]] layerc::runtime::DisplayBackend
display_backend(std::string_view value) noexcept;
[[nodiscard]] layerc::runtime::DisplaySession select_display_session(
    layerc::runtime::DisplayBackend backend, bool wayland, bool x11) noexcept;
[[nodiscard]] layerc::runtime::DisplayCompositor detect_compositor(
    std::string_view desktop, bool kwin_owner, bool gnome_owner,
    bool layer_shell, bool foreign_toplevel) noexcept;

} // namespace layerc::daemon

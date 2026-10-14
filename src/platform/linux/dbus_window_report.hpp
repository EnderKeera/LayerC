#pragma once

// Window snapshots the KWin script or the GNOME Shell extension reports over
// the session bus (docs/Wayland.md, "Window report contract"). Pure: no D-Bus
// header reaches this file, so the bounds are verifiable without a bus.

#include "display_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::platform {

enum class WindowReportSource : std::uint8_t { kwin, gnome };

inline constexpr std::size_t window_report_max_bytes = 64 * 1024;
inline constexpr std::size_t window_report_max_string = 512;
inline constexpr std::size_t window_report_max_id = 128;

// A rejected report: `reason` is a stable status token, `detail` names the
// failing field for diagnostics.
struct WindowReportError {
  std::string reason;
  std::string detail;
};

[[nodiscard]] std::string_view window_report_source_name(WindowReportSource source) noexcept;

// Stable nonzero identity for one reported window: the first eight bytes of
// SHA-256 over the source name, a NUL separator, and the reported id.
[[nodiscard]] std::uint64_t window_report_id(WindowReportSource source, std::string_view id);

// Parses one full snapshot from `source`. The report must name that source;
// a malformed report is rejected as a whole. More than window_scan_limit
// windows are truncated and counted. Executables resolve through
// process_executable_path, so only same-UID PIDs yield a path.
[[nodiscard]] std::variant<WindowScan, WindowReportError>
parse_window_report(std::string_view payload, WindowReportSource source);

} // namespace layerc::platform

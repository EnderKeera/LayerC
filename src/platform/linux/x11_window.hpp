#pragma once

#include "x11_display.hpp"

#include "layerc/core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace layerc::platform {

// One window as scanned. Identity fields are empty when the server or the WM
// does not publish them, and executable resolution degrades silently.
struct X11WindowSnapshot {
  std::uint64_t window{};
  std::string title;
  std::string class_name;
  std::string instance_name;
  std::optional<std::uint32_t> pid;
  std::string executable_name;
  std::string executable_path;
  int x{};
  int y{};
  int width{};
  int height{};
  bool active{};
  bool fullscreen{};
  bool hidden{};
};

struct X11WindowScan {
  bool ewmh{};
  std::vector<X11WindowSnapshot> windows;
  std::optional<std::uint64_t> active;
  // Set when the EWMH list is missing, unreadable, or a query failed: the
  // caller surfaces it instead of silently tracking a partial list.
  std::string warning;
  std::uint32_t truncated{};
};

class X11WindowTracker {
public:
  explicit X11WindowTracker(const X11Display &display) noexcept;

  // Bounded scan: at most `limit` mapped top-level windows, root-relative.
  [[nodiscard]] X11WindowScan scan(std::uint32_t limit = 64) const;

  // Drains queued X events while input is inactive. true means a root
  // property changed and the caller must rescan windows.
  [[nodiscard]] bool poll_changes() noexcept;

  [[nodiscard]] std::optional<X11WindowSnapshot> find(std::uint64_t window) const;

private:
  const X11Display *display_{};
};

// Resolved /proc/<pid>/exe for PIDs owned by the effective UID (root may read
// any). Empty for every other case — the caller degrades to title/class.
[[nodiscard]] std::string process_executable_path(std::uint32_t pid);

// Executable-name-first matching (V2.5 parity): name, then advanced
// executable_path opt-in, then window class, then title substring. A profile
// with no identity field never matches. A previous window breaks background
// ties so losing focus does not change an existing session's target.
[[nodiscard]] const X11WindowSnapshot *
match_window(const X11WindowScan &scan, const layerc::core::GameMatch &match,
             std::uint64_t previous_window = 0);

} // namespace layerc::platform

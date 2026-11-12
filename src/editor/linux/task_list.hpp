#pragma once

#include "layerc/core/types.hpp"
#include "status_kind.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace layerc::editor {

// One top-level X11 window as Detected Tasks shows it; identity fields stay
// empty when the window manager or /proc does not publish them.
struct TaskCandidate {
  std::uint64_t window{};
  std::string title;
  std::string class_name;
  std::string executable_name;
  std::string executable_path;
  std::optional<std::uint32_t> pid;
  int width{};
  int height{};
  bool active{};
  bool hidden{};
  bool operator==(const TaskCandidate &) const = default;
};

// The editor's own windows are never listed. Without `show_all`, hidden and
// untitled windows are left out as on Windows.
[[nodiscard]] std::vector<TaskCandidate>
visible_tasks(std::vector<TaskCandidate> all, std::uint32_t own_pid, bool show_all);

// "executable<TAB>title<TAB>PID" for a tab-separated list row.
[[nodiscard]] std::string task_row_text(const TaskCandidate &task);
// Executable path, else the window class: the selected row's detail line.
[[nodiscard]] std::string task_detail_text(const TaskCandidate &task);

struct TaskApplied {
  StatusKind kind{StatusKind::information};
  bool applied{};
  std::string status;
};
// "Use selected task": points the profile's matcher at the window. The
// executable name (and path when `exact_path`) wins; a window whose process
// is not readable falls back to its class. A new, unsaved "New Profile"
// takes the executable's name and a matching ID.
[[nodiscard]] TaskApplied apply_task(layerc::core::GameProfile &profile, const TaskCandidate &task,
                                     bool exact_path, bool unsaved);

// The target window's frame, aspect-fitted and centred in a preview of
// `width` x `height` pixels. `scale` converts game pixels to preview pixels.
struct PlacementFrame {
  int x{};
  int y{};
  int width{};
  int height{};
  double scale{1.0};
  bool operator==(const PlacementFrame &) const = default;
};
[[nodiscard]] PlacementFrame placement_frame(int x, int y, int width, int height,
                                             int target_width, int target_height);
// LayerD's X11 overlay centres a panel on (anchor x width, anchor y height)
// of its target; these map that rule into and out of the frame.
struct PreviewPoint {
  int x{};
  int y{};
  bool operator==(const PreviewPoint &) const = default;
};
[[nodiscard]] PreviewPoint placed_origin(const PlacementFrame &frame, double anchor_x,
                                         double anchor_y, int panel_width, int panel_height);
struct Anchor {
  double x{};
  double y{};
};
[[nodiscard]] Anchor anchor_at(const PlacementFrame &frame, int x, int y);

} // namespace layerc::editor

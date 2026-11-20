#pragma once

#include "layerc/core/panel_presentation.hpp"

#include <cstdint>
#include <string>
#include <vector>

// How the local overlay paints one panel, for the editor's profile-faithful
// preview. Each platform mirrors its own overlay renderer, so the preview
// matches what the daemon draws there. Toolkit-free: the frontend scales
// and paints these operations.
namespace layerc::editor {

struct OverlayPreviewOp {
  enum class Kind : std::uint8_t { fill, stroke, text };
  Kind kind{Kind::fill};
  // Panel-local pixels at 96 dpi.
  int x{}, y{}, width{}, height{};
  // Corner radius in pixels; 0 is a square corner.
  int radius{};
  // 0xRRGGBB, opaque.
  std::uint32_t rgb{};
  // Text operations only: centred inside the rectangle.
  std::string text;
  int font_px{};

  bool operator==(const OverlayPreviewOp &) const = default;
};

enum class OverlayPreviewFace : std::uint8_t { monospace, interface_semibold };

struct OverlayPreviewPaint {
  OverlayPreviewFace face{OverlayPreviewFace::monospace};
  std::vector<OverlayPreviewOp> ops;
};

[[nodiscard]] OverlayPreviewPaint
overlay_preview_paint(const core::Panel &panel,
                      const std::vector<core::ItemPresentation> &items);

} // namespace layerc::editor

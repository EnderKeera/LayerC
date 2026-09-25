#pragma once

#include "layerc/capture/window_capture.hpp"
#include "layerc/core/template_matcher.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <windows.h>

namespace layerc::calibration {

class CapturePreview final {
public:
  void attach(HWND window) noexcept { window_ = window; }
  [[nodiscard]] HWND window() const noexcept { return window_; }

  void set_frame(std::shared_ptr<const layerc::capture::CaptureFrame> frame);
  void clear_frame();
  [[nodiscard]] const std::shared_ptr<const layerc::capture::CaptureFrame> &
  frame() const noexcept {
    return frame_;
  }
  [[nodiscard]] const std::vector<std::uint32_t> &pixels() const noexcept {
    return pixels_;
  }

  [[nodiscard]] RECT image_rect(std::uint32_t source_width = 0,
                                std::uint32_t source_height = 0) const;
  void begin_drag(POINT point) noexcept;
  void finish_drag() noexcept { dragging_ = false; }
  [[nodiscard]] bool dragging() const noexcept { return dragging_; }
  [[nodiscard]] POINT drag_start() const noexcept { return drag_start_; }
  void invalidate(bool erase = false) const noexcept;

private:
  HWND window_{};
  std::shared_ptr<const layerc::capture::CaptureFrame> frame_;
  std::vector<std::uint32_t> pixels_;
  bool dragging_{};
  POINT drag_start_{};
};

class MaskEditor final {
public:
  void begin(layerc::core::TemplateImage &image);
  void finish() noexcept;
  void clear() noexcept { finish(); }

  [[nodiscard]] bool editing() const noexcept { return editing_; }
  [[nodiscard]] bool changed() const noexcept { return changed_; }
  [[nodiscard]] bool brush_opaque() const noexcept { return brush_opaque_; }
  void set_brush_opaque(bool opaque) noexcept { brush_opaque_ = opaque; }
  [[nodiscard]] const std::vector<std::uint32_t> &pixels() const noexcept {
    return pixels_;
  }

  void refresh(const layerc::core::TemplateImage &image);
  [[nodiscard]] bool update(layerc::core::TemplateImage &image, POINT point,
                            RECT image_bounds, bool opaque);

private:
  bool editing_{};
  bool changed_{};
  bool brush_opaque_{};
  std::vector<std::uint32_t> pixels_;
};

} // namespace layerc::calibration

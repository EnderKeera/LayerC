#pragma once

#include "layerc/core/pixel_engine.hpp"

#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

namespace layerc::capture {

enum class OutputRotation : std::uint8_t {
  identity,
  rotate90,
  rotate180,
  rotate270,
};

struct PixelBounds {
  std::uint32_t left{};
  std::uint32_t top{};
  std::uint32_t right{};
  std::uint32_t bottom{};

  bool operator==(const PixelBounds &) const = default;
};

struct CaptureOptions {
  bool allow_desktop_duplication{true};
  core::CaptureQuality quality{core::CaptureQuality::balanced};
};

struct CaptureFrame {
  std::vector<core::RgbPixel> pixels;
  std::uint32_t width{};
  std::uint32_t height{};
  // Desktop Duplication can only supply the portion of a window on its chosen
  // output. WGC frames always report the full normalized region.
  core::NormalizedRegion available_region{0.0, 0.0, 1.0, 1.0};
  // Regions hidden because LayerC overlays occupy those screen pixels. Rules
  // intersecting one are unavailable rather than evaluated against synthetic
  // black pixels.
  std::vector<core::NormalizedRegion> unavailable_regions;
  core::TimePoint captured_at{};

  [[nodiscard]] core::PixelFrameView view() const noexcept {
    return {pixels, width, height, width};
  }
};

struct CapturePollResult {
  core::CaptureAvailability availability{core::CaptureAvailability::inactive};
  core::CaptureBackendKind backend{core::CaptureBackendKind::none};
  std::string diagnostic;
  std::shared_ptr<const CaptureFrame> frame;
};

// Hardware-only window capture. Construction is cheap; D3D11 and WinRT are
// initialized lazily by start(). If WGC cannot start, the implementation can
// fall back to Desktop Duplication when allowed.
class WindowCaptureSession {
public:
  WindowCaptureSession(HWND window, CaptureOptions options = {});
  ~WindowCaptureSession();
  WindowCaptureSession(WindowCaptureSession &&) noexcept;
  WindowCaptureSession &operator=(WindowCaptureSession &&) noexcept;
  WindowCaptureSession(const WindowCaptureSession &) = delete;
  WindowCaptureSession &operator=(const WindowCaptureSession &) = delete;

  [[nodiscard]] bool start();
  void stop();
  [[nodiscard]] CapturePollResult poll(core::TimePoint now,
                                       bool target_is_foreground);
  void set_excluded_screen_rects(std::span<const RECT> rectangles);
  void set_quality(core::CaptureQuality quality);

  [[nodiscard]] core::CaptureBackendKind backend() const noexcept;
  [[nodiscard]] std::string gpu_description() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool region_is_available(
    const core::NormalizedRegion &rule_region,
    const core::NormalizedRegion &available_region) noexcept;
[[nodiscard]] bool region_is_available(
    const core::NormalizedRegion &rule_region,
    const CaptureFrame &frame) noexcept;
[[nodiscard]] PixelBounds transform_output_crop(
    PixelBounds logical_crop, std::uint32_t logical_output_width,
    std::uint32_t logical_output_height, OutputRotation rotation) noexcept;
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
capture_sample_dimensions(std::uint32_t width, std::uint32_t height,
                          core::CaptureQuality quality) noexcept;
[[nodiscard]] bool frame_is_nearly_black(const CaptureFrame &frame) noexcept;

// One-shot screen snapshot used only when calibration's Desktop Duplication
// fallback cannot obtain an initial frame from an otherwise static desktop.
[[nodiscard]] std::shared_ptr<CaptureFrame> capture_static_window_snapshot(
    HWND window, core::CaptureQuality quality, core::TimePoint captured_at);

} // namespace layerc::capture

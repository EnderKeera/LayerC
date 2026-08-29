#pragma once

#include "layerc/capture/window_capture.hpp"
#include "layerc/core/calibration.hpp"
#include "layerc/core/template_matcher.hpp"
#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/native_theme.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace layerc::calibration {

class CalibrationWindow {
public:
  explicit CalibrationWindow(layerc::core::CalibrationRequest request);
  int run(HINSTANCE instance, int show_command);
  [[nodiscard]] const layerc::core::CalibrationResponse &response() const {
    return response_;
  }

private:
  struct Palette {
    COLORREF background{};
    COLORREF panel{};
    COLORREF input{};
    COLORREF border{};
    COLORREF text{};
    COLORREF muted{};
    COLORREF selection{};
  };

  static constexpr UINT_PTR capture_timer = 1;
  static INT_PTR CALLBACK dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK preview_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                       DWORD_PTR);
  INT_PTR handle_message(HWND, UINT, WPARAM, LPARAM);
  [[nodiscard]] bool handle_command(WPARAM wparam);
  LRESULT handle_preview(HWND, UINT, WPARAM, LPARAM);

  bool initialize(HWND);
  void initialize_palette();
  void initialize_controls();
  void update_control_enablement();
  void update_capture();
  void refresh_preview_frame();
  void begin_fallback_snapshot();
  void finish_fallback_snapshot(std::wstring_view status);
  void paint_preview(const DRAWITEMSTRUCT &draw);
  void update_region_from_pointer(POINT point, bool final);
  void sample_target_color();
  void capture_template_reference();
  void import_template_reference();
  void install_template_reference(const layerc::core::NormalizedRegion &selected);
  void clear_template_references();
  void toggle_template_mask_editing();
  [[nodiscard]] bool finish_template_mask_editing();
  void update_template_mask(POINT point, bool opaque);
  void refresh_template_mask_preview();
  void refresh_template_reference_label();
  void update_match_status(bool force = true);
  bool apply();

  [[nodiscard]] int integer_control(int id, int fallback, int minimum,
                                    int maximum) const;
  void set_control_text(int id, std::wstring_view text) const;
  [[nodiscard]] RECT preview_image_rect() const;
  [[nodiscard]] layerc::core::PixelRule current_rule_from_controls() const;
  [[nodiscard]] layerc::core::TemplateState
  current_template_from_controls() const;
  [[nodiscard]] bool template_mode() const;

  layerc::core::CalibrationRequest request_;
  layerc::core::CalibrationResponse response_;
  layerc::core::GameProfile profile_;
  std::optional<layerc::core::VisualProfile> existing_visual_;
  layerc::core::PixelRule rule_;
  layerc::core::TemplateState template_state_;
  layerc::core::VisualTrigger trigger_;
  std::uint8_t sample_rate_hz_{10};
  layerc::core::CaptureQuality quality_{layerc::core::CaptureQuality::balanced};
  std::uint32_t target_color_{};

  HINSTANCE instance_{};
  HWND dialog_{};
  HWND preview_{};
  HWND target_window_{};
  Palette palette_{};
  layerc::platform::NativeThemeColors button_colors_{};
  HBRUSH background_brush_{};
  HBRUSH panel_brush_{};
  HBRUSH input_brush_{};
  HFONT font_{};
  std::unique_ptr<layerc::capture::WindowCaptureSession> capture_;
  std::shared_ptr<const layerc::capture::CaptureFrame> frame_;
  std::vector<std::uint32_t> preview_pixels_;
  std::vector<std::uint32_t> mask_preview_pixels_;
  std::vector<layerc::core::TemplateImage> template_images_;
  layerc::core::CpuTemplateMatcher template_matcher_;
  ULONGLONG next_template_match_at_{};
  bool dragging_{};
  bool mask_editing_{};
  bool mask_changed_{};
  bool mask_opaque_{};
  POINT drag_start_{};
  bool fallback_snapshot_{};
  bool fallback_started_{};
  ULONGLONG fallback_deadline_{};
  HWND previous_foreground_{};
};

} // namespace layerc::calibration

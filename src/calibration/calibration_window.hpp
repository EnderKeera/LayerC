#pragma once

#include "calibration_mode.hpp"
#include "calibration_preview.hpp"
#include "visual_calibration.hpp"

#include "layerc/capture/window_capture.hpp"
#include "layerc/core/calibration.hpp"
#include "layerc/core/calibration_readiness.hpp"
#include "layerc/core/template_matcher.hpp"
#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/native_theme.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace layerc::platform {
struct ImportedTemplateAsset;
}

namespace layerc::calibration {

class CalibrationWindow {
public:
  explicit CalibrationWindow(layerc::core::CalibrationRequest request);
  int run(HINSTANCE instance, int show_command);
  [[nodiscard]] const layerc::core::CalibrationResponse &response() const {
    return response_;
  }

private:
  friend struct CalibrationWindowTestAccess;

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
  [[nodiscard]] std::optional<layerc::core::TemplateImage>
  captured_reference_image() const;
  [[nodiscard]] bool append_template_reference(
      const layerc::platform::ImportedTemplateAsset &asset);
  void clear_template_references();
  void toggle_template_mask_editing();
  [[nodiscard]] bool finish_template_mask_editing();
  void update_template_mask(POINT point, bool opaque);
  void refresh_template_reference_label();
  void update_match_status(bool force = true);
  [[nodiscard]] bool sampling_ready() const;
  void refresh_apply_readiness();
  bool apply();

  [[nodiscard]] int integer_control(int id, int fallback, int minimum,
                                    int maximum) const;
  void set_control_text(int id, std::wstring_view text) const;
  [[nodiscard]] RECT preview_image_rect() const;
  // The one adapter between the dialog and the shared model's controls.
  void write_controls(const layerc::editor::CalibrationControls &controls);
  [[nodiscard]] layerc::editor::CalibrationControls controls() const;
  [[nodiscard]] CalibrationMode mode() const;

  layerc::core::CalibrationRequest request_;
  layerc::core::CalibrationResponse response_;
  layerc::core::GameProfile profile_;
  std::optional<layerc::core::VisualProfile> existing_visual_;
  // The shared model owns the rule, template state, selection, trigger and
  // target colour; only the live capture quality is adapter state here.
  std::optional<layerc::editor::CalibrationModel> model_;
  layerc::core::CaptureQuality quality_{layerc::core::CaptureQuality::balanced};

  HINSTANCE instance_{};
  HWND dialog_{};
  CapturePreview preview_;
  HWND target_window_{};
  Palette palette_{};
  layerc::platform::NativeThemeColors button_colors_{};
  HBRUSH background_brush_{};
  HBRUSH panel_brush_{};
  HBRUSH input_brush_{};
  HFONT font_{};
  std::unique_ptr<layerc::capture::WindowCaptureSession> capture_;
  MaskEditor mask_editor_;
  std::vector<layerc::core::TemplateImage> template_images_;
  layerc::core::CpuTemplateMatcher template_matcher_;
  layerc::core::CalibrationReadiness readiness_;
  ULONGLONG next_template_match_at_{};
  bool fallback_snapshot_{};
  bool fallback_started_{};
  ULONGLONG fallback_deadline_{};
  HWND previous_foreground_{};
};

} // namespace layerc::calibration

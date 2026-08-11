#include "calibration_window.hpp"

#include "calibration_resource.h"

#include "layerc/core/profile.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets.hpp"
#include "layerc/platform/windows.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <variant>

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace layerc::calibration {
namespace {

constexpr COLORREF background = RGB(27, 31, 36);
constexpr COLORREF input = RGB(32, 37, 43);
constexpr COLORREF text = RGB(233, 228, 221);
constexpr COLORREF muted = RGB(156, 163, 173);
constexpr COLORREF accent = RGB(216, 142, 75);

std::wstring color_text(const std::uint32_t color) {
  wchar_t buffer[16]{};
  swprintf_s(buffer, L"#%06X", color & 0xFFFFFFU);
  return buffer;
}

bool contains_binding(const layerc::core::GameProfile &profile,
                      const std::string_view cooldown_id) {
  return std::ranges::any_of(profile.bindings, [&](const auto &binding) {
    return binding.cooldown_id == cooldown_id;
  });
}

layerc::core::NormalizedRegion expanded_template_search_region(
    const layerc::core::NormalizedRegion selected) {
  const auto expand = [](const double origin, const double extent) {
    const double expanded = std::min(1.0, extent * 2.0);
    const double center = origin + extent * 0.5;
    return std::pair{std::clamp(center - expanded * 0.5, 0.0,
                                1.0 - expanded),
                     expanded};
  };
  const auto [x, width] = expand(selected.x, selected.width);
  const auto [y, height] = expand(selected.y, selected.height);
  return {x, y, width, height};
}

} // namespace

CalibrationWindow::CalibrationWindow(layerc::core::CalibrationRequest request)
    : request_(std::move(request)) {}

int CalibrationWindow::run(const HINSTANCE instance, const int show_command) {
  (void)show_command;
  instance_ = instance;
  const auto result = DialogBoxParamW(
      instance_, MAKEINTRESOURCEW(IDD_CALIBRATION), nullptr, &dialog_proc,
      reinterpret_cast<LPARAM>(this));
  return result == -1 ? 1 : 0;
}

INT_PTR CALLBACK CalibrationWindow::dialog_proc(
    const HWND window, const UINT message, const WPARAM wparam,
    const LPARAM lparam) {
  auto *calibration = reinterpret_cast<CalibrationWindow *>(
      GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    calibration = reinterpret_cast<CalibrationWindow *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER,
                      reinterpret_cast<LONG_PTR>(calibration));
    return calibration->initialize(window) ? TRUE : FALSE;
  }
  return calibration == nullptr
             ? FALSE
             : calibration->handle_message(window, message, wparam, lparam);
}

LRESULT CALLBACK CalibrationWindow::preview_proc(
    const HWND window, const UINT message, const WPARAM wparam,
    const LPARAM lparam, const UINT_PTR subclass_id,
    const DWORD_PTR reference) {
  (void)subclass_id;
  auto *calibration = reinterpret_cast<CalibrationWindow *>(reference);
  if (calibration != nullptr) {
    const auto result = calibration->handle_preview(window, message, wparam,
                                                     lparam);
    if (result != 0 || message == WM_LBUTTONDOWN ||
        message == WM_RBUTTONDOWN || message == WM_MOUSEMOVE ||
        message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
        message == WM_CAPTURECHANGED)
      return result;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

bool CalibrationWindow::initialize(const HWND window) {
  dialog_ = window;
  target_window_ = reinterpret_cast<HWND>(
      static_cast<std::uintptr_t>(request_.target_window));
  auto parsed = layerc::core::parse_profile_json(request_.profile_json);
  const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
  if (profile == nullptr || !IsWindow(target_window_)) {
    response_.error = "calibration profile or target is invalid";
    EndDialog(dialog_, IDCANCEL);
    return false;
  }
  profile_ = *profile;
  auto matching_profile = profile_;
  matching_profile.enabled = true;
  const auto candidate =
      layerc::platform::describe_attachable_window(target_window_);
  if (!candidate.has_value() ||
      !layerc::platform::profile_matches_window(*candidate,
                                                matching_profile)) {
    response_.error = "calibration target does not match the selected profile";
    EndDialog(dialog_, IDCANCEL);
    return false;
  }
  const auto cooldown = std::ranges::find_if(
      profile_.cooldowns, [&](const auto &definition) {
        return definition.id == request_.cooldown_id;
      });
  if (cooldown == profile_.cooldowns.end()) {
    response_.error = "selected calibration cooldown is invalid";
    EndDialog(dialog_, IDCANCEL);
    return false;
  }

  if (!request_.visual_json.empty()) {
    auto visual = layerc::core::parse_visual_profile_json(
        request_.visual_json, profile_);
    if (const auto *value =
            std::get_if<layerc::core::VisualProfile>(&visual)) {
      existing_visual_ = *value;
      sample_rate_hz_ = value->sample_rate_hz;
      quality_ = value->quality;
    } else {
      response_.error = "existing visual sidecar is invalid";
      EndDialog(dialog_, IDCANCEL);
      return false;
    }
  }

  trigger_ = {.cooldown_id = cooldown->id,
              .observation_id = cooldown->id + "-visual",
              .enabled = true,
              .mode = layerc::core::VisualTriggerMode::pixel_only,
              .keep_input = false};
  rule_ = {.id = trigger_.observation_id,
            .name = cooldown->name + " Ready",
            .region = {0.4, 0.4, 0.2, 0.2},
            .predicate = layerc::core::AverageColorPredicate{0xFFFFFF, 16}};
  template_state_ = {
      .id = trigger_.observation_id,
      .name = cooldown->name + " Ready",
      .search_region = rule_.region,
      .expected_width = rule_.region.width,
      .expected_height = rule_.region.height};
  if (existing_visual_) {
    const auto assignment = std::ranges::find_if(
        existing_visual_->triggers, [&](const auto &candidate_trigger) {
          return candidate_trigger.cooldown_id == cooldown->id;
        });
    if (assignment != existing_visual_->triggers.end()) {
      trigger_ = *assignment;
      rule_.id = trigger_.observation_id;
      template_state_.id = trigger_.observation_id;
      if (const auto *existing_rule = layerc::core::find_pixel_rule(
              *existing_visual_, trigger_.observation_id)) {
        rule_ = *existing_rule;
      } else if (const auto *existing_template =
                     layerc::core::find_template_state(
                         *existing_visual_, trigger_.observation_id)) {
        template_state_ = *existing_template;
        rule_.region = template_state_.search_region;
        for (const auto &reference : template_state_.references) {
          auto loaded = layerc::platform::load_template_asset(
              reference.asset_sha256);
          if (auto *image =
                  std::get_if<layerc::core::TemplateImage>(&loaded))
            template_images_.push_back(std::move(*image));
        }
      }
    } else {
      std::string base = trigger_.observation_id;
      unsigned suffix = 2;
      while (layerc::core::has_observation(*existing_visual_,
                                           trigger_.observation_id))
        trigger_.observation_id = base + "-" + std::to_string(suffix++);
      rule_.id = trigger_.observation_id;
      template_state_.id = trigger_.observation_id;
    }
  }
  if (const auto *average =
          std::get_if<layerc::core::AverageColorPredicate>(&rule_.predicate))
    target_color_ = average->target_color;
  else if (const auto *coverage =
               std::get_if<layerc::core::ColorCoveragePredicate>(
                   &rule_.predicate))
    target_color_ = coverage->target_color;

  const BOOL dark = TRUE;
  (void)DwmSetWindowAttribute(dialog_, 20, &dark, sizeof(dark));
  background_brush_ = CreateSolidBrush(background);
  input_brush_ = CreateSolidBrush(input);
  const UINT dpi = GetDpiForWindow(dialog_);
  font_ = CreateFontW(-MulDiv(9, dpi == 0 ? 96 : static_cast<int>(dpi), 72),
                      0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  EnumChildWindows(
      dialog_,
      [](const HWND child, const LPARAM parameter) -> BOOL {
        SendMessageW(child, WM_SETFONT, parameter, TRUE);
        SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(font_));
  preview_ = GetDlgItem(dialog_, IDC_CAL_PREVIEW);
  SetWindowSubclass(preview_, &preview_proc, 1,
                    reinterpret_cast<DWORD_PTR>(this));
  initialize_controls();
  set_control_text(IDC_CAL_TARGET,
                   candidate->executable_name + L" — " + candidate->title);

  capture_ = std::make_unique<layerc::capture::WindowCaptureSession>(
      target_window_, layerc::capture::CaptureOptions{
                          .allow_desktop_duplication = true,
                          .quality = quality_});
  if (!capture_->start()) {
    set_control_text(IDC_CAL_BACKEND, L"Capture could not start.");
  }
  SetTimer(dialog_, capture_timer, 66, nullptr);
  return true;
}

void CalibrationWindow::initialize_controls() {
  const auto add = [&](const int id, const wchar_t *text_value) {
    SendDlgItemMessageW(dialog_, id, CB_ADDSTRING, 0,
                        reinterpret_cast<LPARAM>(text_value));
  };
  add(IDC_CAL_PREDICATE, L"Average color near target");
  add(IDC_CAL_PREDICATE, L"Brightness threshold");
  add(IDC_CAL_PREDICATE, L"Target color coverage");
  add(IDC_CAL_PREDICATE, L"Template match");
  add(IDC_CAL_COMPARISON, L"Above");
  add(IDC_CAL_COMPARISON, L"Below");
  add(IDC_CAL_REPEAT, L"Edge");
  add(IDC_CAL_REPEAT, L"When ready");
  add(IDC_CAL_TRIGGER_MODE, L"Pixel only");
  add(IDC_CAL_TRIGGER_MODE, L"Confirm input");
  add(IDC_CAL_UNAVAILABLE, L"Ignore input (fail closed)");
  add(IDC_CAL_UNAVAILABLE, L"Trigger from input");
  add(IDC_CAL_SAMPLE_RATE, L"5 Hz");
  add(IDC_CAL_SAMPLE_RATE, L"10 Hz");
  add(IDC_CAL_SAMPLE_RATE, L"15 Hz");
  add(IDC_CAL_QUALITY, L"Low");
  add(IDC_CAL_QUALITY, L"Balanced");
  add(IDC_CAL_QUALITY, L"Exact");

  int predicate_index{};
  int tolerance = 16;
  int threshold = 128;
  int comparison{};
  int minimum_percent = 50;
  if (!template_state_.references.empty()) {
    predicate_index = 3;
    tolerance = template_state_.color_tolerance;
  } else if (const auto *average =
          std::get_if<layerc::core::AverageColorPredicate>(&rule_.predicate)) {
    tolerance = average->tolerance;
  } else if (const auto *brightness =
                 std::get_if<layerc::core::BrightnessPredicate>(
                     &rule_.predicate)) {
    predicate_index = 1;
    threshold = brightness->threshold;
    comparison = brightness->comparison ==
                         layerc::core::BrightnessComparison::below
                     ? 1
                     : 0;
  } else if (const auto *coverage =
                 std::get_if<layerc::core::ColorCoveragePredicate>(
                     &rule_.predicate)) {
    predicate_index = 2;
    tolerance = coverage->tolerance;
    minimum_percent = static_cast<int>(
        std::lround(coverage->minimum_fraction * 100.0));
  }
  SendDlgItemMessageW(dialog_, IDC_CAL_PREDICATE, CB_SETCURSEL,
                      predicate_index, 0);
  SendDlgItemMessageW(dialog_, IDC_CAL_COMPARISON, CB_SETCURSEL, comparison, 0);
  SendDlgItemMessageW(dialog_, IDC_CAL_REPEAT, CB_SETCURSEL,
                       (predicate_index == 3 ? template_state_.repeat
                                             : rule_.repeat) ==
                               layerc::core::PixelRepeatPolicy::when_fully_ready
                          ? 1
                          : 0,
                      0);
  SendDlgItemMessageW(dialog_, IDC_CAL_TRIGGER_MODE, CB_SETCURSEL,
                      trigger_.mode ==
                              layerc::core::VisualTriggerMode::confirm_input
                          ? 1
                          : 0,
                      0);
  SendDlgItemMessageW(
      dialog_, IDC_CAL_UNAVAILABLE, CB_SETCURSEL,
      trigger_.unavailable_policy ==
              layerc::core::CaptureUnavailablePolicy::trigger_from_input
          ? 1
          : 0,
      0);
  SendDlgItemMessageW(dialog_, IDC_CAL_SAMPLE_RATE, CB_SETCURSEL,
                      sample_rate_hz_ == 5 ? 0 : sample_rate_hz_ == 15 ? 2 : 1,
                      0);
  SendDlgItemMessageW(
      dialog_, IDC_CAL_QUALITY, CB_SETCURSEL,
      quality_ == layerc::core::CaptureQuality::low
          ? 0
          : quality_ == layerc::core::CaptureQuality::exact ? 2 : 1,
      0);
  CheckDlgButton(dialog_, IDC_CAL_KEEP_INPUT,
                 trigger_.keep_input ? BST_CHECKED : BST_UNCHECKED);
  set_control_text(IDC_CAL_COLOR_VALUE, color_text(target_color_));
  set_control_text(IDC_CAL_TOLERANCE, std::to_wstring(tolerance));
  set_control_text(IDC_CAL_THRESHOLD, std::to_wstring(threshold));
  set_control_text(IDC_CAL_MINIMUM_PERCENT,
                   std::to_wstring(minimum_percent));
  set_control_text(IDC_CAL_STABLE_SAMPLES,
                   std::to_wstring(predicate_index == 3
                                      ? template_state_.stable_samples
                                      : rule_.stable_samples));
  set_control_text(
      IDC_CAL_CONFIDENCE,
      std::to_wstring(static_cast<int>(
          std::lround(template_state_.confidence_threshold * 100.0))));
  set_control_text(
      IDC_CAL_SCALE_TOLERANCE,
      std::to_wstring(static_cast<int>(
          std::lround(template_state_.scale_tolerance * 100.0))));
  set_control_text(IDC_CAL_TIMEOUT,
                   std::to_wstring(trigger_.confirmation_timeout.count()));
  refresh_template_reference_label();
  update_control_enablement();
}

void CalibrationWindow::update_control_enablement() {
  const auto predicate = static_cast<int>(SendDlgItemMessageW(
      dialog_, IDC_CAL_PREDICATE, CB_GETCURSEL, 0, 0));
  const bool color_predicate = predicate == 0 || predicate == 2;
  const bool template_predicate = predicate == 3;
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_PREDICATE), !mask_editing_);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_SAMPLE_COLOR), color_predicate);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_COLOR_VALUE), color_predicate);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_TOLERANCE), color_predicate);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_COMPARISON), predicate == 1);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_THRESHOLD), predicate == 1);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_MINIMUM_PERCENT), predicate == 2);
  constexpr std::array template_controls{
      IDC_CAL_TEMPLATE_GROUP, IDC_CAL_CAPTURE_TEMPLATE,
      IDC_CAL_IMPORT_TEMPLATE, IDC_CAL_TEMPLATE_REFERENCES,
      IDC_CAL_CONFIDENCE, IDC_CAL_SCALE_TOLERANCE,
      IDC_CAL_CLEAR_TEMPLATES, IDC_CAL_CONFIDENCE_LABEL,
      IDC_CAL_SCALE_LABEL, IDC_CAL_TEMPLATE_HELP,
      IDC_CAL_EDIT_TEMPLATE_MASK};
  for (const int id : template_controls)
    ShowWindow(GetDlgItem(dialog_, id),
               template_predicate ? SW_SHOW : SW_HIDE);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_CLEAR_TEMPLATES),
               template_predicate && !mask_editing_ &&
                   !template_state_.references.empty());
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_CAPTURE_TEMPLATE),
               template_predicate && !mask_editing_);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_IMPORT_TEMPLATE),
               template_predicate && !mask_editing_);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_EDIT_TEMPLATE_MASK),
               template_predicate &&
                   (mask_editing_ ||
                    (template_images_.size() ==
                         template_state_.references.size() &&
                     !template_images_.empty())));

  const bool confirmation =
      SendDlgItemMessageW(dialog_, IDC_CAL_TRIGGER_MODE, CB_GETCURSEL, 0, 0) ==
      1;
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_KEEP_INPUT), !confirmation);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_TIMEOUT), confirmation);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_UNAVAILABLE), TRUE);
}

INT_PTR CalibrationWindow::handle_message(const HWND window,
                                          const UINT message,
                                          const WPARAM wparam,
                                          const LPARAM lparam) {
  switch (message) {
  case WM_TIMER:
    if (wparam == capture_timer) {
      update_capture();
      return TRUE;
    }
    break;
  case WM_COMMAND: {
    const int id = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    if ((id == IDC_CAL_PREDICATE || id == IDC_CAL_TRIGGER_MODE) &&
        notification == CBN_SELCHANGE) {
      update_control_enablement();
      update_match_status();
      return TRUE;
    }
    if (id == IDC_CAL_QUALITY && notification == CBN_SELCHANGE) {
      const auto selection = SendDlgItemMessageW(
          dialog_, IDC_CAL_QUALITY, CB_GETCURSEL, 0, 0);
      quality_ = selection == 0
                     ? layerc::core::CaptureQuality::low
                     : selection == 2
                           ? layerc::core::CaptureQuality::exact
                           : layerc::core::CaptureQuality::balanced;
      if (capture_) capture_->set_quality(quality_);
      if (fallback_snapshot_ && capture_) {
        fallback_snapshot_ = false;
        frame_.reset();
        preview_pixels_.clear();
        begin_fallback_snapshot();
      }
      return TRUE;
    }
    if (id == IDC_CAL_SAMPLE_COLOR && notification == BN_CLICKED) {
      sample_target_color();
      return TRUE;
    }
    if (id == IDC_CAL_CAPTURE_TEMPLATE && notification == BN_CLICKED) {
      capture_template_reference();
      return TRUE;
    }
    if (id == IDC_CAL_IMPORT_TEMPLATE && notification == BN_CLICKED) {
      import_template_reference();
      return TRUE;
    }
    if (id == IDC_CAL_CLEAR_TEMPLATES && notification == BN_CLICKED) {
      clear_template_references();
      return TRUE;
    }
    if (id == IDC_CAL_EDIT_TEMPLATE_MASK && notification == BN_CLICKED) {
      toggle_template_mask_editing();
      return TRUE;
    }
    if (id == IDC_CAL_APPLY && notification == BN_CLICKED) {
      if (apply()) EndDialog(dialog_, IDOK);
      return TRUE;
    }
    if (id == IDCANCEL) {
      EndDialog(dialog_, IDCANCEL);
      return TRUE;
    }
    break;
  }
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlID == IDC_CAL_PREVIEW) {
      paint_preview(*draw);
      return TRUE;
    }
    break;
  }
  case WM_CTLCOLORDLG:
    return reinterpret_cast<INT_PTR>(background_brush_);
  case WM_CTLCOLORSTATIC: {
    const HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, text);
    SetBkColor(dc, background);
    return reinterpret_cast<INT_PTR>(background_brush_);
  }
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX: {
    const HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, text);
    SetBkColor(dc, input);
    return reinterpret_cast<INT_PTR>(input_brush_);
  }
  case WM_CLOSE:
    EndDialog(window, IDCANCEL);
    return TRUE;
  case WM_DESTROY:
    KillTimer(window, capture_timer);
    if (capture_) capture_->stop();
    if (preview_ != nullptr) RemoveWindowSubclass(preview_, &preview_proc, 1);
    if (font_ != nullptr) DeleteObject(font_);
    if (input_brush_ != nullptr) DeleteObject(input_brush_);
    if (background_brush_ != nullptr) DeleteObject(background_brush_);
    return TRUE;
  default:
    break;
  }
  return FALSE;
}

LRESULT CalibrationWindow::handle_preview(const HWND window,
                                          const UINT message,
                                          const WPARAM wparam,
                                          const LPARAM lparam) {
  (void)wparam;
  POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  switch (message) {
  case WM_LBUTTONDOWN:
  case WM_RBUTTONDOWN:
    if (mask_editing_) {
      dragging_ = true;
      mask_opaque_ = message == WM_RBUTTONDOWN;
      SetCapture(window);
      update_template_mask(point, mask_opaque_);
      return 0;
    }
    if (message == WM_RBUTTONDOWN) return 0;
    dragging_ = true;
    drag_start_ = point;
    SetCapture(window);
    update_region_from_pointer(point, false);
    return 0;
  case WM_MOUSEMOVE:
    if (dragging_) {
      if (mask_editing_)
        update_template_mask(point, mask_opaque_);
      else
        update_region_from_pointer(point, false);
    }
    return 0;
  case WM_LBUTTONUP:
  case WM_RBUTTONUP:
    if (dragging_) {
      if (mask_editing_)
        update_template_mask(point, mask_opaque_);
      else if (message == WM_LBUTTONUP)
        update_region_from_pointer(point, true);
      dragging_ = false;
      if (GetCapture() == window) ReleaseCapture();
    }
    return 0;
  case WM_CAPTURECHANGED:
    dragging_ = false;
    return 0;
  default:
    return 0;
  }
}

void CalibrationWindow::update_capture() {
  if (!capture_ || fallback_snapshot_) return;
  if (fallback_started_) {
    const HWND foreground_root =
        GetAncestor(GetForegroundWindow(), GA_ROOT);
    const HWND target_root = GetAncestor(target_window_, GA_ROOT);
    if (foreground_root == nullptr || foreground_root != target_root) {
      if (GetTickCount64() >= fallback_deadline_) {
        frame_.reset();
        finish_fallback_snapshot(
            L"Desktop Duplication fallback stopped because the target was not foreground.");
      }
      return;
    }
    const auto captured = capture_->poll(layerc::core::MonotonicClock::now(),
                                         true);
    if (captured.frame) {
      frame_ = captured.frame;
      finish_fallback_snapshot(
          L"Desktop Duplication fallback — frozen foreground snapshot.");
    } else if (GetTickCount64() >= fallback_deadline_) {
      frame_ = layerc::capture::capture_static_window_snapshot(
          target_window_, quality_, layerc::core::MonotonicClock::now());
      finish_fallback_snapshot(
          frame_ ? L"Desktop Duplication was static — frozen screen snapshot."
                 : L"Desktop Duplication fallback could not capture the target.");
    }
    return;
  }

  const auto captured = capture_->poll(layerc::core::MonotonicClock::now(),
                                       false);
  if (captured.backend ==
      layerc::core::CaptureBackendKind::desktop_duplication) {
    begin_fallback_snapshot();
    return;
  }
  if (captured.frame) frame_ = captured.frame;
  if (captured.availability == layerc::core::CaptureAvailability::available)
    set_control_text(IDC_CAL_BACKEND, L"Windows Graphics Capture — live preview");
  else if (!captured.diagnostic.empty())
    set_control_text(IDC_CAL_BACKEND,
                     layerc::platform::utf8_to_wide(captured.diagnostic));
  refresh_preview_frame();
}

void CalibrationWindow::refresh_preview_frame() {
  if (!frame_) return;
  preview_pixels_.resize(frame_->pixels.size());
  for (std::size_t index = 0; index < frame_->pixels.size(); ++index) {
    const auto pixel = frame_->pixels[index];
    preview_pixels_[index] = pixel.blue |
                             (static_cast<std::uint32_t>(pixel.green) << 8U) |
                             (static_cast<std::uint32_t>(pixel.red) << 16U);
  }
  InvalidateRect(preview_, nullptr, FALSE);
  update_match_status(false);
}

void CalibrationWindow::begin_fallback_snapshot() {
  fallback_started_ = true;
  fallback_deadline_ = GetTickCount64() + 2'000;
  previous_foreground_ = GetForegroundWindow();
  set_control_text(IDC_CAL_BACKEND,
                   L"Desktop Duplication fallback — capturing foreground snapshot...");
  ShowWindow(dialog_, SW_HIDE);
  ShowWindow(target_window_, SW_RESTORE);
  SetForegroundWindow(target_window_);
  RedrawWindow(target_window_, nullptr, nullptr,
               RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void CalibrationWindow::finish_fallback_snapshot(
    const std::wstring_view status) {
  fallback_started_ = false;
  fallback_snapshot_ = true;
  const HWND restore =
      previous_foreground_ != nullptr && IsWindow(previous_foreground_)
          ? previous_foreground_
          : dialog_;
  ShowWindow(dialog_, restore == dialog_ ? SW_SHOW : SW_SHOWNOACTIVATE);
  SetForegroundWindow(restore);
  previous_foreground_ = nullptr;
  set_control_text(IDC_CAL_BACKEND, status);
  refresh_preview_frame();
}

void CalibrationWindow::paint_preview(const DRAWITEMSTRUCT &draw) {
  FillRect(draw.hDC, &draw.rcItem, input_brush_);
  const bool mask_preview =
      mask_editing_ && !template_images_.empty() &&
      !mask_preview_pixels_.empty();
  if (!mask_preview && (!frame_ || preview_pixels_.empty())) {
    SetTextColor(draw.hDC, muted);
    SetBkMode(draw.hDC, TRANSPARENT);
    DrawTextW(draw.hDC, L"Waiting for captured pixels...", -1,
              const_cast<RECT *>(&draw.rcItem),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }
  const auto image = preview_image_rect();
  const auto width =
      mask_preview ? template_images_.back().width : frame_->width;
  const auto height =
      mask_preview ? template_images_.back().height : frame_->height;
  const auto *pixels = mask_preview ? mask_preview_pixels_.data()
                                    : preview_pixels_.data();
  BITMAPINFO bitmap{};
  bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap.bmiHeader.biWidth = static_cast<LONG>(width);
  bitmap.bmiHeader.biHeight = -static_cast<LONG>(height);
  bitmap.bmiHeader.biPlanes = 1;
  bitmap.bmiHeader.biBitCount = 32;
  bitmap.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(draw.hDC, HALFTONE);
  StretchDIBits(draw.hDC, image.left, image.top, image.right - image.left,
                image.bottom - image.top, 0, 0, width, height, pixels, &bitmap,
                DIB_RGB_COLORS, SRCCOPY);

  if (mask_preview) return;

  RECT region{
      image.left + static_cast<LONG>(
                       std::lround(rule_.region.x * (image.right - image.left))),
      image.top + static_cast<LONG>(
                      std::lround(rule_.region.y * (image.bottom - image.top))),
      image.left + static_cast<LONG>(std::lround(
                       (rule_.region.x + rule_.region.width) *
                       (image.right - image.left))),
      image.top + static_cast<LONG>(std::lround(
                      (rule_.region.y + rule_.region.height) *
                      (image.bottom - image.top)))};
  const HPEN pen = CreatePen(PS_SOLID, 2, accent);
  const auto old_pen = SelectObject(draw.hDC, pen);
  const auto old_brush = SelectObject(draw.hDC, GetStockObject(HOLLOW_BRUSH));
  Rectangle(draw.hDC, region.left, region.top, region.right, region.bottom);
  SelectObject(draw.hDC, old_brush);
  SelectObject(draw.hDC, old_pen);
  DeleteObject(pen);
}

RECT CalibrationWindow::preview_image_rect() const {
  RECT client{};
  GetClientRect(preview_, &client);
  const auto source_width = mask_editing_ && !template_images_.empty()
                                ? template_images_.back().width
                                : frame_ ? frame_->width : 0;
  const auto source_height = mask_editing_ && !template_images_.empty()
                                 ? template_images_.back().height
                                 : frame_ ? frame_->height : 0;
  if (source_width == 0 || source_height == 0) return client;
  const double frame_aspect =
      static_cast<double>(source_width) / source_height;
  const double client_aspect =
      static_cast<double>(client.right) / std::max<LONG>(1, client.bottom);
  if (client_aspect > frame_aspect) {
    const auto display_width =
        static_cast<LONG>(std::lround(client.bottom * frame_aspect));
    const auto left = (client.right - display_width) / 2;
    return {left, 0, left + display_width, client.bottom};
  }
  const auto display_height =
      static_cast<LONG>(std::lround(client.right / frame_aspect));
  const auto top = (client.bottom - display_height) / 2;
  return {0, top, client.right, top + display_height};
}

void CalibrationWindow::update_region_from_pointer(POINT point,
                                                   const bool final) {
  const auto image = preview_image_rect();
  const auto clamp_point = [&](POINT value) {
    value.x = std::clamp(value.x, image.left, image.right);
    value.y = std::clamp(value.y, image.top, image.bottom);
    return value;
  };
  const auto start = clamp_point(drag_start_);
  point = clamp_point(point);
  const auto width = std::max<LONG>(1, image.right - image.left);
  const auto height = std::max<LONG>(1, image.bottom - image.top);
  const auto left = std::min(start.x, point.x);
  const auto top = std::min(start.y, point.y);
  const auto right = std::max(start.x, point.x);
  const auto bottom = std::max(start.y, point.y);
  if (right - left < 2 || bottom - top < 2) return;
  rule_.region = {
      static_cast<double>(left - image.left) / width,
      static_cast<double>(top - image.top) / height,
      static_cast<double>(right - left) / width,
      static_cast<double>(bottom - top) / height};
  if (template_mode()) template_state_.search_region = rule_.region;
  InvalidateRect(preview_, nullptr, FALSE);
  update_match_status(final);
}

void CalibrationWindow::sample_target_color() {
  if (!frame_) {
    MessageBoxW(dialog_, L"Wait for a captured frame first.", L"LayerC",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  auto sample = rule_;
  sample.predicate = layerc::core::AverageColorPredicate{0, 255};
  const auto evaluation = layerc::core::evaluate_pixel_rule(
      sample, frame_->view(), quality_);
  if (evaluation.sampled_pixels == 0) return;
  target_color_ = evaluation.average_color;
  set_control_text(IDC_CAL_COLOR_VALUE, color_text(target_color_));
  update_match_status();
}

bool CalibrationWindow::template_mode() const {
  return dialog_ != nullptr &&
         SendDlgItemMessageW(dialog_, IDC_CAL_PREDICATE, CB_GETCURSEL, 0, 0) ==
             3;
}

layerc::core::TemplateState
CalibrationWindow::current_template_from_controls() const {
  auto state = template_state_;
  state.search_region = rule_.region;
  state.confidence_threshold =
      integer_control(IDC_CAL_CONFIDENCE, 90, 0, 100) / 100.0;
  state.scale_tolerance =
      integer_control(IDC_CAL_SCALE_TOLERANCE, 5, 0, 20) / 100.0;
  state.color_tolerance = static_cast<std::uint8_t>(
      integer_control(IDC_CAL_TOLERANCE, 24, 0, 255));
  state.stable_samples = static_cast<std::uint8_t>(
      integer_control(IDC_CAL_STABLE_SAMPLES, 2, 1, 15));
  state.repeat =
      SendDlgItemMessageW(dialog_, IDC_CAL_REPEAT, CB_GETCURSEL, 0, 0) == 1
          ? layerc::core::PixelRepeatPolicy::when_fully_ready
          : layerc::core::PixelRepeatPolicy::edge;
  return state;
}

void CalibrationWindow::refresh_template_reference_label() {
  std::wstring label =
      std::to_wstring(template_state_.references.size()) + L" / " +
      std::to_wstring(layerc::core::max_template_references) +
      L" references";
  if (template_images_.size() != template_state_.references.size())
    label += L" (one or more assets unavailable)";
  set_control_text(IDC_CAL_TEMPLATE_REFERENCES, label);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_CLEAR_TEMPLATES),
               !template_state_.references.empty());
}

void CalibrationWindow::capture_template_reference() {
  if (!frame_) {
    MessageBoxW(dialog_, L"Wait for a captured frame first.", L"LayerC",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (template_state_.references.size() >=
      layerc::core::max_template_references) {
    MessageBoxW(dialog_, L"A template state supports at most four references.",
                L"Template reference limit", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto &region = rule_.region;
  const auto left = static_cast<std::uint32_t>(std::clamp(
      std::floor(region.x * frame_->width), 0.0,
      static_cast<double>(frame_->width)));
  const auto top = static_cast<std::uint32_t>(std::clamp(
      std::floor(region.y * frame_->height), 0.0,
      static_cast<double>(frame_->height)));
  const auto right = static_cast<std::uint32_t>(std::clamp(
      std::ceil((region.x + region.width) * frame_->width), 0.0,
      static_cast<double>(frame_->width)));
  const auto bottom = static_cast<std::uint32_t>(std::clamp(
      std::ceil((region.y + region.height) * frame_->height), 0.0,
      static_cast<double>(frame_->height)));
  if (right <= left || bottom <= top ||
      static_cast<std::uint64_t>(right - left) * (bottom - top) < 64U) {
    MessageBoxW(dialog_,
                L"Select a region containing at least 64 captured pixels.",
                L"Template region too small", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto source_width = right - left;
  const auto source_height = bottom - top;
  const double factor =
      std::min(1.0, 512.0 / std::max(source_width, source_height));
  layerc::core::TemplateImage image;
  image.width = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(source_width * factor)));
  image.height = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(source_height * factor)));
  image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
  image.alpha.assign(image.pixels.size(), 255);
  for (std::uint32_t y = 0; y < image.height; ++y) {
    const auto source_y =
        top + std::min(source_height - 1,
                       static_cast<std::uint32_t>(
                           (static_cast<std::uint64_t>(y) * 2U + 1U) *
                           source_height /
                           (static_cast<std::uint64_t>(image.height) * 2U)));
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const auto source_x =
          left + std::min(source_width - 1,
                          static_cast<std::uint32_t>(
                              (static_cast<std::uint64_t>(x) * 2U + 1U) *
                              source_width /
                              (static_cast<std::uint64_t>(image.width) * 2U)));
      image.pixels[static_cast<std::size_t>(y) * image.width + x] =
          frame_->pixels[static_cast<std::size_t>(source_y) * frame_->width +
                         source_x];
    }
  }
  auto stored = layerc::platform::store_template_asset(image);
  const auto *asset =
      std::get_if<layerc::platform::ImportedTemplateAsset>(&stored);
  if (asset == nullptr) {
    const auto &error =
        std::get<layerc::platform::VisualAssetError>(stored);
    MessageBoxW(dialog_,
                layerc::platform::utf8_to_wide(error.message).c_str(),
                L"Template capture failed", MB_OK | MB_ICONERROR);
    return;
  }
  if (std::ranges::none_of(template_state_.references, [&](const auto &item) {
        return item.asset_sha256 == asset->reference.asset_sha256;
      })) {
    template_state_.references.push_back(asset->reference);
    template_images_.push_back(asset->image);
  }
  template_state_.expected_width = region.width;
  template_state_.expected_height = region.height;
  template_state_.search_region = expanded_template_search_region(region);
  rule_.region = template_state_.search_region;
  template_matcher_.clear();
  refresh_template_reference_label();
  InvalidateRect(preview_, nullptr, FALSE);
  update_match_status();
}

void CalibrationWindow::import_template_reference() {
  if (template_state_.references.size() >=
      layerc::core::max_template_references) {
    MessageBoxW(dialog_, L"A template state supports at most four references.",
                L"Template reference limit", MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::array<wchar_t, 32'768> path{};
  OPENFILENAMEW picker{};
  picker.lStructSize = sizeof(OPENFILENAMEW);
  picker.hwndOwner = dialog_;
  picker.lpstrFilter = L"PNG images (*.png)\0*.png\0\0";
  picker.lpstrFile = path.data();
  picker.nMaxFile = static_cast<DWORD>(path.size());
  picker.lpstrDefExt = L"png";
  picker.Flags =
      OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT;
  if (GetOpenFileNameW(&picker) == FALSE) return;
  auto imported =
      layerc::platform::import_template_asset(std::filesystem::path(path.data()));
  const auto *asset =
      std::get_if<layerc::platform::ImportedTemplateAsset>(&imported);
  if (asset == nullptr) {
    const auto &error =
        std::get<layerc::platform::VisualAssetError>(imported);
    MessageBoxW(dialog_,
                layerc::platform::utf8_to_wide(error.message).c_str(),
                L"Template import failed", MB_OK | MB_ICONERROR);
    return;
  }
  if (std::ranges::none_of(template_state_.references, [&](const auto &item) {
        return item.asset_sha256 == asset->reference.asset_sha256;
      })) {
    template_state_.references.push_back(asset->reference);
    template_images_.push_back(asset->image);
  }
  const auto selected = rule_.region;
  template_state_.expected_width = selected.width;
  template_state_.expected_height = selected.height;
  template_state_.search_region = expanded_template_search_region(selected);
  rule_.region = template_state_.search_region;
  template_matcher_.clear();
  refresh_template_reference_label();
  update_match_status();
}

void CalibrationWindow::clear_template_references() {
  mask_editing_ = false;
  mask_changed_ = false;
  mask_preview_pixels_.clear();
  template_state_.references.clear();
  template_images_.clear();
  template_matcher_.clear();
  refresh_template_reference_label();
  update_match_status();
}

void CalibrationWindow::toggle_template_mask_editing() {
  if (mask_editing_) {
    (void)finish_template_mask_editing();
    return;
  }
  if (template_images_.empty() ||
      template_images_.size() != template_state_.references.size()) {
    MessageBoxW(dialog_, L"All template references must be available first.",
                L"Mask unavailable", MB_OK | MB_ICONINFORMATION);
    return;
  }
  mask_editing_ = true;
  mask_changed_ = false;
  SetDlgItemTextW(dialog_, IDC_CAL_EDIT_TEMPLATE_MASK, L"Done");
  refresh_template_mask_preview();
  update_control_enablement();
  update_match_status();
}

bool CalibrationWindow::finish_template_mask_editing() {
  if (!mask_editing_) return true;
  auto &image = template_images_.back();
  if (!image.valid()) {
    MessageBoxW(dialog_,
                L"Keep at least 64 opaque pixels in the template mask.",
                L"Mask is too small", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (mask_changed_) {
    auto stored = layerc::platform::store_template_asset(image);
    const auto *asset =
        std::get_if<layerc::platform::ImportedTemplateAsset>(&stored);
    if (asset == nullptr) {
      const auto &error =
          std::get<layerc::platform::VisualAssetError>(stored);
      MessageBoxW(dialog_,
                  layerc::platform::utf8_to_wide(error.message).c_str(),
                  L"Mask save failed", MB_OK | MB_ICONERROR);
      return false;
    }
    const bool duplicates_earlier = std::ranges::any_of(
        template_state_.references.begin(),
        template_state_.references.end() - 1, [&](const auto &reference) {
          return reference.asset_sha256 == asset->reference.asset_sha256;
        });
    if (duplicates_earlier) {
      MessageBoxW(dialog_,
                  L"This mask duplicates an earlier reference. Change it or clear the duplicate reference.",
                  L"Duplicate template", MB_OK | MB_ICONWARNING);
      return false;
    }
    template_state_.references.back() = asset->reference;
    image = asset->image;
    template_matcher_.clear();
  }
  mask_editing_ = false;
  mask_changed_ = false;
  mask_preview_pixels_.clear();
  SetDlgItemTextW(dialog_, IDC_CAL_EDIT_TEMPLATE_MASK, L"Mask...");
  refresh_template_reference_label();
  update_control_enablement();
  InvalidateRect(preview_, nullptr, FALSE);
  refresh_preview_frame();
  return true;
}

void CalibrationWindow::refresh_template_mask_preview() {
  if (!mask_editing_ || template_images_.empty()) return;
  const auto &image = template_images_.back();
  mask_preview_pixels_.resize(image.pixels.size());
  for (std::uint32_t y = 0; y < image.height; ++y) {
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const auto index = static_cast<std::size_t>(y) * image.width + x;
      const auto pixel = image.pixels[index];
      if (image.alpha[index] == 0) {
        const auto checker = ((x / 12U) + (y / 12U)) % 2U == 0U
                                 ? 0x3A3F46U
                                 : 0x24282DU;
        mask_preview_pixels_[index] = checker;
      } else {
        mask_preview_pixels_[index] =
            pixel.blue | (static_cast<std::uint32_t>(pixel.green) << 8U) |
            (static_cast<std::uint32_t>(pixel.red) << 16U);
      }
    }
  }
  InvalidateRect(preview_, nullptr, FALSE);
}

void CalibrationWindow::update_template_mask(POINT point,
                                             const bool opaque) {
  if (!mask_editing_ || template_images_.empty()) return;
  const auto bounds = preview_image_rect();
  if (point.x < bounds.left || point.x >= bounds.right ||
      point.y < bounds.top || point.y >= bounds.bottom)
    return;
  auto &image = template_images_.back();
  const auto x = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(point.x - bounds.left) * image.width /
      std::max<LONG>(1, bounds.right - bounds.left));
  const auto y = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(point.y - bounds.top) * image.height /
      std::max<LONG>(1, bounds.bottom - bounds.top));
  const int radius = std::max<int>(1, std::min(image.width, image.height) / 32);
  for (int offset_y = -radius; offset_y <= radius; ++offset_y) {
    for (int offset_x = -radius; offset_x <= radius; ++offset_x) {
      if (offset_x * offset_x + offset_y * offset_y > radius * radius)
        continue;
      const int target_x = static_cast<int>(x) + offset_x;
      const int target_y = static_cast<int>(y) + offset_y;
      if (target_x < 0 || target_y < 0 ||
          target_x >= static_cast<int>(image.width) ||
          target_y >= static_cast<int>(image.height))
        continue;
      image.alpha[static_cast<std::size_t>(target_y) * image.width +
                  target_x] = opaque ? 255 : 0;
    }
  }
  mask_changed_ = true;
  refresh_template_mask_preview();
}

layerc::core::PixelRule
CalibrationWindow::current_rule_from_controls() const {
  auto rule = rule_;
  rule.stable_samples = static_cast<std::uint8_t>(integer_control(
      IDC_CAL_STABLE_SAMPLES, rule.stable_samples, 1, 15));
  rule.repeat = SendDlgItemMessageW(dialog_, IDC_CAL_REPEAT, CB_GETCURSEL, 0,
                                    0) == 1
                    ? layerc::core::PixelRepeatPolicy::when_fully_ready
                    : layerc::core::PixelRepeatPolicy::edge;
  const auto predicate = SendDlgItemMessageW(dialog_, IDC_CAL_PREDICATE,
                                              CB_GETCURSEL, 0, 0);
  if (predicate == 1) {
    rule.predicate = layerc::core::BrightnessPredicate{
        static_cast<std::uint8_t>(
            integer_control(IDC_CAL_THRESHOLD, 128, 0, 255)),
        SendDlgItemMessageW(dialog_, IDC_CAL_COMPARISON, CB_GETCURSEL, 0, 0) == 1
            ? layerc::core::BrightnessComparison::below
            : layerc::core::BrightnessComparison::above};
  } else if (predicate == 2) {
    rule.predicate = layerc::core::ColorCoveragePredicate{
        target_color_,
        static_cast<std::uint8_t>(
            integer_control(IDC_CAL_TOLERANCE, 16, 0, 255)),
        integer_control(IDC_CAL_MINIMUM_PERCENT, 50, 0, 100) / 100.0};
  } else {
    rule.predicate = layerc::core::AverageColorPredicate{
        target_color_, static_cast<std::uint8_t>(
                           integer_control(IDC_CAL_TOLERANCE, 16, 0, 255))};
  }
  return rule;
}

void CalibrationWindow::update_match_status(const bool force) {
  if (mask_editing_) {
    set_control_text(IDC_CAL_MATCH,
                     L"Editing alpha mask — left drag ignores pixels; right drag restores them.");
    return;
  }
  if (!frame_) return;
  if (template_mode()) {
    if (template_state_.references.empty()) {
      set_control_text(
          IDC_CAL_MATCH,
          L"Capture or import at least one template reference.");
      return;
    }
    if (template_images_.size() != template_state_.references.size()) {
      set_control_text(
          IDC_CAL_MATCH,
          L"One or more referenced template assets are unavailable.");
      return;
    }
    const auto now = GetTickCount64();
    if (!force && now < next_template_match_at_) return;
    // Exact can consume the full 8M-comparison allowance. Keep that bounded
    // evaluation off the 15 Hz paint/capture cadence so the modal UI remains
    // responsive while still refreshing live status twice per second.
    next_template_match_at_ = now + 500;
    const auto state = current_template_from_controls();
    const auto result = template_matcher_.evaluate(
        state, template_images_, frame_->view(), quality_);
    std::wstring status =
        result.matched ? L"TEMPLATE MATCHED" : L"Template not matched";
    status += L"\nConfidence " +
              std::to_wstring(static_cast<int>(
                  std::lround(result.confidence * 100.0))) +
              L"%, candidates " +
              std::to_wstring(result.candidates_evaluated);
    set_control_text(IDC_CAL_MATCH, status);
    return;
  }
  const auto rule = current_rule_from_controls();
  const auto result = layerc::core::evaluate_pixel_rule(
      rule, frame_->view(), quality_);
  std::wstring status = result.matched ? L"MATCHED now" : L"Not matched now";
  status += L"\nAverage " + color_text(result.average_color) +
            L", brightness " +
            std::to_wstring(static_cast<int>(std::lround(
                result.average_brightness))) +
            L", coverage " +
            std::to_wstring(static_cast<int>(
                std::lround(result.color_coverage * 100.0))) +
            L"%";
  set_control_text(IDC_CAL_MATCH, status);
}

bool CalibrationWindow::apply() {
  if (!finish_template_mask_editing()) return false;
  const bool use_template = template_mode();
  if (use_template) {
    template_state_ = current_template_from_controls();
    template_state_.id = trigger_.observation_id;
    if (template_state_.references.empty()) {
      MessageBoxW(dialog_,
                  L"Capture or import at least one template reference first.",
                  L"Template reference required",
                  MB_OK | MB_ICONINFORMATION);
      return false;
    }
    if (template_images_.size() != template_state_.references.size() ||
        !std::ranges::all_of(template_images_,
                             layerc::core::template_image_has_structure)) {
      MessageBoxW(dialog_,
                  L"Every referenced template asset must be available and contain visible visual structure.",
                  L"Template assets unavailable", MB_OK | MB_ICONWARNING);
      return false;
    }
  } else {
    rule_ = current_rule_from_controls();
    rule_.id = trigger_.observation_id;
  }
  const auto rate_index = SendDlgItemMessageW(dialog_, IDC_CAL_SAMPLE_RATE,
                                               CB_GETCURSEL, 0, 0);
  sample_rate_hz_ = rate_index == 0 ? 5 : rate_index == 2 ? 15 : 10;
  const auto quality_index = SendDlgItemMessageW(dialog_, IDC_CAL_QUALITY,
                                                  CB_GETCURSEL, 0, 0);
  quality_ = quality_index == 0
                 ? layerc::core::CaptureQuality::low
                 : quality_index == 2
                       ? layerc::core::CaptureQuality::exact
                       : layerc::core::CaptureQuality::balanced;
  if (quality_ == layerc::core::CaptureQuality::exact &&
      MessageBoxW(dialog_,
                  L"Exact retains full-resolution pixel and template data and is not covered by the V2.1 Low/Balanced CPU/memory budget. Continue?",
                  L"Exact quality", MB_YESNO | MB_ICONWARNING) != IDYES)
    return false;

  trigger_.enabled = true;
  trigger_.mode = SendDlgItemMessageW(dialog_, IDC_CAL_TRIGGER_MODE,
                                       CB_GETCURSEL, 0, 0) == 1
                      ? layerc::core::VisualTriggerMode::confirm_input
                      : layerc::core::VisualTriggerMode::pixel_only;
  trigger_.keep_input =
      IsDlgButtonChecked(dialog_, IDC_CAL_KEEP_INPUT) == BST_CHECKED;
  trigger_.confirmation_timeout = layerc::core::Milliseconds(integer_control(
      IDC_CAL_TIMEOUT, 500, 50, 5'000));
  trigger_.unavailable_policy =
      SendDlgItemMessageW(dialog_, IDC_CAL_UNAVAILABLE, CB_GETCURSEL, 0, 0) == 1
          ? layerc::core::CaptureUnavailablePolicy::trigger_from_input
          : layerc::core::CaptureUnavailablePolicy::ignore;
  if (trigger_.mode == layerc::core::VisualTriggerMode::confirm_input &&
      !contains_binding(profile_, trigger_.cooldown_id)) {
    MessageBoxW(dialog_,
                L"Input confirmation requires a captured input binding for this cooldown.",
                L"LayerC", MB_OK | MB_ICONWARNING);
    return false;
  }

  layerc::core::VisualProfile result;
  result.profile_id = profile_.id;
  result.sample_rate_hz = sample_rate_hz_;
  result.quality = quality_;
  if (use_template)
    result.templates.push_back(template_state_);
  else
    result.rules.push_back(rule_);
  result.triggers.push_back(trigger_);
  const auto validation =
      layerc::core::validate_visual_profile(result, profile_);
  if (!validation.message.empty()) {
    MessageBoxW(dialog_,
                layerc::platform::utf8_to_wide(validation.message).c_str(),
                L"Visual rule is invalid", MB_OK | MB_ICONWARNING);
    return false;
  }
  response_.applied = true;
  response_.visual_json =
      layerc::core::serialize_visual_profile_json(result);
  return true;
}

int CalibrationWindow::integer_control(const int id, const int fallback,
                                       const int minimum,
                                       const int maximum) const {
  BOOL translated{};
  const auto value = GetDlgItemInt(dialog_, id, &translated, FALSE);
  return translated ? std::clamp<int>(static_cast<int>(value), minimum, maximum)
                    : fallback;
}

std::wstring CalibrationWindow::control_text(const int id) const {
  const HWND control = GetDlgItem(dialog_, id);
  const int length = GetWindowTextLengthW(control);
  std::wstring output(static_cast<std::size_t>(std::max(0, length)) + 1U,
                      L'\0');
  if (length != 0) GetWindowTextW(control, output.data(), length + 1);
  output.resize(static_cast<std::size_t>(std::max(0, length)));
  return output;
}

void CalibrationWindow::set_control_text(const int id,
                                         const std::wstring_view value) const {
  SetDlgItemTextW(dialog_, id, std::wstring(value).c_str());
}

} // namespace layerc::calibration

#include "calibration_window.hpp"

#include "calibration_resource.h"

#include "layerc/core/profile.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/windows.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <variant>

#include <commctrl.h>
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
    if (result != 0 || message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE ||
        message == WM_LBUTTONUP || message == WM_CAPTURECHANGED)
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
              .rule_id = cooldown->id + "-visual",
              .enabled = true,
              .mode = layerc::core::VisualTriggerMode::pixel_only,
              .keep_input = false};
  rule_ = {.id = trigger_.rule_id,
           .name = cooldown->name + " Ready",
           .region = {0.4, 0.4, 0.2, 0.2},
           .predicate = layerc::core::AverageColorPredicate{0xFFFFFF, 16}};
  if (existing_visual_) {
    const auto assignment = std::ranges::find_if(
        existing_visual_->triggers, [&](const auto &candidate_trigger) {
          return candidate_trigger.cooldown_id == cooldown->id;
        });
    if (assignment != existing_visual_->triggers.end()) {
      trigger_ = *assignment;
      const auto existing_rule = std::ranges::find_if(
          existing_visual_->rules, [&](const auto &candidate_rule) {
            return candidate_rule.id == trigger_.rule_id;
          });
      if (existing_rule != existing_visual_->rules.end()) rule_ = *existing_rule;
    } else {
      std::string base = trigger_.rule_id;
      unsigned suffix = 2;
      while (std::ranges::any_of(existing_visual_->rules, [&](const auto &item) {
        return item.id == trigger_.rule_id;
      }))
        trigger_.rule_id = base + "-" + std::to_string(suffix++);
      rule_.id = trigger_.rule_id;
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
  if (const auto *average =
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
                      rule_.repeat ==
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
                   std::to_wstring(rule_.stable_samples));
  set_control_text(IDC_CAL_TIMEOUT,
                   std::to_wstring(trigger_.confirmation_timeout.count()));
  update_control_enablement();
}

void CalibrationWindow::update_control_enablement() {
  const auto predicate = static_cast<int>(SendDlgItemMessageW(
      dialog_, IDC_CAL_PREDICATE, CB_GETCURSEL, 0, 0));
  const bool color_predicate = predicate == 0 || predicate == 2;
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_SAMPLE_COLOR), color_predicate);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_COLOR_VALUE), color_predicate);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_TOLERANCE), color_predicate);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_COMPARISON), predicate == 1);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_THRESHOLD), predicate == 1);
  EnableWindow(GetDlgItem(dialog_, IDC_CAL_MINIMUM_PERCENT), predicate == 2);

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
    dragging_ = true;
    drag_start_ = point;
    SetCapture(window);
    update_region_from_pointer(point, false);
    return 0;
  case WM_MOUSEMOVE:
    if (dragging_) update_region_from_pointer(point, false);
    return 0;
  case WM_LBUTTONUP:
    if (dragging_) {
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
  update_match_status();
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
  if (!frame_ || preview_pixels_.empty()) {
    SetTextColor(draw.hDC, muted);
    SetBkMode(draw.hDC, TRANSPARENT);
    DrawTextW(draw.hDC, L"Waiting for captured pixels...", -1,
              const_cast<RECT *>(&draw.rcItem),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }
  const auto image = preview_image_rect();
  BITMAPINFO bitmap{};
  bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap.bmiHeader.biWidth = static_cast<LONG>(frame_->width);
  bitmap.bmiHeader.biHeight = -static_cast<LONG>(frame_->height);
  bitmap.bmiHeader.biPlanes = 1;
  bitmap.bmiHeader.biBitCount = 32;
  bitmap.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(draw.hDC, HALFTONE);
  StretchDIBits(draw.hDC, image.left, image.top, image.right - image.left,
                image.bottom - image.top, 0, 0, frame_->width, frame_->height,
                preview_pixels_.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);

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
  if (!frame_ || frame_->width == 0 || frame_->height == 0) return client;
  const double frame_aspect =
      static_cast<double>(frame_->width) / frame_->height;
  const double client_aspect =
      static_cast<double>(client.right) / std::max<LONG>(1, client.bottom);
  if (client_aspect > frame_aspect) {
    const auto width = static_cast<LONG>(std::lround(client.bottom * frame_aspect));
    const auto left = (client.right - width) / 2;
    return {left, 0, left + width, client.bottom};
  }
  const auto height = static_cast<LONG>(std::lround(client.right / frame_aspect));
  const auto top = (client.bottom - height) / 2;
  return {0, top, client.right, top + height};
}

void CalibrationWindow::update_region_from_pointer(POINT point, bool) {
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
  InvalidateRect(preview_, nullptr, FALSE);
  update_match_status();
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

void CalibrationWindow::update_match_status() {
  if (!frame_) return;
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
  rule_ = current_rule_from_controls();
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
                  L"Exact evaluates every pixel in the selected region and is not covered by the V1.1 CPU/memory budget. Continue?",
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

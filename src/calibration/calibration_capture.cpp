#include "calibration_window.hpp"

#include "calibration_resource.h"

#include "layerc/platform/text.hpp"

#include <algorithm>
#include <cmath>

namespace layerc::calibration {

void CalibrationWindow::update_capture() {
  if (!capture_ || fallback_snapshot_)
    return;
  if (fallback_started_) {
    const HWND foreground_root = GetAncestor(GetForegroundWindow(), GA_ROOT);
    const HWND target_root = GetAncestor(target_window_, GA_ROOT);
    if (foreground_root == nullptr || foreground_root != target_root) {
      if (GetTickCount64() >= fallback_deadline_) {
        preview_.clear_frame();
        finish_fallback_snapshot(L"Desktop Duplication fallback stopped "
                                 L"because the target was not foreground.");
      }
      return;
    }
    const auto captured =
        capture_->poll(layerc::core::MonotonicClock::now(), true).result;
    if (captured.frame) {
      preview_.set_frame(captured.frame);
      finish_fallback_snapshot(
          L"Desktop Duplication fallback — frozen foreground snapshot.");
    } else if (GetTickCount64() >= fallback_deadline_) {
      preview_.set_frame(layerc::capture::capture_static_window_snapshot(
          target_window_, quality_, layerc::core::MonotonicClock::now()));
      finish_fallback_snapshot(
          preview_.frame()
              ? L"Desktop Duplication was static — frozen screen snapshot."
              : L"Desktop Duplication fallback could not capture the target.");
    }
    return;
  }

  const auto captured =
      capture_->poll(layerc::core::MonotonicClock::now(), false).result;
  if (captured.backend ==
      layerc::core::CaptureBackendKind::desktop_duplication) {
    begin_fallback_snapshot();
    return;
  }
  if (captured.frame)
    preview_.set_frame(captured.frame);
  if (captured.availability == layerc::core::CaptureAvailability::available)
    set_control_text(IDC_CAL_BACKEND,
                     L"Windows Graphics Capture — live preview");
  else if (!captured.diagnostic.empty())
    set_control_text(IDC_CAL_BACKEND,
                     layerc::platform::utf8_to_wide(captured.diagnostic));
  refresh_preview_frame();
}

void CalibrationWindow::refresh_preview_frame() {
  if (!preview_.frame())
    return;
  preview_.invalidate();
  update_match_status(false);
}

void CalibrationWindow::begin_fallback_snapshot() {
  fallback_started_ = true;
  fallback_deadline_ = GetTickCount64() + 2'000;
  previous_foreground_ = GetForegroundWindow();
  set_control_text(
      IDC_CAL_BACKEND,
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
  const auto &frame = preview_.frame();
  const bool mask_preview =
      mask_editor_.editing() && !template_images_.empty() &&
      !mask_editor_.pixels().empty();
  if (!mask_preview && (!frame || preview_.pixels().empty())) {
    SetTextColor(draw.hDC, palette_.muted);
    SetBkMode(draw.hDC, TRANSPARENT);
    DrawTextW(draw.hDC, L"Waiting for captured pixels...", -1,
              const_cast<RECT *>(&draw.rcItem),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }
  const auto image = preview_image_rect();
  std::uint32_t width;
  std::uint32_t height;
  const std::uint32_t *pixels;
  if (mask_preview) {
    const auto &mask_image = template_images_.back();
    width = mask_image.width;
    height = mask_image.height;
    pixels = mask_editor_.pixels().data();
  } else {
    width = frame->width;
    height = frame->height;
    pixels = preview_.pixels().data();
  }
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

  if (mask_preview)
    return;

  const auto to_x = [&](const double fraction) {
    return image.left +
           static_cast<LONG>(std::lround(fraction * (image.right - image.left)));
  };
  const auto to_y = [&](const double fraction) {
    return image.top +
           static_cast<LONG>(std::lround(fraction * (image.bottom - image.top)));
  };
  const RECT region{to_x(rule_.region.x), to_y(rule_.region.y),
                    to_x(rule_.region.x + rule_.region.width),
                    to_y(rule_.region.y + rule_.region.height)};
  const HPEN pen = CreatePen(PS_SOLID, 2, palette_.selection);
  const auto old_pen = SelectObject(draw.hDC, pen);
  const auto old_brush = SelectObject(draw.hDC, GetStockObject(HOLLOW_BRUSH));
  Rectangle(draw.hDC, region.left, region.top, region.right, region.bottom);
  SelectObject(draw.hDC, old_brush);
  SelectObject(draw.hDC, old_pen);
  DeleteObject(pen);
}

RECT CalibrationWindow::preview_image_rect() const {
  if (mask_editor_.editing() && !template_images_.empty())
    return preview_.image_rect(template_images_.back().width,
                               template_images_.back().height);
  return preview_.image_rect();
}

void CalibrationWindow::update_region_from_pointer(POINT point,
                                                   const bool final) {
  const auto image = preview_image_rect();
  const auto clamp_point = [&](POINT value) {
    value.x = std::clamp(value.x, image.left, image.right);
    value.y = std::clamp(value.y, image.top, image.bottom);
    return value;
  };
  const auto start = clamp_point(preview_.drag_start());
  point = clamp_point(point);
  const auto width = std::max<LONG>(1, image.right - image.left);
  const auto height = std::max<LONG>(1, image.bottom - image.top);
  const auto left = std::min(start.x, point.x);
  const auto top = std::min(start.y, point.y);
  const auto right = std::max(start.x, point.x);
  const auto bottom = std::max(start.y, point.y);
  if (right - left < 2 || bottom - top < 2)
    return;
  rule_.region = {static_cast<double>(left - image.left) / width,
                  static_cast<double>(top - image.top) / height,
                  static_cast<double>(right - left) / width,
                  static_cast<double>(bottom - top) / height};
  if (mode() == CalibrationMode::template_match)
    template_state_.search_region = rule_.region;
  preview_.invalidate();
  update_match_status(final);
}

} // namespace layerc::calibration

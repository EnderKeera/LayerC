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
        frame_.reset();
        finish_fallback_snapshot(L"Desktop Duplication fallback stopped "
                                 L"because the target was not foreground.");
      }
      return;
    }
    const auto captured =
        capture_->poll(layerc::core::MonotonicClock::now(), true);
    if (captured.frame) {
      frame_ = captured.frame;
      finish_fallback_snapshot(
          L"Desktop Duplication fallback — frozen foreground snapshot.");
    } else if (GetTickCount64() >= fallback_deadline_) {
      frame_ = layerc::capture::capture_static_window_snapshot(
          target_window_, quality_, layerc::core::MonotonicClock::now());
      finish_fallback_snapshot(
          frame_
              ? L"Desktop Duplication was static — frozen screen snapshot."
              : L"Desktop Duplication fallback could not capture the target.");
    }
    return;
  }

  const auto captured =
      capture_->poll(layerc::core::MonotonicClock::now(), false);
  if (captured.backend ==
      layerc::core::CaptureBackendKind::desktop_duplication) {
    begin_fallback_snapshot();
    return;
  }
  if (captured.frame)
    frame_ = captured.frame;
  if (captured.availability == layerc::core::CaptureAvailability::available)
    set_control_text(IDC_CAL_BACKEND,
                     L"Windows Graphics Capture — live preview");
  else if (!captured.diagnostic.empty())
    set_control_text(IDC_CAL_BACKEND,
                     layerc::platform::utf8_to_wide(captured.diagnostic));
  refresh_preview_frame();
}

void CalibrationWindow::refresh_preview_frame() {
  if (!frame_)
    return;
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
  const bool mask_preview = mask_editing_ && !template_images_.empty() &&
                            !mask_preview_pixels_.empty();
  if (!mask_preview && (!frame_ || preview_pixels_.empty())) {
    SetTextColor(draw.hDC, palette_.muted);
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
  const auto *pixels =
      mask_preview ? mask_preview_pixels_.data() : preview_pixels_.data();
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

  RECT region{
      image.left + static_cast<LONG>(std::lround(rule_.region.x *
                                                 (image.right - image.left))),
      image.top + static_cast<LONG>(
                      std::lround(rule_.region.y * (image.bottom - image.top))),
      image.left +
          static_cast<LONG>(std::lround((rule_.region.x + rule_.region.width) *
                                        (image.right - image.left))),
      image.top +
          static_cast<LONG>(std::lround((rule_.region.y + rule_.region.height) *
                                        (image.bottom - image.top)))};
  const HPEN pen = CreatePen(PS_SOLID, 2, palette_.selection);
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
                            : frame_ ? frame_->width
                                     : 0;
  const auto source_height = mask_editing_ && !template_images_.empty()
                                 ? template_images_.back().height
                             : frame_ ? frame_->height
                                      : 0;
  if (source_width == 0 || source_height == 0)
    return client;
  const double frame_aspect = static_cast<double>(source_width) / source_height;
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
  if (right - left < 2 || bottom - top < 2)
    return;
  rule_.region = {static_cast<double>(left - image.left) / width,
                  static_cast<double>(top - image.top) / height,
                  static_cast<double>(right - left) / width,
                  static_cast<double>(bottom - top) / height};
  if (template_mode())
    template_state_.search_region = rule_.region;
  InvalidateRect(preview_, nullptr, FALSE);
  update_match_status(final);
}

} // namespace layerc::calibration

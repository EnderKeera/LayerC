#include "capture_backend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace layerc::capture {

std::shared_ptr<CaptureFrame> capture_static_window_snapshot(
    const HWND window, const core::CaptureQuality quality,
    const core::TimePoint captured_at) {
  RECT bounds{};
  if (!IsWindow(window) || !capture_window_bounds(window, bounds)) return {};
  const auto source_width =
      static_cast<std::uint32_t>(bounds.right - bounds.left);
  const auto source_height =
      static_cast<std::uint32_t>(bounds.bottom - bounds.top);
  const auto [width, height] =
      capture_sample_dimensions(source_width, source_height, quality);
  if (width == 0 || height == 0) return {};

  HDC screen = GetDC(nullptr);
  if (screen == nullptr) return {};
  HDC memory = CreateCompatibleDC(screen);
  if (memory == nullptr) {
    ReleaseDC(nullptr, screen);
    return {};
  }

  BITMAPINFO information{};
  information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  information.bmiHeader.biWidth = static_cast<LONG>(width);
  information.bmiHeader.biHeight = -static_cast<LONG>(height);
  information.bmiHeader.biPlanes = 1;
  information.bmiHeader.biBitCount = 32;
  information.bmiHeader.biCompression = BI_RGB;
  void *bits{};
  HBITMAP bitmap = CreateDIBSection(screen, &information, DIB_RGB_COLORS,
                                    &bits, nullptr, 0);
  if (bitmap == nullptr || bits == nullptr) {
    if (bitmap != nullptr) DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return {};
  }

  const HGDIOBJ previous = SelectObject(memory, bitmap);
  SetStretchBltMode(memory, COLORONCOLOR);
  const BOOL copied = StretchBlt(
      memory, 0, 0, static_cast<int>(width), static_cast<int>(height), screen,
      bounds.left, bounds.top, static_cast<int>(source_width),
      static_cast<int>(source_height), SRCCOPY | CAPTUREBLT);
  GdiFlush();

  auto frame = copied != FALSE ? std::make_shared<CaptureFrame>() : nullptr;
  if (frame) {
    frame->width = width;
    frame->height = height;
    frame->captured_at = captured_at;
    frame->available_region = {0.0, 0.0, 1.0, 1.0};
    frame->pixels.resize(static_cast<std::size_t>(width) * height);
    const auto *source = static_cast<const std::uint8_t *>(bits);
    for (std::size_t index = 0; index < frame->pixels.size(); ++index) {
      const auto offset = index * 4U;
      frame->pixels[index] =
          {source[offset + 2U], source[offset + 1U], source[offset]};
    }
  }

  SelectObject(memory, previous);
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  return frame;
}

} // namespace layerc::capture

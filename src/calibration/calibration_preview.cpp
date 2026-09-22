#include "calibration_preview.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace layerc::calibration {

void CapturePreview::set_frame(
    std::shared_ptr<const layerc::capture::CaptureFrame> frame) {
  frame_ = std::move(frame);
  pixels_.clear();
  if (frame_) {
    pixels_.resize(frame_->pixels.size());
    for (std::size_t index = 0; index < frame_->pixels.size(); ++index) {
      const auto pixel = frame_->pixels[index];
      pixels_[index] = pixel.blue |
                       (static_cast<std::uint32_t>(pixel.green) << 8U) |
                       (static_cast<std::uint32_t>(pixel.red) << 16U);
    }
  }
  invalidate();
}

void CapturePreview::clear_frame() {
  frame_.reset();
  pixels_.clear();
  invalidate();
}

RECT CapturePreview::image_rect(std::uint32_t source_width,
                                std::uint32_t source_height) const {
  RECT client{};
  if (window_ != nullptr)
    GetClientRect(window_, &client);
  if (source_width == 0 || source_height == 0) {
    source_width = frame_ ? frame_->width : 0;
    source_height = frame_ ? frame_->height : 0;
  }
  if (source_width == 0 || source_height == 0)
    return client;

  const double source_aspect =
      static_cast<double>(source_width) / source_height;
  const double client_aspect =
      static_cast<double>(client.right) / std::max<LONG>(1, client.bottom);
  if (client_aspect > source_aspect) {
    const auto display_width =
        static_cast<LONG>(std::lround(client.bottom * source_aspect));
    const auto left = (client.right - display_width) / 2;
    return {left, 0, left + display_width, client.bottom};
  }
  const auto display_height =
      static_cast<LONG>(std::lround(client.right / source_aspect));
  const auto top = (client.bottom - display_height) / 2;
  return {0, top, client.right, top + display_height};
}

void CapturePreview::begin_drag(const POINT point) noexcept {
  dragging_ = true;
  drag_start_ = point;
}

void CapturePreview::invalidate(const bool erase) const noexcept {
  if (window_ != nullptr)
    InvalidateRect(window_, nullptr, erase ? TRUE : FALSE);
}

void MaskEditor::begin(layerc::core::TemplateImage &image) {
  editing_ = true;
  changed_ = false;
  refresh(image);
}

void MaskEditor::finish() noexcept {
  editing_ = false;
  changed_ = false;
  brush_opaque_ = false;
  pixels_.clear();
}

void MaskEditor::refresh(const layerc::core::TemplateImage &image) {
  if (!editing_)
    return;
  pixels_.resize(image.pixels.size());
  for (std::uint32_t y = 0; y < image.height; ++y) {
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const auto index = static_cast<std::size_t>(y) * image.width + x;
      const auto pixel = image.pixels[index];
      pixels_[index] =
          image.alpha[index] == 0
              ? (((x / 12U) + (y / 12U)) % 2U == 0U ? 0x3A3F46U
                                                     : 0x24282DU)
              : pixel.blue |
                    (static_cast<std::uint32_t>(pixel.green) << 8U) |
                    (static_cast<std::uint32_t>(pixel.red) << 16U);
    }
  }
}

bool MaskEditor::update(layerc::core::TemplateImage &image, const POINT point,
                        const RECT image_bounds, const bool opaque) {
  if (!editing_ || point.x < image_bounds.left ||
      point.x >= image_bounds.right || point.y < image_bounds.top ||
      point.y >= image_bounds.bottom)
    return false;

  const auto x = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(point.x - image_bounds.left) * image.width /
      std::max<LONG>(1, image_bounds.right - image_bounds.left));
  const auto y = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(point.y - image_bounds.top) * image.height /
      std::max<LONG>(1, image_bounds.bottom - image_bounds.top));
  const int radius =
      std::max<int>(1, std::min(image.width, image.height) / 32);
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
      image.alpha[static_cast<std::size_t>(target_y) * image.width + target_x] =
          opaque ? 255 : 0;
    }
  }
  changed_ = true;
  refresh(image);
  return true;
}

} // namespace layerc::calibration

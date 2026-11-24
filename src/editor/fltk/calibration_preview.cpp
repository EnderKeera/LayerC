#include "calibration_window.hpp"

#include "editor_style.hpp"
#include "accessible_widgets.hpp"
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <vector>

namespace layerc::editor {
namespace {
Fl_Color token_color(const std::uint32_t value) {
  return fl_rgb_color(static_cast<uchar>((value >> 16U) & 0xFFU),
                      static_cast<uchar>((value >> 8U) & 0xFFU),
                      static_cast<uchar>(value & 0xFFU));
}
} // namespace

CalibrationPreview::CalibrationPreview(int x, int y, int width, int height)
    : Fl_Widget(x, y, width, height, "Calibration preview") {
  accessibility::set_accessible_role(*this, a11y::Role::canvas);
}
layerc::core::NormalizedRegion CalibrationPreview::image_bounds() const {
  const auto width = mask ? mask->width : frame ? frame->width : 0;
  const auto height = mask ? mask->height : frame ? frame->height : 0;
  if (!width || !height) return {double(x()), double(y()), double(w()), double(h())};
  const auto scale = std::min(double(w()) / width, double(h()) / height);
  const auto iw = width * scale, ih = height * scale;
  return {x() + (w() - iw) * 0.5, y() + (h() - ih) * 0.5, iw, ih};
}
void CalibrationPreview::draw() {
  fl_push_clip(x(), y(), w(), h());
  fl_color(token_color(current_editor_tokens().input));
  fl_rectf(x(), y(), w(), h());
  const auto bounds = image_bounds();
  const auto source_width = mask ? mask->width : frame ? frame->width : 0;
  const auto source_height = mask ? mask->height : frame ? frame->height : 0;
  const auto width = static_cast<int>(bounds.width);
  const auto height = static_cast<int>(bounds.height);
  if (source_width && source_height && width > 0 && height > 0) {
    const auto &pixels = mask ? mask->pixels : frame->pixels;
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
    for (int row = 0; row < height; ++row) {
      for (int column = 0; column < width; ++column) {
        const auto at = std::min(source_height - 1,
            static_cast<unsigned>(static_cast<double>(row) * source_height / height)) *
            source_width + std::min(source_width - 1,
            static_cast<unsigned>(static_cast<double>(column) * source_width / width));
        const auto &pixel = pixels[at];
        const auto out = (static_cast<std::size_t>(row) * width + column) * 3;
        const bool erased = mask && mask->alpha[at] == 0;
        rgb[out] = erased ? 200 : pixel.red;
        rgb[out + 1] = erased ? 40 : pixel.green;
        rgb[out + 2] = erased ? 160 : pixel.blue;
      }
    }
    fl_draw_image(rgb.data(), static_cast<int>(bounds.x), static_cast<int>(bounds.y),
                  width, height, 3);
    if (!mask) {
      fl_color(token_color(current_editor_tokens().accent));
      fl_rect(static_cast<int>(bounds.x + selection.x * bounds.width),
              static_cast<int>(bounds.y + selection.y * bounds.height),
              std::max(1, static_cast<int>(selection.width * bounds.width)),
              std::max(1, static_cast<int>(selection.height * bounds.height)));
    }
  } else {
    fl_color(token_color(fixed_ink(current_editor_tokens(), FixedInk::subtext)));
    fl_draw("Acknowledge capture, select a window, then enable Live preview.",
            x() + 12, y() + h() / 2);
  }
  fl_pop_clip();
}
int CalibrationPreview::handle(const int event) {
  if (event == FL_FOCUS || event == FL_UNFOCUS) return 1;
  if (event != FL_PUSH && event != FL_DRAG && event != FL_RELEASE)
    return Fl_Widget::handle(event);
  if ((!frame && !mask) || (event == FL_PUSH && Fl::event_button() != FL_LEFT_MOUSE))
    return 0;
  const auto bounds = image_bounds();
  if (bounds.width <= 0.0 || bounds.height <= 0.0) return 0;
  const auto nx = std::clamp((Fl::event_x() - bounds.x) / bounds.width, 0.0, 1.0);
  const auto ny = std::clamp((Fl::event_y() - bounds.y) / bounds.height, 0.0, 1.0);
  if (event == FL_PUSH) {
    if (Fl::event_x() < bounds.x || Fl::event_x() > bounds.x + bounds.width ||
        Fl::event_y() < bounds.y || Fl::event_y() > bounds.y + bounds.height) return 0;
    start_x_ = nx; start_y_ = ny; dragging_ = true;
    take_focus();
  }
  if (!dragging_) return 0;
  if (tool && mask && painted) {
    // Interpolate in source pixels: native motion may coalesce positions.
    const auto distance = std::max(std::abs(nx - start_x_) * mask->width,
                                   std::abs(ny - start_y_) * mask->height);
    const auto steps = static_cast<int>(std::clamp(std::ceil(distance), 1.0,
        static_cast<double>(layerc::core::max_template_axis)));
    for (int step = 1; step <= steps; ++step) {
      const auto fraction = static_cast<double>(step) / steps;
      painted(std::min(start_x_ + (nx - start_x_) * fraction, 0.999999),
              std::min(start_y_ + (ny - start_y_) * fraction, 0.999999), tool == 2);
    }
    start_x_ = nx; start_y_ = ny;
  } else if (!tool) {
    selection = {std::min(start_x_, nx), std::min(start_y_, ny),
                 std::abs(nx - start_x_), std::abs(ny - start_y_)};
    if (event == FL_RELEASE && selected && layerc::core::valid_normalized_region(selection))
      selected(selection);
  }
  if (event == FL_RELEASE) dragging_ = false;
  redraw();
  return 1;
}
} // namespace layerc::editor

#include "input.hpp"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <string>

namespace layerc::editor::a11y {
Text Input::accessible_text() const {
  Text text;
  text.value.assign(value(), static_cast<std::size_t>(size()));
  text.caret = static_cast<std::size_t>(insert_position());
  text.anchor = static_cast<std::size_t>(mark());
  struct FontRestore {
    Fl_Font font{fl_font()};
    Fl_Fontsize size{fl_size()};
    ~FontRestore() { fl_font(font, size); }
  } restore;
  fl_font(textfont(), textsize());
  const double height = fl_height();
  const bool multiline = input_type() == FL_MULTILINE_INPUT;
  // FLTK 1.4's stock input drawtext() adds this text inset after box_dx().
  // This mirrors caret geometry; it does not introduce a LayerE layout metric.
  const double left = x() + Fl::box_dx(box()) + 3 - xscroll();
  double top = y() + Fl::box_dy(box()) - yscroll();
  double advance{};
  std::size_t column{};
  std::string expanded;
  text.caret_bounds = {left, top, 1, height};
  for (std::size_t offset = 0; offset < text.value.size();) {
    const int encoded_size = fl_utf8len(
        static_cast<unsigned char>(text.value[offset]));
    const auto length = static_cast<std::size_t>(std::max(encoded_size, 1));
    const auto end = std::min(offset + length, text.value.size());
    const char character = text.value[offset];
    const double begin = advance;
    if (offset == text.caret)
      text.caret_bounds = {left + begin, top, 1, height};
    if (character == '\n' && multiline) {
      text.runs.push_back({offset, end, {left + begin, top, 1, height}});
      top += height;
      advance = 0;
      column = 0;
      expanded.clear();
    } else {
      if (character == '\t') {
        const auto spaces = 8 - column % 8;
        expanded.append(spaces, ' ');
        column += spaces;
      } else if (static_cast<unsigned char>(character) < 32 ||
                 character == 127) {
        expanded += '^';
        expanded += character == 127 ? '?' : static_cast<char>(character + 64);
        column += 2;
      } else {
        expanded.append(text.value, offset, end - offset);
        ++column;
      }
      advance = fl_width(expanded.c_str(), static_cast<int>(expanded.size()));
      text.runs.push_back(
          {offset, end, {left + begin, top, std::max(advance - begin, 1.0), height}});
    }
    offset = end;
  }
  if (text.caret == text.value.size())
    text.caret_bounds = {left + advance, top, 1, height};
  return text;
}
} // namespace layerc::editor::a11y

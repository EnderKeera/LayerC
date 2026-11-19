#include "input.hpp"
#include "text_metrics.hpp"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace layerc::editor::a11y {
namespace {
// FLTK keeps the scroll offsets protected; deriving is the one access route it
// documents, and taking the member's address here avoids casting a widget.
struct FieldAccess : Fl_Input_ {
  static int scroll_x(const Fl_Input_ &field) { return (field.*(&FieldAccess::xscroll))(); }
  static int scroll_y(const Fl_Input_ &field) {
    return (field.*(static_cast<int (Fl_Input_::*)() const>(&FieldAccess::yscroll)))();
  }
  static Fl_Font font_of(const Fl_Input_ &field) { return field.textfont(); }
  static Fl_Fontsize size_of(const Fl_Input_ &field) { return field.textsize(); }
};

// One glyph of one source line: its byte range in the value and the byte end of
// its expansion in the line's drawn text. A line break draws a caret run.
struct Glyph {
  std::size_t offset{};
  std::size_t end{};
  std::size_t expanded_end{};
  bool line_break{};
};

struct Line {
  std::size_t index{};
  double top{};
  std::string expanded;
  std::vector<Glyph> glyphs;
  std::vector<std::size_t> ends;
};

// Measures every advance of one expanded line itself.
struct DirectAdvances {
  std::vector<double> widths;

  const std::vector<double> &operator()(std::size_t, std::string_view expanded,
                                        std::span<const std::size_t> ends) {
    widths.clear();
    widths.reserve(ends.size());
    for (const auto end : ends) {
      widths.push_back(fl_width(expanded.data(), static_cast<int>(end)));
    }
    return widths;
  }
};

// Measures only the advances a metrics pass has not measured yet.
struct CachedAdvances {
  TextMetrics &metrics;
  const Fl_Input_ *field;
  int font;
  int size;

  const std::vector<double> &operator()(std::size_t index, std::string_view expanded,
                                        std::span<const std::size_t> ends) {
    return metrics.line(field, font, size, index, expanded, ends);
  }
};

template <typename Advances>
std::optional<Text> text_of(const Fl_Input_ &field, Advances advances) {
  if (field.value() == nullptr) return std::nullopt;
  Text text;
  text.value.assign(field.value(), static_cast<std::size_t>(field.size()));
  text.caret = static_cast<std::size_t>(field.insert_position());
  text.anchor = static_cast<std::size_t>(field.mark());
  struct FontRestore {
    Fl_Font font{fl_font()};
    Fl_Fontsize size{fl_size()};
    ~FontRestore() { fl_font(font, size); }
  } restore;
  fl_font(FieldAccess::font_of(field), FieldAccess::size_of(field));
  const double height = fl_height();
  const bool multiline = (field.input_type() & FL_INPUT_TYPE) == FL_MULTILINE_INPUT;
  text.multiline = multiline;
  // FLTK 1.4's stock input drawtext() adds this text inset after box_dx().
  // This mirrors caret geometry; it does not introduce a LayerE layout metric.
  const double left = field.x() + Fl::box_dx(field.box()) + 3 - FieldAccess::scroll_x(field);
  double top = field.y() + Fl::box_dy(field.box()) - FieldAccess::scroll_y(field);
  std::size_t column{};
  std::vector<Line> lines;
  std::vector<Glyph> glyphs;
  std::vector<std::size_t> ends;
  std::string expanded;
  for (std::size_t offset = 0; offset < text.value.size();) {
    const int encoded_size = fl_utf8len(static_cast<unsigned char>(text.value[offset]));
    const auto length = static_cast<std::size_t>(std::max(encoded_size, 1));
    const auto end = std::min(offset + length, text.value.size());
    const char character = text.value[offset];
    if (character == '\n' && multiline) {
      glyphs.push_back({offset, end, expanded.size(), true});
      lines.push_back({lines.size(), top, std::move(expanded), std::move(glyphs), std::move(ends)});
      expanded.clear();
      glyphs.clear();
      ends.clear();
      top += height;
      column = 0;
    } else {
      if (character == '\t') {
        const auto spaces = 8 - column % 8;
        expanded.append(spaces, ' ');
        column += spaces;
      } else if (static_cast<unsigned char>(character) < 32 || character == 127) {
        expanded += '^';
        expanded += character == 127 ? '?' : static_cast<char>(character + 64);
        column += 2;
      } else {
        expanded.append(text.value, offset, end - offset);
        ++column;
      }
      glyphs.push_back({offset, end, expanded.size(), false});
      ends.push_back(expanded.size());
    }
    offset = end;
  }
  lines.push_back({lines.size(), top, std::move(expanded), std::move(glyphs), std::move(ends)});

  text.caret_bounds = {left, lines.front().top, 1, height};
  double advance{};
  double line_top = lines.front().top;
  for (const Line &line : lines) {
    const auto &widths = advances(line.index, line.expanded, line.ends);
    for (std::size_t index = 0; index < line.glyphs.size(); ++index) {
      const Glyph &glyph = line.glyphs[index];
      const double begin = index == 0 ? 0.0 : widths[index - 1];
      if (glyph.offset == text.caret) text.caret_bounds = {left + begin, line.top, 1, height};
      if (glyph.line_break) {
        text.runs.push_back({glyph.offset, glyph.end, {left + begin, line.top, 1, height}});
      } else {
        const double after = widths[index];
        text.runs.push_back({glyph.offset, glyph.end,
                             {left + begin, line.top, std::max(after - begin, 1.0), height}});
      }
    }
    advance = widths.empty() ? 0.0 : widths.back();
    line_top = line.top;
  }
  if (text.caret == text.value.size()) text.caret_bounds = {left + advance, line_top, 1, height};
  return text;
}
} // namespace

std::optional<Text> field_text(const Fl_Input_ &field) { return text_of(field, DirectAdvances{}); }

std::optional<Text> field_text(const Fl_Input_ &field, TextMetrics *metrics) {
  if (metrics == nullptr) return text_of(field, DirectAdvances{});
  CachedAdvances advances{*metrics, &field, static_cast<int>(FieldAccess::font_of(field)),
                          static_cast<int>(FieldAccess::size_of(field))};
  return text_of(field, advances);
}

Text Input::accessible_text() const {
  auto text = field_text(*this);
  return text ? *text : Text{};
}
} // namespace layerc::editor::a11y

#include "text_metrics.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Graphics_Driver.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace layerc::editor::a11y {
namespace {
// Everything fl_width reads for one expanded line: the font id and size the
// caller set, the face name FLTK resolves them to, and the driver's scale.
struct FontKey {
  int font{};
  int size{};
  double scale{1.0};
  std::string face;

  bool operator<(const FontKey &other) const {
    return std::tie(font, size, scale, face) <
           std::tie(other.font, other.size, other.scale, other.face);
  }
  bool operator==(const FontKey &other) const {
    return font == other.font && size == other.size && scale == other.scale && face == other.face;
  }
};

// The advance after one glyph of an expanded line, at its byte end offset.
struct Advance {
  std::size_t end{};
  double width{};
};

using Advances = std::vector<Advance>;

struct Line {
  FontKey font;
  Advances advances;
  std::uint64_t used{};
};

// The expanded lines one pass measured, and the line each field index held
// during the pass before it.
struct FontTable {
  std::map<std::string, Line, std::less<>> lines;
  std::uint64_t used{};
};

struct FieldTable {
  std::map<std::string, Line, std::less<>> lines;
  std::vector<std::string> order;
  std::vector<std::string> previous_order;
  std::uint64_t used{};
};

std::size_t common_prefix(std::string_view left, std::string_view right) {
  const auto limit = std::min(left.size(), right.size());
  std::size_t length = 0;
  while (length < limit && left[length] == right[length]) ++length;
  return length;
}

Advances::const_iterator seek(const Advances &advances, std::size_t end) {
  return std::lower_bound(advances.begin(), advances.end(), end,
                          [](const Advance &advance, std::size_t value) {
                            return advance.end < value;
                          });
}

const Advance *find(const Advances &advances, std::size_t end) {
  const auto found = seek(advances, end);
  if (found == advances.end() || found->end != end) return nullptr;
  return &*found;
}

void insert(Advances &advances, std::size_t end, double width) {
  const auto position = std::lower_bound(advances.begin(), advances.end(), end,
                                         [](const Advance &advance, std::size_t value) {
                                           return advance.end < value;
                                         });
  if (position != advances.end() && position->end == end) {
    position->width = width;
    return;
  }
  advances.insert(position, Advance{end, width});
}

FontKey key_of(int font, int size) {
  FontKey key;
  key.font = font;
  key.size = size;
  key.scale = fl_graphics_driver->scale();
  if (const char *face = Fl::get_font(font)) key.face = face;
  return key;
}
} // namespace

struct TextMetrics::Impl {
  std::map<FontKey, FontTable> fonts;
  std::map<const Fl_Input_ *, FieldTable> fields;
  std::vector<double> result;
  std::uint64_t pass{1};
  std::size_t measured{};
};

TextMetrics::TextMetrics() : impl_(std::make_unique<Impl>()) {}

TextMetrics::~TextMetrics() = default;

TextMetrics::TextMetrics(TextMetrics &&) noexcept = default;

TextMetrics &TextMetrics::operator=(TextMetrics &&) noexcept = default;

TextMetrics::Pass::Pass(TextMetrics &metrics) noexcept : metrics_(&metrics) {
  metrics_->impl_->measured = 0;
  metrics_->impl_->result.clear();
}

TextMetrics::Pass::~Pass() {
  Impl &impl = *metrics_->impl_;
  for (auto entry = impl.fonts.begin(); entry != impl.fonts.end();) {
    FontTable &table = entry->second;
    for (auto line = table.lines.begin(); line != table.lines.end();) {
      if (line->second.used == impl.pass) {
        ++line;
      } else {
        line = table.lines.erase(line);
      }
    }
    if (table.lines.empty()) {
      entry = impl.fonts.erase(entry);
    } else {
      ++entry;
    }
  }
  for (auto entry = impl.fields.begin(); entry != impl.fields.end();) {
    FieldTable &field = entry->second;
    for (auto line = field.lines.begin(); line != field.lines.end();) {
      if (line->second.used == impl.pass) {
        ++line;
      } else {
        line = field.lines.erase(line);
      }
    }
    if (field.lines.empty()) {
      entry = impl.fields.erase(entry);
      continue;
    }
    field.previous_order = std::move(field.order);
    field.order.clear();
    ++entry;
  }
  ++impl.pass;
}

const std::vector<double> &TextMetrics::line(const Fl_Input_ *field, int font, int size,
                                             std::size_t index, std::string_view expanded,
                                             std::span<const std::size_t> ends) {
  Impl &impl = *impl_;
  impl.result.clear();
  impl.result.reserve(ends.size());
  const FontKey key = key_of(font, size);

  FieldTable &fields = impl.fields[field];
  fields.used = impl.pass;
  if (fields.order.size() <= index) fields.order.resize(index + 1);
  if (fields.order[index] != expanded) fields.order[index].assign(expanded);

  FontTable &table = impl.fonts[key];
  table.used = impl.pass;

  // A line this field measured in the same font is reused as it stands, even
  // when an edit moved it to another index. The pass table then offers it to
  // every other field with that text.
  Line *line = nullptr;
  if (const auto found = fields.lines.find(expanded);
      found != fields.lines.end() && found->second.font == key) {
    line = &found->second;
  } else if (const auto found = table.lines.find(expanded); found != table.lines.end()) {
    line = &fields.lines.insert_or_assign(std::string(expanded),
                                          Line{key, found->second.advances, impl.pass})
                .first->second;
  } else {
    line = &fields.lines.insert_or_assign(std::string(expanded), Line{key, {}, impl.pass})
                .first->second;
  }
  line->used = impl.pass;

  if (line->advances.size() < ends.size()) {
    // The line this index held last pass is the candidate an edit changed: its
    // advances stay valid up to the last glyph the two texts share.
    const Advances *previous = nullptr;
    std::string_view previous_text;
    if (index < fields.previous_order.size()) {
      previous_text = fields.previous_order[index];
      const auto found = fields.lines.find(previous_text);
      if (found != fields.lines.end() && found->second.font == key && &found->second != line) {
        previous = &found->second.advances;
      }
    }
    const std::size_t prefix = previous == nullptr ? 0 : common_prefix(previous_text, expanded);
    for (const auto end : ends) {
      if (find(line->advances, end) != nullptr) continue;
      const Advance *reusable =
          (previous != nullptr && end <= prefix) ? find(*previous, end) : nullptr;
      double width = 0;
      if (reusable != nullptr) {
        width = reusable->width;
      } else {
        // The bytes fl_width read for this advance are the expanded line's
        // first `end` bytes, exactly what the direct measurement passes.
        width = fl_width(expanded.data(), static_cast<int>(end));
        ++impl.measured;
      }
      insert(line->advances, end, width);
    }
    table.lines.insert_or_assign(std::string(expanded), *line);
  }

  for (const auto end : ends) {
    const Advance *found = find(line->advances, end);
    impl.result.push_back(found != nullptr ? found->width : 0.0);
  }
  return impl.result;
}

std::size_t TextMetrics::measured() const noexcept { return impl_->measured; }

std::size_t TextMetrics::cached_lines() const noexcept {
  std::size_t total = 0;
  for (const auto &entry : impl_->fields) total += entry.second.lines.size();
  return total;
}
} // namespace layerc::editor::a11y

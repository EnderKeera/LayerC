#include "a11y/atspi/objects.hpp"

#include "a11y/atspi/mapping.hpp"
#include "a11y/atspi/objects_detail.hpp"
#include "a11y/fltk_actions.hpp"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::editor::a11y::atspi {
namespace {

using namespace detail;

// AtspiTextBoundaryType, the deprecated *Offset methods' unit selector.
constexpr std::uint32_t kBoundaryCharacter = 0;
constexpr std::uint32_t kBoundaryWordStart = 1;
constexpr std::uint32_t kBoundaryWordEnd = 2;
constexpr std::uint32_t kBoundarySentenceStart = 3;
constexpr std::uint32_t kBoundarySentenceEnd = 4;
constexpr std::uint32_t kBoundaryLineStart = 5;
constexpr std::uint32_t kBoundaryLineEnd = 6;
constexpr std::uint32_t kBoundaryAttribute = 7;

const Node *text_node(const ObjectSource &source, const char *path,
                      sd_bus_error *error) {
  const Node *node = node_at(source, path, error);
  if (node == nullptr) return nullptr;
  if (!node->text) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s carries no text",
                      node->name.c_str());
    return nullptr;
  }
  return node;
}

const Node *request_text(const ObjectSource &source, sd_bus_message *message,
                         sd_bus_error *error) {
  const Node *node = node_of_request(source, message, error);
  if (node == nullptr) return nullptr;
  if (!node->text) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s carries no text",
                      node->name.c_str());
    return nullptr;
  }
  return node;
}

std::size_t character_count(const std::string_view value) {
  return char_offset(value, value.size()).value_or(0);
}

// The ATK adaptor answered an unrepresentable range with an empty string
// rather than an error, and clients do ask for the text of a collapsed field.
std::optional<std::pair<std::size_t, std::size_t>>
character_range(const std::string_view value, const std::int32_t start,
                const std::int32_t end) {
  const std::size_t count = character_count(value);
  const auto from = static_cast<std::int64_t>(start);
  const auto to = static_cast<std::int64_t>(end) == -1
                      ? static_cast<std::int64_t>(count)
                      : static_cast<std::int64_t>(end);
  if (from < 0 || to < from || from > static_cast<std::int64_t>(count) ||
      to > static_cast<std::int64_t>(count))
    return std::nullopt;
  return std::pair{static_cast<std::size_t>(from), static_cast<std::size_t>(to)};
}

std::size_t byte_of(const std::string_view value, const std::size_t character) {
  if (character >= character_count(value)) return value.size();
  return byte_offset(value, character).value_or(value.size());
}

int reply_span(sd_bus_message *message, const std::string_view text,
               const std::size_t start, const std::size_t end) {
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  const std::string copy{text};
  const auto from = static_cast<std::int32_t>(start);
  const auto to = static_cast<std::int32_t>(end);
  int appended = sd_bus_message_append(reply, "sii", copy.c_str(), from, to);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int reply_rect(sd_bus_message *message, const Bounds &bounds) {
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  const auto x = static_cast<std::int32_t>(std::lround(bounds.left));
  const auto y = static_cast<std::int32_t>(std::lround(bounds.top));
  const auto width = static_cast<std::int32_t>(std::lround(bounds.width));
  const auto height = static_cast<std::int32_t>(std::lround(bounds.height));
  int appended = sd_bus_message_append(reply, "(iiii)", x, y, width, height);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

// A boundary this model cannot name answers with an empty span at the offset,
// the way the ATK adaptor answered a unit its text could not describe.
std::optional<Boundary> unit_at(const std::string_view value,
                                const std::size_t character,
                                const std::uint32_t boundary) {
  if (boundary == kBoundaryCharacter)
    return boundary_at(value, character, Granularity::character);
  switch (boundary) {
  case kBoundaryWordStart:
  case kBoundaryWordEnd:
    return boundary_at(value, character, Granularity::word);
  case kBoundarySentenceStart:
  case kBoundarySentenceEnd:
    return boundary_at(value, character, Granularity::sentence);
  case kBoundaryLineStart:
  case kBoundaryLineEnd:
    return boundary_at(value, character, Granularity::line);
  default:
    return std::nullopt;
  }
}

// Runs are published per code point in screen space, so a run's byte range
// and rect answer extents and hit tests without recomputing text metrics.
const TextRun *run_of(const Text &text, const std::size_t byte) {
  const auto found = std::find_if(
      text.runs.begin(), text.runs.end(), [byte](const TextRun &run) {
        return byte >= run.begin && byte < run.end;
      });
  return found == text.runs.end() ? nullptr : &*found;
}

int get_version(sd_bus *, const char *path, const char *, const char *,
                sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!text_node(source, path, error)) return -ENOENT;
  const std::uint32_t version = 1;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_UINT32, &version);
}

int get_character_count(sd_bus *, const char *path, const char *, const char *,
                        sd_bus_message *reply, void *data,
                        sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = text_node(source, path, error);
  if (!node) return -ENOENT;
  const auto count = static_cast<std::int32_t>(character_count(node->text->value));
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &count);
}

int get_caret_offset(sd_bus *, const char *path, const char *, const char *,
                     sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = text_node(source, path, error);
  if (!node) return -ENOENT;
  const Text &text = *node->text;
  const std::int32_t caret =
      static_cast<std::int32_t>(char_offset(text.value, text.caret).value_or(0));
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &caret);
}

int get_text(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t start = 0;
  std::int32_t end = -1;
  if (sd_bus_message_read(message, "ii", &start, &end) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetText needs two offsets");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const Text &text = *node->text;
  const auto range = character_range(text.value, start, end);
  if (!range) return reply_string(message, "");
  const std::size_t from = byte_of(text.value, range->first);
  const std::size_t to = byte_of(text.value, range->second);
  return reply_string(message, std::string_view{text.value}.substr(from, to - from));
}

int get_character_at_offset(sd_bus_message *message, void *data,
                            sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t offset = 0;
  if (sd_bus_message_read(message, "i", &offset) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetCharacterAtOffset needs an offset");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const Text &text = *node->text;
  std::int32_t codepoint = 0;
  if (offset >= 0 && static_cast<std::size_t>(offset) < character_count(text.value)) {
    const std::size_t byte = byte_of(text.value, static_cast<std::size_t>(offset));
    if (const TextRun *run = run_of(text, byte)) {
      const auto bytes = std::string_view{text.value}.substr(run->begin, run->end - run->begin);
      const auto lead = static_cast<unsigned char>(bytes.front());
      const auto size = static_cast<std::size_t>(std::max(1, std::min(4, static_cast<int>(bytes.size()))));
      std::uint32_t value = size == 1 ? lead : lead & (0xFFu >> (size + 1));
      for (std::size_t i = 1; i < size && lead >= 0xC0; ++i)
        value = (value << 6) | (static_cast<unsigned char>(bytes[i]) & 0x3Fu);
      codepoint = static_cast<std::int32_t>(value);
    }
  }
  return reply_int32(message, codepoint);
}

int get_string_at_offset(sd_bus_message *message, void *data,
                         sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t offset = 0;
  std::uint32_t granularity = 0;
  if (sd_bus_message_read(message, "iu", &offset, &granularity) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetStringAtOffset needs an offset and a granularity");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const Text &text = *node->text;
  const std::size_t count = character_count(text.value);
  const auto character = static_cast<std::int64_t>(offset);
  if (character < 0 || character > static_cast<std::int64_t>(count))
    return reply_span(message, "", 0, 0);
  const std::size_t from = static_cast<std::size_t>(character);
  const auto unit = unit_at(text.value, from,
                            granularity == 0   ? kBoundaryCharacter
                            : granularity == 1 ? kBoundaryWordStart
                            : granularity == 2 ? kBoundarySentenceStart
                            : granularity == 3 ? kBoundaryLineStart
                                               : kBoundaryAttribute);
  if (!unit) return reply_span(message, "", from, from);
  return reply_span(message, unit->text, unit->start, unit->end);
}

// The deprecated methods name a unit by the offset it contains; BEFORE names
// the unit in front of that offset and AFTER the one behind it.
int offset_method(sd_bus_message *message, void *data, sd_bus_error *error,
                  const char *method, const int direction) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t offset = 0;
  std::uint32_t boundary = 0;
  if (sd_bus_message_read(message, "iu", &offset, &boundary) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "%s needs an offset and a boundary", method);
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const Text &text = *node->text;
  const std::size_t count = character_count(text.value);
  const auto character = static_cast<std::int64_t>(offset);
  if (character < 0 || character > static_cast<std::int64_t>(count))
    return reply_span(message, "", 0, 0);
  const std::size_t from = static_cast<std::size_t>(character);
  if (boundary > kBoundaryLineEnd) return reply_span(message, "", from, from);
  auto unit = unit_at(text.value, from, boundary);
  if (!unit) return reply_span(message, "", from, from);
  if (direction < 0) {
    // A boundary that ends before the offset, skipping the gap a newline and
    // other separators leave between published units.
    for (std::size_t probe = unit->start; probe > 0; --probe) {
      const auto previous = unit_at(text.value, probe - 1, boundary);
      if (!previous) continue;
      return reply_span(message, previous->text, previous->start, previous->end);
    }
    return reply_span(message, "", 0, 0);
  }
  if (direction > 0) {
    for (std::size_t probe = unit->end; probe < count; ++probe) {
      const auto next = unit_at(text.value, probe, boundary);
      if (!next) continue;
      return reply_span(message, next->text, next->start, next->end);
    }
    return reply_span(message, "", count, count);
  }
  return reply_span(message, unit->text, unit->start, unit->end);
}

int get_text_before_offset(sd_bus_message *message, void *data,
                           sd_bus_error *error) {
  return offset_method(message, data, error, "GetTextBeforeOffset", -1);
}

int get_text_at_offset(sd_bus_message *message, void *data,
                       sd_bus_error *error) {
  return offset_method(message, data, error, "GetTextAtOffset", 0);
}

int get_text_after_offset(sd_bus_message *message, void *data,
                          sd_bus_error *error) {
  return offset_method(message, data, error, "GetTextAfterOffset", 1);
}

int set_caret_offset(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t offset = 0;
  if (sd_bus_message_read(message, "i", &offset) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "SetCaretOffset needs an offset");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const Text &text = *node->text;
  const std::size_t count = character_count(text.value);
  if (offset < 0 || static_cast<std::size_t>(offset) > count)
    return reply_bool(message, false);
  const auto target = target_of(source, *node);
  if (!target) return reply_bool(message, false);
  return reply_bool(message,
                    a11y::set_caret(*target, byte_of(text.value, offset)));
}

int get_character_extents(sd_bus_message *message, void *data,
                          sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t offset = 0;
  std::uint32_t coordinate = kCoordScreen;
  if (sd_bus_message_read(message, "iu", &offset, &coordinate) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetCharacterExtents needs an offset and a coordinate");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const Text &text = *node->text;
  const std::size_t count = character_count(text.value);
  if (offset < 0 || static_cast<std::size_t>(offset) >= count)
    return reply_rect(message, Bounds{});
  const std::size_t byte = byte_of(text.value, static_cast<std::size_t>(offset));
  if (const TextRun *run = run_of(text, byte))
    return reply_rect(message, to_coordinate(source, *node, run->bounds, coordinate));
  // A field without published glyph geometry answers an empty rect: the model
  // never invents a position a client could act on.
  return reply_rect(message, Bounds{});
}

int get_range_extents(sd_bus_message *message, void *data,
                      sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t start = 0;
  std::int32_t end = 0;
  std::uint32_t coordinate = kCoordScreen;
  if (sd_bus_message_read(message, "iiu", &start, &end, &coordinate) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetRangeExtents needs two offsets and a coordinate");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const Text &text = *node->text;
  const std::size_t count = character_count(text.value);
  const auto clamp = [count](const std::int32_t value) {
    if (value < 0) return std::size_t{0};
    const auto offset = static_cast<std::size_t>(value);
    return offset > count ? count : offset;
  };
  const std::size_t from = clamp(start);
  const std::size_t to = clamp(end);
  if (to <= from || from >= count) return reply_rect(message, Bounds{});
  const std::size_t from_byte = byte_of(text.value, from);
  const std::size_t to_byte = byte_of(text.value, to);
  Bounds union_rect;
  bool any = false;
  for (const auto &run : text.runs) {
    if (run.end <= from_byte || run.begin >= to_byte) continue;
    if (!any) {
      union_rect = run.bounds;
      any = true;
      continue;
    }
    const double right = std::max(union_rect.left + union_rect.width,
                                  run.bounds.left + run.bounds.width);
    const double bottom = std::max(union_rect.top + union_rect.height,
                                   run.bounds.top + run.bounds.height);
    union_rect.left = std::min(union_rect.left, run.bounds.left);
    union_rect.top = std::min(union_rect.top, run.bounds.top);
    union_rect.width = right - union_rect.left;
    union_rect.height = bottom - union_rect.top;
  }
  if (!any) return reply_rect(message, Bounds{});
  return reply_rect(message, to_coordinate(source, *node, union_rect, coordinate));
}

int get_offset_at_point(sd_bus_message *message, void *data,
                        sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::uint32_t coordinate = kCoordScreen;
  if (sd_bus_message_read(message, "iiu", &x, &y, &coordinate) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetOffsetAtPoint needs x, y and a coordinate");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const Text &text = *node->text;
  const Point point = to_screen(source, *node,
                                {static_cast<double>(x), static_cast<double>(y)},
                                coordinate);
  for (const auto &run : text.runs) {
    const Bounds &bounds = run.bounds;
    if (point.x < bounds.left || point.y < bounds.top ||
        point.x >= bounds.left + bounds.width ||
        point.y >= bounds.top + bounds.height)
      continue;
    if (const auto character = char_offset(text.value, run.begin))
      return reply_int32(message, static_cast<std::int32_t>(*character));
    break;
  }
  return reply_int32(message, -1);
}

// Attributes are not part of the portable model, so every attribute answer is
// the empty set and every attribute range is the whole text.
int get_attributes(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t offset = 0;
  if (sd_bus_message_read(message, "i", &offset) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetAttributes needs an offset");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const std::size_t count = character_count(node->text->value);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  int appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{ss}");
  if (appended >= 0) appended = sd_bus_message_close_container(reply);
  const auto start = static_cast<std::int32_t>(
      std::clamp(static_cast<std::int64_t>(offset), std::int64_t{0},
                 static_cast<std::int64_t>(count)));
  const auto end = static_cast<std::int32_t>(count);
  if (appended >= 0) appended = sd_bus_message_append(reply, "ii", start, end);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_attribute_run(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t offset = 0;
  int include_defaults = 0;
  if (sd_bus_message_read(message, "ib", &offset, &include_defaults) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetAttributeRun needs an offset and a flag");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const std::size_t count = character_count(node->text->value);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  int appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{ss}");
  if (appended >= 0) appended = sd_bus_message_close_container(reply);
  const auto start = static_cast<std::int32_t>(
      std::clamp(static_cast<std::int64_t>(offset), std::int64_t{0},
                 static_cast<std::int64_t>(count)));
  const auto end = static_cast<std::int32_t>(count);
  if (appended >= 0) appended = sd_bus_message_append(reply, "ii", start, end);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_attribute_set(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  int appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{ss}");
  if (appended >= 0) appended = sd_bus_message_close_container(reply);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_attribute_value(sd_bus_message *message, void *data,
                        sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t offset = 0;
  const char *name = nullptr;
  if (sd_bus_message_read(message, "is", &offset, &name) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetAttributeValue needs an offset and a name");
  if (!request_text(source, message, error)) return -ENOENT;
  return reply_string(message, "");
}

int get_n_selections(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  const std::int32_t count =
      node->text->anchor != node->text->caret ? 1 : 0;
  return reply_int32(message, count);
}

int get_selection(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t index = 0;
  if (sd_bus_message_read(message, "i", &index) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetSelection needs a selection number");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  if (index != 0) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s has one selection",
                      node->name.c_str());
    return -EINVAL;
  }
  const Text &text = *node->text;
  const std::size_t anchor = char_offset(text.value, text.anchor).value_or(0);
  const std::size_t caret = char_offset(text.value, text.caret).value_or(0);
  const auto start = static_cast<std::int32_t>(std::min(anchor, caret));
  const auto end = static_cast<std::int32_t>(std::max(anchor, caret));
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  const int appended = sd_bus_message_append(reply, "(ii)", start, end);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int select_range(sd_bus_message *message, void *data, sd_bus_error *error,
                 const char *method, const bool has_index) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t index = 0;
  std::int32_t start = 0;
  std::int32_t end = 0;
  const int read = has_index ? sd_bus_message_read(message, "iii", &index, &start, &end)
                             : sd_bus_message_read(message, "ii", &start, &end);
  if (read < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "%s needs a character range", method);
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  if (has_index && index != 0) return reply_bool(message, false);
  const Text &text = *node->text;
  const auto range = character_range(text.value, start, end);
  if (!range) return reply_bool(message, false);
  const auto target = target_of(source, *node);
  if (!target) return reply_bool(message, false);
  return reply_bool(message, a11y::select_text(*target,
                                               byte_of(text.value, range->first),
                                               byte_of(text.value, range->second)));
}

int add_selection(sd_bus_message *message, void *data, sd_bus_error *error) {
  return select_range(message, data, error, "AddSelection", false);
}

int set_selection(sd_bus_message *message, void *data, sd_bus_error *error) {
  return select_range(message, data, error, "SetSelection", true);
}

int remove_selection(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t index = 0;
  if (sd_bus_message_read(message, "i", &index) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "RemoveSelection needs a selection number");
  const Node *node = request_text(source, message, error);
  if (!node) return -ENOENT;
  if (index != 0) return reply_bool(message, false);
  // Collapsing the selection is what FLTK's own Deselect does; the caret stays
  // where the user left it.
  const auto target = target_of(source, *node);
  if (!target) return reply_bool(message, false);
  return reply_bool(message,
                    a11y::select_text(*target, node->text->caret, node->text->caret));
}

int refuse(sd_bus_message *message, void *, sd_bus_error *) {
  return reply_bool(message, false);
}

int get_bounded_ranges(sd_bus_message *message, void *data,
                       sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!request_text(source, message, error)) return -ENOENT;
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  int appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(iisv)");
  if (appended >= 0) appended = sd_bus_message_close_container(reply);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

const sd_bus_vtable kText[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("version", "u", get_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CharacterCount", "i", get_character_count, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CaretOffset", "i", get_caret_offset, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("GetStringAtOffset", "iu", "sii", get_string_at_offset, 0),
    SD_BUS_METHOD("GetText", "ii", "s", get_text, 0),
    SD_BUS_METHOD("SetCaretOffset", "i", "b", set_caret_offset, 0),
    SD_BUS_METHOD("GetTextBeforeOffset", "iu", "sii", get_text_before_offset, 0),
    SD_BUS_METHOD("GetTextAtOffset", "iu", "sii", get_text_at_offset, 0),
    SD_BUS_METHOD("GetTextAfterOffset", "iu", "sii", get_text_after_offset, 0),
    SD_BUS_METHOD("GetCharacterAtOffset", "i", "i", get_character_at_offset, 0),
    SD_BUS_METHOD("GetAttributeValue", "is", "s", get_attribute_value, 0),
    SD_BUS_METHOD("GetAttributes", "i", "a{ss}ii", get_attributes, 0),
    SD_BUS_METHOD("GetDefaultAttributes", "", "a{ss}", get_attribute_set, 0),
    SD_BUS_METHOD("GetCharacterExtents", "iu", "(iiii)", get_character_extents, 0),
    SD_BUS_METHOD("GetOffsetAtPoint", "iiu", "i", get_offset_at_point, 0),
    SD_BUS_METHOD("GetNSelections", "", "i", get_n_selections, 0),
    SD_BUS_METHOD("GetSelection", "i", "(ii)", get_selection, 0),
    SD_BUS_METHOD("AddSelection", "ii", "b", add_selection, 0),
    SD_BUS_METHOD("RemoveSelection", "i", "b", remove_selection, 0),
    SD_BUS_METHOD("SetSelection", "iii", "b", set_selection, 0),
    SD_BUS_METHOD("GetRangeExtents", "iiu", "(iiii)", get_range_extents, 0),
    SD_BUS_METHOD("GetBoundedRanges", "iiiiuuu", "a(iisv)", get_bounded_ranges, 0),
    SD_BUS_METHOD("GetAttributeRun", "ib", "a{ss}ii", get_attribute_run, 0),
    SD_BUS_METHOD("GetDefaultAttributeSet", "", "a{ss}", get_attribute_set, 0),
    SD_BUS_METHOD("ScrollSubstringTo", "iiu", "b", refuse, 0),
    SD_BUS_METHOD("ScrollSubstringToPoint", "iiuii", "b", refuse, 0),
    SD_BUS_VTABLE_END};

} // namespace

namespace detail {

int text_find(sd_bus *bus, const char *path, const char *interface, void *data,
              void **found, sd_bus_error *error) {
  return interface_find(kTextInterface, bus, path, interface, data, found,
                        error);
}

const sd_bus_vtable *text_vtable() { return kText; }

} // namespace detail
} // namespace layerc::editor::a11y::atspi

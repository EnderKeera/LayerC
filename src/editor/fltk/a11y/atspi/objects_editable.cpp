#include "a11y/atspi/objects.hpp"

#include "a11y/atspi/mapping.hpp"
#include "a11y/atspi/objects_detail.hpp"
#include "a11y/fltk_actions.hpp"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace layerc::editor::a11y::atspi {
namespace {

using namespace detail;

const Node *editable_node(const ObjectSource &source, sd_bus_message *message,
                          sd_bus_error *error) {
  const Node *node = node_of_request(source, message, error);
  if (node == nullptr) return nullptr;
  if (!node->text || !exposes(source, *node, kEditableTextInterface)) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s is not editable",
                      node->name.c_str());
    return nullptr;
  }
  return node;
}

std::size_t character_count(const std::string_view value) {
  return char_offset(value, value.size()).value_or(0);
}

// Characters are the protocol's unit and bytes the model's; every offset is
// validated at a code-point boundary before it reaches the action layer.
std::optional<std::size_t> byte_of(const std::string_view value,
                                   const std::int32_t character) {
  if (character < 0 || static_cast<std::size_t>(character) > character_count(value))
    return std::nullopt;
  return byte_offset(value, static_cast<std::size_t>(character))
      .value_or(value.size());
}

int get_version(sd_bus *, const char *path, const char *, const char *,
                sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_at(source, path, error);
  if (!node) return -ENOENT;
  if (!exposes(source, *node, kEditableTextInterface)) return -EINVAL;
  const std::uint32_t version = 1;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_UINT32, &version);
}

int set_text_contents(sd_bus_message *message, void *data,
                      sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const char *text = nullptr;
  if (sd_bus_message_read(message, "s", &text) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "SetTextContents needs text");
  const Node *node = editable_node(source, message, error);
  if (!node) return -ENOENT;
  const auto target = target_of(source, *node);
  if (!target) return -EPERM;
  return reply_bool(message,
                    a11y::set_text(*target, text != nullptr ? text : ""));
}

int insert_text(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t offset = 0;
  std::int32_t length = 0;
  const char *text = nullptr;
  if (sd_bus_message_read(message, "isi", &offset, &text, &length) <= 0 ||
      text == nullptr || length < 0)
    return sd_bus_error_set_const(error, SD_BUS_ERROR_INVALID_ARGS,
                                  "InsertText needs an offset, text and a nonnegative byte count");
  const Node *node = editable_node(source, message, error);
  if (!node) return -ENOENT;
  const auto at = byte_of(node->text->value, offset);
  if (!at) return -EINVAL;
  const auto target = target_of(source, *node);
  if (!target) return -EPERM;
  // Positions count characters, but the client limits this UTF-8 string in
  // bytes. The action rejects a prefix ending inside a code point.
  std::string_view prefix{text};
  prefix = prefix.substr(0, std::min(prefix.size(), static_cast<std::size_t>(length)));
  return reply_bool(message, a11y::insert_text(*target, *at, prefix));
}

int delete_text(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t start = 0;
  std::int32_t end = 0;
  if (sd_bus_message_read(message, "ii", &start, &end) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "DeleteText needs a character range");
  const Node *node = editable_node(source, message, error);
  if (!node) return -ENOENT;
  if (end < start || end < 0) return -EINVAL;
  const auto from = byte_of(node->text->value, start);
  const auto to = byte_of(node->text->value, end);
  if (!from || !to) return -EINVAL;
  const auto target = target_of(source, *node);
  if (!target) return -EPERM;
  return reply_bool(message, a11y::delete_text(*target, *from, *to));
}

// Preserve the old adaptor's lack of clipboard operations. CopyText has no
// boolean result on the wire; an error reports refusal without claiming a copy.
int refuse_copy(sd_bus_message *, void *, sd_bus_error *error) {
  return sd_bus_error_set_const(error, SD_BUS_ERROR_NOT_SUPPORTED,
                                "Accessible clipboard copy is not supported");
}

int refuse(sd_bus_message *message, void *, sd_bus_error *) {
  return reply_bool(message, false);
}

const sd_bus_vtable kEditable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("version", "u", get_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("SetTextContents", "s", "b", set_text_contents, 0),
    SD_BUS_METHOD("InsertText", "isi", "b", insert_text, 0),
    SD_BUS_METHOD("DeleteText", "ii", "b", delete_text, 0),
    SD_BUS_METHOD("CopyText", "ii", "", refuse_copy, 0),
    SD_BUS_METHOD("CutText", "ii", "b", refuse, 0),
    SD_BUS_METHOD("PasteText", "i", "b", refuse, 0),
    SD_BUS_VTABLE_END};

} // namespace

namespace detail {

int editable_find(sd_bus *bus, const char *path, const char *interface,
                  void *data, void **found, sd_bus_error *error) {
  return interface_find(kEditableTextInterface, bus, path, interface, data,
                        found, error);
}

const sd_bus_vtable *editable_vtable() { return kEditable; }

} // namespace detail
} // namespace layerc::editor::a11y::atspi

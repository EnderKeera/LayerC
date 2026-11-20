#include "a11y/atspi/objects.hpp"

#include "a11y/atspi/mapping.hpp"
#include "a11y/atspi/objects_detail.hpp"
#include "a11y/fltk_actions.hpp"

#include <systemd/sd-bus.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace layerc::editor::a11y::atspi {
namespace {

using namespace detail;

const Node *selection_node(const ObjectSource &source, const char *path,
                           sd_bus_error *error) {
  const Node *node = node_at(source, path, error);
  if (node == nullptr) return nullptr;
  if (!exposes(source, *node, kSelectionInterface)) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s has no selection",
                      node->name.c_str());
    return nullptr;
  }
  return node;
}

const Node *request_selection(const ObjectSource &source,
                              sd_bus_message *message, sd_bus_error *error) {
  const Node *node = node_of_request(source, message, error);
  if (node == nullptr) return nullptr;
  if (!exposes(source, *node, kSelectionInterface)) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s has no selection",
                      node->name.c_str());
    return nullptr;
  }
  return node;
}

std::vector<NodeId> selected_children(const ObjectSource &source, const Node &node) {
  std::vector<NodeId> selected;
  for (const NodeId id : node.children) {
    const Node *child = find_node(source, id);
    if (child && selection_member(node.role, *child) && selected_item(*child))
      selected.push_back(id);
  }
  return selected;
}

const Node *child_at(const ObjectSource &source, const Node &node,
                     const std::int32_t index) {
  if (index < 0 || static_cast<std::size_t>(index) >= node.children.size())
    return nullptr;
  return find_node(source, node.children[static_cast<std::size_t>(index)]);
}

std::optional<std::int32_t> read_index(sd_bus_message *message,
                                       sd_bus_error *error,
                                       const char *method) {
  std::int32_t index = 0;
  if (sd_bus_message_read(message, "i", &index) < 0) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s needs a child index",
                      method);
    return std::nullopt;
  }
  return index;
}

int get_version(sd_bus *, const char *path, const char *, const char *,
                sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!selection_node(source, path, error)) return -ENOENT;
  const std::uint32_t version = 1;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_UINT32, &version);
}

int get_n_selected(sd_bus *, const char *path, const char *, const char *,
                   sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = selection_node(source, path, error);
  if (!node) return -ENOENT;
  const auto count = static_cast<std::int32_t>(selected_children(source, *node).size());
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &count);
}

int get_selected_child(sd_bus_message *message, void *data,
                       sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const auto index = read_index(message, error, "GetSelectedChild");
  if (!index) return -EINVAL;
  const Node *node = request_selection(source, message, error);
  if (!node) return -ENOENT;
  const auto chosen = selected_children(source, *node);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  const bool found = *index >= 0 && static_cast<std::size_t>(*index) < chosen.size();
  // append_reference opens and closes the (so) struct itself.
  const int appended =
      found ? append_reference(reply, source.unique_name,
                               chosen[static_cast<std::size_t>(*index)],
                               source.root)
            : append_null_reference(reply);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int select_child(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const auto index = read_index(message, error, "SelectChild");
  if (!index) return -EINVAL;
  const Node *node = request_selection(source, message, error);
  if (!node) return -ENOENT;
  const Node *child = child_at(source, *node, *index);
  if (child == nullptr) return -EINVAL;
  if (!selection_member(node->role, *child) || selected_item(*child))
    return reply_bool(message, false);
  if (child->states.disabled || !child->states.showing)
    return reply_bool(message, false);
  const auto target = target_of(source, *child);
  if (!target) return reply_bool(message, false);
  // FLTK's own selection route is the child's activation: clicking a list row
  // or a radio button is what chooses it.
  return reply_bool(message, a11y::activate(*target));
}

// FLTK's list, tab and radio containers expose no deselect operation to the
// portable action layer, and the ATK interface this replaces answered the same
// requests with false.
int refuse(sd_bus_message *message, void *, sd_bus_error *) {
  return reply_bool(message, false);
}

int is_child_selected(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const auto index = read_index(message, error, "IsChildSelected");
  if (!index) return -EINVAL;
  const Node *node = request_selection(source, message, error);
  if (!node) return -ENOENT;
  const Node *child = child_at(source, *node, *index);
  return reply_bool(message, child && selection_member(node->role, *child) && selected_item(*child));
}

const sd_bus_vtable kSelection[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("version", "u", get_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("NSelectedChildren", "i", get_n_selected, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("GetSelectedChild", "i", "(so)", get_selected_child, 0),
    SD_BUS_METHOD("SelectChild", "i", "b", select_child, 0),
    SD_BUS_METHOD("DeselectSelectedChild", "i", "b", refuse, 0),
    SD_BUS_METHOD("IsChildSelected", "i", "b", is_child_selected, 0),
    SD_BUS_METHOD("SelectAll", "", "b", refuse, 0),
    SD_BUS_METHOD("ClearSelection", "", "b", refuse, 0),
    SD_BUS_METHOD("DeselectChild", "i", "b", refuse, 0),
    SD_BUS_VTABLE_END};

} // namespace

namespace detail {

int selection_find(sd_bus *bus, const char *path, const char *interface,
                   void *data, void **found, sd_bus_error *error) {
  return interface_find(kSelectionInterface, bus, path, interface, data, found,
                        error);
}

const sd_bus_vtable *selection_vtable() { return kSelection; }

} // namespace detail
} // namespace layerc::editor::a11y::atspi

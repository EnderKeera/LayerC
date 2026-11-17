#include "a11y/atspi/objects.hpp"

#include "a11y/atspi/mapping.hpp"
#include "a11y/atspi/objects_detail.hpp"

#include <systemd/sd-bus.h>

#include <cerrno>
#include <clocale>
#include <cstddef>
#include <string>

namespace layerc::editor::a11y::atspi {
namespace detail {

const Node *node_at(const ObjectSource &source, const char *path,
                    sd_bus_error *error) {
  const auto id = node_id_of(path ? path : "", source.root);
  if (id) {
    if (const Node *node = find_node(source, *id)) return node;
  }
  sd_bus_error_setf(error, SD_BUS_ERROR_UNKNOWN_OBJECT, "Unknown object '%s'",
                    path ? path : "");
  return nullptr;
}

const Node *node_of_request(const ObjectSource &source, sd_bus_message *message,
                            sd_bus_error *error) {
  return node_at(source, sd_bus_message_get_path(message), error);
}

std::int32_t index_in_parent(const ObjectSource &source, const Node &node) {
  if (!node.parent) return -1;
  const Node *parent = find_node(source, node.parent);
  if (!parent) return -1;
  for (std::size_t i = 0; i < parent->children.size(); ++i)
    if (parent->children[i] == node.id) return static_cast<std::int32_t>(i);
  return -1;
}

std::string locale_name() {
  const char *name = std::setlocale(LC_ALL, nullptr);
  return name && *name ? std::string{name} : std::string{"C"};
}

int finish(sd_bus_message *message, sd_bus_message *reply) {
  if (!reply) return -ENOMEM;
  const int sent = sd_bus_send(sd_bus_message_get_bus(message), reply, nullptr);
  sd_bus_message_unref(reply);
  return sent < 0 ? sent : 1;
}

int reply_string(sd_bus_message *message, std::string_view value) {
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  const std::string text{value};
  if (sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, text.c_str()) < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

} // namespace detail

using namespace detail;

std::string node_path(const NodeId id, const NodeId root) {
  if (id == root) return std::string{kRootPath};
  return std::string{kAccessiblePrefix} + "/" + std::to_string(id);
}

std::optional<NodeId> node_id_of(const std::string_view path,
                                 const NodeId root) {
  if (path == kRootPath) return root;
  if (path.size() <= kAccessiblePrefix.size() + 1) return std::nullopt;
  if (path.substr(0, kAccessiblePrefix.size()) != kAccessiblePrefix)
    return std::nullopt;
  if (path[kAccessiblePrefix.size()] != '/') return std::nullopt;
  NodeId id = 0;
  for (const char digit : path.substr(kAccessiblePrefix.size() + 1)) {
    if (digit < '0' || digit > '9') return std::nullopt;
    const NodeId next = id * 10 + static_cast<NodeId>(digit - '0');
    if (next < id) return std::nullopt;
    id = next;
  }
  if (id == 0) return std::nullopt;
  return id;
}

const Node *find_node(const ObjectSource &source, const NodeId id) {
  if (!source.snapshot) return nullptr;
  const auto found = source.snapshot->nodes.find(id);
  return found == source.snapshot->nodes.end() ? nullptr : &found->second;
}

int append_reference(sd_bus_message *message, const std::string_view name,
                     const NodeId id, const NodeId root) {
  int appended = sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT,
                                               "so");
  const std::string text{name};
  if (appended >= 0)
    appended =
        sd_bus_message_append_basic(message, SD_BUS_TYPE_STRING, text.c_str());
  if (appended >= 0) {
    const std::string path = node_path(id, root);
    appended = sd_bus_message_append_basic(message, SD_BUS_TYPE_OBJECT_PATH,
                                           path.c_str());
  }
  if (appended >= 0) appended = sd_bus_message_close_container(message);
  return appended;
}

int append_null_reference(sd_bus_message *message) {
  int appended = sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT,
                                               "so");
  if (appended >= 0)
    appended = sd_bus_message_append_basic(message, SD_BUS_TYPE_STRING, "");
  if (appended >= 0)
    appended = sd_bus_message_append_basic(message, SD_BUS_TYPE_OBJECT_PATH,
                                           std::string{kNullPath}.c_str());
  if (appended >= 0) appended = sd_bus_message_close_container(message);
  return appended;
}

int append_desktop_reference(sd_bus_message *message,
                             const ObjectSource &source) {
  int appended = sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT,
                                               "so");
  if (appended >= 0)
    appended = sd_bus_message_append_basic(
        message, SD_BUS_TYPE_STRING, source.desktop_name.c_str());
  if (appended >= 0)
    appended = sd_bus_message_append_basic(
        message, SD_BUS_TYPE_OBJECT_PATH, source.desktop_path.c_str());
  if (appended >= 0) appended = sd_bus_message_close_container(message);
  return appended;
}

void append_parent_reference(sd_bus_message *message,
                             const ObjectSource &source, const Node &node) {
  if (node.id == source.root) {
    append_desktop_reference(message, source);
  } else if (!node.parent) {
    append_null_reference(message);
  } else {
    append_reference(message, source.unique_name, node.parent, source.root);
  }
}

void append_cache_item(sd_bus_message *message, const ObjectSource &source,
                       const NodeId id, const Node &node) {
  append_reference(message, source.unique_name, id, source.root);
  append_reference(message, source.unique_name, source.root, source.root);
  append_parent_reference(message, source, node);
  const std::int32_t index = index_in_parent(source, node);
  const auto children = static_cast<std::int32_t>(node.children.size());
  sd_bus_message_append_basic(message, SD_BUS_TYPE_INT32, &index);
  sd_bus_message_append_basic(message, SD_BUS_TYPE_INT32, &children);
  const auto names = interfaces(node, id == source.root);
  sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "s");
  for (const auto name : names) {
    const std::string text{name};
    sd_bus_message_append_basic(message, SD_BUS_TYPE_STRING, text.c_str());
  }
  sd_bus_message_close_container(message);
  sd_bus_message_append_basic(message, SD_BUS_TYPE_STRING, node.name.c_str());
  const std::uint32_t role = role_code(node);
  sd_bus_message_append_basic(message, SD_BUS_TYPE_UINT32, &role);
  sd_bus_message_append_basic(message, SD_BUS_TYPE_STRING,
                              node.description.c_str());
  const StateSet states = state_bits(node);
  sd_bus_message_append_array(message, SD_BUS_TYPE_UINT32, states.data(),
                              states.size() * sizeof(std::uint32_t));
}

void add_accessible_object(sd_bus *bus, ObjectSource *source,
                           sd_bus_slot **slot) {
  sd_bus_add_fallback_vtable(bus, slot, kAccessiblePrefix.data(),
                             kAccessibleInterface, accessible_vtable(),
                             accessible_find, source);
}

void add_application_object(sd_bus *bus, ObjectSource *source,
                            sd_bus_slot **slot) {
  sd_bus_add_fallback_vtable(bus, slot, kAccessiblePrefix.data(),
                             kApplicationInterface, application_vtable(),
                             application_find, source);
}

void add_cache_object(sd_bus *bus, ObjectSource *source, sd_bus_slot **slot) {
  sd_bus_add_fallback_vtable(bus, slot, std::string{kCachePath}.c_str(),
                             kCacheInterface, cache_vtable(), cache_find,
                             source);
}

} // namespace layerc::editor::a11y::atspi

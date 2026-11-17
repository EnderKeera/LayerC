#include "a11y/atspi/objects.hpp"

#include "a11y/atspi/mapping.hpp"
#include "a11y/atspi/objects_detail.hpp"

#include <systemd/sd-bus.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace layerc::editor::a11y::atspi {
namespace {

using namespace detail;

// AT-SPI relation numbers.
constexpr std::uint32_t kLabelFor = 1;
constexpr std::uint32_t kLabelledBy = 2;
constexpr std::uint32_t kDescriptionFor = 17;
constexpr std::uint32_t kDescribedBy = 18;

int get_name(sd_bus *, const char *path, const char *, const char *,
             sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_at(source, path, error);
  if (!node) return -ENOENT;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING,
                                     node->name.c_str());
}

int get_description(sd_bus *, const char *path, const char *, const char *,
                    sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_at(source, path, error);
  if (!node) return -ENOENT;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING,
                                     node->description.c_str());
}

int get_parent(sd_bus *, const char *path, const char *, const char *,
               sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_at(source, path, error);
  if (!node) return -ENOENT;
  if (node->id == source.root)
    return append_desktop_reference(reply, source);
  if (!node->parent) return append_null_reference(reply);
  return append_reference(reply, source.unique_name, node->parent,
                          source.root);
}

int get_child_count(sd_bus *, const char *path, const char *, const char *,
                    sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_at(source, path, error);
  if (!node) return -ENOENT;
  const auto count = static_cast<std::int32_t>(node->children.size());
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &count);
}

int get_locale(sd_bus *, const char *path, const char *, const char *,
               sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!node_at(source, path, error)) return -ENOENT;
  const std::string locale = locale_name();
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, locale.c_str());
}

int get_accessible_id(sd_bus *, const char *path, const char *, const char *,
                      sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_at(source, path, error);
  if (!node) return -ENOENT;
  const std::string id = std::to_string(node->id);
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, id.c_str());
}

int get_help_text(sd_bus *, const char *path, const char *, const char *,
                  sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!node_at(source, path, error)) return -ENOENT;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, "");
}

int get_child_at_index(sd_bus_message *message, void *data,
                       sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t index = 0;
  if (sd_bus_message_read(message, "i", &index) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetChildAtIndex needs an index");
  const Node *node = node_of_request(source, message, error);
  if (!node) return -ENOENT;
  if (index < 0 || static_cast<std::size_t>(index) >= node->children.size())
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "%s has no child %d", node->name.c_str(), index);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  if (append_reference(reply, source.unique_name,
                       node->children[static_cast<std::size_t>(index)],
                       source.root) < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_children(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_of_request(source, message, error);
  if (!node) return -ENOENT;
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  int appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(so)");
  for (const NodeId child : node->children) {
    if (appended < 0) break;
    appended = append_reference(reply, source.unique_name, child, source.root);
  }
  if (appended >= 0)
    appended = sd_bus_message_close_container(reply);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_index_in_parent(sd_bus_message *message, void *data,
                        sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_of_request(source, message, error);
  if (!node) return -ENOENT;
  const std::int32_t index = index_in_parent(source, *node);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  if (sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &index) < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

// Relations come from the model's label and description links, in both
// directions: the label names its target and the target names its label.
int get_relation_set(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_of_request(source, message, error);
  if (!node) return -ENOENT;
  std::vector<std::uint32_t> kinds;
  std::vector<std::vector<NodeId>> targets;
  const auto add = [&](std::uint32_t kind, const std::vector<NodeId> &ids) {
    if (ids.empty()) return;
    kinds.push_back(kind);
    targets.push_back(ids);
  };
  const auto reverse = [&](std::vector<NodeId> Node::*link,
                           std::uint32_t kind) {
    std::vector<NodeId> found;
    for (const auto &[id, other] : source.snapshot->nodes) {
      if (id == node->id) continue;
      for (const NodeId target : other.*link)
        if (target == node->id) found.push_back(id);
    }
    add(kind, found);
  };
  add(kLabelledBy, node->labelled_by);
  add(kDescribedBy, node->described_by);
  reverse(&Node::labelled_by, kLabelFor);
  reverse(&Node::described_by, kDescriptionFor);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  int appended =
      sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(ua(so))");
  for (std::size_t i = 0; appended >= 0 && i < kinds.size(); ++i) {
    appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT,
                                             "ua(so)");
    if (appended >= 0)
      appended =
          sd_bus_message_append_basic(reply, SD_BUS_TYPE_UINT32, &kinds[i]);
    if (appended >= 0)
      appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY,
                                               "(so)");
    for (std::size_t j = 0; appended >= 0 && j < targets[i].size(); ++j)
      appended = append_reference(reply, source.unique_name, targets[i][j],
                                  source.root);
    if (appended >= 0) appended = sd_bus_message_close_container(reply);
    if (appended >= 0) appended = sd_bus_message_close_container(reply);
  }
  if (appended >= 0) appended = sd_bus_message_close_container(reply);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_role(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_of_request(source, message, error);
  if (!node) return -ENOENT;
  const std::uint32_t role = role_code(*node);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  if (sd_bus_message_append_basic(reply, SD_BUS_TYPE_UINT32, &role) < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_role_name(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_of_request(source, message, error);
  if (!node) return -ENOENT;
  return reply_string(message, role_name(*node));
}

int get_state(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_of_request(source, message, error);
  if (!node) return -ENOENT;
  const StateSet states = state_bits(*node);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  if (sd_bus_message_append_array(reply, SD_BUS_TYPE_UINT32, states.data(),
                                  states.size() * sizeof(std::uint32_t)) < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_attributes(sd_bus_message *message, void *, sd_bus_error *) {
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  if (sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{ss}") < 0 ||
      sd_bus_message_close_container(reply) < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_application(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!node_of_request(source, message, error)) return -ENOENT;
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  if (append_reference(reply, source.unique_name, source.root, source.root) <
      0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_interfaces(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_of_request(source, message, error);
  if (!node) return -ENOENT;
  const auto names = interfaces(*node, node->id == source.root);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  int appended =
      sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "s");
  for (std::size_t i = 0; appended >= 0 && i < names.size(); ++i) {
    const std::string name{names[i]};
    appended = sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING,
                                           name.c_str());
  }
  if (appended >= 0) appended = sd_bus_message_close_container(reply);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

const sd_bus_vtable kAccessible[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Name", "s", get_name, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Description", "s", get_description, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Parent", "(so)", get_parent, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ChildCount", "i", get_child_count, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Locale", "s", get_locale, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AccessibleId", "s", get_accessible_id, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HelpText", "s", get_help_text, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("GetChildAtIndex", "i", "(so)", get_child_at_index, 0),
    SD_BUS_METHOD("GetChildren", "", "a(so)", get_children, 0),
    SD_BUS_METHOD("GetIndexInParent", "", "i", get_index_in_parent, 0),
    SD_BUS_METHOD("GetRelationSet", "", "a(ua(so))", get_relation_set, 0),
    SD_BUS_METHOD("GetRole", "", "u", get_role, 0),
    SD_BUS_METHOD("GetRoleName", "", "s", get_role_name, 0),
    SD_BUS_METHOD("GetLocalizedRoleName", "", "s", get_role_name, 0),
    SD_BUS_METHOD("GetState", "", "au", get_state, 0),
    SD_BUS_METHOD("GetAttributes", "", "a{ss}", get_attributes, 0),
    SD_BUS_METHOD("GetApplication", "", "(so)", get_application, 0),
    SD_BUS_METHOD("GetInterfaces", "", "as", get_interfaces, 0),
    SD_BUS_VTABLE_END};

} // namespace

namespace detail {

int accessible_find(sd_bus *, const char *path, const char *interface,
                    void *data, void **found, sd_bus_error *error) {
  auto *source = static_cast<ObjectSource *>(data);
  if (!source) return 0;
  if (interface && std::string{interface} != kAccessibleInterface) return 0;
  const auto id = node_id_of(path ? path : "", source->root);
  if (!id || !find_node(*source, *id)) {
    sd_bus_error_setf(error, SD_BUS_ERROR_UNKNOWN_OBJECT,
                      "Unknown object '%s'", path ? path : "");
    return -ENOENT;
  }
  *found = source;
  return 1;
}

const sd_bus_vtable *accessible_vtable() { return kAccessible; }

} // namespace detail
} // namespace layerc::editor::a11y::atspi

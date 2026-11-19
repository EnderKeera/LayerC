#include "a11y/atspi/objects.hpp"

#include "a11y/atspi/mapping.hpp"
#include "a11y/atspi/objects_detail.hpp"
#include "a11y/fltk_actions.hpp"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace layerc::editor::a11y::atspi {
namespace {

using namespace detail;

// The description the ATK adaptor gave every action, kept so a client reading
// both adaptors sees the same words.
constexpr const char *kActionDescription = "Activate this control";

const Node *request_node(const ObjectSource &source, sd_bus_message *message,
                         sd_bus_error *error) {
  return node_of_request(source, message, error);
}

std::optional<std::int32_t> read_index(sd_bus_message *message,
                                       sd_bus_error *error,
                                       const char *method) {
  std::int32_t index = 0;
  if (sd_bus_message_read(message, "i", &index) < 0) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s needs an index",
                      method);
    return std::nullopt;
  }
  return index;
}

// The requested action's name, or nothing with a D-Bus error set: an out of
// range index is the client's mistake, not an empty answer.
const char *action_at(const Node &node, const std::int32_t index,
                      sd_bus_error *error) {
  if (index < 0 || static_cast<std::size_t>(index) >= node.actions.size()) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s has no action %d",
                      node.name.c_str(), index);
    return nullptr;
  }
  return node.actions[static_cast<std::size_t>(index)].c_str();
}

int get_version(sd_bus *, const char *path, const char *, const char *,
                sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!node_at(source, path, error)) return -ENOENT;
  const std::uint32_t version = 1;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_UINT32, &version);
}

int get_n_actions(sd_bus *, const char *path, const char *, const char *,
                  sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = node_at(source, path, error);
  if (!node) return -ENOENT;
  const auto count = static_cast<std::int32_t>(node->actions.size());
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &count);
}

int get_name(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const auto index = read_index(message, error, "GetName");
  if (!index) return -EINVAL;
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  const char *name = action_at(*node, *index, error);
  if (name == nullptr) return -EINVAL;
  return reply_string(message, name);
}

int get_description(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const auto index = read_index(message, error, "GetDescription");
  if (!index) return -EINVAL;
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  if (action_at(*node, *index, error) == nullptr) return -EINVAL;
  return reply_string(message, kActionDescription);
}

// Only the first action carries the widget's mnemonic or shortcut; the others
// are synthetic verbs with nothing a client could press.
int get_key_binding(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const auto index = read_index(message, error, "GetKeyBinding");
  if (!index) return -EINVAL;
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  if (action_at(*node, *index, error) == nullptr) return -EINVAL;
  return reply_string(message, *index == 0 ? node->keybinding : std::string_view{});
}

int get_actions(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  int appended =
      sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(sss)");
  for (std::size_t i = 0; appended >= 0 && i < node->actions.size(); ++i) {
    const bool first = i == 0;
    appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "sss");
    if (appended >= 0)
      appended = sd_bus_message_append_basic(
          reply, SD_BUS_TYPE_STRING, node->actions[i].c_str());
    if (appended >= 0)
      appended = sd_bus_message_append_basic(
          reply, SD_BUS_TYPE_STRING, kActionDescription);
    if (appended >= 0) {
      const std::string binding = first ? node->keybinding : std::string{};
      appended = sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING,
                                             binding.c_str());
    }
    if (appended >= 0) appended = sd_bus_message_close_container(reply);
  }
  if (appended >= 0) appended = sd_bus_message_close_container(reply);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int do_action(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const auto index = read_index(message, error, "DoAction");
  if (!index) return -EINVAL;
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  if (action_at(*node, *index, error) == nullptr) return -EINVAL;
  // Visibility is the shared layer's rule, not the snapshot's: an item of a
  // closed menu is not showing, yet it is invoked through its visible menu.
  if (node->states.disabled) return reply_bool(message, false);
  const auto target = target_of(source, *node);
  if (!target) return reply_bool(message, false);
  // True means queued on FLTK's loop; the application callback runs there, so
  // the protocol call never re-enters widget code.
  return reply_bool(message, a11y::activate(*target));
}

const sd_bus_vtable kAction[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("version", "u", get_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("NActions", "i", get_n_actions, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("GetName", "i", "s", get_name, 0),
    SD_BUS_METHOD("GetLocalizedName", "i", "s", get_name, 0),
    SD_BUS_METHOD("GetDescription", "i", "s", get_description, 0),
    SD_BUS_METHOD("GetKeyBinding", "i", "s", get_key_binding, 0),
    SD_BUS_METHOD("GetActions", "", "a(sss)", get_actions, 0),
    SD_BUS_METHOD("DoAction", "i", "b", do_action, 0),
    SD_BUS_VTABLE_END};

const Node *value_node(const ObjectSource &source, const char *path,
                       sd_bus_error *error) {
  const Node *node = node_at(source, path, error);
  if (node == nullptr) return nullptr;
  if (!node->range) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                      "%s carries no value", node->name.c_str());
    return nullptr;
  }
  return node;
}

int get_value_version(sd_bus *, const char *path, const char *, const char *,
                      sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!value_node(source, path, error)) return -ENOENT;
  const std::uint32_t version = 1;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_UINT32, &version);
}

int get_minimum(sd_bus *, const char *path, const char *, const char *,
                sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = value_node(source, path, error);
  if (!node) return -ENOENT;
  const double value = node->range->minimum;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_DOUBLE, &value);
}

int get_maximum(sd_bus *, const char *path, const char *, const char *,
                sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = value_node(source, path, error);
  if (!node) return -ENOENT;
  const double value = node->range->maximum;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_DOUBLE, &value);
}

int get_increment(sd_bus *, const char *path, const char *, const char *,
                  sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = value_node(source, path, error);
  if (!node) return -ENOENT;
  const double value = node->range->step;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_DOUBLE, &value);
}

int get_current(sd_bus *, const char *path, const char *, const char *,
                sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = value_node(source, path, error);
  if (!node) return -ENOENT;
  const double value = node->range->value;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_DOUBLE, &value);
}

int set_current(sd_bus *, const char *path, const char *, const char *,
                sd_bus_message *value, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = value_node(source, path, error);
  if (!node) return -ENOENT;
  if (node->states.disabled || node->states.read_only) return -EPERM;
  double requested = 0.0;
  if (sd_bus_message_read(value, "d", &requested) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "CurrentValue needs a double");
  const auto target = target_of(source, *node);
  if (!target) return -EPERM;
  // AT-SPI asks for clamping rather than rejection, so a client's rounding
  // error at either end still sets the end stop.
  const double clamped =
      std::clamp(requested, node->range->minimum, node->range->maximum);
  if (!a11y::set_number(*target, clamped)) return -EPERM;
  return 1;
}

int get_value_text(sd_bus *, const char *path, const char *, const char *,
                   sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!value_node(source, path, error)) return -ENOENT;
  // The ATK adaptor exposed no value text for these widgets.
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, "");
}

const sd_bus_vtable kValue[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("version", "u", get_value_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MinimumValue", "d", get_minimum, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MaximumValue", "d", get_maximum, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MinimumIncrement", "d", get_increment, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_WRITABLE_PROPERTY("CurrentValue", "d", get_current, set_current, 0,
                             SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Text", "s", get_value_text, 0, 0),
    SD_BUS_VTABLE_END};

} // namespace

namespace detail {

int action_find(sd_bus *bus, const char *path, const char *interface,
                void *data, void **found, sd_bus_error *error) {
  return interface_find(kActionInterface, bus, path, interface, data, found,
                        error);
}

const sd_bus_vtable *action_vtable() { return kAction; }

int value_find(sd_bus *bus, const char *path, const char *interface,
               void *data, void **found, sd_bus_error *error) {
  return interface_find(kValueInterface, bus, path, interface, data, found,
                        error);
}

const sd_bus_vtable *value_vtable() { return kValue; }

} // namespace detail
} // namespace layerc::editor::a11y::atspi

#include "a11y/atspi/objects.hpp"

#include "a11y/atspi/mapping.hpp"
#include "a11y/atspi/objects_detail.hpp"
#include "a11y/fltk_actions.hpp"

#include <systemd/sd-bus.h>

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace layerc::editor::a11y::atspi {
namespace {

using namespace detail;

// AtspiComponentLayer.
constexpr std::uint32_t kLayerWidget = 3;
constexpr std::uint32_t kLayerWindow = 7;

std::int32_t rounded(const double value) {
  return static_cast<std::int32_t>(std::lround(value));
}

bool contains(const Bounds &bounds, const double x, const double y) {
  return x >= bounds.left && y >= bounds.top &&
         x < bounds.left + bounds.width && y < bounds.top + bounds.height;
}

// The deepest showing descendant under the point. The node itself never
// answers, like ATK's ref_accessible_at_point.
NodeId find_at(const ObjectSource &source, const Node &parent, const double x,
               const double y) {
  for (const NodeId child_id : parent.children) {
    const Node *child = find_node(source, child_id);
    if (child == nullptr || !child->states.showing ||
        child->bounds.width <= 0.0 || child->bounds.height <= 0.0)
      continue;
    if (!contains(child->bounds, x, y)) continue;
    const NodeId deeper = find_at(source, *child, x, y);
    return deeper != no_node ? deeper : child_id;
  }
  return no_node;
}

const Node *request_node(const ObjectSource &source, sd_bus_message *message,
                         sd_bus_error *error) {
  return node_of_request(source, message, error);
}

// SPI_DBUS_COMPONENT_VERSION: GetAccessibleAtPoint is the second revision of
// the interface, and every other method here exists in it.
int get_version(sd_bus *, const char *path, const char *, const char *,
                sd_bus_message *reply, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!node_at(source, path, error)) return -ENOENT;
  const std::uint32_t version = 2;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_UINT32, &version);
}

int contains_point(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::uint32_t coordinate = kCoordScreen;
  if (sd_bus_message_read(message, "iiu", &x, &y, &coordinate) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "Contains needs x, y and a coordinate type");
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  const Point point = to_screen(source, *node,
                                {static_cast<double>(x), static_cast<double>(y)},
                                coordinate);
  return reply_bool(message, contains(node->bounds, point.x, point.y));
}

int accessible_at_point(sd_bus_message *message, void *data,
                        sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::uint32_t coordinate = kCoordScreen;
  if (sd_bus_message_read(message, "iiu", &x, &y, &coordinate) < 0)
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "GetAccessibleAtPoint needs x, y and a coordinate");
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  const Point point = to_screen(source, *node,
                                {static_cast<double>(x), static_cast<double>(y)},
                                coordinate);
  const NodeId found = find_at(source, *node, point.x, point.y);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  const int appended =
      found == no_node
          ? append_null_reference(reply)
          : append_reference(reply, source.unique_name, found, source.root);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

std::optional<std::uint32_t> read_coordinate(sd_bus_message *message,
                                             sd_bus_error *error,
                                             const char *method) {
  std::uint32_t coordinate = kCoordScreen;
  if (sd_bus_message_read(message, "u", &coordinate) < 0) {
    sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                      "%s needs a coordinate type", method);
    return std::nullopt;
  }
  return coordinate;
}

int get_extents(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const auto coordinate = read_coordinate(message, error, "GetExtents");
  if (!coordinate) return -EINVAL;
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  const Bounds bounds = to_coordinate(source, *node, node->bounds, *coordinate);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  int appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "iiii");
  const std::int32_t values[4] = {rounded(bounds.left), rounded(bounds.top),
                                  rounded(bounds.width), rounded(bounds.height)};
  for (const std::int32_t value : values) {
    if (appended < 0) break;
    appended = sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &value);
  }
  if (appended >= 0) appended = sd_bus_message_close_container(reply);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_position(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const auto coordinate = read_coordinate(message, error, "GetPosition");
  if (!coordinate) return -EINVAL;
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  const Bounds bounds = to_coordinate(source, *node, node->bounds, *coordinate);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  const std::int32_t x = rounded(bounds.left);
  const std::int32_t y = rounded(bounds.top);
  int appended = sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &x);
  if (appended >= 0)
    appended = sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &y);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_size(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  const std::int32_t width = rounded(node->bounds.width);
  const std::int32_t height = rounded(node->bounds.height);
  int appended = sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &width);
  if (appended >= 0)
    appended = sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &height);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

int get_layer(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  const bool is_window =
      node->role == Role::window || node->role == Role::dialog;
  return reply_uint32(message, is_window ? kLayerWindow : kLayerWidget);
}

int get_mdi_order(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!request_node(source, message, error)) return -ENOENT;
  return reply_int16(message, -1);
}

int get_alpha(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  if (!request_node(source, message, error)) return -ENOENT;
  return reply_double(message, 1.0);
}

int grab_focus(sd_bus_message *message, void *data, sd_bus_error *error) {
  const auto &source = *static_cast<ObjectSource *>(data);
  const Node *node = request_node(source, message, error);
  if (!node) return -ENOENT;
  if (!node->states.focusable || node->states.disabled || !node->states.showing)
    return reply_bool(message, false);
  const auto target = target_of(source, *node);
  if (!target) return reply_bool(message, false);
  return reply_bool(message, a11y::grab_focus(*target));
}

// The interface has no setters in this adaptor: every position, size and
// scroll method answers false, exactly like a read-only component.
int refuse(sd_bus_message *message, void *, sd_bus_error *) {
  return reply_bool(message, false);
}

const sd_bus_vtable kComponent[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("version", "u", get_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Contains", "iiu", "b", contains_point, 0),
    SD_BUS_METHOD("GetAccessibleAtPoint", "iiu", "(so)", accessible_at_point, 0),
    SD_BUS_METHOD("GetExtents", "u", "(iiii)", get_extents, 0),
    SD_BUS_METHOD("GetPosition", "u", "ii", get_position, 0),
    SD_BUS_METHOD("GetSize", "", "ii", get_size, 0),
    SD_BUS_METHOD("GetLayer", "", "u", get_layer, 0),
    SD_BUS_METHOD("GetMDIZOrder", "", "n", get_mdi_order, 0),
    SD_BUS_METHOD("GrabFocus", "", "b", grab_focus, 0),
    SD_BUS_METHOD("GetAlpha", "", "d", get_alpha, 0),
    SD_BUS_METHOD("SetExtents", "iiiiu", "b", refuse, 0),
    SD_BUS_METHOD("SetPosition", "iiu", "b", refuse, 0),
    SD_BUS_METHOD("SetSize", "ii", "b", refuse, 0),
    SD_BUS_METHOD("ScrollTo", "u", "b", refuse, 0),
    SD_BUS_METHOD("ScrollToPoint", "uii", "b", refuse, 0),
    SD_BUS_VTABLE_END};

} // namespace

namespace detail {

int component_find(sd_bus *bus, const char *path, const char *interface,
                   void *data, void **found, sd_bus_error *error) {
  return interface_find(kComponentInterface, bus, path, interface, data, found,
                        error);
}

const sd_bus_vtable *component_vtable() { return kComponent; }

} // namespace detail
} // namespace layerc::editor::a11y::atspi

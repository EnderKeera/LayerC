#pragma once

#include "a11y/atspi/objects.hpp"

#include <systemd/sd-bus.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::editor::a11y {
struct Target;
}

namespace layerc::editor::a11y::atspi::detail {

inline constexpr const char *kAccessibleInterface = "org.a11y.atspi.Accessible";
inline constexpr const char *kApplicationInterface = "org.a11y.atspi.Application";
inline constexpr const char *kCacheInterface = "org.a11y.atspi.Cache";
inline constexpr const char *kComponentInterface = "org.a11y.atspi.Component";
inline constexpr const char *kActionInterface = "org.a11y.atspi.Action";
inline constexpr const char *kTextInterface = "org.a11y.atspi.Text";
inline constexpr const char *kEditableTextInterface = "org.a11y.atspi.EditableText";
inline constexpr const char *kValueInterface = "org.a11y.atspi.Value";
inline constexpr const char *kSelectionInterface = "org.a11y.atspi.Selection";
inline constexpr const char *kCacheItemText = kCacheItem.data();
inline constexpr const char *kCacheFieldsText = kCacheFields.data();

// The node a request names, or nothing with a D-Bus error set.
[[nodiscard]] const Node *node_at(const ObjectSource &source, const char *path,
                                  sd_bus_error *error);
[[nodiscard]] const Node *node_of_request(const ObjectSource &source,
                                          sd_bus_message *message,
                                          sd_bus_error *error);
[[nodiscard]] std::int32_t index_in_parent(const ObjectSource &source,
                                           const Node &node);
[[nodiscard]] std::string locale_name();

// Sends a finished reply; the vtable handlers' shared return convention.
int finish(sd_bus_message *message, sd_bus_message *reply);
int reply_string(sd_bus_message *message, std::string_view value);
int reply_bool(sd_bus_message *message, bool value);
int reply_int16(sd_bus_message *message, std::int16_t value);
int reply_int32(sd_bus_message *message, std::int32_t value);
int reply_uint32(sd_bus_message *message, std::uint32_t value);
int reply_double(sd_bus_message *message, double value);

// The live target a published node names, for the action layer. Nothing when
// the adaptor has no tree or the target left it since the snapshot.
[[nodiscard]] std::optional<Target> target_of(const ObjectSource &source,
                                              const Node &node);
// Whether the node's published interface list names this interface, including
// the radio-group Selection derivation.
[[nodiscard]] bool exposes(const ObjectSource &source, const Node &node,
                           std::string_view interface);

// AtspiCoordType, the coordinate spaces every geometry method names.
inline constexpr std::uint32_t kCoordScreen = 0;
inline constexpr std::uint32_t kCoordWindow = 1;
inline constexpr std::uint32_t kCoordParent = 2;

struct Point {
  double x{};
  double y{};
};

// A rect in the node's own space, expressed in the client's coordinate type.
// Screen space is the model's own space; published bounds are already clipped
// to what the viewport shows.
[[nodiscard]] Bounds to_coordinate(const ObjectSource &source, const Node &node,
                                   Bounds bounds, std::uint32_t coordinate);
// A client point in the requested space, converted into the model's screen
// space so a lookup converts once, not once per candidate.
[[nodiscard]] Point to_screen(const ObjectSource &source, const Node &node,
                              Point point, std::uint32_t coordinate);

// The shared fallback lookup: a registered interface answers only for paths
// whose node exists and exposes it, so a client never sees an interface the
// cache did not announce for that node.
int interface_find(const char *wanted, sd_bus *bus, const char *path,
                   const char *interface, void *data, void **found,
                   sd_bus_error *error);

// One interface's table and fallback lookup, each defined beside its handlers.
int accessible_find(sd_bus *bus, const char *path, const char *interface,
                    void *data, void **found, sd_bus_error *error);
[[nodiscard]] const sd_bus_vtable *accessible_vtable();
int application_find(sd_bus *bus, const char *path, const char *interface,
                     void *data, void **found, sd_bus_error *error);
[[nodiscard]] const sd_bus_vtable *application_vtable();
int cache_find(sd_bus *bus, const char *path, const char *interface, void *data,
               void **found, sd_bus_error *error);
[[nodiscard]] const sd_bus_vtable *cache_vtable();

int component_find(sd_bus *bus, const char *path, const char *interface,
                   void *data, void **found, sd_bus_error *error);
[[nodiscard]] const sd_bus_vtable *component_vtable();
int action_find(sd_bus *bus, const char *path, const char *interface,
                void *data, void **found, sd_bus_error *error);
[[nodiscard]] const sd_bus_vtable *action_vtable();
int text_find(sd_bus *bus, const char *path, const char *interface, void *data,
              void **found, sd_bus_error *error);
[[nodiscard]] const sd_bus_vtable *text_vtable();
int editable_find(sd_bus *bus, const char *path, const char *interface,
                  void *data, void **found, sd_bus_error *error);
[[nodiscard]] const sd_bus_vtable *editable_vtable();
int value_find(sd_bus *bus, const char *path, const char *interface, void *data,
               void **found, sd_bus_error *error);
[[nodiscard]] const sd_bus_vtable *value_vtable();
int selection_find(sd_bus *bus, const char *path, const char *interface,
                   void *data, void **found, sd_bus_error *error);
[[nodiscard]] const sd_bus_vtable *selection_vtable();

} // namespace layerc::editor::a11y::atspi::detail

#pragma once

#include "a11y/atspi/loop_bus.hpp"
#include "a11y/model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct sd_bus;
struct sd_bus_slot;

namespace layerc::editor::a11y::atspi {

inline constexpr std::string_view kAccessiblePrefix =
    "/org/a11y/atspi/accessible";
inline constexpr std::string_view kRootPath = "/org/a11y/atspi/accessible/root";
inline constexpr std::string_view kNullPath = "/org/a11y/atspi/null";
inline constexpr std::string_view kCachePath = "/org/a11y/atspi/cache";
// The org.a11y.atspi.Cache item struct, and the same struct's contents without
// its outer parentheses, for opening one inside an array.
inline constexpr std::string_view kCacheItem = "((so)(so)(so)iiassusau)";
inline constexpr std::string_view kCacheFields = "(so)(so)(so)iiassusau";

// What the object implementations read: the published snapshot (owned by the
// adaptor and alive for as long as the objects are registered), the root node,
// the three well-known identity strings, and the desktop reference the registry
// answered Embed with.
struct ObjectSource {
  const Snapshot *snapshot{};
  NodeId root{};
  std::string unique_name;
  std::string desktop_name{"org.a11y.atspi.Registry"};
  std::string desktop_path{std::string{kRootPath}};
  std::int32_t application_id{};
};

// The object path of a node; the root keeps the fixed path AT-SPI clients and
// the registry rely on.
[[nodiscard]] std::string node_path(NodeId id, NodeId root);
// The node a path names, or nothing for any other path.
[[nodiscard]] std::optional<NodeId> node_id_of(std::string_view path,
                                               NodeId root);
[[nodiscard]] const Node *find_node(const ObjectSource &source, NodeId id);

// One (so) reference; the name is the connection that owns the object.
int append_reference(sd_bus_message *message, std::string_view name, NodeId id,
                     NodeId root);
int append_null_reference(sd_bus_message *message);
int append_desktop_reference(sd_bus_message *message,
                             const ObjectSource &source);
void append_parent_reference(sd_bus_message *message,
                             const ObjectSource &source, const Node &node);

// The org.a11y.atspi.Cache item layout: (so) object, (so) application,
// (so) parent, i index, i child count, as interfaces, s name, u role,
// s description, au states.
void append_cache_item(sd_bus_message *message, const ObjectSource &source,
                       NodeId id, const Node &node);

// Registers the fallback objects under /org/a11y/atspi/accessible and the cache
// object at /org/a11y/atspi/cache. The slots belong to the caller.
void add_accessible_object(sd_bus *bus, ObjectSource *source,
                           sd_bus_slot **slot);
void add_application_object(sd_bus *bus, ObjectSource *source,
                            sd_bus_slot **slot);
void add_cache_object(sd_bus *bus, ObjectSource *source, sd_bus_slot **slot);

} // namespace layerc::editor::a11y::atspi

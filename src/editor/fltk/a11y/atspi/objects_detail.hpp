#pragma once

#include "a11y/atspi/objects.hpp"

#include <systemd/sd-bus.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace layerc::editor::a11y::atspi::detail {

inline constexpr const char *kAccessibleInterface = "org.a11y.atspi.Accessible";
inline constexpr const char *kApplicationInterface = "org.a11y.atspi.Application";
inline constexpr const char *kCacheInterface = "org.a11y.atspi.Cache";
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

} // namespace layerc::editor::a11y::atspi::detail

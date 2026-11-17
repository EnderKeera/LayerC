#include "a11y/atspi/objects.hpp"

#include "a11y/atspi/mapping.hpp"
#include "a11y/atspi/objects_detail.hpp"

#include <systemd/sd-bus.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>

namespace layerc::editor::a11y::atspi {
namespace {

using namespace detail;

// --- org.a11y.atspi.Application ------------------------------------------

int get_toolkit_name(sd_bus *, const char *, const char *, const char *,
                     sd_bus_message *reply, void *, sd_bus_error *) {
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, "FLTK");
}

int get_toolkit_version(sd_bus *, const char *, const char *, const char *,
                        sd_bus_message *reply, void *, sd_bus_error *) {
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, "1.4");
}

int get_atspi_version(sd_bus *, const char *, const char *, const char *,
                      sd_bus_message *reply, void *, sd_bus_error *) {
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, "2.1");
}

int get_id(sd_bus *, const char *, const char *, const char *,
           sd_bus_message *reply, void *data, sd_bus_error *) {
  const auto &source = *static_cast<ObjectSource *>(data);
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32,
                                     &source.application_id);
}

int set_id(sd_bus *, const char *, const char *, const char *,
           sd_bus_message *value, void *data, sd_bus_error *) {
  auto &source = *static_cast<ObjectSource *>(data);
  std::int32_t id = 0;
  if (sd_bus_message_read(value, "i", &id) < 0) return -EINVAL;
  source.application_id = id;
  return 0;
}

int application_locale(sd_bus_message *message, void *, sd_bus_error *) {
  std::uint32_t type = 0;
  if (sd_bus_message_read(message, "u", &type) < 0) type = 0;
  return reply_string(message, locale_name());
}

int application_register(sd_bus_message *message, void *, sd_bus_error *) {
  return sd_bus_reply_method_return(message, nullptr);
}

const sd_bus_vtable kApplication[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("ToolkitName", "s", get_toolkit_name, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Version", "s", get_toolkit_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AtspiVersion", "s", get_atspi_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_WRITABLE_PROPERTY("Id", "i", get_id, set_id, 0,
                             SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("GetLocale", "u", "s", application_locale, 0),
    SD_BUS_METHOD("RegisterEventListener", "s", "", application_register, 0),
    SD_BUS_METHOD("DeregisterEventListener", "s", "", application_register, 0),
    SD_BUS_VTABLE_END};

// --- org.a11y.atspi.Cache ------------------------------------------------

int get_items(sd_bus_message *message, void *data, sd_bus_error *) {
  const auto &source = *static_cast<ObjectSource *>(data);
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  int appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY,
                                               kCacheItemText);
  if (source.snapshot) {
    for (const auto &[id, node] : source.snapshot->nodes) {
      if (appended < 0) break;
      appended = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT,
                                               kCacheFieldsText);
      if (appended >= 0) append_cache_item(reply, source, id, node);
      if (appended >= 0) appended = sd_bus_message_close_container(reply);
    }
  }
  if (appended >= 0) appended = sd_bus_message_close_container(reply);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  return finish(message, reply);
}

const sd_bus_vtable kCache[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetItems", "", "a((so)(so)(so)iiassusau)", get_items, 0),
    SD_BUS_SIGNAL("AddAccessible", kCacheItemText, 0),
    SD_BUS_SIGNAL("RemoveAccessible", "(so)", 0),
    SD_BUS_VTABLE_END};

} // namespace

namespace detail {

int application_find(sd_bus *, const char *path, const char *interface,
                     void *data, void **found, sd_bus_error *error) {
  auto *source = static_cast<ObjectSource *>(data);
  if (!source) return 0;
  if (interface && std::string{interface} != kApplicationInterface) return 0;
  if (!path || std::string{path} != kRootPath) return 0;
  if (!find_node(*source, source->root)) {
    sd_bus_error_setf(error, SD_BUS_ERROR_UNKNOWN_OBJECT,
                      "Unknown object '%s'", path);
    return -ENOENT;
  }
  *found = source;
  return 1;
}

const sd_bus_vtable *application_vtable() { return kApplication; }

int cache_find(sd_bus *, const char *path, const char *interface, void *data,
               void **found, sd_bus_error *) {
  auto *source = static_cast<ObjectSource *>(data);
  if (!source) return 0;
  if (interface && std::string{interface} != kCacheInterface) return 0;
  if (!path || std::string{path} != kCachePath) return 0;
  *found = source;
  return 1;
}

const sd_bus_vtable *cache_vtable() { return kCache; }

} // namespace detail
} // namespace layerc::editor::a11y::atspi

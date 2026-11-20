#include "a11y/atspi/events.hpp"

#include "a11y/atspi/mapping.hpp"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace layerc::editor::a11y::atspi {
namespace {

constexpr const char *kObjectEvent = "org.a11y.atspi.Event.Object";
constexpr const char *kWindowEvent = "org.a11y.atspi.Event.Window";
constexpr const char *kFocusEvent = "org.a11y.atspi.Event.Focus";
constexpr const char *kCacheInterface = "org.a11y.atspi.Cache";

// The any_data variant of an event: absent (a zero uint32), text, an int32, a
// double, or an object reference.
struct EventData {
  enum class Kind { none, text, integer, real, reference } kind{Kind::none};
  std::string text;
  std::int32_t integer{};
  double real{};
  NodeId reference{no_node};
};

[[nodiscard]] const Node *node_in(const Snapshot &snapshot, const NodeId id) {
  const auto found = snapshot.nodes.find(id);
  return found == snapshot.nodes.end() ? nullptr : &found->second;
}

int append_data(sd_bus_message *message, const ObjectSource &source,
                const EventData &data) {
  switch (data.kind) {
  case EventData::Kind::text: {
    int appended =
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "s");
    if (appended >= 0)
      appended = sd_bus_message_append_basic(message, SD_BUS_TYPE_STRING,
                                             data.text.c_str());
    if (appended >= 0) appended = sd_bus_message_close_container(message);
    return appended;
  }
  case EventData::Kind::integer: {
    int appended =
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "i");
    if (appended >= 0)
      appended = sd_bus_message_append_basic(message, SD_BUS_TYPE_INT32,
                                             &data.integer);
    if (appended >= 0) appended = sd_bus_message_close_container(message);
    return appended;
  }
  case EventData::Kind::real: {
    int appended =
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "d");
    if (appended >= 0)
      appended = sd_bus_message_append_basic(message, SD_BUS_TYPE_DOUBLE,
                                             &data.real);
    if (appended >= 0) appended = sd_bus_message_close_container(message);
    return appended;
  }
  case EventData::Kind::reference: {
    int appended =
        sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "(so)");
    if (appended >= 0)
      appended = append_reference(message, source.unique_name, data.reference,
                                  source.root);
    if (appended >= 0) appended = sd_bus_message_close_container(message);
    return appended;
  }
  default:
    break;
  }
  std::uint32_t zero = 0;
  int appended = sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "u");
  if (appended >= 0)
    appended =
        sd_bus_message_append_basic(message, SD_BUS_TYPE_UINT32, &zero);
  if (appended >= 0) appended = sd_bus_message_close_container(message);
  return appended;
}

// The AT-SPI event signal: path, interface, CamelCase member, then
// (s name, i detail1, i detail2, v any_data, a{sv} properties).
void send(LoopBus &loop, const ObjectSource &source, const NodeId node,
          const char *interface, const char *member,
          const std::string_view name, const std::int32_t detail1,
          const std::int32_t detail2, const EventData &data) {
  sd_bus *bus = loop.bus();
  if (!bus) return;
  const std::string path = node_path(node, source.root);
  sd_bus_message *message = nullptr;
  if (sd_bus_message_new_signal(bus, &message, path.c_str(), interface, member) <
      0)
    return;
  const std::string text{name};
  std::int32_t first = detail1;
  std::int32_t second = detail2;
  int appended =
      sd_bus_message_append_basic(message, SD_BUS_TYPE_STRING, text.c_str());
  if (appended >= 0)
    appended =
        sd_bus_message_append_basic(message, SD_BUS_TYPE_INT32, &first);
  if (appended >= 0)
    appended =
        sd_bus_message_append_basic(message, SD_BUS_TYPE_INT32, &second);
  if (appended >= 0) appended = append_data(message, source, data);
  if (appended >= 0)
    appended =
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
  if (appended >= 0) appended = sd_bus_message_close_container(message);
  if (appended < 0) {
    sd_bus_message_unref(message);
    return;
  }
  // A failed send means the loop already recorded the connection failure.
  if (!send_message(loop, message)) return;
}

// The components of a canonical name, which has no empty ones.
[[nodiscard]] std::vector<std::string_view>
components(const std::string_view canonical) {
  std::vector<std::string_view> parts;
  std::string_view rest = canonical;
  while (!rest.empty()) {
    const auto colon = rest.find(':');
    parts.push_back(rest.substr(0, colon));
    if (colon == std::string_view::npos) break;
    rest.remove_prefix(colon + 1);
  }
  return parts;
}

[[nodiscard]] std::int32_t detail_offset(const std::string_view value,
                                         const std::size_t byte) {
  const auto offset = char_offset(value, byte);
  return static_cast<std::int32_t>(offset ? *offset : 0);
}

[[nodiscard]] std::int32_t detail_count(const std::string_view text) {
  const auto count = char_offset(text, text.size());
  return static_cast<std::int32_t>(count ? *count : 0);
}

} // namespace

std::string canonical_event(const std::string_view name) {
  // ensure_proper_format, character for character (ASCII, so no locale).
  std::string spelled;
  spelled.reserve(name.size());
  bool upper = true;
  for (const char c : name) {
    if (upper) {
      spelled.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A')
                                             : c);
      upper = false;
    } else if (c == '-') {
      upper = true;
    } else if (c == ':') {
      upper = true;
      spelled.push_back(c);
    } else {
      spelled.push_back(c);
    }
  }
  // The registry and the bridge stop comparing at the first empty component.
  std::string canonical;
  std::string_view rest = spelled;
  while (true) {
    const auto colon = rest.find(':');
    const auto part = rest.substr(0, colon);
    if (part.empty()) break;
    if (!canonical.empty()) canonical.push_back(':');
    canonical.append(part);
    if (colon == std::string_view::npos) break;
    rest.remove_prefix(colon + 1);
  }
  return canonical;
}

bool covers(const std::string_view pattern, const std::string_view event) {
  const std::string given_name = canonical_event(pattern);
  const std::string wanted_name = canonical_event(event);
  const auto given = components(given_name);
  const auto wanted = components(wanted_name);
  return given.size() <= wanted.size() &&
         std::equal(given.begin(), given.end(), wanted.begin());
}

bool listens(const std::vector<Listener> &listeners,
             const std::string_view event) {
  return std::any_of(listeners.begin(), listeners.end(),
                     [&](const Listener &listener) {
                       return !canonical_event(listener.event).empty() &&
                              covers(listener.event, event);
                     });
}

void emit_cache_added(LoopBus &loop, const ObjectSource &source,
                      const NodeId id) {
  const Node *node = find_node(source, id);
  if (!loop.bus() || !node) return;
  const std::string path{kCachePath};
  sd_bus_message *message = nullptr;
  if (sd_bus_message_new_signal(loop.bus(), &message, path.c_str(),
                                kCacheInterface, "AddAccessible") < 0)
    return;
  int appended = sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT,
                                               kCacheFields.data());
  if (appended >= 0) append_cache_item(message, source, id, *node);
  if (appended >= 0) appended = sd_bus_message_close_container(message);
  if (appended < 0) {
    sd_bus_message_unref(message);
    return;
  }
  // A failed send means the loop already recorded the connection failure.
  if (!send_message(loop, message)) return;
}

void emit_cache_removed(LoopBus &loop, const ObjectSource &source,
                        const NodeId id) {
  if (!loop.bus()) return;
  const std::string path{kCachePath};
  sd_bus_message *message = nullptr;
  if (sd_bus_message_new_signal(loop.bus(), &message, path.c_str(),
                                kCacheInterface, "RemoveAccessible") < 0)
    return;
  if (append_reference(message, source.unique_name, id, source.root) < 0) {
    sd_bus_message_unref(message);
    return;
  }
  // A failed send means the loop already recorded the connection failure.
  if (!send_message(loop, message)) return;
}

void emit_events(LoopBus &loop, const ObjectSource &source,
                 const Snapshot &before, const Snapshot &after,
                 const std::vector<Change> &changes,
                 const std::vector<Listener> *listeners) {
  const auto emit = [&](const NodeId node, const char *interface,
                        const char *member, const std::string_view name,
                        const std::int32_t detail1, const std::int32_t detail2,
                        const EventData &data, const std::string_view type,
                        const bool always) {
    if (listeners && !always && !listens(*listeners, type)) return;
    send(loop, source, node, interface, member, name, detail1, detail2, data);
  };
  for (const Change &change : changes) {
    switch (change.kind) {
    case ChangeKind::node_removed: {
      emit_cache_removed(loop, source, change.node);
      if (change.role == Role::window || change.role == Role::dialog) {
        const Node *node = node_in(before, change.node);
        EventData data;
        data.kind = EventData::Kind::text;
        data.text = node ? node->name : std::string{};
        emit(change.node, kWindowEvent, "Destroy", {}, 0, 0, data,
             "window:destroy", true);
      }
      break;
    }
    case ChangeKind::node_added: {
      emit_cache_added(loop, source, change.node);
      if (change.role == Role::window || change.role == Role::dialog) {
        const Node *node = node_in(after, change.node);
        EventData data;
        data.kind = EventData::Kind::text;
        data.text = node ? node->name : std::string{};
        emit(change.node, kWindowEvent, "Create", {}, 0, 0, data,
             "window:create", true);
      }
      break;
    }
    case ChangeKind::child_removed:
    case ChangeKind::child_added: {
      const bool added = change.kind == ChangeKind::child_added;
      EventData data;
      data.kind = EventData::Kind::reference;
      data.reference = change.node;
      const std::int32_t index =
          change.index == no_index ? -1 : static_cast<std::int32_t>(change.index);
      emit(change.parent, kObjectEvent, "ChildrenChanged",
           added ? "add" : "remove", index, 0, data,
           added ? "object:children-changed:add"
                 : "object:children-changed:remove",
           true);
      break;
    }
    case ChangeKind::name_changed:
    case ChangeKind::description_changed: {
      const bool named = change.kind == ChangeKind::name_changed;
      const Node *node = node_in(after, change.node);
      if (!node) break;
      EventData data;
      data.kind = EventData::Kind::text;
      data.text = named ? node->name : node->description;
      emit(change.node, kObjectEvent, "PropertyChange",
           named ? "accessible-name" : "accessible-description", 0, 0, data,
           named ? "object:property-change:accessible-name"
                 : "object:property-change:accessible-description",
           true);
      break;
    }
    case ChangeKind::value_changed:
      // Text fields report their edits via the Text interface.
      break;
    case ChangeKind::range_value_changed: {
      EventData data;
      data.kind = EventData::Kind::real;
      data.real = change.number;
      emit(change.node, kObjectEvent, "PropertyChange", "accessible-value", 0, 0,
           data, "object:property-change:accessible-value", false);
      break;
    }
    case ChangeKind::state_changed: {
      const Node *node = node_in(after, change.node);
      (void)node;
      const bool value = state_value_matches(change.state)
                             ? change.state_value
                             : !change.state_value;
      EventData data;
      data.kind = EventData::Kind::integer;
      for (const auto name : state_names(change.state)) {
        const std::string type = "object:state-changed:" + std::string{name};
        emit(change.node, kObjectEvent, "StateChanged", name, value ? 1 : 0, 0,
             data, type, true);
      }
      break;
    }
    case ChangeKind::text_deleted:
    case ChangeKind::text_inserted: {
      const bool inserted = change.kind == ChangeKind::text_inserted;
      const Node *text_node = node_in(inserted ? after : before, change.node);
      if (!text_node || !text_node->text) break;
      const std::string &value = text_node->text->value;
      EventData data;
      data.kind = EventData::Kind::text;
      data.text = inserted ? change.text : change.previous;
      emit(change.node, kObjectEvent, "TextChanged", inserted ? "insert" : "delete",
           detail_offset(value, change.offset), detail_count(data.text), data,
           inserted ? "object:text-changed:insert" : "object:text-changed:delete",
           false);
      break;
    }
    case ChangeKind::caret_moved: {
      const Node *node = node_in(after, change.node);
      if (!node || !node->text) break;
      EventData data;
      data.kind = EventData::Kind::integer;
      emit(change.node, kObjectEvent, "TextCaretMoved", {},
           detail_offset(node->text->value, change.offset), 0, data,
           "object:text-caret-moved", false);
      break;
    }
    case ChangeKind::text_selection_changed:
    case ChangeKind::container_selection_changed: {
      const bool text = change.kind == ChangeKind::text_selection_changed;
      EventData data;
      data.kind = EventData::Kind::integer;
      emit(change.node, kObjectEvent, text ? "TextSelectionChanged" : "SelectionChanged",
           {}, 0, 0, data, text ? "object:text-selection-changed" : "object:selection-changed",
           false);
      break;
    }
    case ChangeKind::focus_moved: {
      EventData data;
      data.kind = EventData::Kind::integer;
      emit(change.node, kFocusEvent, "Focus", {}, 0, 0, data, "focus:", false);
      break;
    }
    case ChangeKind::window_activated:
    case ChangeKind::window_deactivated: {
      const bool active = change.kind == ChangeKind::window_activated;
      const Node *node = node_in(after, change.node);
      EventData data;
      data.kind = EventData::Kind::text;
      data.text = node ? node->name : std::string{};
      emit(change.node, kWindowEvent, active ? "Activate" : "Deactivate", {}, 0,
           0, data, active ? "window:activate" : "window:deactivate", false);
      break;
    }
    case ChangeKind::announced: {
      EventData data;
      data.kind = EventData::Kind::text;
      data.text = change.text;
      emit(change.node, kObjectEvent, "Announcement", {}, 1, 0, data,
           "object:announcement", false);
      break;
    }
    }
  }
}

} // namespace layerc::editor::a11y::atspi

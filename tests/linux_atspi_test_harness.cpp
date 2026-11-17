#include "linux_atspi_test_harness.hpp"

#include "linux_atspi_test_fixture.hpp"

namespace layerc::test::atspi_fixture {

// --- harness object implementations --------------------------------------

static int get_is_enabled(sd_bus *, const char *, const char *, const char *,
                           sd_bus_message *reply, void *data, sd_bus_error *) {
  const auto *harness = static_cast<const Harness *>(data);
  harness->note_status_read();
  int value = harness->enabled() ? 1 : 0;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_BOOLEAN, &value);
}

static int get_screen_reader(sd_bus *, const char *, const char *,
                             const char *,
                             sd_bus_message *reply, void *data, sd_bus_error *) {
  int value = static_cast<const Harness *>(data)->screen_reader() ? 1 : 0;
  return sd_bus_message_append_basic(reply, SD_BUS_TYPE_BOOLEAN, &value);
}

static int bus_get_address(sd_bus_message *message, void *data, sd_bus_error *) {
  const auto *harness = static_cast<const Harness *>(data);
  harness->note_address_read();
  if (harness->address_error())
    return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_FAILED,
                                      "the accessibility bus is unavailable");
  return sd_bus_reply_method_return(message, "s",
                                    harness->reported_address().c_str());
}

static int socket_embed(sd_bus_message *message, void *data, sd_bus_error *) {
  auto *harness = static_cast<Harness *>(data);
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "so") < 0)
    return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_INVALID_ARGS,
                                      "Embed needs a reference");
  const char *name = nullptr;
  const char *path = nullptr;
  const int read = sd_bus_message_read(message, "so", &name, &path);
  sd_bus_message_exit_container(message);
  if (read < 0)
    return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_INVALID_ARGS,
                                      "Embed needs a reference");
  harness->note_embed(name ? name : "", path ? path : "");
  if (!harness->embed_error().empty())
    return sd_bus_reply_method_errorf(message, harness->embed_error().c_str(),
                                      "the registry refused the application");
  return sd_bus_reply_method_return(message, "(so)", kRegistryName,
                                    std::string{atspi::kRootPath}.c_str());
}

static int registry_get_events(sd_bus_message *message, void *data, sd_bus_error *) {
  auto *harness = static_cast<Harness *>(data);
  harness->note_registered_call();
  // A held answer lets a listener signal arrive first, the way a busy registry
  // delivers them.
  if (harness->hold_registered(message)) return 1;
  sd_bus_message *reply = nullptr;
  if (sd_bus_message_new_method_return(message, &reply) < 0) return -ENOMEM;
  const int appended = harness->append_registered_events(reply);
  if (appended < 0) {
    sd_bus_message_unref(reply);
    return -ENOMEM;
  }
  const int sent = sd_bus_send(sd_bus_message_get_bus(message), reply, nullptr);
  sd_bus_message_unref(reply);
  return sent < 0 ? sent : 1;
}

static int registry_deregister(sd_bus_message *message, void *data, sd_bus_error *) {
  auto *harness = static_cast<Harness *>(data);
  const char *name = nullptr;
  if (sd_bus_message_read(message, "s", &name) < 0)
    return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_INVALID_ARGS,
                                      "DeregisterApplication needs a name");
  harness->note_deregister(name ? name : "");
  return sd_bus_reply_method_return(message, nullptr);
}

const sd_bus_vtable Harness::kStatusVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("IsEnabled", "b", get_is_enabled, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ScreenReaderEnabled", "b", get_screen_reader, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END};

const sd_bus_vtable Harness::kBusVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetAddress", "", "s", bus_get_address, 0),
    SD_BUS_VTABLE_END};

const sd_bus_vtable Harness::kSocketVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Embed", "(so)", "(so)", socket_embed, 0),
    SD_BUS_VTABLE_END};

const sd_bus_vtable Harness::kRegistryVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetRegisteredEvents", "", "a(ss)", registry_get_events, 0),
    SD_BUS_METHOD("DeregisterApplication", "s", "", registry_deregister, 0),
    SD_BUS_SIGNAL("EventListenerRegistered", "ss", 0),
    SD_BUS_SIGNAL("EventListenerDeregistered", "ss", 0),
    SD_BUS_VTABLE_END};

// --- a private accessibility bus ------------------------------------------

// A bus of the test's own. dbus-daemon prints its address on a pipe, so a case
// can lose the accessibility bus without disturbing the session bus.

// The registry and socket root a case hosts on its own bus.

int host_socket_embed(sd_bus_message *message, void *data, sd_bus_error *) {
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "so") < 0)
    return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_INVALID_ARGS,
                                      "Embed needs a reference");
  const char *name = nullptr;
  const char *path = nullptr;
  const int read = sd_bus_message_read(message, "so", &name, &path);
  sd_bus_message_exit_container(message);
  if (read < 0)
    return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_INVALID_ARGS,
                                      "Embed needs a reference");
  ++static_cast<HostState *>(data)->embeds;
  return sd_bus_reply_method_return(message, "(so)", kRegistryName,
                                    std::string{atspi::kRootPath}.c_str());
}

const sd_bus_vtable kHostSocketVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Embed", "(so)", "(so)", host_socket_embed, 0),
    SD_BUS_VTABLE_END};

// Owns org.a11y.atspi.Registry and the socket root on a bus the test controls.

// --- reply parsers --------------------------------------------------------

std::optional<std::string> variant_text(sd_bus_message *message) {
  if (message == nullptr) return std::nullopt;
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "s") < 0)
    return std::nullopt;
  const char *text = nullptr;
  if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &text) < 0 ||
      text == nullptr)
    return std::nullopt;
  sd_bus_message_exit_container(message);
  return std::string{text};
}

std::optional<std::string> reply_text(sd_bus_message *message) {
  if (message == nullptr) return std::nullopt;
  const char *text = nullptr;
  if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &text) < 0 ||
      text == nullptr)
    return std::nullopt;
  return std::string{text};
}

std::optional<std::uint32_t> reply_u32(sd_bus_message *message) {
  if (message == nullptr) return std::nullopt;
  std::uint32_t value = 0;
  if (sd_bus_message_read_basic(message, SD_BUS_TYPE_UINT32, &value) < 0)
    return std::nullopt;
  return value;
}

std::optional<std::int32_t> reply_i32(sd_bus_message *message) {
  if (message == nullptr) return std::nullopt;
  std::int32_t value = 0;
  if (sd_bus_message_read_basic(message, SD_BUS_TYPE_INT32, &value) < 0)
    return std::nullopt;
  return value;
}

std::vector<std::uint32_t> reply_u32_array(sd_bus_message *message) {
  std::vector<std::uint32_t> values;
  if (message == nullptr ||
      sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "u") < 0)
    return values;
  std::uint32_t value = 0;
  while (sd_bus_message_read_basic(message, SD_BUS_TYPE_UINT32, &value) > 0)
    values.push_back(value);
  sd_bus_message_exit_container(message);
  return values;
}

std::vector<std::string> reply_strings(sd_bus_message *message) {
  std::vector<std::string> values;
  if (message == nullptr ||
      sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s") < 0)
    return values;
  const char *text = nullptr;
  while (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &text) > 0)
    values.emplace_back(text != nullptr ? text : "");
  sd_bus_message_exit_container(message);
  return values;
}

std::vector<Reference> reply_references(sd_bus_message *message) {
  std::vector<Reference> values;
  if (message == nullptr ||
      sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "(so)") < 0)
    return values;
  while (sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "so") > 0) {
    const char *name = nullptr;
    const char *path = nullptr;
    if (sd_bus_message_read(message, "so", &name, &path) < 0) break;
    values.push_back({name ? name : "", path ? path : ""});
    sd_bus_message_exit_container(message);
  }
  sd_bus_message_exit_container(message);
  return values;
}

std::optional<Reference> reply_reference(sd_bus_message *message) {
  if (message == nullptr ||
      sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "so") < 0)
    return std::nullopt;
  const char *name = nullptr;
  const char *path = nullptr;
  if (sd_bus_message_read(message, "so", &name, &path) < 0) return std::nullopt;
  sd_bus_message_exit_container(message);
  return Reference{name ? name : "", path ? path : ""};
}

std::optional<Reference> reply_variant_reference(sd_bus_message *message) {
  if (message == nullptr ||
      sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "(so)") < 0)
    return std::nullopt;
  const auto reference = reply_reference(message);
  sd_bus_message_exit_container(message);
  return reference;
}

std::size_t reply_relation_kinds(sd_bus_message *message) {
  std::size_t count = 0;
  if (message == nullptr) return count;
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "ua(so)") <
      0)
    return count;
  while (sd_bus_message_skip(message, "ua(so)") > 0) ++count;
  sd_bus_message_exit_container(message);
  return count;
}

std::size_t reply_cache_items(sd_bus_message *message) {
  std::size_t count = 0;
  if (message == nullptr)
    return count;
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY,
                                     std::string{atspi::kCacheItem}.c_str()) < 0)
    return count;
  char type = 0;
  while (sd_bus_message_peek_type(message, &type, nullptr) > 0 &&
         type == SD_BUS_TYPE_STRUCT &&
         sd_bus_message_skip(message, std::string{atspi::kCacheItem}.c_str()) >
             0) {
    ++count;
  }
  sd_bus_message_exit_container(message);
  return count;
}

[[nodiscard]] bool contains(const std::vector<std::string> &values,
                            const std::string_view wanted) {
  for (const auto &value : values)
    if (value == wanted) return true;
  return false;
}

[[nodiscard]] bool has_bit(const std::vector<std::uint32_t> &words,
                           const atspi::State state) {
  const auto number = static_cast<std::size_t>(state);
  return number / 32 < words.size() &&
         (words[number / 32] & (1U << (number % 32))) != 0U;
}

void require_session_bus() {
  const char *address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
  if (address == nullptr || *address == '\0') layerc::test::skip();
}

// The harness must own the accessibility names itself; a session bus that
// already hosts a registry (a real desktop session) answers in its place, so
// these cases need a disposable bus.
void require_private_bus(const Harness &harness) {
  if (harness.is_open()) return;
  std::fputs("the accessibility names are taken or no session bus is "
             "reachable; run under dbus-run-session\n",
             stderr);
  layerc::test::skip();
}

// Every node a client can reach from the root, by path.
std::size_t reachable_objects(Harness &harness) {
  std::vector<std::string> queue{std::string{atspi::kRootPath}};
  std::vector<std::string> seen;
  while (!queue.empty()) {
    const std::string path = queue.back();
    queue.pop_back();
    if (contains(seen, path)) continue;
    seen.push_back(path);
    const auto children = reply_references(
        harness.get(path, kAccessibleInterface, "GetChildren").reply);
    for (const auto &child : children) queue.push_back(child.path);
  }
  return seen.size();
}

std::optional<Signal> find_event(const Harness &harness, const std::size_t mark,
                                 const EventQuery &query) {
  const auto &signals = harness.signals();
  for (std::size_t i = mark; i < signals.size(); ++i) {
    const auto &signal = signals[i];
    if (signal.interface != query.interface) continue;
    if (!query.member.empty() && signal.member != query.member) continue;
    if (!query.name.empty() && signal.name != query.name) continue;
    return signal;
  }
  return std::nullopt;
}

bool wait_event(Harness &harness, const std::size_t mark, const EventQuery &query,
                const std::chrono::milliseconds limit) {
  return harness.spin_until(
      [&] { return find_event(harness, mark, query).has_value(); }, limit);
}

} // namespace layerc::test::atspi_fixture

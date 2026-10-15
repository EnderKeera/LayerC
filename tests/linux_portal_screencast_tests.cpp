#include "portal_screencast.hpp"
#include "linux_dbus_test_support.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace layerc::platform;
using layerc::test::expect;
namespace fixture = layerc::test::dbus;

int variant(sd_bus_message *message, const char *key, const char *type,
            const std::function<int(sd_bus_message *)> &append) {
  int result = sd_bus_message_open_container(message, 'e', "sv");
  if (result >= 0) result = sd_bus_message_append(message, "s", key);
  if (result >= 0) result = sd_bus_message_open_container(message, 'v', type);
  if (result >= 0) result = append(message);
  if (result >= 0) result = sd_bus_message_close_container(message);
  if (result >= 0) result = sd_bus_message_close_container(message);
  return result;
}

struct FakePortal {
  fixture::Peer peer{fixture::peer(portal_bus_name)};
  std::vector<BusSlot> objects;
  std::uint32_t version{4}, types{3}, cursors{7}, response_code{};
  unsigned probes{}, creates{}, selects{}, starts{}, opens{}, requests_closed{}, sessions_closed{};
  bool malformed{}, extra_stream{}, object_path_handle{};
  std::string hold_member, held_request, held_member, session;
  std::string restore, next_restore{"new-token"};
  std::uint32_t selected_types{}, persist{}, cursor{};
  int multiple{1};

  FakePortal() {
    static const sd_bus_vtable vtable[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_PROPERTY("version", "u", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("AvailableSourceTypes", "u", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("AvailableCursorModes", "u", get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_METHOD("CreateSession", "a{sv}", "o", request, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_METHOD("SelectSources", "oa{sv}", "o", request, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_METHOD("Start", "osa{sv}", "o", request, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_METHOD("OpenPipeWireRemote", "oa{sv}", "h", remote, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};
    objects.push_back(fixture::method(peer.get(), portal_path, portal_screencast_interface, vtable, this));
  }
  static int get_property(sd_bus *, const char *, const char *, const char *property,
                         sd_bus_message *reply, void *userdata, sd_bus_error *) {
    auto &self = *static_cast<FakePortal *>(userdata);
    ++self.probes;
    const std::string_view name{property};
    const auto value = name == "version" ? self.version
                     : name == "AvailableSourceTypes" ? self.types : self.cursors;
    return sd_bus_message_append(reply, "u", value);
  }
  void export_close(const std::string &path, const char *interface) {
    static const sd_bus_vtable vtable[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_METHOD("Close", "", "", close, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};
    objects.push_back(fixture::method(peer.get(), path.c_str(), interface, vtable, this));
  }
  static int close(sd_bus_message *call, void *userdata, sd_bus_error *) {
    auto &self = *static_cast<FakePortal *>(userdata);
    if (std::string_view{sd_bus_message_get_interface(call)} == "org.freedesktop.portal.Session")
      ++self.sessions_closed;
    else ++self.requests_closed;
    return sd_bus_reply_method_return(call, "");
  }
  static int request(sd_bus_message *call, void *userdata, sd_bus_error *) {
    auto &self = *static_cast<FakePortal *>(userdata);
    const std::string member{sd_bus_message_get_member(call)};
    if (member == "CreateSession") ++self.creates;
    else {
      const char *session{}, *parent{};
      int result = member == "Start" ? sd_bus_message_read(call, "os", &session, &parent)
                                    : sd_bus_message_read(call, "o", &session);
      if (result < 0 || !session || self.session != session) return -EINVAL;
      if (member == "Start") {
        ++self.starts;
        if (!parent || *parent != '\0') return -EINVAL;
      } else ++self.selects;
    }
    if (sd_bus_message_enter_container(call, 'a', "{sv}") <= 0) return -EINVAL;
    std::string handle, session_token;
    int entered{};
    while ((entered = sd_bus_message_enter_container(call, 'e', "sv")) > 0) {
      const char *key{};
      if (sd_bus_message_read(call, "s", &key) <= 0 || !key) return -EINVAL;
      const std::string_view name{key};
      if (name == "handle_token" || name == "session_handle_token" || name == "restore_token") {
        const char *value{};
        if (sd_bus_message_enter_container(call, 'v', "s") <= 0 ||
            sd_bus_message_read(call, "s", &value) <= 0 ||
            sd_bus_message_exit_container(call) < 0) return -EINVAL;
        if (name == "handle_token") handle = value;
        else if (name == "session_handle_token") session_token = value;
        else self.restore = value;
      } else if (name == "types" || name == "persist_mode" || name == "cursor_mode") {
        std::uint32_t value{};
        if (sd_bus_message_enter_container(call, 'v', "u") <= 0 ||
            sd_bus_message_read(call, "u", &value) <= 0 ||
            sd_bus_message_exit_container(call) < 0) return -EINVAL;
        if (name == "types") self.selected_types = value;
        else if (name == "persist_mode") self.persist = value;
        else self.cursor = value;
      } else if (name == "multiple") {
        if (sd_bus_message_enter_container(call, 'v', "b") <= 0 ||
            sd_bus_message_read(call, "b", &self.multiple) <= 0 ||
            sd_bus_message_exit_container(call) < 0) return -EINVAL;
      } else if (sd_bus_message_skip(call, "v") < 0) return -EINVAL;
      if (sd_bus_message_exit_container(call) < 0) return -EINVAL;
    }
    if (entered < 0 || handle.empty() || sd_bus_message_exit_container(call) < 0) return -EINVAL;
    const auto path = portal_request_path(sd_bus_message_get_sender(call), handle);
    self.export_close(path, "org.freedesktop.portal.Request");
    if (member == "CreateSession") {
      self.session = portal_request_path(sd_bus_message_get_sender(call), session_token);
      self.session.replace(self.session.find("/request/"), 9, "/session/");
      self.export_close(self.session, "org.freedesktop.portal.Session");
    }
    if (self.hold_member == member) {
      self.held_request = path;
      self.held_member = member;
    } else self.respond(path, member);
    // Response deliberately precedes the method return. The client must have
    // subscribed before sending the request, not after receiving its path.
    return sd_bus_reply_method_return(call, "o", path.c_str());
  }
  void respond(const std::string &path, const std::string &member, sd_bus *sender = nullptr) {
    sd_bus_message *message{};
    auto *bus = sender ? sender : peer.get();
    expect(sd_bus_message_new_signal(bus, &message, path.c_str(),
        "org.freedesktop.portal.Request", "Response") >= 0, "response message allocates");
    expect(sd_bus_message_append(message, "u", response_code) >= 0 &&
           sd_bus_message_open_container(message, 'a', "{sv}") >= 0, "response opens results");
    if (!response_code && member == "CreateSession") {
      const auto value = malformed ? "/wrong/session" : session;
      const char *signature = object_path_handle ? "o" : "s";
      expect(variant(message, "session_handle", signature, [&](sd_bus_message *out) {
        return sd_bus_message_append(out, signature, value.c_str());
      }) >= 0, "standard string or compatible object-path session handle appended");
    } else if (!response_code && member == "Start") {
      expect(variant(message, "restore_token", "s", [&](sd_bus_message *out) {
        return sd_bus_message_append(out, "s", next_restore.c_str());
      }) >= 0, "new token appended");
      expect(variant(message, "streams", "a(ua{sv})", [&](sd_bus_message *out) {
        int result = sd_bus_message_open_container(out, 'a', "(ua{sv})");
        const int count = extra_stream ? 2 : 1;
        for (int index = 0; index < count && result >= 0; ++index) {
          result = sd_bus_message_open_container(out, 'r', "ua{sv}");
          if (result >= 0) result = sd_bus_message_append(out, "u", 99U);
          if (result >= 0) result = sd_bus_message_open_container(out, 'a', "{sv}");
          if (result >= 0) result = variant(out, "source_type", "u", [&](sd_bus_message *dict) {
            return sd_bus_message_append(dict, "u", selected_types);
          });
          if (result >= 0) result = variant(out, "position", "(ii)", [](sd_bus_message *dict) {
            return sd_bus_message_append(dict, "(ii)", -1920, 100);
          });
          if (result >= 0) result = variant(out, "size", "(ii)", [](sd_bus_message *dict) {
            return sd_bus_message_append(dict, "(ii)", 1920, 1080);
          });
          if (result >= 0) result = variant(out, "future", "s", [](sd_bus_message *dict) {
            return sd_bus_message_append(dict, "s", "ignored");
          });
          if (result >= 0) result = sd_bus_message_close_container(out);
          if (result >= 0) result = sd_bus_message_close_container(out);
        }
        if (result >= 0) result = sd_bus_message_close_container(out);
        return result;
      }) >= 0, "one source and metadata appended");
    }
    expect(sd_bus_message_close_container(message) >= 0 && sd_bus_send(bus, message, nullptr) >= 0,
           "response emitted");
    sd_bus_message_unref(message);
  }
  void release() {
    respond(held_request, held_member);
    held_request.clear(); held_member.clear();
  }
  static int remote(sd_bus_message *call, void *userdata, sd_bus_error *) {
    auto &self = *static_cast<FakePortal *>(userdata);
    ++self.opens;
    const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    const int result = sd_bus_reply_method_return(call, "h", fd);
    ::close(fd);
    return result;
  }
};

void portal_sequence_persistence_and_fd_ownership() {
  layerc::test::TempDirectory root{"layerc-portal-sequence"};
  ScreencastTokenStore tokens{root.path() / "layerc/state/screencast.json"};
  expect(tokens.replace("p", "old-token"), "initial restore token stored");
  EventLoop loop; BusConnection bus;
  if (!expect(bus.open(loop), "portal client opens isolated bus")) return;
  FakePortal fake; fake.hold_member = "Start";
  PortalScreencast portal{bus, tokens};
  expect(fake.probes == 0 && fake.creates == 0, "no consent or IPC without demand");
  expect(portal.start("p"), "explicit demand starts consent");
  expect(fixture::pump(bus, {fake.peer.get()}, [&] { portal.sync(); return fake.starts == 1; }),
         "CreateSession and SelectSources complete even with early Response signals");
  expect(fake.selected_types == 2 && fake.multiple == 0 && fake.persist == 2 && fake.cursor == 1 &&
         fake.restore == "old-token", "window preference, single source, hidden cursor and persistence");
  expect(!tokens.token("p"), "old single-use token consumed durably before Start");
  fake.release();
  expect(fixture::pump(bus, {fake.peer.get()}, [&] { portal.sync(); return portal.ready(); }),
         "Start and OpenPipeWireRemote complete");
  auto remote = portal.take_remote();
  expect(remote && remote->fd >= 0 && (::fcntl(remote->fd, F_GETFD) & FD_CLOEXEC) &&
             remote->node == 99 && !remote->monitor && remote->geometry &&
             remote->geometry->x == -1920 && remote->geometry->width == 1920,
         "remote fd is a CLOEXEC-owned duplicate with stream metadata");
  expect(!portal.take_remote(), "descriptor ownership transfers exactly once");
  ScreencastTokenStore reloaded{tokens.path()};
  expect(reloaded.token("p") == std::optional<std::string>{"new-token"}, "new token survives restart");
  const int owned = remote ? remote->fd : -1;
  remote.reset();
  expect(owned >= 0 && ::fcntl(owned, F_GETFD) < 0 && errno == EBADF, "transferred fd closes with its owner");
  portal.stop();
  expect(fixture::settle(bus, {fake.peer.get()}) && fake.sessions_closed == 1,
         "session closes on demand drop");
  expect(!portal.ready() && !loop.poll_once(10).any(), "no capture demand blocks idle");
}

void portal_denial_is_sticky_and_monitor_fallback_is_explicit() {
  layerc::test::TempDirectory root{"layerc-portal-denial"};
  ScreencastTokenStore tokens{root.path() / "layerc/state/screencast.json"};
  EventLoop loop; BusConnection bus;
  if (!expect(bus.open(loop), "denial client connects")) return;
  FakePortal fake; fake.response_code = 1;
  PortalScreencast portal{bus, tokens};
  expect(portal.start("p"), "deniable demand accepted");
  expect(fixture::pump(bus, {fake.peer.get()}, [&] { portal.sync(); return portal.reason() == "portal_denied"; }),
         "user cancel is visible");
  for (int index = 0; index < 100; ++index) portal.sync();
  expect(fake.creates == 1 && !portal.start("p"), "denial never retries on refresh or repeated start");
  portal.stop();
  fake.response_code = 0; fake.types = 1; fake.cursors = 2;
  expect(portal.start("p"), "a new demand can request consent again");
  expect(fixture::pump(bus, {fake.peer.get()}, [&] { portal.sync(); return portal.ready(); }),
         "monitor-only portal works after demand change");
  auto remote = portal.take_remote();
  expect(remote && remote->monitor && fake.selected_types == 1 && fake.cursor == 0,
         "monitor source retained and hidden cursor requested only when supported");
  expect(sd_bus_emit_signal(fake.peer.get(), fake.session.c_str(), "org.freedesktop.portal.Session",
                           "Closed", "a{sv}", 0) >= 0, "session revocation emitted");
  expect(fixture::pump(bus, {fake.peer.get()}, [&] { return portal.reason() == "portal_session_closed"; }),
         "revoked consent invalidates ready state");
  expect(!portal.ready(), "revoked session cannot remain active");
}

void portal_rejects_spoofed_responses_and_cancels_pending_requests() {
  layerc::test::TempDirectory root{"layerc-portal-auth"};
  ScreencastTokenStore tokens{root.path() / "layerc/state/screencast.json"};
  EventLoop loop; BusConnection bus;
  if (!expect(bus.open(loop), "auth client connects")) return;
  FakePortal fake; fake.hold_member = "CreateSession";
  auto rogue = fixture::peer();
  PortalScreencast portal{bus, tokens};
  expect(portal.start("p"), "pending demand starts");
  expect(fixture::pump(bus, {fake.peer.get()}, [&] { return !fake.held_request.empty() && !bus.next_deadline(); }),
         "CreateSession response held");
  fake.respond(fake.held_request, fake.held_member, rogue.get());
  expect(fixture::pump(bus, {fake.peer.get(), rogue.get()}, [&] { return !bus.next_deadline(); }) &&
             fake.selects == 0 && !portal.ready(), "another unique sender cannot approve consent");
  portal.stop();
  expect(fixture::pump(bus, {fake.peer.get()}, [&] {
    return fake.requests_closed == 1 && fake.sessions_closed == 1;
  }), "pending request and predicted session both closed");
  fake.release();
  expect(fixture::pump(bus, {fake.peer.get()}, [&] { return !bus.next_deadline(); }) &&
             fake.selects == 0 && !portal.ready(), "late response cannot resurrect a cancelled session");
}

void portal_version_malformed_token_failure_and_owner_loss() {
  layerc::test::TempDirectory root{"layerc-portal-fault"};
  ScreencastTokenStore tokens{root.path() / "layerc/state/screencast.json"};
  EventLoop loop; BusConnection bus;
  if (!expect(bus.open(loop), "fault client connects")) return;
  auto fake = std::make_unique<FakePortal>(); fake->version = 3;
  PortalScreencast portal{bus, tokens};
  expect(portal.start("p"), "version probe begins");
  expect(fixture::pump(bus, {fake->peer.get()}, [&] { return portal.reason() == "portal_version"; }) &&
             fake->creates == 0, "portal v3 rejected before consent");
  for (int i = 0; i < 50; ++i) portal.sync();
  expect(fake->probes == 3, "version failure has no probe loop");
  portal.stop(); fake->version = 4; fake->malformed = true;
  fake->object_path_handle = true; // Keep legacy variants checked, including wrong paths.
  expect(portal.start("p"), "malformed fixture starts");
  expect(fixture::pump(bus, {fake->peer.get()}, [&] { return portal.reason() == "portal_invalid_response"; }),
         "unexpected session handle rejects the whole response");
  portal.stop(); fake->malformed = false; fake->extra_stream = true;
  expect(portal.start("p"), "multiple-source fixture starts");
  expect(fixture::pump(bus, {fake->peer.get()}, [&] { return portal.reason() == "portal_invalid_response"; }),
         "multiple streams rejected instead of guessing a target");
  portal.stop(); fake->extra_stream = false;
  ScreencastTokenStore nowhere{{}};
  PortalScreencast no_storage{bus, nowhere};
  expect(no_storage.start("p"), "storage-failure fixture starts");
  expect(fixture::pump(bus, {fake->peer.get()}, [&] { return no_storage.reason() == "portal_token_write_failed"; }) &&
             fake->opens == 0, "failed durable token publication prevents remote acquisition");
  no_storage.stop();
  expect(portal.start("p"), "owner-loss fixture starts");
  expect(fixture::pump(bus, {fake->peer.get()}, [&] { portal.sync(); return portal.ready(); }),
         "consent ready before owner loss");
  fake.reset();
  expect(fixture::pump(bus, {}, [&] { portal.sync(); return portal.reason() == "portal_owner_changed"; }) &&
             !portal.ready(), "vanished portal owner invalidates consent and closes held fd");
  fake = std::make_unique<FakePortal>();
  expect(fixture::pump(bus, {fake->peer.get()}, [&] { portal.sync(); return !bus.next_deadline(); }) &&
             fake->creates == 0, "new owner cannot silently reopen consent");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(portal_sequence_persistence_and_fd_ownership),
                 LAYERC_TEST_CASE(portal_denial_is_sticky_and_monitor_fallback_is_explicit),
                 LAYERC_TEST_CASE(portal_rejects_spoofed_responses_and_cancels_pending_requests),
                 LAYERC_TEST_CASE(portal_version_malformed_token_failure_and_owner_loss))

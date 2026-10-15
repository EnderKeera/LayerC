#include "gnome_bridge.hpp"
#include "dbus_shell_service.hpp"
#include "linux_dbus_test_support.hpp"

#include <cerrno>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace layerc::platform;
using layerc::test::expect;
namespace fixture = layerc::test::dbus;

struct FakeGnome {
  fixture::Peer peer{fixture::peer(gnome_shell_bus_name)};
  BusSlot object;
  std::uint32_t version{1};
  unsigned presents{}, clears{}, probes{};
  bool hold{}, fail{};
  sd_bus_message *held{};
  std::string frame;
  FakeGnome() {
    expect(sd_bus_request_name(peer.get(), gnome_bridge_bus_name, 0) >= 0, "extension name claimed");
    static const sd_bus_vtable vtable[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_PROPERTY("Version", "u", get_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_METHOD("Present", "s", "", present, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_METHOD("Clear", "", "", clear, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};
    object = fixture::method(peer.get(), gnome_bridge_bus_path, gnome_bridge_interface, vtable, this);
  }
  ~FakeGnome() { sd_bus_message_unref(held); }
  static int get_version(sd_bus *, const char *, const char *, const char *,
                         sd_bus_message *reply, void *userdata, sd_bus_error *) {
    auto &self = *static_cast<FakeGnome *>(userdata);
    ++self.probes;
    return sd_bus_message_append(reply, "u", self.version);
  }
  static int present(sd_bus_message *call, void *userdata, sd_bus_error *error) {
    auto &self = *static_cast<FakeGnome *>(userdata);
    const char *frame{};
    if (sd_bus_message_read(call, "s", &frame) < 0) return -EINVAL;
    ++self.presents;
    self.frame = frame;
    if (self.fail) {
      sd_bus_error_set_const(error, SD_BUS_ERROR_FAILED, "fixture failure");
      return -EIO;
    }
    if (self.hold) {
      self.held = sd_bus_message_ref(call);
      return 1;
    }
    return sd_bus_reply_method_return(call, "");
  }
  static int clear(sd_bus_message *call, void *userdata, sd_bus_error *) {
    auto &self = *static_cast<FakeGnome *>(userdata);
    ++self.clears;
    self.frame.clear();
    return sd_bus_reply_method_return(call, "");
  }
  void release() {
    (void)sd_bus_reply_method_return(held, "");
    held = sd_bus_message_unref(held);
  }
};

std::vector<GnomePanel> panels(const std::string &id) {
  OverlayDisplayList list;
  list.bounds = {10, 20, 30, 40};
  list.ops.push_back({OverlayOpKind::fill_rect, list.bounds, 0x112233, {}, 0});
  return {{id, list}};
}

void gnome_authentication_and_bounded_queue() {
  EventLoop loop; BusConnection bus;
  if (!expect(bus.open(loop), "GNOME client connects")) return;
  FakeGnome shell;
  GnomeBridge bridge{bus};
  expect(shell.probes == 0, "no demand does no bridge IPC");
  bridge.set_wanted(true);
  const auto ready = [&] { bridge.sync(); return bridge.available(); };
  expect(fixture::pump(bus, {shell.peer.get()}, ready) && shell.probes == 1, "matching owners + Version 1 verified");
  const auto first = panels("first");
  expect(bridge.present(first), "verified bridge accepts a panel");
  expect(fixture::pump(bus, {shell.peer.get()}, [&] { return shell.presents == 1; }), "Present sent");
  expect(shell.frame == *build_gnome_overlay_frame(first), "shared byte-stable display list transported");
  shell.hold = true;
  expect(bridge.present(panels("held")), "new frame accepted");
  expect(fixture::pump(bus, {shell.peer.get()}, [&] { return shell.held != nullptr; }), "one call held");
  for (int i = 0; i < 100; ++i) expect(bridge.present(panels("latest")), "replacement coalesced");
  expect(shell.presents == 2, "no unbounded pending frame calls");
  shell.hold = false; shell.release();
  expect(fixture::pump(bus, {shell.peer.get()}, [&] { return shell.presents == 3; }) &&
             shell.frame == *build_gnome_overlay_frame(panels("latest")), "only newest frame follows reply");
  std::vector<GnomePanel> too_many(65, first.front());
  expect(!bridge.present(too_many) && bridge.reason() == "gnome_frame_too_large", "oversized frames refused visibly");
  expect(bridge.present(panels("latest")), "valid frame recovers a local bounds failure");
  expect(fixture::pump(bus, {shell.peer.get()}, [&] { return !bus.next_deadline(); }), "replies drained");
  expect(shell.presents == 3, "duplicate frames cause no IPC");
  shell.hold = true;
  expect(bridge.present(panels("cancel")), "a cancellable frame accepted");
  expect(fixture::pump(bus, {shell.peer.get()}, [&] { return shell.held != nullptr; }), "pending frame observed");
  bridge.set_wanted(false);
  expect(fixture::pump(bus, {shell.peer.get()}, [&] { return shell.clears == 1; }), "demand drop queues Clear after Present");
  shell.release();
  expect(fixture::settle(bus, {shell.peer.get()}) && shell.frame.empty(),
         "late reply cannot resurrect removed panels");
  expect(!bridge.available() && !loop.poll_once(10).any(), "bridge remains idle without demand");
}

void gnome_untrusted_missing_version_and_method_faults() {
  EventLoop loop; BusConnection bus;
  if (!expect(bus.open(loop), "fault client connects")) return;
  auto shell = fixture::peer(gnome_shell_bus_name);
  auto rogue = fixture::peer(gnome_bridge_bus_name);
  GnomeBridge bridge{bus}; bridge.set_wanted(true);
  expect(fixture::pump(bus, {shell.get(), rogue.get()}, [&] {
    bridge.sync(); return bridge.reason() == "gnome_extension_untrusted";
  }), "same-UID extension-name squatting is refused");
  expect(!bridge.present(panels("no")) && !bridge.available(), "rogue owner never gets Present");
  rogue.reset(); shell.reset();
  expect(fixture::pump(bus, {}, [&] {
    bridge.sync(); return bridge.reason() == "gnome_extension_missing";
  }), "owner disappearance remains visible");
  auto authentic = std::make_unique<FakeGnome>(); authentic->version = 2;
  expect(fixture::pump(bus, {authentic->peer.get()}, [&] {
    bridge.sync(); return bridge.reason() == "gnome_extension_version";
  }), "unsupported extension version refused");
  for (int i = 0; i < 20; ++i) bridge.sync();
  expect(authentic->probes == 1, "version faults do not retry every refresh");
  authentic.reset();
  expect(fixture::pump(bus, {}, [&] { bridge.sync(); return !bus.owner(gnome_shell_bus_name); }), "old owner gone");
  authentic = std::make_unique<FakeGnome>();
  expect(fixture::pump(bus, {authentic->peer.get()}, [&] { bridge.sync(); return bridge.available(); }),
         "replacement owner is independently verified");
  expect(fixture::pump(bus, {authentic->peer.get()}, [&] { return !bus.next_deadline(); }) &&
             authentic->presents == 1, "retained frame is acknowledged before injecting a method fault");
  authentic->fail = true;
  expect(bridge.present(panels("fail")), "method fault is asynchronous");
  expect(fixture::pump(bus, {authentic->peer.get()}, [&] { return bridge.reason() == "gnome_extension_failed"; }),
         "method failure disables bridge visibly");
  for (int i = 0; i < 20; ++i) bridge.sync();
  expect(authentic->presents == 2, "faulted bridge does not retry (retained desired frame then new one)");
  const std::vector<GnomePanel> oversized(65, panels("oversized").front());
  expect(!bridge.present(oversized) && bridge.reason() == "gnome_extension_failed",
         "a local frame fault must not hide the transport failure");
  bridge.set_wanted(false);
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(gnome_authentication_and_bounded_queue),
                 LAYERC_TEST_CASE(gnome_untrusted_missing_version_and_method_faults))

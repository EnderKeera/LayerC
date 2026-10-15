#include "dbus_shell_service.hpp"
#include "kwin_script.hpp"
#include "linux_dbus_test_support.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <string>

namespace {
using namespace layerc::platform;
using layerc::test::expect;
namespace fixture = layerc::test::dbus;
constexpr const char *report = R"({"version":1,"source":"kwin","windows":[{"id":"w","title":"Game"}]})";

void report_authority_and_lifecycle() {
  EventLoop loop;
  BusConnection bus;
  if (!expect(bus.open(loop), "daemon bus connects")) return;
  ShellService shell{bus};
  auto kwin = fixture::peer(kwin_bus_name);
  auto stranger = fixture::peer();
  shell.start();
  if (!expect(fixture::pump(bus, {kwin.get(), stranger.get()}, [&] {
        return shell.name_state() == ShellService::NameState::owned && bus.owner_known(kwin_bus_name);
      }), "Shell1 takes the name and learns KWin's owner")) return;
  const auto send = [&](sd_bus *peer, const char *payload) {
    fixture::Result result;
    auto slot = fixture::call(peer, layerc_bus_name, layerc_bus_path, layerc_shell_interface,
        "ReportWindows", [payload](sd_bus_message *call) {
          return sd_bus_message_append(call, "s", payload);
        }, result);
    expect(fixture::pump(bus, {kwin.get(), stranger.get()}, [&] { return result.done; }),
           "ReportWindows replies");
    return result.error;
  };
  expect(send(stranger.get(), report) == SD_BUS_ERROR_ACCESS_DENIED,
         "unrelated same-UID callers cannot spoof a report");
  expect(!shell.windows(WindowReportSource::kwin), "a denied report leaves no scan");
  expect(send(kwin.get(), report).empty(), "the current KWin owner is authorized");
  const auto scan = shell.windows(WindowReportSource::kwin);
  expect(scan && scan->windows.size() == 1 && scan->windows[0].title == "Game", "accepted scan published");
  expect(send(kwin.get(), "{") == SD_BUS_ERROR_INVALID_ARGS &&
             shell.report_error(WindowReportSource::kwin) == "window_report_invalid",
         "malformed reports surface a stable provider reason");
  expect(send(kwin.get(), report).empty() && shell.report_error(WindowReportSource::kwin).empty(),
         "a fresh accepted report recovers");
  const auto counts = shell.counters();
  expect(counts.accepted == 2 && counts.denied == 1 && counts.rejected == 1, "aggregate counters only");
  kwin.reset();
  expect(fixture::pump(bus, {stranger.get()}, [&] { return !bus.owner(kwin_bus_name); }),
         "name loss observed");
  expect(!shell.windows(WindowReportSource::kwin), "reports expire with the compositor owner");
  auto replacement = fixture::peer(kwin_bus_name);
  expect(fixture::pump(bus, {replacement.get()}, [&] { return bus.owner(kwin_bus_name).has_value(); }),
         "replacement owner observed");
  expect(!shell.windows(WindowReportSource::kwin), "replacement cannot reuse the old report");
  bool done = false;
  std::uint32_t version{};
  const auto guard = make_bus_guard();
  (void)bus.get_property(layerc_bus_name, layerc_bus_path, layerc_shell_interface, "Version",
      [&](sd_bus_message *reply, const sd_bus_error *error) {
        if (!error && sd_bus_message_enter_container(reply, 'v', "u") > 0)
          (void)sd_bus_message_read(reply, "u", &version);
        done = true;
      }, guard);
  expect(fixture::pump(bus, {}, [&] { return done; }) && version == 1, "Shell1 advertises Version 1");
}

void pending_name_stop_and_conflict() {
  EventLoop loop;
  BusConnection bus;
  if (!expect(bus.open(loop), "daemon bus connects")) return;
  {
    ShellService shell{bus};
    shell.start();
    shell.stop();
    bool known = false;
    std::string owner;
    auto guard = make_bus_guard();
    (void)bus.call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "GetNameOwner", [](sd_bus_message *call) { return sd_bus_message_append(call, "s", layerc_bus_name); },
      [&](sd_bus_message *reply, const sd_bus_error *error) {
        const char *value{};
        if (!error && sd_bus_message_read(reply, "s", &value) > 0) owner = value;
        known = true;
      }, guard);
    expect(fixture::pump(bus, {}, [&] { return known; }) && owner.empty(),
           "stop before acquisition releases the queued claim");
  }
  auto blocker = fixture::peer(layerc_bus_name);
  ShellService second{bus};
  second.start();
  expect(fixture::pump(bus, {blocker.get()}, [&] {
    return second.name_state() == ShellService::NameState::taken;
  }) && second.reason() == "bus_name_taken", "second daemon does not queue or steal the name");
  second.stop();
  expect(sd_bus_release_name(blocker.get(), layerc_bus_name) >= 0, "other owner's name remains intact");
}

struct FakeKwin {
  fixture::Peer peer{fixture::peer(kwin_bus_name)};
  BusSlot scripting, script;
  unsigned loads{}, unloads{}, runs{};
  bool fail_run{};
  std::string path, plugin;
  explicit FakeKwin(bool legacy = false) {
    static const sd_bus_vtable loader[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_METHOD("loadScript", "ss", "i", load, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_METHOD("unloadScript", "s", "b", unload, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};
    static const sd_bus_vtable runner[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_METHOD("run", "", "", run, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};
    scripting = fixture::method(peer.get(), "/Scripting", "org.kde.kwin.Scripting", loader, this);
    script = fixture::method(peer.get(), legacy ? "/37" : "/Scripting/Script37",
                             "org.kde.kwin.Script", runner, this);
  }
  static int load(sd_bus_message *message, void *userdata, sd_bus_error *) {
    auto &self = *static_cast<FakeKwin *>(userdata);
    const char *path{}, *plugin{};
    if (sd_bus_message_read(message, "ss", &path, &plugin) < 0) return -EINVAL;
    self.path = path; self.plugin = plugin; ++self.loads;
    return sd_bus_reply_method_return(message, "i", std::int32_t{37});
  }
  static int unload(sd_bus_message *message, void *userdata, sd_bus_error *) {
    const char *plugin{};
    if (sd_bus_message_read(message, "s", &plugin) < 0) return -EINVAL;
    ++static_cast<FakeKwin *>(userdata)->unloads;
    return sd_bus_reply_method_return(message, "b", 1);
  }
  static int run(sd_bus_message *message, void *userdata, sd_bus_error *error) {
    auto &self = *static_cast<FakeKwin *>(userdata); ++self.runs;
    if (self.fail_run) {
      sd_bus_error_set_const(error, SD_BUS_ERROR_FAILED, "fixture run failure");
      return -EIO;
    }
    return sd_bus_reply_method_return(message, "");
  }
};

void kwin_paths_demand_and_owner_restart() {
  layerc::test::TempDirectory root{"layerc-kwin"};
  (void)::chmod(root.path().c_str(), 0700);
  layerc::test::ScopedEnvironment runtime{"XDG_RUNTIME_DIR", root.path()};
  for (bool legacy : {false, true}) {
    EventLoop loop; BusConnection bus;
    if (!expect(bus.open(loop), "KWin client connects")) return;
    auto kwin = std::make_unique<FakeKwin>(legacy);
    KwinScript script{bus};
    expect(!std::filesystem::exists(kwin_script_path()), "no tracking demand writes nothing");
    script.set_wanted(true);
    const auto running = [&] { script.sync(); return script.state() == KwinScript::State::running; };
    expect(fixture::pump(bus, {kwin->peer.get()}, running),
           legacy ? "KWin 5.27 legacy path runs" : "KWin 6 script object runs");
    struct stat file{}, directory{};
    expect(::stat(kwin_script_path().c_str(), &file) == 0 && (file.st_mode & 0777) == 0600 &&
               ::stat(kwin_script_path().parent_path().c_str(), &directory) == 0 &&
               (directory.st_mode & 0777) == 0700, "script storage private");
    expect(kwin->plugin == kwin_script_plugin && kwin->loads == 1 && kwin->runs == 1 &&
               kwin->unloads == 1, "unload stale plugin then load and run");
    expect(layerc::test::read_file(kwin_script_path()) ==
               std::optional<std::string>{std::string{kwin_window_script()}}, "embedded script written exactly");
    kwin.reset();
    expect(fixture::pump(bus, {}, [&] {
      script.sync(); return script.state() == KwinScript::State::waiting_for_kwin;
    }) && script.reason() == "kwin_unavailable", "owner loss visible");
    kwin = std::make_unique<FakeKwin>(legacy);
    expect(fixture::pump(bus, {kwin->peer.get()}, running) && kwin->loads == 1, "new owner reloads");
    script.set_wanted(false);
    expect(fixture::pump(bus, {kwin->peer.get()}, [&] { return kwin->unloads == 2; }), "demand drop unloads");
    expect(script.state() == KwinScript::State::idle, "quiesced script stays idle");
    expect(fixture::settle(bus, {kwin->peer.get()}), "bus settles");
    expect(!loop.poll_once(15).any(), "no periodic or writable wake while idle");
    std::filesystem::remove(kwin_script_path());
  }
}

void kwin_failure_pending_cancel_and_safe_file() {
  layerc::test::TempDirectory root{"layerc-kwin-fault"};
  (void)::chmod(root.path().c_str(), 0700);
  layerc::test::ScopedEnvironment runtime{"XDG_RUNTIME_DIR", root.path()};
  EventLoop loop; BusConnection bus;
  if (!expect(bus.open(loop), "fault client connects")) return;
  FakeKwin kwin; kwin.fail_run = true;
  KwinScript script{bus};
  script.set_wanted(true);
  expect(fixture::pump(bus, {kwin.peer.get()}, [&] {
    script.sync(); return script.state() == KwinScript::State::failed;
  }) && script.reason() == "kwin_script_failed", "failure visible");
  for (int i = 0; i < 10; ++i) script.sync();
  expect(kwin.loads == 1, "failure has no automatic retry loop");
  script.set_wanted(false);
  expect(fixture::pump(bus, {kwin.peer.get()}, [&] { return kwin.unloads == 2; }), "failed run unloads");
  kwin.fail_run = false;
  script.set_wanted(true); script.set_wanted(false);
  expect(fixture::pump(bus, {kwin.peer.get()}, [&] { return kwin.unloads == 4; }), "pending load cancelled");
  expect(script.state() == KwinScript::State::idle, "late reply cannot resurrect demand");
  std::filesystem::remove(kwin_script_path());
  const auto victim = root.path() / "keep.txt";
  layerc::test::write_file(victim, "unchanged");
  for (bool hard : {false, true}) {
    if (hard) std::filesystem::create_hard_link(victim, kwin_script_path());
    else std::filesystem::create_symlink(victim, kwin_script_path());
    KwinScript unsafe{bus};
    unsafe.set_wanted(true);
    expect(fixture::pump(bus, {kwin.peer.get()}, [&] {
      unsafe.sync(); return unsafe.state() == KwinScript::State::failed;
    }) && unsafe.reason() == "kwin_script_write_failed", "unsafe script path refused");
    expect(layerc::test::read_file(victim) == std::optional<std::string>{"unchanged"},
           "unsafe file never truncated");
    std::filesystem::remove(kwin_script_path());
  }
}

void released_guard_cancels_pending_calls() {
  EventLoop loop;
  BusConnection bus;
  if (!expect(bus.open(loop), "daemon bus connects")) return;
  expect(fixture::pump(bus, {}, [&] { return bus.pending_calls() == 0 && !bus.next_deadline(); }),
         "connection setup leaves no pending call");
  // A peer that never answers keeps every call pending until its timeout.
  auto silent = fixture::peer();
  const auto destination = fixture::unique(silent.get());
  const auto captured = std::make_shared<int>(0);
  const auto queue = [&](const BusGuard &guard, int count) {
    bool queued = true;
    for (int i = 0; i < count; ++i)
      queued = bus.call(destination.c_str(), "/", "org.freedesktop.DBus.Peer", "Ping", {},
                        [captured](sd_bus_message *, const sd_bus_error *) {}, guard) && queued;
    return queued;
  };
  auto guard = make_bus_guard();
  expect(queue(guard, 128) && bus.pending_calls() == 128 && captured.use_count() == 129 &&
             bus.next_deadline(),
         "unanswered calls hold their captures and a reply timeout");
  bus.release(guard);
  expect(!guard && bus.pending_calls() == 0 && captured.use_count() == 1 && !bus.next_deadline(),
         "releasing the guard cancels the calls, their captures and their timeout");

  // Released from inside another reply callback: the slots are freed after
  // dispatch, never inside their own callback.
  auto inner = make_bus_guard();
  auto outer = make_bus_guard();
  expect(queue(inner, 4), "inner calls queue");
  bool released = false;
  (void)bus.call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                 "GetId", {}, [&](sd_bus_message *, const sd_bus_error *) {
                   bus.release(inner);
                   released = true;
                 }, outer);
  expect(fixture::pump(bus, {}, [&] { return released; }) && bus.pending_calls() == 0 &&
             captured.use_count() == 1,
         "a release during dispatch cancels pending calls once dispatch ends");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(report_authority_and_lifecycle),
                 LAYERC_TEST_CASE(pending_name_stop_and_conflict),
                 LAYERC_TEST_CASE(kwin_paths_demand_and_owner_restart),
                 LAYERC_TEST_CASE(kwin_failure_pending_cancel_and_safe_file),
                 LAYERC_TEST_CASE(released_guard_cancels_pending_calls))

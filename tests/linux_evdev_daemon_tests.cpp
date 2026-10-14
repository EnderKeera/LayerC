// The daemon's opted-in evdev route: demand-driven open/close on the shared
// EventLoop, synthetic FIFO streams feeding the session registry, hotplug
// re-registration, the permission fallback reason and keymap replacement.
// No display is needed. The FIFO writers are test-only; product sources never
// write a device (tools/tests/test_evdev_boundary.cmake).
#include "display_selection.hpp"
#include "evdev_route.hpp"
#include "input_service.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store_linux.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include "test_support.hpp"

#include <linux/input.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using layerc::test::expect;
using namespace layerc::platform;
using layerc::daemon::InputOptions;
using layerc::daemon::InputSelection;

EvdevDeviceBits keyboard_bits() {
  EvdevDeviceBits bits;
  bits.name = "Synthetic keyboard";
  for (const unsigned key : {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H,
                             KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P,
                             KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X,
                             KEY_Y, KEY_Z, KEY_SPACE, KEY_ENTER})
    bits.keys.set(key);
  return bits;
}
EvdevDeviceBits mouse_bits() {
  EvdevDeviceBits bits;
  bits.name = "Synthetic mouse";
  bits.keys.set(BTN_LEFT).set(BTN_RIGHT);
  bits.relative_axes.set(REL_X).set(REL_Y).set(REL_WHEEL);
  return bits;
}

struct FakeDevice {
  EvdevDeviceBits bits;
  int probe_error{};
};
class FakeBackend final : public EvdevDeviceBackend {
public:
  std::map<std::string, FakeDevice> entries;
  bool passive_flags{true};
  int probe(const int fd, const std::filesystem::path &path, EvdevDeviceBits &bits) override {
    const int flags = ::fcntl(fd, F_GETFL);
    passive_flags = passive_flags && (flags & O_ACCMODE) == O_RDONLY &&
                    (flags & O_NONBLOCK) != 0 && (::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
    const auto found = entries.find(path.filename().string());
    if (found == entries.end()) return ENOTTY;
    bits = found->second.bits;
    return found->second.probe_error;
  }
  int key_state(int, std::bitset<KEY_CNT> &down) override {
    down.reset();
    return 0;
  }
  int absolute_value(int, std::uint16_t, int &value) override {
    value = 0;
    return 0;
  }
};

bool ok(const layerc::runtime::ServiceReconcileResult &result) {
  return result.status == layerc::runtime::ServiceReconcileStatus::ok;
}

std::optional<layerc::core::GameProfile> fixture_profile() {
  // KEY_Q is keysym q under the default (or built-in US) map: VK 'Q' = 81.
  const std::string json =
      R"json({"schemaVersion":1,"id":"evdev-fixture","name":"Fixture","game":{"global":true},)json"
      R"json("cooldowns":[{"id":"dash","name":"Dash","durationMs":60000}],)json"
      R"json("panels":[{"id":"main","name":"Main","cooldowns":["dash"]}],)json"
      R"json("bindings":[{"cooldownId":"dash","chord":[{"device":"keyboard","code":81}]}]})json";
  auto parsed = layerc::core::parse_profile_json(json);
  if (auto *profile = std::get_if<layerc::core::GameProfile>(&parsed)) return std::move(*profile);
  std::fprintf(stderr, "profile parse failed: %s\n",
               std::get<layerc::core::ProfileError>(parsed).message.c_str());
  return std::nullopt;
}

X11WindowScan active_scan() {
  X11WindowScan scan;
  X11WindowSnapshot window;
  window.window = 0x1234;
  window.active = true;
  scan.windows.push_back(window);
  scan.active = window.window;
  return scan;
}

struct Fixture {
  layerc::test::TempDirectory temp{"layerc-evdev-daemon"};
  layerc::test::ScopedEnvironment config{"XDG_CONFIG_HOME", temp.path() / "config"};
  layerc::test::ScopedEnvironment data{"XDG_DATA_HOME", temp.path() / "data"};
  std::filesystem::path devices{temp.path() / "input"};
  std::shared_ptr<FakeBackend> backend{std::make_shared<FakeBackend>()};
  layerc::platform::EventLoop loop;
  std::map<std::string, int> writers;

  Fixture() { std::filesystem::create_directories(devices); }
  ~Fixture() {
    for (const auto &[name, fd] : writers) { (void)name; ::close(fd); }
  }
  [[nodiscard]] InputOptions options(InputSelection selection = InputSelection::evdev) const {
    return {.selection = selection, .device_dir = devices, .backend = backend};
  }
  void add(const std::string &name, EvdevDeviceBits bits, const int probe_error = 0) {
    backend->entries[name] = {std::move(bits), probe_error};
    expect(::mkfifo((devices / name).c_str(), 0600) == 0, "create event FIFO");
  }
  void connect(const std::string &name) {
    const int fd = ::open((devices / name).c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (expect(fd >= 0, "connect test-only FIFO writer")) writers[name] = fd;
  }
  void send(const std::string &name, const unsigned short type, const unsigned short code,
            const int value) {
    input_event record{};
    record.type = type;
    record.code = code;
    record.value = value;
    if (!expect(writers.contains(name), "FIFO writer exists")) return;
    expect(::write(writers.at(name), &record, sizeof(record)) == sizeof(record),
           "send test-only synthetic input record");
  }
  void key(const std::string &name, const unsigned short code, const bool down) {
    send(name, EV_KEY, code, down ? 1 : 0);
    send(name, EV_SYN, SYN_REPORT, 0);
  }
  // One loop iteration: the route's descriptors must be the ones that wake.
  std::vector<int> wake(const int timeout_ms = 1000) {
    const auto woke = loop.poll_once(timeout_ms);
    return woke.fd ? loop.ready_fds() : std::vector<int>{};
  }
};

struct Engine {
  layerc::daemon::X11SessionRegistry registry;
  layerc::daemon::X11InputService service{registry};
  layerc::platform::StoredProfile stored;
  layerc::runtime::CapabilityDemand demand;

  [[nodiscard]] bool start(Fixture &fixture, const InputOptions &options) {
    const auto roots = layerc::platform::profile_store_roots();
    if (!expect(layerc::platform::ensure_profile_store_roots(roots), "isolated store")) return false;
    auto profile = fixture_profile();
    if (!expect(profile.has_value(), "fixture profile parses")) return false;
    stored = {roots.config_profiles / "evdev.json", std::move(*profile)};
    registry.configure(std::span{&stored, 1});
    registry.refresh(active_scan());
    demand = layerc::runtime::calculate_capability_demand(registry.session_requests(), false);
    service.attach_loop(fixture.loop);
    service.configure_input(options);
    return expect(registry.session_count() == 1 &&
                      demand.required(layerc::runtime::Capability::input),
                  "a global binding session demands input");
  }
};

void test_opt_out_never_opens() {
  Fixture fixture;
  fixture.add("event0", keyboard_bits());
  Engine engine;
  if (!engine.start(fixture, fixture.options(InputSelection::automatic))) return;
  const auto result = engine.service.reconcile(engine.demand);
  expect(!ok(result), "without the opt-in and without X there is no input source");
  expect(!engine.service.evdev_selected() && !engine.service.evdev_attached() &&
             engine.service.resources().handles == 0,
         "no device is opened without the opt-in");
  expect(!fixture.loop.poll_once(0).fd, "no descriptor was registered");
  const auto route = engine.service.route_status();
  expect(route.built && route.selection == InputSelection::automatic && !route.evdev_selected,
         "the route reports no opt-in");
}

void test_opted_in_stream_and_hotplug() {
  Fixture fixture;
  fixture.add("event0", keyboard_bits());
  Engine engine;
  if (!engine.start(fixture, fixture.options())) return;
  expect(engine.service.evdev_selected(), "without X the opted-in evdev route is selected");
  expect(engine.service.resources().handles == 0, "nothing opens before demand reconciles");
  if (!expect(ok(engine.service.reconcile(engine.demand)), "evdev serves the demand")) return;
  expect(fixture.backend->passive_flags, "devices open read-only, nonblocking, close-on-exec");
  expect(engine.service.evdev_attached() && !engine.service.attached() &&
             engine.service.resources().handles == 2,
         "one device plus the hotplug watch are held");
  expect(engine.service.unavailable().empty(), "the keyboard the binding needs is present");
  const auto route = engine.service.route_status();
  expect(route.evdev_attached && route.evdev_reasons.empty(), "the evdev row is active");

  fixture.connect("event0");
  fixture.key("event0", KEY_Q, true);
  auto ready = fixture.wake();
  expect(!ready.empty(), "the device descriptor wakes the shared loop");
  auto dispatch = engine.service.dispatch_evdev(ready);
  expect(dispatch.events == 1 &&
             std::ranges::find(dispatch.triggered, "dash") != dispatch.triggered.end(),
         "a passive evdev press triggers the bound cooldown");
  fixture.key("event0", KEY_Q, false);
  dispatch = engine.service.dispatch_evdev(fixture.wake());
  expect(dispatch.events == 1 && dispatch.triggered.empty(), "the release triggers nothing");
  expect(engine.registry.sessions().front().last_input_match.starts_with("keyboard:81"),
         "the session records the evdev match like XI2");

  // Hotplug: a new device is registered on the loop, a removed one leaves.
  fixture.add("event1", mouse_bits());
  dispatch = engine.service.dispatch_evdev(fixture.wake());
  expect(dispatch.devices_changed && engine.service.resources().handles == 3,
         "a hotplugged device is opened and registered");
  std::filesystem::remove(fixture.devices / "event1");
  dispatch = engine.service.dispatch_evdev(fixture.wake());
  expect(dispatch.devices_changed && engine.service.resources().handles == 2,
         "a removed device is closed and unregistered");
  expect(!fixture.loop.poll_once(0).fd, "no stale descriptor stays in the poll set");

  // Zero demand releases every device and registration.
  engine.service.quiesce();
  expect(!engine.service.evdev_attached() && engine.service.resources().handles == 0,
         "quiesce closes the evdev source");
  expect(!fixture.loop.poll_once(0).fd, "quiesce unregisters every descriptor");
  expect(engine.service.dispatch_evdev(ready).events == 0, "a closed route reads nothing");
}

void test_permission_denied_reason() {
  Fixture fixture;
  fixture.add("event0", keyboard_bits(), EACCES);
  Engine engine;
  if (!engine.start(fixture, fixture.options())) return;
  const auto result = engine.service.reconcile(engine.demand);
  expect(!ok(result) && !engine.service.evdev_attached() &&
             engine.service.resources().handles == 0,
         "a route that can read no device is released, not kept open");
  const auto route = engine.service.route_status();
  expect(route.evdev_selected && !route.evdev_attached && route.evdev_reasons.size() == 1 &&
             route.evdev_reasons.front() == "evdev_permission_denied",
         "the stable token is evdev_permission_denied");
  const auto unavailable = engine.service.unavailable();
  expect(unavailable.size() == 1 && unavailable.front().find("'input' group") != std::string::npos,
         "the diagnostic carries the input group hint");
  expect(!fixture.loop.poll_once(0).fd, "nothing stays registered after the failure");
}

void test_missing_class_reasons() {
  Fixture fixture;
  fixture.add("event0", mouse_bits());
  Engine engine;
  if (!engine.start(fixture, fixture.options())) return;
  if (!expect(ok(engine.service.reconcile(engine.demand)), "a mouse alone still opens")) return;
  const auto route = engine.service.route_status();
  expect(route.evdev_reasons == std::vector<std::string>{"evdev_no_keyboard"},
         "only the class a profile needs is reported missing");
  engine.service.quiesce();
}

void test_keymap_replacement() {
  Fixture fixture;
  fixture.add("event0", keyboard_bits());
  layerc::daemon::EvdevInputRoute route;
  route.attach(fixture.loop);
  route.configure(fixture.options());
  route.set_keymap(std::string{"not an xkb keymap"});
  std::string error;
  if (!expect(route.open(error), "an invalid compositor keymap does not cost the input")) return;
#ifdef LAYERC_HAVE_XKBCOMMON
  expect(route.fault().starts_with("evdev_keymap"), "the rejected keymap is reported");
#endif
  route.set_keymap(std::nullopt);
  expect(route.fault().empty(), "the default keymap clears the fault");
  route.set_keymap(std::string{"still not a keymap"});
#ifdef LAYERC_HAVE_XKBCOMMON
  expect(route.fault().starts_with("evdev_keymap"), "a live replacement failure is reported");
#endif
  fixture.connect("event0");
  fixture.key("event0", KEY_Q, true);
  const auto poll = route.poll(fixture.wake());
  expect(poll.events.size() == 1 && poll.events.front().pressed &&
             poll.events.front().token.code == 81,
         "the previous keymap keeps translating after a failed replacement");
  route.close();
  expect(route.registered().empty() && !fixture.loop.poll_once(0).fd,
         "close unregisters before closing");
}

void test_reason_tokens() {
  const std::vector<std::string> reasons{
      "evdev_permission_denied: add the user to the 'input' group; /dev/input/event3: denied",
      "evdev_no_keyboard", "evdev_permission_denied: /dev/input/event4: denied"};
  expect(layerc::daemon::evdev_reason_tokens(reasons) ==
             std::vector<std::string>{"evdev_permission_denied", "evdev_no_keyboard"},
         "status tokens drop paths and repeat reasons");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(test_opt_out_never_opens),
                 LAYERC_TEST_CASE(test_opted_in_stream_and_hotplug),
                 LAYERC_TEST_CASE(test_permission_denied_reason),
                 LAYERC_TEST_CASE(test_missing_class_reasons),
                 LAYERC_TEST_CASE(test_keymap_replacement),
                 LAYERC_TEST_CASE(test_reason_tokens))
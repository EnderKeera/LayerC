#include "evdev_input.hpp"
#include "keysym_vk.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/profile.hpp"
#include "test_support.hpp"

#include <linux/input.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef LAYERC_HAVE_XKBCOMMON
#include <xkbcommon/xkbcommon.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace {
using namespace layerc::platform;
using layerc::core::InputDevice;
using layerc::core::InputEvent;
using layerc::test::expect;

EvdevDeviceBits keyboard_bits() {
  EvdevDeviceBits bits;
  bits.name = "Synthetic keyboard";
  constexpr unsigned keys[] = {
      KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
      KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
      KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
      KEY_SPACE, KEY_ENTER, KEY_LEFTSHIFT, KEY_RIGHTSHIFT};
  for (const auto key : keys) bits.keys.set(key);
  return bits;
}
EvdevDeviceBits mouse_bits() {
  EvdevDeviceBits bits;
  bits.name = "Synthetic mouse";
  constexpr unsigned buttons[] = {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE,
                                   BTN_EXTRA, BTN_FORWARD, BTN_BACK};
  for (const auto button : buttons) bits.keys.set(button);
  bits.relative_axes.set(REL_X).set(REL_Y).set(REL_WHEEL).set(REL_WHEEL_HI_RES);
  return bits;
}
EvdevDeviceBits gamepad_bits() {
  EvdevDeviceBits bits;
  bits.name = "Synthetic gamepad";
  constexpr unsigned buttons[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
      BTN_TL, BTN_TR, BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_MODE,
      BTN_THUMBL, BTN_THUMBR, BTN_DPAD_UP, BTN_DPAD_DOWN,
      BTN_DPAD_LEFT, BTN_DPAD_RIGHT};
  for (const auto button : buttons) bits.keys.set(button);
  bits.absolute_axes.set(ABS_X).set(ABS_Y).set(ABS_HAT0X).set(ABS_HAT0Y);
  return bits;
}

struct FakeDevice {
  EvdevDeviceBits bits;
  std::bitset<KEY_CNT> down;
  int hat_x{};
  int hat_y{};
  int probe_error{};
  int state_error{};
};
class FakeBackend final : public EvdevDeviceBackend {
public:
  std::map<std::string, FakeDevice> entries;
  std::map<int, std::string> descriptors;
  bool passive_flags{true};
  int probe(const int fd, const std::filesystem::path &path,
            EvdevDeviceBits &bits) override {
    const int flags = ::fcntl(fd, F_GETFL);
    passive_flags &= (flags & O_ACCMODE) == O_RDONLY && (flags & O_NONBLOCK) != 0 &&
                     (::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
    const auto name = path.filename().string();
    auto found = entries.find(name);
    if (found == entries.end()) return ENOTTY;
    descriptors[fd] = name;
    bits = found->second.bits;
    return found->second.probe_error;
  }
  int key_state(const int fd, std::bitset<KEY_CNT> &down) override {
    auto &device = entries.at(descriptors.at(fd));
    down = device.down;
    return device.state_error;
  }
  int absolute_value(const int fd, const std::uint16_t axis, int &value) override {
    auto &device = entries.at(descriptors.at(fd));
    value = axis == ABS_HAT0X ? device.hat_x : device.hat_y;
    return device.state_error;
  }
};

EvdevPoll poll_ready(EvdevInputSource &source, const int timeout = 100) {
  std::vector<pollfd> descriptors;
  for (const auto fd : source.fds()) descriptors.push_back({fd, POLLIN, 0});
  const int count = ::poll(descriptors.data(), descriptors.size(), timeout);
  expect(count >= 0, "poll synthetic source descriptors");
  std::vector<int> ready;
  for (const auto &descriptor : descriptors)
    if (descriptor.revents != 0) ready.push_back(descriptor.fd);
  return source.poll(ready);
}

struct Fixture {
  layerc::test::TempDirectory temp{"layerc-evdev"};
  std::shared_ptr<FakeBackend> backend{std::make_shared<FakeBackend>()};
  std::optional<EvdevInputSource> source;
  std::map<std::string, int> writers;
  ~Fixture() {
    for (const auto &[name, fd] : writers) { (void)name; ::close(fd); }
  }
  void add(const std::string &name, EvdevDeviceBits bits) {
    backend->entries[name].bits = std::move(bits);
    expect(::mkfifo((temp.path() / name).c_str(), 0600) == 0, "create event FIFO");
  }
  void create(const std::optional<std::string> &text = std::nullopt) {
    std::string error;
    source = EvdevInputSource::create({temp.path(), text}, error, backend);
    expect(source.has_value() && error.empty(), "create synthetic evdev source");
  }
  void connect(const std::string &name) {
    const int fd = ::open((temp.path() / name).c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (expect(fd >= 0, "connect test-only FIFO writer")) writers[name] = fd;
  }
  void send(const std::string &name, const unsigned short type,
            const unsigned short code, const int value) {
    input_event input{};
    input.type = type;
    input.code = code;
    input.value = value;
    if (!writers.contains(name)) { expect(false, "FIFO writer must exist"); return; }
    expect(::write(writers.at(name), &input, sizeof(input)) == sizeof(input),
           "send test-only synthetic input record");
    if (type == EV_KEY && code < KEY_CNT && (value == 0 || value == 1))
      backend->entries[name].down[code] = value == 1;
    if (type == EV_ABS && code == ABS_HAT0X) backend->entries[name].hat_x = value;
    if (type == EV_ABS && code == ABS_HAT0Y) backend->entries[name].hat_y = value;
  }
  EvdevPoll poll() { return source ? poll_ready(*source) : EvdevPoll{}; }
  void remove(const std::string &name) {
    expect(std::filesystem::remove(temp.path() / name), "remove synthetic device");
  }
};

bool event_is(const InputEvent &event, const InputDevice device,
              const std::uint16_t code, const bool pressed, const std::uint8_t slot = 0) {
  return event.token == layerc::core::InputToken{device, code, slot} &&
         event.pressed == pressed;
}

void test_classification_and_flags() {
  auto keyboard = keyboard_bits();
  expect(classify_evdev_device(keyboard).keyboard, "complete letter keyboard");
  keyboard.keys.reset(KEY_Y);
  expect(!classify_evdev_device(keyboard).keyboard, "not just numeric KEY_A..KEY_Z range");
  keyboard = keyboard_bits();
  keyboard.keys.reset(KEY_ENTER);
  expect(!classify_evdev_device(keyboard).keyboard, "Enter required for keyboard");
  auto mouse = mouse_bits();
  expect(classify_evdev_device(mouse).mouse, "relative X/Y plus left button");
  mouse.relative_axes.reset(REL_Y);
  expect(!classify_evdev_device(mouse).mouse, "one-axis device is not a mouse");
  auto gamepad = gamepad_bits();
  expect(classify_evdev_device(gamepad).gamepad, "gamepad button recognized");
  EvdevDeviceBits joystick;
  joystick.keys.set(BTN_JOYSTICK);
  joystick.absolute_axes.set(ABS_X);
  expect(!classify_evdev_device(joystick).gamepad, "joystick needs both absolute axes");
  joystick.absolute_axes.set(ABS_Y);
  expect(classify_evdev_device(joystick).gamepad, "joystick with X/Y axes recognized");
  auto composite = keyboard_bits();
  composite.keys |= mouse_bits().keys;
  composite.relative_axes = mouse_bits().relative_axes;
  expect(classify_evdev_device(composite) == EvdevInputCapabilities{true, true, false},
         "composite keyboard/mouse retains both classes");
  Fixture fixture;
  fixture.add("event0", keyboard_bits());
  fixture.add("event1", mouse_bits());
  fixture.add("event2", gamepad_bits());
  fixture.add("event3", {});
  fixture.add("not-an-event", keyboard_bits());
  fixture.create();
  if (!fixture.source) return;
  expect(fixture.source->devices().size() == 3, "only recognized event devices opened");
  expect(fixture.source->capabilities() == EvdevInputCapabilities{true, true, true},
         "all classes available");
  expect(fixture.source->fds().size() == 4, "three devices and one inotify fd");
  expect(fixture.backend->passive_flags, "device flags read-only, nonblocking, close-on-exec");
  for (const int fd : fixture.source->fds())
    expect((::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0 &&
           (::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "all descriptors nonblocking/CLOEXEC");
  const auto resources = fixture.source->resources();
  expect(resources.handles == 4 && resources.objects == 3 &&
         resources.threads == 0 && resources.timers == 0, "cheap resource accounting");
  expect(fixture.source->unavailable().empty(), "no missing classes");
}

void test_keyboard_repeat_partial_and_aliases() {
  Fixture fixture;
  fixture.add("event0", keyboard_bits());
  fixture.create();
  if (!fixture.source) return;
  fixture.connect("event0");
  fixture.send("event0", EV_KEY, KEY_E, 1);
  fixture.send("event0", EV_KEY, KEY_E, 2);
  fixture.send("event0", EV_KEY, KEY_E, 1);
  fixture.send("event0", EV_KEY, KEY_E, 0);
  fixture.send("event0", EV_KEY, KEY_E, 0);
  fixture.send("event0", EV_KEY, KEY_RESERVED, 1);
  auto result = fixture.poll();
  if (expect(result.events.size() == 2, "autorepeat, duplicate transitions, unknown keys ignored")) {
    expect(event_is(result.events[0], InputDevice::keyboard, 0x45, true), "E press VK");
    expect(event_is(result.events[1], InputDevice::keyboard, 0x45, false), "E release VK");
  }
  fixture.send("event0", EV_KEY, KEY_LEFTSHIFT, 1);
  fixture.send("event0", EV_KEY, KEY_RIGHTSHIFT, 1);
  fixture.send("event0", EV_KEY, KEY_LEFTSHIFT, 0);
  fixture.send("event0", EV_KEY, KEY_RIGHTSHIFT, 0);
  result = fixture.poll();
  if (expect(result.events.size() == 2, "modifier aliases held until both released"))
    expect(event_is(result.events.back(), InputDevice::keyboard, 0x10, false), "final shift release");
  input_event input{};
  input.type = EV_KEY; input.code = KEY_A; input.value = 1;
  const auto *bytes = reinterpret_cast<const char *>(&input);
  expect(::write(fixture.writers.at("event0"), bytes, 7) == 7, "partial event prefix");
  expect(fixture.poll().events.empty(), "partial record retained without a bogus event");
  expect(::write(fixture.writers.at("event0"), bytes + 7, sizeof(input) - 7) == sizeof(input) - 7,
         "partial event suffix");
  result = fixture.poll();
  expect(result.events.size() == 1 && event_is(result.events.front(), InputDevice::keyboard, 0x41, true),
         "partial record completed on next readiness");
  expect(fixture.source->poll({}).events.empty(), "only ready descriptors drained");
}

void test_mouse_buttons_and_wheel() {
  Fixture fixture;
  fixture.add("event0", mouse_bits());
  fixture.create();
  if (!fixture.source) return;
  fixture.connect("event0");
  constexpr std::pair<std::uint16_t, std::uint16_t> buttons[] = {
      {BTN_LEFT, 1}, {BTN_MIDDLE, 2}, {BTN_RIGHT, 3},
      {BTN_SIDE, 8}, {BTN_BACK, 8}, {BTN_EXTRA, 9}, {BTN_FORWARD, 9}};
  for (const auto &[raw, code] : buttons) {
    expect(evdev_button_to_mouse_code(raw) == code, "exact current X button mapping");
    fixture.send("event0", EV_KEY, raw, 1);
    fixture.send("event0", EV_KEY, raw, 0);
  }
  auto result = fixture.poll();
  if (expect(result.events.size() == std::size(buttons) * 2, "all mapped mouse transitions"))
    for (std::size_t index = 0; index < std::size(buttons); ++index) {
      expect(event_is(result.events[index * 2], InputDevice::mouse, buttons[index].second, true),
             "mouse press");
      expect(event_is(result.events[index * 2 + 1], InputDevice::mouse, buttons[index].second, false),
             "mouse release");
    }
  fixture.send("event0", EV_REL, REL_WHEEL_HI_RES, 240);
  fixture.send("event0", EV_REL, REL_WHEEL, 2);
  fixture.send("event0", EV_REL, REL_WHEEL_HI_RES, -120);
  fixture.send("event0", EV_REL, REL_WHEEL, -1);
  fixture.send("event0", EV_REL, REL_WHEEL, 0);
  fixture.send("event0", EV_REL, REL_X, 25);
  fixture.send("event0", EV_REL, REL_HWHEEL, 1);
  result = fixture.poll();
  if (expect(result.events.size() == 6, "wheel emits one pair per vertical step, not high-res duplicates"))
    for (std::size_t index = 0; index < 3; ++index) {
      const std::uint16_t code = index < 2 ? 6 : 7;
      expect(event_is(result.events[index * 2], InputDevice::mouse, code, true), "wheel press");
      expect(event_is(result.events[index * 2 + 1], InputDevice::mouse, code, false), "wheel release");
    }
  expect(!evdev_button_to_mouse_code(BTN_TOUCH), "touch not assigned mouse button");
}

void test_gamepad_buttons_and_hats() {
  Fixture fixture;
  fixture.add("event0", gamepad_bits());
  fixture.create();
  if (!fixture.source) return;
  fixture.connect("event0");
  constexpr std::pair<std::uint16_t, std::uint16_t> buttons[] = {
      {BTN_DPAD_UP, 1}, {BTN_DPAD_DOWN, 2}, {BTN_DPAD_LEFT, 3}, {BTN_DPAD_RIGHT, 4},
      {BTN_START, 5}, {BTN_SELECT, 6}, {BTN_THUMBL, 7}, {BTN_THUMBR, 8},
      {BTN_TL, 9}, {BTN_TR, 10}, {BTN_MODE, 11},
      {BTN_SOUTH, 13}, {BTN_EAST, 14}, {BTN_WEST, 15}, {BTN_NORTH, 16},
      {BTN_TL2, 17}, {BTN_TR2, 18}};
  for (const auto &[raw, code] : buttons) {
    expect(evdev_button_to_controller_code(raw) == code, "XInput bit index + 1 / trigger code");
    fixture.send("event0", EV_KEY, raw, 1);
    fixture.send("event0", EV_KEY, raw, 0);
  }
  auto result = fixture.poll();
  if (expect(result.events.size() == std::size(buttons) * 2, "all gamepad buttons mapped"))
    for (std::size_t index = 0; index < std::size(buttons); ++index) {
      expect(event_is(result.events[index * 2], InputDevice::controller, buttons[index].second, true),
             "controller press");
      expect(event_is(result.events[index * 2 + 1], InputDevice::controller, buttons[index].second, false),
             "controller release");
    }
  fixture.send("event0", EV_ABS, ABS_HAT0X, -1);
  fixture.send("event0", EV_ABS, ABS_HAT0X, 1);
  fixture.send("event0", EV_ABS, ABS_HAT0X, 0);
  fixture.send("event0", EV_ABS, ABS_HAT0Y, -1);
  fixture.send("event0", EV_ABS, ABS_HAT0Y, 1);
  fixture.send("event0", EV_ABS, ABS_HAT0Y, 0);
  fixture.send("event0", EV_ABS, ABS_X, 32767);
  result = fixture.poll();
  if (expect(result.events.size() == 8, "hats translate both axes; sticks are not buttons")) {
    constexpr std::uint16_t codes[] = {3, 3, 4, 4, 1, 1, 2, 2};
    for (std::size_t index = 0; index < std::size(codes); ++index)
      expect(event_is(result.events[index], InputDevice::controller, codes[index], index % 2 == 0),
             "hat reversal releases before pressing opposite direction");
  }
  fixture.send("event0", EV_KEY, BTN_DPAD_LEFT, 1);
  fixture.send("event0", EV_ABS, ABS_HAT0X, -1);
  fixture.send("event0", EV_KEY, BTN_DPAD_LEFT, 0);
  fixture.send("event0", EV_ABS, ABS_HAT0X, 0);
  expect(fixture.poll().events.size() == 2, "hat/button aliases do not duplicate D-pad transitions");
  expect(!evdev_button_to_controller_code(BTN_TRIGGER), "unmapped joystick button is not guessed");
}

void test_windows_authored_controller_binding() {
  auto parsed = layerc::core::parse_profile_json(R"json({
    "schemaVersion": 1, "id": "evdev-portable", "name": "Portable",
    "game": {"executableName": "game.exe"},
    "cooldowns": [{"id": "dash", "name": "Dash", "durationMs": 1000}],
    "panels": [{"id": "main", "name": "Main", "cooldowns": ["dash"]}],
    "bindings": [{"cooldownId": "dash", "chord": [
      {"device": "controller", "code": 13, "controllerIndex": 0}
    ]}]
  })json");
  const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
  if (!expect(profile != nullptr, "Windows-authored A/code13 profile parses")) return;
  layerc::core::InputMatcher matcher(profile->bindings);
  Fixture fixture;
  fixture.add("event0", gamepad_bits());
  fixture.create();
  if (!fixture.source) return;
  fixture.connect("event0");
  fixture.send("event0", EV_KEY, BTN_SOUTH, 1);
  const auto result = fixture.poll();
  if (expect(result.events.size() == 1, "controller event"))
    expect(matcher.process(result.events.front()) == std::vector<std::string>{"dash"},
           "Windows profile fires on Linux south/A button, not raw evdev/XI2 code");
}

void test_syn_dropped_resync() {
  Fixture fixture;
  fixture.add("event0", keyboard_bits());
  fixture.add("event1", gamepad_bits());
  fixture.create();
  if (!fixture.source) return;
  fixture.connect("event0"); fixture.connect("event1");
  fixture.send("event0", EV_KEY, KEY_A, 1);
  fixture.send("event0", EV_KEY, KEY_E, 1);
  expect(fixture.poll().events.size() == 2, "seed held keys");
  fixture.send("event0", EV_SYN, SYN_DROPPED, 0);
  fixture.send("event0", EV_KEY, KEY_E, 0);
  fixture.send("event0", EV_KEY, KEY_Y, 1);
  expect(fixture.poll().events.empty(), "discard until synchronization report");
  fixture.send("event0", EV_SYN, SYN_REPORT, 0);
  auto result = fixture.poll();
  expect(result.events.size() == 1 && event_is(result.events.front(), InputDevice::keyboard, 0x45, false),
         "EVIOCGKEY releases lost E, retains A, never synthesizes unobserved Y press");
  fixture.send("event0", EV_KEY, KEY_A, 0);
  expect(fixture.poll().events.size() == 1, "still-held A remains tracked");
  fixture.send("event1", EV_KEY, BTN_SOUTH, 1);
  fixture.send("event1", EV_ABS, ABS_HAT0Y, -1);
  expect(fixture.poll().events.size() == 2, "seed gamepad key and hat");
  fixture.backend->entries["event1"].down.reset();
  fixture.backend->entries["event1"].hat_y = 0;
  fixture.send("event1", EV_SYN, SYN_DROPPED, 0);
  fixture.send("event1", EV_SYN, SYN_REPORT, 0);
  result = fixture.poll();
  expect(result.events.size() == 2 && !result.events[0].pressed && !result.events[1].pressed,
         "gamepad key and stale hat released by resync");
}

void test_hotplug_slots_and_removal() {
  Fixture fixture;
  fixture.add("event0", gamepad_bits());
  fixture.add("event1", gamepad_bits());
  fixture.create();
  if (!fixture.source) return;
  fixture.connect("event0"); fixture.connect("event1");
  auto roster = fixture.source->devices();
  expect(roster[0].controller_index == 0 && roster[1].controller_index == 1,
         "initial slots follow deterministic discovery order");
  fixture.send("event0", EV_KEY, BTN_SOUTH, 1);
  expect(fixture.poll().events.size() == 1, "held button before removal");
  fixture.remove("event0");
  auto result = fixture.poll();
  expect(result.devices_changed && result.events.size() == 1 &&
         event_is(result.events.front(), InputDevice::controller, 13, false, 0),
         "inotify deletion closes removed device and releases held button");
  expect(fixture.source->devices().size() == 1 &&
         fixture.source->devices().front().controller_index == 1, "survivor is never renumbered");
  fixture.add("event2", gamepad_bits());
  result = fixture.poll();
  expect(result.devices_changed && fixture.source->devices().size() == 2, "inotify create opens gamepad");
  fixture.connect("event2");
  fixture.send("event2", EV_KEY, BTN_EAST, 1);
  result = fixture.poll();
  expect(result.events.size() == 1 && event_is(result.events.front(), InputDevice::controller, 14, true, 2),
         "new device gets next discovery slot");
  fixture.add("event0", gamepad_bits());
  expect(fixture.poll().devices_changed, "reconnect event path");
  ::close(fixture.writers.at("event0")); fixture.writers.erase("event0");
  fixture.connect("event0");
  fixture.send("event0", EV_KEY, BTN_WEST, 1);
  result = fixture.poll();
  expect(result.events.size() == 1 && event_is(result.events.front(), InputDevice::controller, 15, true, 0),
         "rediscovered path reuses stable slot");
}

void test_inotify_move_and_attrib() {
  Fixture fixture;
  fixture.add("pending", keyboard_bits());
  fixture.backend->entries["event0"] = fixture.backend->entries["pending"];
  fixture.create();
  if (!fixture.source) return;
  expect(fixture.source->devices().empty(), "non-event filename ignored");
  std::filesystem::rename(fixture.temp.path() / "pending", fixture.temp.path() / "event0");
  expect(fixture.poll().devices_changed && fixture.source->capabilities().keyboard,
         "IN_MOVED_TO discovers event device");
  fixture.connect("event0");
  fixture.backend->entries["event0"].probe_error = EACCES;
  expect(::chmod((fixture.temp.path() / "event0").c_str(), 0400) == 0, "change mode to generate IN_ATTRIB");
  expect(fixture.poll().devices_changed && !fixture.source->capabilities().keyboard,
         "IN_ATTRIB reopens/reprobes device and exposes revoked permissions");
  auto reasons = fixture.source->unavailable({.keyboard = true});
  expect(std::ranges::any_of(reasons, [](const auto &reason) {
    return reason.starts_with("evdev_permission_denied") &&
           reason.find("add the user to the 'input' group") != std::string::npos;
  }), "permission reason retains user input-group hint");
  fixture.backend->entries["event0"].probe_error = 0;
  expect(::chmod((fixture.temp.path() / "event0").c_str(), 0600) == 0, "restore synthetic permissions");
  expect(fixture.poll().devices_changed && fixture.source->capabilities().keyboard,
         "permission repair rediscovers keyboard without retry timer");
  std::filesystem::rename(fixture.temp.path() / "event0", fixture.temp.path() / "pending");
  expect(fixture.poll().devices_changed && !fixture.source->capabilities().keyboard,
         "IN_MOVED_FROM retires event device");
}

void test_permission_denied_and_needed_filter() {
  Fixture fixture;
  fixture.backend->entries["event0"].bits = keyboard_bits();
  expect(layerc::test::write_file(fixture.temp.path() / "event0", ""), "permission fixture");
  expect(::chmod(fixture.temp.path().c_str(), 0755) == 0, "allow dropped-privilege child to enter fixture");
  expect(::chmod((fixture.temp.path() / "event0").c_str(), 0000) == 0, "000 file permission fixture");
  // Even when ctest is run as root, exercise a genuine open() EACCES. This
  // child only drops authority; product code never changes users or groups.
  const pid_t child = ::fork();
  if (!expect(child >= 0, "fork permission check")) return;
  if (child == 0) {
    if (::geteuid() == 0 && (::setgid(65534) != 0 || ::setuid(65534) != 0)) ::_exit(2);
    std::string error;
    auto source = EvdevInputSource::create({fixture.temp.path(), std::nullopt}, error, fixture.backend);
    if (!source) ::_exit(3);
    const auto reasons = source->unavailable({.keyboard = true});
    const bool permission = std::ranges::any_of(reasons, [](const auto &reason) {
      return reason.starts_with("evdev_permission_denied") &&
             reason.find("add the user to the 'input' group") != std::string::npos;
    });
    const bool keyboard = std::ranges::find(reasons, "evdev_no_keyboard") != reasons.end();
    const bool filtered = std::ranges::find(reasons, "evdev_no_mouse") == reasons.end() &&
                          std::ranges::find(reasons, "evdev_no_gamepad") == reasons.end();
    ::_exit(permission && keyboard && filtered && source->unavailable({}).empty() ? 0 : 4);
  }
  int status{};
  expect(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "real 000-file permission denial includes hint and filters unneeded classes");
  expect(::chmod((fixture.temp.path() / "event0").c_str(), 0600) == 0, "restore fixture mode");
  fixture.create();
  if (!fixture.source) return;
  expect(fixture.source->unavailable({.keyboard = true}).empty(), "available needed class not degraded");
  expect(fixture.source->unavailable({.mouse = true}) == std::vector<std::string>{"evdev_no_mouse"},
         "only requested mouse reason");
  expect(fixture.source->unavailable({.gamepad = true}) == std::vector<std::string>{"evdev_no_gamepad"},
         "only requested gamepad reason");
}

void test_enodev_eof_and_ownership() {
  Fixture fixture;
  fixture.add("event0", keyboard_bits());
  fixture.create();
  if (!fixture.source) return;
  fixture.connect("event0");
  fixture.send("event0", EV_KEY, KEY_E, 1);
  expect(fixture.poll().events.size() == 1, "seed key before ENODEV");
  fixture.backend->entries["event0"].state_error = ENODEV;
  fixture.backend->entries["event0"].probe_error = ENODEV;
  fixture.send("event0", EV_SYN, SYN_DROPPED, 0);
  fixture.send("event0", EV_SYN, SYN_REPORT, 0);
  auto result = fixture.poll();
  expect(result.devices_changed && result.events.size() == 1 && !result.events.front().pressed &&
         fixture.source->devices().empty(), "ENODEV resync drops device and releases held key");
  expect(fixture.source->fds().size() == 1, "only inotify remains after ENODEV");
  fixture.add("event1", keyboard_bits());
  expect(fixture.poll().devices_changed, "discover replacement FIFO");
  fixture.connect("event1");
  fixture.send("event1", EV_KEY, KEY_A, 1);
  expect(fixture.poll().events.size() == 1, "seed key before EOF");
  ::close(fixture.writers.at("event1")); fixture.writers.erase("event1");
  result = fixture.poll();
  expect(result.devices_changed && result.events.size() == 1 && !result.events.front().pressed &&
         fixture.source->devices().empty(), "EOF drops device and clears held state");
  const auto old_fds = fixture.source->fds();
  EvdevInputSource moved(std::move(*fixture.source));
  expect(moved.valid() && !fixture.source->valid(), "move transfers ownership");
  EvdevInputSource assigned;
  assigned = std::move(moved);
  expect(assigned.valid() && !moved.valid(), "move assignment transfers ownership");
  assigned = EvdevInputSource{};
  for (const auto fd : old_fds)
    expect(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "destruction closes owned descriptors");
  expect(assigned.resources().handles == 0, "moved/default source has no resources");
}

void test_creation_failures_and_real_backend() {
  layerc::test::TempDirectory temp{"layerc-evdev-create"};
  std::string error;
  expect(!EvdevInputSource::create({temp.path() / "missing", std::nullopt}, error) && !error.empty(),
         "missing device directory gives visible creation error");
  expect(!EvdevInputSource::create({temp.path(), std::nullopt}, error, nullptr) && !error.empty(),
         "null backend rejected");
  expect(layerc::test::write_file(temp.path() / "event0", "not evdev"), "non-device regular file");
  auto source = EvdevInputSource::create({temp.path(), std::nullopt}, error);
  expect(source.has_value() && error.empty() && source->devices().empty(),
         "production ioctl backend ignores ENOTTY nondevices");
}

void test_german_keyboard_stream() {
#ifdef LAYERC_HAVE_XKBCOMMON
  auto *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!expect(context != nullptr, "XKB context for German stream")) return;
  xkb_rule_names names{}; names.layout = "de";
  auto *map = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  xkb_context_unref(context);
  if (!expect(map != nullptr, "German keymap names")) return;
  char *text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
  if (!expect(text != nullptr, "German v1 keymap text")) { xkb_keymap_unref(map); return; }
  Fixture fixture;
  fixture.add("event0", keyboard_bits());
  fixture.create(std::string(text));
  std::free(text); xkb_keymap_unref(map);
  if (!fixture.source) return;
  fixture.connect("event0");
  fixture.send("event0", EV_KEY, KEY_Y, 1);
  fixture.send("event0", EV_KEY, KEY_Y, 0);
  const auto result = fixture.poll();
  expect(result.events.size() == 2 && event_is(result.events[0], InputDevice::keyboard, 0x5A, true) &&
         event_is(result.events[1], InputDevice::keyboard, 0x5A, false), "German KEY_Y delivers VK_Z press/release");
#endif
}
} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(test_classification_and_flags),
    LAYERC_TEST_CASE(test_keyboard_repeat_partial_and_aliases),
    LAYERC_TEST_CASE(test_mouse_buttons_and_wheel),
    LAYERC_TEST_CASE(test_gamepad_buttons_and_hats),
    LAYERC_TEST_CASE(test_windows_authored_controller_binding),
    LAYERC_TEST_CASE(test_syn_dropped_resync),
    LAYERC_TEST_CASE(test_hotplug_slots_and_removal),
    LAYERC_TEST_CASE(test_inotify_move_and_attrib),
    LAYERC_TEST_CASE(test_permission_denied_and_needed_filter),
    LAYERC_TEST_CASE(test_enodev_eof_and_ownership),
    LAYERC_TEST_CASE(test_creation_failures_and_real_backend),
    LAYERC_TEST_CASE(test_german_keyboard_stream))

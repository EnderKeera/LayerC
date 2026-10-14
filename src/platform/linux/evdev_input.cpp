#include "evdev_input.hpp"

#include "evdev_keymap.hpp"
#include "keysym_vk.hpp"

#include <linux/input.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace layerc::platform {
namespace {
constexpr std::uint16_t letters[] = {
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
    KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
    KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
constexpr unsigned hat_left = KEY_CNT;
constexpr unsigned hat_right = KEY_CNT + 1U;
constexpr unsigned hat_up = KEY_CNT + 2U;
constexpr unsigned hat_down = KEY_CNT + 3U;

bool event_path(const std::filesystem::path &path) {
  const auto name = path.filename().string();
  return name.starts_with("event");
}

class DeviceBackend final : public EvdevDeviceBackend {
  template <std::size_t Count>
  static int bitmap(const int fd, const unsigned long request,
                    std::bitset<Count> &bits) {
    std::array<unsigned char, (Count + 7U) / 8U> bytes{};
    if (::ioctl(fd, request, bytes.data()) < 0) return errno;
    bits.reset();
    for (std::size_t bit = 0; bit < Count; ++bit)
      bits[bit] = (bytes[bit / 8U] & (1U << (bit % 8U))) != 0;
    return 0;
  }

public:
  int probe(const int fd, const std::filesystem::path &,
            EvdevDeviceBits &bits) override {
    std::bitset<EV_CNT> events;
    if (const int error = bitmap(fd, EVIOCGBIT(0, (EV_CNT + 7U) / 8U), events))
      return error;
    if (events[EV_KEY])
      if (const int error = bitmap(fd, EVIOCGBIT(EV_KEY, (KEY_CNT + 7U) / 8U),
                                   bits.keys)) return error;
    if (events[EV_REL])
      if (const int error = bitmap(fd, EVIOCGBIT(EV_REL, (REL_CNT + 7U) / 8U),
                                   bits.relative_axes)) return error;
    if (events[EV_ABS])
      if (const int error = bitmap(fd, EVIOCGBIT(EV_ABS, (ABS_CNT + 7U) / 8U),
                                   bits.absolute_axes)) return error;
    std::array<char, 256> name{};
    if (::ioctl(fd, EVIOCGNAME(name.size()), name.data()) >= 0)
      bits.name.assign(name.data(), ::strnlen(name.data(), name.size()));
    return 0;
  }
  int key_state(const int fd, std::bitset<KEY_CNT> &down) override {
    return bitmap(fd, EVIOCGKEY((KEY_CNT + 7U) / 8U), down);
  }
  int absolute_value(const int fd, const std::uint16_t axis,
                     int &value) override {
    input_absinfo info{};
    if (::ioctl(fd, EVIOCGABS(axis), &info) < 0) return errno;
    value = info.value;
    return 0;
  }
};

struct Device {
  int fd{-1};
  EvdevInputDevice info;
  EvdevDeviceBits bits;
  dev_t filesystem_device{};
  ino_t inode{};
  std::map<unsigned, core::InputToken> held;
  std::array<std::byte, sizeof(input_event)> partial{};
  std::size_t partial_size{};
  bool dropped{};
  ~Device() { if (fd >= 0) ::close(fd); }
};
} // namespace

EvdevInputCapabilities classify_evdev_device(const EvdevDeviceBits &bits) noexcept {
  EvdevInputCapabilities result;
  result.keyboard = bits.keys[KEY_SPACE] && bits.keys[KEY_ENTER] &&
      std::ranges::all_of(letters, [&](const auto key) { return bits.keys[key]; });
  result.mouse = bits.relative_axes[REL_X] && bits.relative_axes[REL_Y] &&
                 bits.keys[BTN_LEFT];
  result.gamepad = bits.keys[BTN_GAMEPAD] ||
      (bits.keys[BTN_JOYSTICK] && bits.absolute_axes[ABS_X] &&
       bits.absolute_axes[ABS_Y]);
  return result;
}

struct EvdevInputSource::Impl {
  EvdevOptions options;
  EvdevKeymap keymap;
  std::shared_ptr<EvdevDeviceBackend> backend;
  int notify{-1};
  int watch{-1};
  std::vector<std::unique_ptr<Device>> devices;
  // Remember slots by event path for this source lifetime. Removing a device
  // never renumbers surviving controllers; rediscovery of the path reuses it.
  std::map<std::filesystem::path, std::uint8_t> slots;
  std::vector<std::string> faults;

  Impl(const EvdevOptions &opts, EvdevKeymap map,
       std::shared_ptr<EvdevDeviceBackend> device_backend)
      : options(opts), keymap(std::move(map)), backend(std::move(device_backend)) {}
  ~Impl() { if (notify >= 0) ::close(notify); }

  bool token_held(const core::InputToken token) const {
    for (const auto &device : devices)
      for (const auto &[raw, held] : device->held) {
        (void)raw;
        if (held == token) return true;
      }
    return false;
  }

  void release(Device &device, const unsigned raw, EvdevPoll &result) {
    const auto found = device.held.find(raw);
    if (found == device.held.end()) return;
    const auto token = found->second;
    device.held.erase(found);
    if (!token_held(token)) result.events.push_back({token, false});
  }
  void release_all(Device &device, EvdevPoll &result) {
    while (!device.held.empty()) release(device, device.held.begin()->first, result);
  }
  void transition(Device &device, const unsigned raw, const core::InputToken token,
                  const bool pressed, EvdevPoll &result) {
    if (!pressed) {
      release(device, raw, result);
    } else if (!device.held.contains(raw)) {
      const bool already_down = token_held(token);
      device.held.emplace(raw, token);
      if (!already_down) result.events.push_back({token, true});
    }
  }

  void fault(const std::filesystem::path &path, const int error) {
    const std::string prefix = (error == EACCES || error == EPERM)
        ? "evdev_permission_denied: add the user to the 'input' group; "
        : "evdev_device_unavailable: ";
    faults.push_back(prefix + path.string() + ": " + std::strerror(error));
  }

  void scan(EvdevPoll &result, const std::set<std::filesystem::path> &reopen = {}) {
    faults.clear();
    std::error_code error;
    std::vector<std::filesystem::path> paths;
    std::filesystem::directory_iterator iterator(options.device_dir, error);
    if (error) {
      fault(options.device_dir, error.value());
    } else {
      const std::filesystem::directory_iterator end;
      while (iterator != end) {
        if (event_path(iterator->path())) paths.push_back(iterator->path());
        iterator.increment(error);
        if (error) { fault(options.device_dir, error.value()); break; }
      }
    }
    std::ranges::sort(paths);
    // First retire vanished/replaced/permission-changed devices. Emit releases
    // before closing them, so a detached held key cannot poison a core chord.
    for (auto it = devices.begin(); it != devices.end();) {
      struct stat status{};
      const auto &path = (*it)->info.path;
      if (!std::ranges::binary_search(paths, path) || reopen.contains(path) ||
          ::stat(path.c_str(), &status) < 0 || status.st_dev != (*it)->filesystem_device ||
          status.st_ino != (*it)->inode) {
        release_all(**it, result);
        it = devices.erase(it);
        result.devices_changed = true;
      } else {
        ++it;
      }
    }
    for (const auto &path : paths) {
      if (std::ranges::any_of(devices, [&](const auto &device) {
            return device->info.path == path;
          })) continue;
      auto device = std::make_unique<Device>();
      device->fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (device->fd < 0) { fault(path, errno); continue; }
      struct stat status{};
      if (::fstat(device->fd, &status) < 0) { fault(path, errno); continue; }
      // No symlink directories or regular fixture files should ever block.
      if (!S_ISCHR(status.st_mode) && !S_ISFIFO(status.st_mode) &&
          !S_ISREG(status.st_mode)) continue;
      const int probe_error = backend->probe(device->fd, path, device->bits);
      if (probe_error != 0) {
        if (probe_error != ENOTTY && probe_error != ENODEV) fault(path, probe_error);
        continue;
      }
      device->info = {path, device->bits.name, classify_evdev_device(device->bits),
                      std::nullopt};
      const auto classes = device->info.classes;
      if (!classes.keyboard && !classes.mouse && !classes.gamepad) continue;
      if (classes.gamepad) {
        auto slot = slots.find(path);
        if (slot == slots.end()) {
          if (slots.size() >= 256U) {
            faults.push_back("evdev_controller_slots_exhausted");
            continue;
          }
          slot = slots.emplace(path, static_cast<std::uint8_t>(slots.size())).first;
        }
        device->info.controller_index = slot->second;
      }
      device->filesystem_device = status.st_dev;
      device->inode = status.st_ino;
      devices.push_back(std::move(device));
      result.devices_changed = true;
    }
  }

  std::optional<core::InputToken> token(const Device &device,
                                       const std::uint16_t code) const {
    if (device.info.classes.mouse)
      if (const auto mouse = evdev_button_to_mouse_code(code))
        return core::InputToken{core::InputDevice::mouse, *mouse, 0};
    if (device.info.classes.gamepad)
      if (const auto controller = evdev_button_to_controller_code(code))
        return core::InputToken{core::InputDevice::controller, *controller,
                               *device.info.controller_index};
    if (device.info.classes.keyboard && code < BTN_MISC)
      if (const auto vk = keymap.virtual_key(code))
        return core::InputToken{core::InputDevice::keyboard, *vk, 0};
    return std::nullopt;
  }

  void hat(Device &device, const unsigned raw, const std::uint16_t code,
           const bool down, EvdevPoll &result) {
    transition(device, raw, {core::InputDevice::controller, code,
                            *device.info.controller_index}, down, result);
  }

  // After SYN_DROPPED discard events until SYN_REPORT, then query the actual
  // state. Only releases are synthesized: never trigger a binding for an
  // unobserved press. Hat state is queried as well as the kernel key bitmap.
  int resync(Device &device, EvdevPoll &result) {
    std::bitset<KEY_CNT> down;
    const int error = backend->key_state(device.fd, down);
    if (error != 0) { release_all(device, result); return error; }
    int x{}, y{};
    int x_error{}, y_error{};
    if (device.bits.absolute_axes[ABS_HAT0X])
      x_error = backend->absolute_value(device.fd, ABS_HAT0X, x);
    if (device.bits.absolute_axes[ABS_HAT0Y])
      y_error = backend->absolute_value(device.fd, ABS_HAT0Y, y);
    for (auto it = device.held.begin(); it != device.held.end();) {
      const unsigned raw = it->first;
      ++it;
      const bool still_down = raw < KEY_CNT ? down[raw]
          : raw == hat_left ? x_error == 0 && x < 0
          : raw == hat_right ? x_error == 0 && x > 0
          : raw == hat_up ? y_error == 0 && y < 0
          : y_error == 0 && y > 0;
      if (!still_down) release(device, raw, result);
    }
    return x_error == ENODEV || y_error == ENODEV ? ENODEV : 0;
  }

  int event(Device &device, const input_event &input, EvdevPoll &result) {
    if (input.type == EV_SYN && input.code == SYN_DROPPED) {
      device.dropped = true;
      return 0;
    }
    if (device.dropped) {
      if (input.type == EV_SYN && input.code == SYN_REPORT) {
        device.dropped = false;
        return resync(device, result);
      }
      return 0;
    }
    if (input.type == EV_KEY && (input.value == 0 || input.value == 1) &&
        input.code < KEY_CNT && device.bits.keys[input.code]) {
      if (const auto mapped = token(device, input.code))
        transition(device, input.code, *mapped, input.value == 1, result);
    } else if (input.type == EV_REL && input.code == REL_WHEEL &&
               device.info.classes.mouse) {
      // High-resolution duplicate wheel records are intentionally ignored.
      const std::uint16_t code = input.value > 0 ? 6 : 7;
      const auto steps = input.value < 0 ? -static_cast<std::int64_t>(input.value)
                                         : static_cast<std::int64_t>(input.value);
      for (std::int64_t step = 0; step < steps; ++step) {
        result.events.push_back({{core::InputDevice::mouse, code, 0}, true});
        result.events.push_back({{core::InputDevice::mouse, code, 0}, false});
      }
    } else if (input.type == EV_ABS && device.info.classes.gamepad &&
               input.code < ABS_CNT && device.bits.absolute_axes[input.code]) {
      if (input.code == ABS_HAT0X) {
        // Release the old direction before pressing the opposite direction.
        if (input.value >= 0) hat(device, hat_left, 3, false, result);
        if (input.value <= 0) hat(device, hat_right, 4, false, result);
        if (input.value < 0) hat(device, hat_left, 3, true, result);
        if (input.value > 0) hat(device, hat_right, 4, true, result);
      } else if (input.code == ABS_HAT0Y) {
        if (input.value >= 0) hat(device, hat_up, 1, false, result);
        if (input.value <= 0) hat(device, hat_down, 2, false, result);
        if (input.value < 0) hat(device, hat_up, 1, true, result);
        if (input.value > 0) hat(device, hat_down, 2, true, result);
      }
    }
    return 0;
  }

  bool drain(Device &device, EvdevPoll &result) {
    for (;;) {
      const auto count = ::read(device.fd, device.partial.data() + device.partial_size,
                                device.partial.size() - device.partial_size);
      if (count > 0) {
        device.partial_size += static_cast<std::size_t>(count);
        if (device.partial_size == device.partial.size()) {
          input_event input{};
          std::memcpy(&input, device.partial.data(), sizeof(input));
          device.partial_size = 0;
          const int error = event(device, input, result);
          if (error == ENODEV) return false;
          if (error != 0) fault(device.info.path, error);
        }
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return true;
      } else {
        if (count < 0 && errno != ENODEV) fault(device.info.path, errno);
        return false;
      }
    }
  }

  void notifications(EvdevPoll &result) {
    alignas(inotify_event) std::array<std::byte, 4096> buffer{};
    std::set<std::filesystem::path> reopen;
    bool changed{};
    for (;;) {
      const auto count = ::read(notify, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      std::size_t offset{};
      while (offset + sizeof(inotify_event) <= static_cast<std::size_t>(count)) {
        const auto *notice = reinterpret_cast<const inotify_event *>(buffer.data() + offset);
        if (notice->mask & (IN_Q_OVERFLOW | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
          changed = true;
        if (notice->len != 0) {
          const auto path = options.device_dir / notice->name;
          if (event_path(path)) {
            changed = true;
            if (notice->mask & (IN_ATTRIB | IN_DELETE | IN_MOVED_FROM)) reopen.insert(path);
          }
        }
        offset += sizeof(inotify_event) + notice->len;
      }
    }
    if (changed) {
      result.devices_changed = true;
      scan(result, reopen);
    }
  }
};

EvdevInputSource::EvdevInputSource() noexcept = default;
EvdevInputSource::EvdevInputSource(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
EvdevInputSource::EvdevInputSource(EvdevInputSource &&) noexcept = default;
EvdevInputSource &EvdevInputSource::operator=(EvdevInputSource &&) noexcept = default;
EvdevInputSource::~EvdevInputSource() = default;

std::optional<EvdevInputSource>
EvdevInputSource::create(const EvdevOptions &options, std::string &error) {
  return create(options, error, std::make_shared<DeviceBackend>());
}
std::optional<EvdevInputSource>
EvdevInputSource::create(const EvdevOptions &options, std::string &error,
                         std::shared_ptr<EvdevDeviceBackend> backend) {
  error.clear();
  if (!backend) { error = "evdev: no device backend"; return std::nullopt; }
  auto map = EvdevKeymap::create(options.xkb_keymap_text, error);
  if (!map) return std::nullopt;
  auto impl = std::make_unique<Impl>(options, std::move(*map), std::move(backend));
  impl->notify = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (impl->notify < 0) {
    error = "evdev: inotify_init1: " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  impl->watch = ::inotify_add_watch(impl->notify, options.device_dir.c_str(),
      IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO |
      IN_DELETE_SELF | IN_MOVE_SELF);
  if (impl->watch < 0) {
    error = "evdev: watch " + options.device_dir.string() + ": " + std::strerror(errno);
    if (errno == EACCES || errno == EPERM)
      error = "evdev_permission_denied: add the user to the 'input' group; " + error;
    return std::nullopt;
  }
  EvdevPoll initial;
  impl->scan(initial);
  return EvdevInputSource(std::move(impl));
}

bool EvdevInputSource::valid() const noexcept { return impl_ != nullptr; }
bool EvdevInputSource::set_keymap(const std::optional<std::string> &text,
                                  std::string &error) {
  error.clear();
  if (!impl_) { error = "evdev: no source"; return false; }
  auto map = EvdevKeymap::create(text, error);
  if (!map) return false;
  // Held keys keep the tokens they were pressed with, so releases match.
  impl_->keymap = std::move(*map);
  impl_->options.xkb_keymap_text = text;
  return true;
}
std::vector<int> EvdevInputSource::fds() const {
  std::vector<int> result;
  if (impl_) {
    for (const auto &device : impl_->devices) result.push_back(device->fd);
    result.push_back(impl_->notify);
  }
  return result;
}
EvdevPoll EvdevInputSource::poll(const std::span<const int> ready) {
  EvdevPoll result;
  if (!impl_) return result;
  // Drain old device descriptors before hotplug can reuse their fd numbers.
  for (auto it = impl_->devices.begin(); it != impl_->devices.end();) {
    if (std::ranges::find(ready, (*it)->fd) != ready.end() && !impl_->drain(**it, result)) {
      impl_->release_all(**it, result);
      it = impl_->devices.erase(it);
      result.devices_changed = true;
    } else {
      ++it;
    }
  }
  if (std::ranges::find(ready, impl_->notify) != ready.end()) impl_->notifications(result);
  return result;
}
std::vector<EvdevInputDevice> EvdevInputSource::devices() const {
  std::vector<EvdevInputDevice> result;
  if (impl_)
    for (const auto &device : impl_->devices) result.push_back(device->info);
  return result;
}
EvdevInputCapabilities EvdevInputSource::capabilities() const noexcept {
  EvdevInputCapabilities result;
  if (impl_)
    for (const auto &device : impl_->devices) {
      result.keyboard |= device->info.classes.keyboard;
      result.mouse |= device->info.classes.mouse;
      result.gamepad |= device->info.classes.gamepad;
    }
  return result;
}
std::vector<std::string>
EvdevInputSource::unavailable(const EvdevInputCapabilities needed) const {
  std::vector<std::string> reasons;
  if (!needed.keyboard && !needed.mouse && !needed.gamepad) return reasons;
  const auto caps = capabilities();
  const bool missing = (needed.keyboard && !caps.keyboard) ||
                       (needed.mouse && !caps.mouse) || (needed.gamepad && !caps.gamepad);
  if (missing && impl_) reasons = impl_->faults;
  if (needed.keyboard && !caps.keyboard) reasons.push_back("evdev_no_keyboard");
  if (needed.mouse && !caps.mouse) reasons.push_back("evdev_no_mouse");
  if (needed.gamepad && !caps.gamepad) reasons.push_back("evdev_no_gamepad");
  return reasons;
}
runtime::RuntimeResourceStats EvdevInputSource::resources() const noexcept {
  runtime::RuntimeResourceStats result;
  if (impl_) {
    result.handles = static_cast<std::uint32_t>(impl_->devices.size() + 1U);
    result.objects = static_cast<std::uint32_t>(impl_->devices.size());
  }
  return result;
}
} // namespace layerc::platform

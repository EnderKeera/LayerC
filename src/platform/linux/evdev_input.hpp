#pragma once

#include "layerc/core/types.hpp"
#include "layerc/runtime/runtime_services.hpp"

#include <linux/input-event-codes.h>

#include <bitset>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::platform {

struct EvdevOptions {
  std::filesystem::path device_dir{"/dev/input"};
  std::optional<std::string> xkb_keymap_text;
};

struct EvdevInputCapabilities {
  bool keyboard{};
  bool mouse{};
  bool gamepad{};
  bool operator==(const EvdevInputCapabilities &) const = default;
};

// All probe/state operations are read-only. Returns zero on success, an errno
// value otherwise. Production uses EVIOCGBIT/EVIOCGNAME/EVIOCGKEY/EVIOCGABS;
// tests supply bitmaps for FIFO-backed event streams without real devices.
struct EvdevDeviceBits {
  std::bitset<KEY_CNT> keys;
  std::bitset<REL_CNT> relative_axes;
  std::bitset<ABS_CNT> absolute_axes;
  std::string name;
};
class EvdevDeviceBackend {
public:
  virtual ~EvdevDeviceBackend() = default;
  [[nodiscard]] virtual int probe(int fd, const std::filesystem::path &path,
                                  EvdevDeviceBits &bits) = 0;
  [[nodiscard]] virtual int key_state(int fd, std::bitset<KEY_CNT> &down) = 0;
  [[nodiscard]] virtual int absolute_value(int fd, std::uint16_t axis,
                                          int &value) = 0;
};

// Composite devices can expose more than one input class.
[[nodiscard]] EvdevInputCapabilities
classify_evdev_device(const EvdevDeviceBits &bits) noexcept;

struct EvdevInputDevice {
  std::filesystem::path path;
  std::string name;
  EvdevInputCapabilities classes;
  std::optional<std::uint8_t> controller_index;
  bool operator==(const EvdevInputDevice &) const = default;
};
struct EvdevPoll {
  std::vector<core::InputEvent> events;
  bool devices_changed{};
};

// Single event-loop owner, no threads. Merely constructing this object is the
// opt-in; the integrator must only create it while opted-in input is demanded.
// Device state is ephemeral, never logged. Destruction closes every descriptor.
class EvdevInputSource {
public:
  EvdevInputSource() noexcept;
  EvdevInputSource(EvdevInputSource &&) noexcept;
  EvdevInputSource &operator=(EvdevInputSource &&) noexcept;
  EvdevInputSource(const EvdevInputSource &) = delete;
  EvdevInputSource &operator=(const EvdevInputSource &) = delete;
  ~EvdevInputSource();

  [[nodiscard]] static std::optional<EvdevInputSource>
  create(const EvdevOptions &options, std::string &error);
  [[nodiscard]] static std::optional<EvdevInputSource>
  create(const EvdevOptions &options, std::string &error,
         std::shared_ptr<EvdevDeviceBackend> backend);
  [[nodiscard]] bool valid() const noexcept;
  // Replaces the XKB keymap (nullopt: library defaults). On failure the
  // previous keymap stays in use and `error` explains why.
  [[nodiscard]] bool set_keymap(const std::optional<std::string> &text,
                                std::string &error);
  // Device descriptors followed by the inotify descriptor. After poll reports
  // devices_changed, replace event-loop registrations with this fresh list.
  [[nodiscard]] std::vector<int> fds() const;
  [[nodiscard]] EvdevPoll poll(std::span<const int> ready);
  [[nodiscard]] std::vector<EvdevInputDevice> devices() const;
  [[nodiscard]] EvdevInputCapabilities capabilities() const noexcept;
  [[nodiscard]] std::vector<std::string>
  unavailable(EvdevInputCapabilities needed = {.keyboard = true,
                                               .mouse = true,
                                               .gamepad = true}) const;
  [[nodiscard]] runtime::RuntimeResourceStats resources() const noexcept;

private:
  struct Impl;
  explicit EvdevInputSource(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::platform

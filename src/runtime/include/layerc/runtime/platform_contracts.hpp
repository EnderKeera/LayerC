#pragma once

#include "layerc/core/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::runtime {

using SessionId = std::uint64_t;

// Platform code creates WindowIds and keeps native handles in its adapter.
// Runtime code may compare or persist an ID only for the lifetime of a scan.
struct WindowId {
  std::uint64_t opaque{};
  auto operator<=>(const WindowId &) const = default;
};

struct PlatformRect {
  std::int32_t left{};
  std::int32_t top{};
  std::int32_t right{};
  std::int32_t bottom{};
  auto operator<=>(const PlatformRect &) const = default;
};

struct ScaleContext {
  std::uint32_t dpi_x{96};
  std::uint32_t dpi_y{96};
  double logical_to_physical_x{1.0};
  double logical_to_physical_y{1.0};
};

struct GameWindowSnapshot {
  WindowId id;
  std::uint32_t process_id{};
  std::string executable_path;
  std::string title;
  PlatformRect client_rect;
  ScaleContext scale;
  bool foreground{};
  bool minimized{};
};

class IGameWindowPlatform {
public:
  virtual ~IGameWindowPlatform() = default;
  [[nodiscard]] virtual std::vector<GameWindowSnapshot> discover() = 0;
  [[nodiscard]] virtual std::optional<WindowId> foreground_window() const = 0;
};

enum class NormalizedInputBackend : std::uint8_t {
  keyboard,
  mouse,
  controller,
  generic_hid,
};

struct NormalizedInputEvent {
  NormalizedInputBackend backend{};
  std::uint32_t device_instance{};
  std::uint32_t control{};
  bool pressed{};
  core::TimePoint observed_at{};
};

class IInputPlatform {
public:
  virtual ~IInputPlatform() = default;
  virtual bool activate(std::span<const NormalizedInputBackend> backends,
                        std::string &error) = 0;
  virtual void quiesce() noexcept = 0;
};

struct OverlayPresentation {
  SessionId session{};
  WindowId target;
  PlatformRect bounds;
  ScaleContext scale;
  bool visible{};
};

class IOverlayPlatform {
public:
  virtual ~IOverlayPlatform() = default;
  virtual bool reconcile(std::span<const OverlayPresentation> presentations,
                         std::string &error) = 0;
  virtual void quiesce() noexcept = 0;
};

struct CaptureSessionRequest {
  WindowId target;
  std::uint8_t quality{};
  std::vector<PlatformRect> excluded_regions;
};

class ICaptureSession {
public:
  virtual ~ICaptureSession() = default;
  [[nodiscard]] virtual WindowId target() const noexcept = 0;
  virtual void quiesce() noexcept = 0;
};

class ICapturePlatform {
public:
  virtual ~ICapturePlatform() = default;
  [[nodiscard]] virtual std::unique_ptr<ICaptureSession>
  create(const CaptureSessionRequest &request, std::string &error) = 0;
};

class IControlTransport {
public:
  virtual ~IControlTransport() = default;
  virtual bool send(std::string_view bounded_message, std::string &error) = 0;
  virtual void close() noexcept = 0;
};

struct HelperLaunchRequest {
  WindowId target;
  std::string package_version;
  std::string nonce;
};

class IHelperProcess {
public:
  virtual ~IHelperProcess() = default;
  [[nodiscard]] virtual std::uint32_t process_id() const noexcept = 0;
  virtual void terminate() noexcept = 0;
};

class IHelperLifecycle {
public:
  virtual ~IHelperLifecycle() = default;
  [[nodiscard]] virtual std::unique_ptr<IHelperProcess>
  launch(const HelperLaunchRequest &request, std::string &error) = 0;
};

} // namespace layerc::runtime

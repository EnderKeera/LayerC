#pragma once

// Test-only Wayland client used inside tools/wayland-test-session.sh. It owns
// a second connection (the fixture "game" window, screen reads and a virtual
// pointer) so the product adapters can be observed from outside. Virtual
// pointer input is synthesized only inside that disposable compositor and
// never links into layerd.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace layerc::test {

struct WaylandPixels {
  int width{};
  int height{};
  // 0xRRGGBB per pixel, row-major, top row first.
  std::vector<std::uint32_t> rgb;

  [[nodiscard]] std::uint32_t at(int x, int y) const {
    return rgb[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
               static_cast<std::size_t>(x)];
  }
};

struct WaylandPointerPress {
  bool on_fixture{};
  double x{};
  double y{};
};

class WaylandTestClient {
public:
  WaylandTestClient();
  ~WaylandTestClient();
  WaylandTestClient(const WaylandTestClient &) = delete;
  WaylandTestClient &operator=(const WaylandTestClient &) = delete;

  // Connects to $WAYLAND_DISPLAY and binds every fixture global.
  [[nodiscard]] bool connect(std::string &error);
  // Maps a fullscreen, solid-colour toplevel on output `output_name`.
  [[nodiscard]] bool show_window(const std::string &title, const std::string &app_id,
                                 const std::string &output_name, std::uint32_t rgb);
  void close_window();
  // Flushes and dispatches whatever is readable without blocking.
  void pump();
  // Reads output pixels (screencopy, cursor excluded).
  [[nodiscard]] std::optional<WaylandPixels> capture(const std::string &output_name);
  // Moves the virtual pointer to global logical coordinates inside the
  // layout extent and presses/releases BTN_LEFT; returns where it landed.
  [[nodiscard]] std::optional<WaylandPointerPress> click(int x, int y, int layout_width,
                                                         int layout_height);
  [[nodiscard]] bool window_mapped() const noexcept;

  // Public only so the C protocol listeners can name it.
  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

// Pumps `pump` until `predicate` holds or the monotonic deadline passes.
[[nodiscard]] bool wait_until(const std::function<void()> &pump,
                              const std::function<bool()> &predicate,
                              int timeout_ms = 3000);

} // namespace layerc::test

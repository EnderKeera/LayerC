#pragma once

#include <memory>
#include <string>

class Fl_Window;

namespace layerc::prototype {

// Experimental semantic adapter, not a production toolkit abstraction.
// ATK's maintained AT-SPI adaptor handles the local IPC protocol; FLTK owns
// the UI loop. No input synthesis, GTK widgets, or polling thread is used.
class AccessibilityBridge final {
public:
  explicit AccessibilityBridge(Fl_Window &window);
  ~AccessibilityBridge();
  AccessibilityBridge(const AccessibilityBridge &) = delete;
  AccessibilityBridge &operator=(const AccessibilityBridge &) = delete;
  [[nodiscard]] bool available() const;
  [[nodiscard]] const std::string &fault() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::prototype

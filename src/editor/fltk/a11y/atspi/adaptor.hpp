#pragma once

#include "a11y/atspi/status.hpp"

#include <cstdint>
#include <memory>
#include <string>

class Fl_Widget;

namespace layerc::editor::a11y {

class FltkTree;

namespace atspi {

// Whether assistive technology can see the editor, and, when it cannot, why.
struct AdaptorStatus {
  StatusState state{StatusState::off};
  std::string reason;
};

// Publishes the FLTK tree on the accessibility bus on demand: while no
// consumer listens the editor pays one idle session-bus descriptor and takes
// no snapshots. Every message is handled on the FLTK thread, so no call ever
// blocks it and no snapshot is copied across threads.
class Adaptor {
public:
  explicit Adaptor(FltkTree &tree);
  ~Adaptor();
  Adaptor(const Adaptor &) = delete;
  Adaptor &operator=(const Adaptor &) = delete;

  [[nodiscard]] AdaptorStatus status() const;

  // Told once per status change, on the FLTK thread, from the loop: never from
  // a destructor and never while a bus dispatch is on the stack.
  using StatusHandler = void (*)(void *data);
  void on_status_change(StatusHandler handler, void *data);
  // Raises a live-region announcement on the widget's node at the next
  // refresh; ignored while no consumer listens.
  void announce(Fl_Widget *source, std::string text);

  // Test seams: the number of published snapshots, whether the accessibility
  // connection is up, and whether the loop still watches the tree.
  [[nodiscard]] std::uint64_t snapshot_count() const;
  [[nodiscard]] bool connected() const;
  [[nodiscard]] bool watching() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atspi
} // namespace layerc::editor::a11y

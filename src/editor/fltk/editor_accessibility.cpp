// Accessibility bus publication through the editor's own AT-SPI adaptor.
#include "editor_accessibility.hpp"

#include "a11y/fltk_tree.hpp"

#include <FL/Fl.H>

#include <functional>
#include <utility>

namespace layerc::editor {

std::string accessibility_status_line(const a11y::atspi::AdaptorStatus &status) {
  switch (status.state) {
  case a11y::atspi::StatusState::on:
    return "Accessibility: AT-SPI bridge active";
  case a11y::atspi::StatusState::off:
    return "Accessibility: AT-SPI bridge waiting for a screen reader";
  case a11y::atspi::StatusState::unavailable:
    break;
  }
  return "Accessibility unavailable: " + status.reason;
}

struct EditorAccessibility::Impl {
  a11y::FltkTree tree;
  a11y::atspi::Adaptor adaptor;
  StatusHandler handler;
  bool previous_gtk_chooser{};

  explicit Impl(EditorApp &) : adaptor(tree) {
    // GTK's chooser would drag GTK and its own accessibility bridge into this
    // process; FLTK's chooser is what this adaptor exports, so keep it always.
    previous_gtk_chooser = Fl::option(Fl::OPTION_FNFC_USES_GTK);
    Fl::option(Fl::OPTION_FNFC_USES_GTK, false);
  }
  ~Impl() {
    // The tree goes with the chatty handler already detached, so no callback
    // can read widgets that are about to disappear.
    adaptor.on_status_change(nullptr, nullptr);
    tree.clear();
    Fl::option(Fl::OPTION_FNFC_USES_GTK, previous_gtk_chooser);
  }
  static void deliver(void *data) {
    auto &self = *static_cast<Impl *>(data);
    if (self.handler) self.handler(self.adaptor.status());
  }
  void set_handler(StatusHandler next) {
    handler = std::move(next);
    adaptor.on_status_change(handler ? &Impl::deliver : nullptr, handler ? this : nullptr);
    // A late caller renders the current line without waiting for the next
    // change; this runs on the FLTK thread, outside any bus dispatch.
    if (handler) handler(adaptor.status());
  }
};

EditorAccessibility::EditorAccessibility(EditorApp &app)
    : impl_(std::make_unique<Impl>(app)) {}
EditorAccessibility::~EditorAccessibility() = default;

a11y::atspi::AdaptorStatus EditorAccessibility::status() const {
  return impl_->adaptor.status();
}

void EditorAccessibility::set_status_handler(StatusHandler handler) {
  impl_->set_handler(std::move(handler));
}

void EditorAccessibility::announce(Fl_Widget *source, const std::string &text) {
  if (text.empty()) return;
  impl_->adaptor.announce(source, text);
}
} // namespace layerc::editor

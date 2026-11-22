// Accessibility publication through the editor's UI Automation adaptor.
#include "editor_accessibility.hpp"
#include "editor_app.hpp"

#include "a11y/fltk_tree.hpp"
#include "a11y/uia/adaptor.hpp"

#include <functional>
#include <utility>

namespace layerc::editor {

std::string accessibility_status_line(const AccessibilityStatus &status) {
  switch (status.state) {
  case AccessibilityStatusState::active:
    return "Accessibility: UI Automation bridge active";
  case AccessibilityStatusState::waiting:
    return "Accessibility: UI Automation bridge waiting for a client";
  case AccessibilityStatusState::unavailable:
    return "Accessibility unavailable: " + status.reason;
  }
  return "Accessibility unavailable: " + status.reason;
}

struct EditorAccessibility::Impl {
  a11y::FltkTree tree;
  a11y::uia::Adaptor adaptor;
  StatusHandler handler;

  explicit Impl(EditorApp &) : adaptor(tree) {}
  ~Impl() { tree.clear(); }

  [[nodiscard]] AccessibilityStatus status() const {
    if (const auto error = adaptor.error())
      return {AccessibilityStatusState::unavailable, error->message};
    return {AccessibilityStatusState::active, {}};
  }

  void set_handler(StatusHandler next) {
    handler = std::move(next);
    if (handler) handler(status());
  }
};

EditorAccessibility::EditorAccessibility(EditorApp &app)
    : impl_(std::make_unique<Impl>(app)) {}
EditorAccessibility::~EditorAccessibility() = default;

AccessibilityStatus EditorAccessibility::status() const {
  return impl_->status();
}

bool EditorAccessibility::sync() {
  const auto error = impl_->adaptor.sync();
  if (impl_->handler) impl_->handler(impl_->status());
  return !error;
}

bool EditorApp::sync_accessibility() { return accessibility->sync(); }

void EditorAccessibility::set_status_handler(StatusHandler handler) {
  impl_->set_handler(std::move(handler));
}

void EditorAccessibility::announce(Fl_Widget *source, const std::string &text) {
  if (text.empty()) return;
  impl_->adaptor.announce(source, text);
}

} // namespace layerc::editor

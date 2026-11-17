#pragma once

#include "model.hpp"

#include <FL/Fl_Input.H>
#include <FL/Fl_Input_.H>

#include <optional>

namespace layerc::editor::a11y {

// Text geometry belongs to the widget, never to an RPC thread. The free
// function also serves plain Fl_Input_ widgets the tree pass exports, so both
// adaptors report one geometry.
[[nodiscard]] std::optional<Text> field_text(const Fl_Input_ &field);

// Text geometry for an input the editor creates as this class.
class Input : public Fl_Input {
public:
  using Fl_Input::Fl_Input;
  [[nodiscard]] Text accessible_text() const;
};

} // namespace layerc::editor::a11y

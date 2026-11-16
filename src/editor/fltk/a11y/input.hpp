#pragma once

#include "model.hpp"

#include <FL/Fl_Input.H>

namespace layerc::editor::a11y {

// Text geometry belongs to the widget, never to an RPC thread. This supplies
// FLTK's protected scroll offsets without casts into a widget's internals.
class Input : public Fl_Input {
public:
  using Fl_Input::Fl_Input;
  [[nodiscard]] Text accessible_text() const;
};

} // namespace layerc::editor::a11y

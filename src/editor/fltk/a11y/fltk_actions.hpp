#pragma once

// Toolkit-side accessibility actions, shared by the ATK adaptor today and the
// sd-bus bridge and UIA bridge later. Nothing here includes ATK, GLib, X11 or
// Win32 headers: the same validated step runs whatever protocol delivered it.

#include "fltk_semantics.hpp"

#include <cstddef>
#include <string_view>

namespace layerc::editor::a11y {

// Every entry point validates on the calling (FLTK) thread and returns false
// when the request is rejected. Application callbacks never run inside the
// call: the validated step is queued on FLTK's loop, because a callback may
// open a modal dialog and the protocol call must have returned by then. A
// queued step is dropped when its widget was deleted, is no longer active_r(),
// or a modal window excludes it. Actions never synthesize input events.
[[nodiscard]] bool activate(const Target &target);
[[nodiscard]] bool grab_focus(const Target &target);

// UTF-8 byte offsets: the model's unit and FLTK's. An offset past the end or
// inside a code point is rejected, as is a field whose value is not UTF-8.
// Caret and selection move immediately; neither runs a callback.
[[nodiscard]] bool set_caret(const Target &target, std::size_t offset);
[[nodiscard]] bool select_text(const Target &target, std::size_t anchor, std::size_t caret);

// Edits need an editable field: an input that is not read-only, is enabled and
// showing, and holds UTF-8. The new text lands immediately, so the next read
// sees it, and the field's own callback runs from the loop.
[[nodiscard]] bool set_text(const Target &target, std::string_view text);
[[nodiscard]] bool insert_text(const Target &target, std::size_t offset, std::string_view text);
[[nodiscard]] bool delete_text(const Target &target, std::size_t begin, std::size_t end);

// Valuators only, within their own range. A value that does not change the
// widget queues no callback, exactly like the widget's own setter.
[[nodiscard]] bool set_number(const Target &target, double value);

// Drops queued steps and removes the loop timeout: adaptor teardown.
void cancel_deferred();

} // namespace layerc::editor::a11y

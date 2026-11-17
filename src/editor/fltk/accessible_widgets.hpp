#pragma once

#include "a11y/model.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Widget.H>

#include <optional>
#include <string>
#include <string_view>

namespace layerc::editor::accessibility {

// Screen reader text for the Linux editor (DesignSystem.md, Screen readers).
// An override wins over the widget's own label; a description is either text
// or, through describe_with, whatever a live hint widget says. Entries hold
// Fl_Widget_Trackers and dead ones are pruned when the next entry registers,
// so a destroyed widget's address never inherits stale text in a new dialog.
// Everything here runs on the FLTK thread, which creates, changes and
// destroys the widgets: there is no lock and no other thread may call this.
void set_accessible_name(Fl_Widget &control, std::string text);
void set_accessible_description(Fl_Widget &control, std::string text);
// Exported role for controls whose toolkit class does not say what they are.
void set_accessible_role(Fl_Widget &control, a11y::Role role);
void describe_with(Fl_Widget &control, Fl_Widget &hint);

[[nodiscard]] std::optional<std::string> accessible_name(const Fl_Widget &control);
[[nodiscard]] std::optional<a11y::Role> accessible_role(const Fl_Widget &control);
[[nodiscard]] std::string accessible_description(const Fl_Widget &control);

// A color button's name: its label without FLTK's access-key marker or a
// trailing ellipsis, then its color ("Fill, color #DDBC17").
[[nodiscard]] std::string color_button_name(std::string_view label,
                                            std::string_view hex);

} // namespace layerc::editor::accessibility

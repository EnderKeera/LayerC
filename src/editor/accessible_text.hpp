#pragma once

#include "status_voice.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include <windows.h>

namespace layerc::editor {

// Screen reader text (DesignSystem.md, Screen readers). Names and
// descriptions go through Dynamic Annotation, which UI Automation reads as
// the element's name and help text. Both need COM on the calling thread;
// without it a control keeps the name its own text gives it.

// Replaces `control`'s accessible name. Empty text restores its own.
void set_accessible_name(HWND control, std::wstring_view text);
// The hint, reason, warning or tooltip that belongs to `control`. Empty
// text removes it.
void set_accessible_description(HWND control, std::wstring_view text);
// Gives `control` the text of `hint`, the line that belongs to it, as its
// description.
void describe_with(HWND control, HWND hint);

// "#RRGGBB", as profiles and Choose color write a color.
[[nodiscard]] std::wstring color_hex(std::uint32_t color);
// A color button's name: its label without the access-key marker, then its
// color ("Fill, color #DDBC17").
[[nodiscard]] std::wstring color_button_name(std::wstring_view label,
                                             std::wstring_view color);
// Names `button` from its own text and `color`.
void set_color_button_name(HWND button, std::wstring_view color);
// A palette swatch's name with its 1-based place ("Copper #D88E4B, 7 of 24").
[[nodiscard]] std::wstring swatch_name(std::wstring_view name,
                                       std::wstring_view color, int position,
                                       int count);

// How a status line message reaches a screen reader: the three voices and
// their rule are shared with the Linux editor (status_voice.hpp).

// Writes `text` to the status line and announces it as `voice` allows.
void set_status_text(HWND status, std::wstring_view text,
                     StatusVoice voice = StatusVoice::announced);
// Raises a UI Automation notification from `control`, a newer message
// replacing one not yet spoken. Does nothing while no client listens.
void announce(HWND control, std::wstring_view text);

} // namespace layerc::editor

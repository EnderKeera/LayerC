#pragma once

#include "platform/hid_binding_capture.hpp"

class Fl_Widget;

namespace layerc::editor {

struct EditorAppearance;

// The accepting caller validates and records the edit and, after validation,
// assigns the captured device's machine-local slot. Refusal keeps the dialog
// open for another attempt. Cancel never calls accept.
using AcceptHidBinding = std::function<std::optional<std::string>(
    const core::HidControlBinding &, bool replace_slot)>;
using HidBindingAnnouncement =
    std::function<void(Fl_Widget *, const std::string &)>;

[[nodiscard]] bool capture_hid_binding(HidBindingCapture &capture,
                                       const EditorAppearance &appearance,
                                       const AcceptHidBinding &accept,
                                       const HidBindingAnnouncement &announce = {});

} // namespace layerc::editor

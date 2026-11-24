#pragma once

#include <FL/Fl.H>

namespace layerc::editor {

// FLTK thread only. Adding the same (handler, data) replaces its wake.
// A positive delay requires an OS wake; failure leaves nothing queued.
[[nodiscard]] bool steady_wake_add(double seconds, Fl_Timeout_Handler, void *data);
void steady_wake_remove(Fl_Timeout_Handler, void *data);
// True only while the OS wait is outstanding, not after delivery is posted.
[[nodiscard]] bool steady_wake_armed(Fl_Timeout_Handler, void *data);

enum class SteadyWakeFault { none, create, arm };
void steady_wake_fault_for_test(SteadyWakeFault fault) noexcept;

} // namespace layerc::editor

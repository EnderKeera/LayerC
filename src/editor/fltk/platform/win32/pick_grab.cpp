#include "platform/services.hpp"

#include <FL/Fl.H>
#include <windows.h>

// FLTK's Windows grab state (Fl_win32.cxx, Fl.cxx), cleared by hand below.
extern HWND fl_capture;
void fl_fix_focus();

namespace layerc::editor {

bool release_pick_grab(const std::uintptr_t owner) {
  if (!Fl::grab()) return true;
  const HWND capture = GetCapture();
  if (capture == nullptr || capture == reinterpret_cast<HWND>(owner)) {
    Fl::grab(nullptr);
    return true;
  }
  // Another window of this thread took capture after the pick. FLTK's
  // grab(nullptr) would ReleaseCapture() it, so clear only FLTK's grab state,
  // as Fl_WinAPI_Screen_Driver::grab does (pinned FLTK 1.4.5).
  fl_capture = nullptr;
  Fl::grab_ = nullptr;
  fl_fix_focus();
  return false;
}

} // namespace layerc::editor

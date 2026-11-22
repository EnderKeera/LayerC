#include <iostream>

void windows_editor_platform_exclusions() {
  std::cout
      << "Not applicable: linux_editor_accessibility_tree_rows, "
         "linux_editor_accessibility_descriptions, linux_editor_accessibility_hint_and_names, "
         "linux_editor_accessibility_color_names, linux_editor_accessibility_announcements, "
         "linux_editor_accessibility_popup_focus_events (AT-SPI; "
         "Windows: layerc_windows_fltk_accessibility_tests, layerc_windows_fltk_notification_tests)\n"
         "Not applicable: linux_editor_accessibility_keeps_fltk_file_chooser "
         "(GTK/AT-SPI chooser policy; no Windows counterpart)\n"
         "Not applicable: linux_editor_focused_keyboard_capture_contracts "
         "(XTest/X11 focus capture; Windows: layerc_windows_fltk_binding_tests)\n"
         "Not applicable: linux_editor_calibration_layout_contracts, "
         "calibration_unavailable_and_owned_capture (in-process X11 calibration; "
         "Windows: layerc_windows_fltk_calibration_tests)\n";
}

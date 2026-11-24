# The FLTK frontend's toolkit- and platform-independent sources. Keep the two
# components split only to preserve the existing Linux library membership.
set(LAYERC_EDITOR_FLTK_PORTABLE_SUPPORT_SOURCES
    src/editor/fltk/lease_worker.cpp
    src/editor/fltk/editor_session.cpp
    src/editor/fltk/daemon_link_report.cpp
    src/editor/fltk/profile_mutations.cpp
    src/editor/fltk/editor_appearance.cpp
    src/editor/fltk/task_list.cpp)

set(LAYERC_EDITOR_FLTK_PORTABLE_GUI_SOURCES
    src/editor/fltk/editor_app.cpp
    src/editor/fltk/editor_style.cpp
    src/editor/fltk/editor_popup.cpp
    src/editor/fltk/editor_color_chooser.cpp
    src/editor/fltk/editor_autosave.cpp
    src/editor/fltk/diagnostics_window.cpp
    src/editor/fltk/appearance_window.cpp
    src/editor/fltk/editor_tools.cpp
    src/editor/fltk/options_window.cpp
    src/editor/fltk/editor_placement.cpp
    src/editor/fltk/editor_on_window.cpp
    src/editor/fltk/tasks_window.cpp
    src/editor/fltk/automation_editor.cpp
    src/editor/fltk/visual_editor.cpp
    src/editor/fltk/editor_forms.cpp
    src/editor/fltk/editor_geometry.cpp
    src/editor/fltk/editor_import.cpp
    src/editor/fltk/editor_file_chooser.cpp
    src/editor/fltk/cooldown_groups_window.cpp
    src/editor/fltk/export_selection_window.cpp
    src/editor/fltk/editor_commands.cpp
    src/editor/fltk/editor_pages.cpp
    src/editor/fltk/panel_item_dialog.cpp
    src/editor/fltk/panel_items_controls.cpp
    src/editor/fltk/editor_menus.cpp
    src/editor/fltk/editor_preview.cpp
    src/editor/fltk/binding_capture.cpp
    src/editor/fltk/hid_binding_dialog.cpp
    src/editor/fltk/accessible_widgets.cpp)

set(LAYERC_EDITOR_FLTK_PORTABLE_SOURCES
    ${LAYERC_EDITOR_FLTK_PORTABLE_SUPPORT_SOURCES}
    ${LAYERC_EDITOR_FLTK_PORTABLE_GUI_SOURCES})

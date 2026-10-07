#pragma once

#include "dialog_theme.hpp"
#include "editor_state.hpp"
#include "profile_document.hpp"
#include "layerc/core/types.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace layerc::editor {

// The cooldown list, the selected cooldown's fields and bindings, and the
// exclusive binding capture (keyboard chord or HID control). It edits the
// injected document, reports every accepted edit through the change
// recorder, and reaches the host-owned bar and visual views only through
// the injected callbacks; it has no access to its host window's state.
class CooldownEditor final {
public:
  using ChangeRecorder = std::function<void(DocumentChangeKind)>;
  struct Host {
    // Commits pending field edits before a structural change.
    std::function<void()> sync_fields;
    // Selects the bar that shows the selected cooldown.
    std::function<void()> follow_bar;
    // Re-projects the selected cooldown's visual trigger.
    std::function<void()> refresh_visual;
    // Rebuilds the bar list, selecting `preferred_panel` when it exists.
    std::function<void(std::size_t preferred_panel)> refresh_bars;
  };

  CooldownEditor(ProfileDocument &document, EditorShell &shell,
                 const DialogTheme &theme, ChangeRecorder record_change,
                 Host host) noexcept;

  CooldownEditor(const CooldownEditor &) = delete;
  CooldownEditor &operator=(const CooldownEditor &) = delete;

  // Binds the main window's cooldown list box once, during WM_INITDIALOG.
  void bind(HWND list) noexcept { list_ = list; }
  [[nodiscard]] HWND list() const noexcept { return list_; }
  [[nodiscard]] bool capturing_binding() const noexcept {
    return capture_.capturing_binding();
  }

  [[nodiscard]] layerc::core::CooldownDefinition *selected();
  void refresh_list(int selection = 0);
  void refresh_selected_label();
  void refresh_controls();
  // Scan codes of the selected cooldown's binding, for its tooltip.
  [[nodiscard]] std::wstring binding_codes_text();

  // Consumes keyboard and mouse messages while a chord is being captured.
  bool handle_capture_message(const MSG &message);
  void begin_capture();
  void capture_hid();
  void cancel_capture(std::wstring_view status = {});
  // Leaves capture mode without refreshing, before the controls are rebuilt.
  void reset_capture() noexcept { capture_.finish_binding_capture(); }

  void add();
  void duplicate();
  void remove();

private:
  void assign_chord(std::span<const std::uint16_t> scan_codes);
  void set_status(std::wstring_view text) const;

  ProfileDocument &document_;
  EditorShell &shell_;
  const DialogTheme &theme_;
  ChangeRecorder record_change_;
  Host host_;
  HWND list_{};
  BindingCapture capture_;
};

} // namespace layerc::editor

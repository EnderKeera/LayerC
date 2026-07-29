#pragma once

#include "overlay_window.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/runtime_instances.hpp"
#include "layerc/core/types.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <shellapi.h>
#include <windows.h>
#include <xinput.h>

namespace layerc::daemon {

class DaemonApp {
public:
  int run(HINSTANCE instance, int show_command);

private:
  struct ProfileSource {
    std::wstring key;
    layerc::core::GameProfile profile;
  };

  // A Session is deliberately an instance, not a profile. Each matching game
  // window gets its own cooldown engine and its own overlay windows.
  struct Session {
    struct PolledKeyState {
      layerc::core::InputToken token;
      bool pressed{};
    };

    explicit Session(layerc::core::GameProfile source_profile,
                     std::wstring source_key, bool capture_exclusion);

    void clear_input_state();
    void set_capture_exclusion(bool enabled);
    [[nodiscard]] bool is_global() const noexcept {
      return profile.game.global;
    }
    [[nodiscard]] bool requires_input_polling() const noexcept {
      return uses_controller || !global_keyboard.empty();
    }

    std::wstring source_key;
    layerc::core::GameProfile profile;
    std::vector<OverlayCooldown> overlay_cooldowns;
    layerc::core::CooldownEngine cooldowns;
    layerc::core::InputMatcher input;
    std::vector<layerc::core::CooldownState> snapshot_buffer;
    std::vector<std::unique_ptr<OverlayWindow>> overlays;
    std::vector<PolledKeyState> global_keyboard;
    HWND game_window{};
    bool uses_controller{};
  };

  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam);
  LRESULT handle_message(HWND window, UINT message, WPARAM wparam,
                         LPARAM lparam);

  bool create_message_window(HINSTANCE instance);
  bool register_input_devices();
  void add_tray_icon();
  void ensure_tray_icon();
  void remove_tray_icon();
  void show_tray_menu();
  void launch_editor();
  void reload_profiles();
  [[nodiscard]] bool reload_profiles_if_requested();
  [[nodiscard]] bool reload_settings_if_requested();
  void refresh_game_windows();
  void sync_foreground_state();
  void sync_overlay_location(HWND changed_window);
  void handle_raw_input(HRAWINPUT handle);
  void poll_input_devices();
  [[nodiscard]] bool poll_global_keyboard(Session &session);
  void seed_controller_states();
  void poll_controllers();
  void emit_input(const layerc::core::InputEvent &event);
  [[nodiscard]] bool process_input(Session &session,
                                   const layerc::core::InputEvent &event);
  void update_overlays();
  void update_tooltip(const std::wstring &status);
  [[nodiscard]] std::vector<Session *> foreground_sessions();
  [[nodiscard]] RECT overlay_target() const;
  [[nodiscard]] std::filesystem::path profile_directory() const;
  static void CALLBACK foreground_event_proc(HWINEVENTHOOK hook, DWORD event,
                                             HWND window, LONG object_id,
                                             LONG child_id, DWORD event_thread,
                                             DWORD event_time);

  HINSTANCE instance_{};
  HWND window_{};
  NOTIFYICONDATAW tray_{};
  std::vector<ProfileSource> profile_sources_;
  layerc::core::RuntimeInstanceTracker runtime_instances_;
  std::vector<std::unique_ptr<Session>> sessions_;
  std::vector<Session *> active_input_sessions_;
  std::array<XINPUT_STATE, XUSER_MAX_COUNT> controller_states_{};
  HWND input_foreground_{};
  HANDLE reload_event_{};
  HANDLE settings_reload_event_{};
  bool capture_exclusion_{true};
  bool input_timer_running_{};
  bool overlay_timer_running_{};
  HWINEVENTHOOK foreground_event_hook_{};
  HWINEVENTHOOK location_event_hook_{};
};

} // namespace layerc::daemon
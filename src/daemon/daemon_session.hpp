#pragma once

#include "overlay_collection.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/hid.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/core/pixel_engine.hpp"
#include "layerc/core/profile_session.hpp"
#include "layerc/core/types.hpp"
#include "layerc/platform/profile_store.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace layerc::daemon {

template <typename Profile> struct ModuleSidecar {
  std::optional<Profile> profile;
  layerc::platform::ModuleSidecarState state{
      layerc::platform::ModuleSidecarState::absent};

  [[nodiscard]] bool usable() const noexcept {
    return layerc::platform::is_usable(state) && profile.has_value();
  }

  bool operator==(const ModuleSidecar &) const = default;
};

struct ProfileModules {
  ModuleSidecar<layerc::core::VisualProfile> visual;
  ModuleSidecar<layerc::core::AutomationProfile> automation;
  ModuleSidecar<layerc::core::CompanionProfile> companion;

  bool operator==(const ProfileModules &) const = default;
};

struct SessionInit {
  layerc::core::GameProfile profile;
  std::string source_key;
  ProfileModules modules;
  bool visual_enabled{};
};

struct ProfileSource {
  std::string key;
  std::filesystem::path path;
  layerc::core::GameProfile profile;
  ProfileModules modules;
  layerc::core::AutomationPersistentValues automation_state;
  std::optional<layerc::core::TimePoint> automation_state_due;
  layerc::core::ProfileRevision revision{};
  // Bumped whenever automation-relevant content changes (the automation
  // sidecar, its health, or the base revision that owns cooldown groups) so
  // per-reconcile deep profile compares run only after real edits. The
  // sidecar can change on disk while the base profile stays identical, so
  // the base revision alone is not enough.
  layerc::core::ProfileRevision automation_revision{};

  [[nodiscard]] SessionInit to_session_init(bool visual_enabled) const;
};

struct Session : layerc::core::AutomationSession {
  struct PolledKeyState {
    layerc::core::InputToken token;
    bool pressed{};
  };

  explicit Session(SessionInit init);

  void clear_input_state();
  void rebuild_input_configuration();
  void set_capture_exclusion(bool enabled);
  void apply_profile(const layerc::core::GameProfile &updated,
                     bool capture_exclusion, layerc::core::TimePoint now);
  void apply_visual_profile(
      ModuleSidecar<layerc::core::VisualProfile> visual, bool visual_enabled);
  void apply_automation_profile(
      ModuleSidecar<layerc::core::AutomationProfile> automation);
  void apply_companion_profile(
      ModuleSidecar<layerc::core::CompanionProfile> companion);
  [[nodiscard]] bool has_enabled_automations() const noexcept;
  [[nodiscard]] bool has_active_visual_rules() const noexcept;
  [[nodiscard]] bool is_global() const noexcept { return profile.game.global; }
  [[nodiscard]] bool requires_input_polling() const noexcept {
    return uses_controller || !global_keyboard.empty();
  }

  std::string source_key;
  layerc::core::GameProfile profile;
  OverlayCollection overlays;
  layerc::core::InputMatcher input;
  ProfileModules modules;
  // The source automation_revision the runtime was created or last
  // reconfigured against; reconcile compares this instead of deep-comparing
  // profiles on every state change.
  layerc::core::ProfileRevision automation_revision{};
  std::vector<layerc::core::OverlayPropertyOverride>
      applied_overlay_overrides;
  std::uint64_t last_visual_sequence{};
  std::vector<PolledKeyState> global_keyboard;
  std::vector<layerc::core::HidControlState> hid_states;
  HWND game_window{};
  bool uses_controller{};
  bool uses_raw_input{};
  bool uses_hid{};
  std::string last_input_match;
};

struct EditSession {
  layerc::core::EditSessionId id{};
  layerc::core::ProfileRevision revision{};
  layerc::core::ProfileRevision outbound_revision{};
  std::string source_key;
  layerc::core::GameProfile profile;
  std::string selected_panel;
  HWND target{};
  bool direct_drag{};
  layerc::core::TimePoint last_heartbeat{};
  std::vector<layerc::core::CooldownState> preview_states;
  OverlayCollection overlays;
};

} // namespace layerc::daemon

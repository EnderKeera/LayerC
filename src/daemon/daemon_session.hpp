#pragma once

#include "overlay_collection.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/hid.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/core/pixel_engine.hpp"
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

struct ProfileSource {
  std::wstring key;
  std::filesystem::path path;
  layerc::core::GameProfile profile;
  std::optional<layerc::core::VisualProfile> visual_profile;
  layerc::platform::VisualSidecarState visual_health{
      layerc::platform::VisualSidecarState::absent};
  std::string visual_error;
  std::optional<layerc::core::AutomationProfile> automation_profile;
  layerc::platform::AutomationSidecarState automation_health{
      layerc::platform::AutomationSidecarState::absent};
  std::string automation_error;
  std::optional<layerc::core::CompanionProfile> companion_profile;
  layerc::platform::CompanionSidecarState companion_health{
      layerc::platform::CompanionSidecarState::absent};
  std::string companion_error;
  layerc::core::AutomationPersistentValues automation_state;
  std::optional<layerc::core::TimePoint> automation_state_due;
  layerc::core::ProfileRevision revision{};
  // Bumped whenever automation-relevant content changes (the automation
  // sidecar, its health, or the base revision that owns cooldown groups) so
  // per-reconcile deep profile compares run only after real edits. The
  // sidecar can change on disk while the base profile stays identical, so
  // the base revision alone is not enough.
  layerc::core::ProfileRevision automation_revision{};
};

// Everything a Session is constructed from; named fields keep the construction
// inputs readable and order-independent at call sites.
struct SessionInit {
  layerc::core::GameProfile profile;
  std::wstring source_key;
  std::optional<layerc::core::VisualProfile> visual_profile;
  layerc::platform::VisualSidecarState visual_health{
      layerc::platform::VisualSidecarState::absent};
  std::optional<layerc::core::AutomationProfile> automation_profile;
  layerc::platform::AutomationSidecarState automation_health{
      layerc::platform::AutomationSidecarState::absent};
  std::optional<layerc::core::CompanionProfile> companion_profile;
  layerc::platform::CompanionSidecarState companion_health{
      layerc::platform::CompanionSidecarState::absent};
  bool visual_enabled{};
  bool capture_exclusion{};
};

struct Session {
  struct PolledKeyState {
    layerc::core::InputToken token;
    bool pressed{};
  };

  explicit Session(SessionInit init);

  void clear_input_state();
  void rebuild_input_configuration();
  void ensure_overlays(bool capture_exclusion);
  void release_overlays() noexcept;
  void set_capture_exclusion(bool enabled);
  void apply_profile(const layerc::core::GameProfile &updated,
                     bool capture_exclusion);
  void apply_visual_profile(
      std::optional<layerc::core::VisualProfile> visual_profile,
      layerc::platform::VisualSidecarState visual_health, bool visual_enabled);
  void apply_automation_profile(
      std::optional<layerc::core::AutomationProfile> automation_profile,
      layerc::platform::AutomationSidecarState automation_health);
  void apply_companion_profile(
      std::optional<layerc::core::CompanionProfile> companion_profile,
      layerc::platform::CompanionSidecarState companion_health);
  [[nodiscard]] bool has_enabled_automations() const noexcept;
  [[nodiscard]] bool has_active_visual_rules() const noexcept;
  [[nodiscard]] bool has_companion_slots() const noexcept;
  [[nodiscard]] bool blocks_all_triggers() const noexcept {
    return visual_health == layerc::platform::VisualSidecarState::invalid;
  }
  [[nodiscard]] bool is_global() const noexcept { return profile.game.global; }
  [[nodiscard]] bool requires_input_polling() const noexcept {
    return uses_controller || !global_keyboard.empty();
  }

  std::wstring source_key;
  layerc::core::GameProfile profile;
  OverlayCollection overlays;
  layerc::core::CooldownEngine cooldowns;
  layerc::core::InputMatcher input;
  std::optional<layerc::core::VisualProfile> visual_profile;
  std::unique_ptr<layerc::core::VisualTriggerEngine> visual_triggers;
  std::optional<layerc::core::AutomationProfile> automation_profile;
  std::unique_ptr<layerc::core::AutomationRuntime> automation_runtime;
  std::optional<layerc::core::CompanionProfile> companion_profile;
  layerc::platform::CompanionSidecarState companion_health{
      layerc::platform::CompanionSidecarState::absent};
  // The source automation_revision the runtime was created or last
  // reconfigured against; reconcile compares this instead of deep-comparing
  // profiles on every state change.
  layerc::core::ProfileRevision automation_revision{};
  std::vector<layerc::core::OverlayPropertyOverride>
      applied_overlay_overrides;
  layerc::platform::AutomationSidecarState automation_health{
      layerc::platform::AutomationSidecarState::absent};
  layerc::platform::VisualSidecarState visual_health{
      layerc::platform::VisualSidecarState::absent};
  std::uint64_t last_visual_sequence{};
  std::vector<layerc::core::CooldownState> snapshot_buffer;
  std::vector<PolledKeyState> global_keyboard;
  std::vector<layerc::core::HidControlState> hid_states;
  HWND game_window{};
  bool uses_controller{};
  bool uses_raw_input{};
  bool uses_hid{};
  std::string last_input_match;
  layerc::core::TimePoint last_input_at{};
};

struct EditSession {
  layerc::core::EditSessionId id{};
  layerc::core::ProfileRevision revision{};
  layerc::core::ProfileRevision outbound_revision{};
  std::wstring source_key;
  layerc::core::GameProfile profile;
  std::string selected_panel;
  HWND target{};
  bool direct_drag{};
  layerc::core::TimePoint last_heartbeat{};
  std::vector<layerc::core::CooldownState> preview_states;
  OverlayCollection overlays;
};

} // namespace layerc::daemon

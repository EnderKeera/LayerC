#include "companion_session_selection.hpp"
#include "daemon_app.hpp"
#include "input_demand.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/settings_store.hpp"
#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::daemon {
namespace {
std::filesystem::path profile_path_for_key(const std::string_view key) {
  const auto decoded = layerc::core::try_utf8_to_wide(key);
  return decoded.has_value() && !decoded->empty()
             ? std::filesystem::path(*decoded)
             : std::filesystem::path{};
}

template <typename Profile, typename Error>
ModuleSidecar<Profile> module_sidecar_from(
    const layerc::platform::ModuleSidecarLoad<Profile, Error> &loaded) {
  return ModuleSidecar<Profile>::from(loaded);
}

std::vector<ProfileSource>
dedupe_sources(std::vector<ProfileSource> loaded) {
  std::vector<ProfileSource> unique;
  unique.reserve(loaded.size());
  for (auto &source : loaded) {
    source.key = layerc::platform::canonical_profile_key(source.path);
    const auto duplicate = std::ranges::find(unique, source.key,
                                             &ProfileSource::key);
    if (duplicate == unique.end())
      unique.push_back(std::move(source));
    else if (source.revision >= duplicate->revision)
      *duplicate = std::move(source);
  }
  return unique;
}

void advance_source_revisions(
    std::span<ProfileSource> loaded,
    const std::span<const ProfileSource> previous_sources) {
  for (auto &source : loaded) {
    const auto previous = std::ranges::find(previous_sources, source.key,
                                            &ProfileSource::key);
    if (previous == previous_sources.end()) continue;
    if (source.revision == 0) {
      source.revision = previous->revision;
      if (source.profile != previous->profile)
        ++source.revision;
    }
    source.automation_revision = previous->automation_revision;
    if (source.revision != previous->revision ||
        source.modules.automation != previous->modules.automation)
      ++source.automation_revision;
  }
}

std::vector<std::string> compute_recreate_keys(
    std::span<ProfileSource> loaded,
    const std::span<const ProfileSource> previous_sources) {
  std::vector<std::string> keys;
  for (auto &source : loaded) {
    const auto previous = std::ranges::find(previous_sources, source.key,
                                            &ProfileSource::key);
    if (previous == previous_sources.end()) {
      keys.push_back(source.key);
      continue;
    }
    if (layerc::core::plan_session_reconciliation(previous->profile,
                                                  source.profile)
            .recreate_instances)
      keys.push_back(source.key);
  }
  for (const auto &previous : previous_sources) {
    if (std::ranges::none_of(loaded, [&](const ProfileSource &source) {
          return source.key == previous.key;
        }))
      keys.push_back(previous.key);
  }
  return keys;
}

} // namespace

SessionInit ProfileSource::to_session_init(const bool visual_enabled) const {
  return {.profile = profile,
          .source_key = key,
          .modules = modules,
          .visual_enabled = visual_enabled};
}

Session::Session(SessionInit init)
    : layerc::core::AutomationSession(std::move(init.profile)),
      source_key(std::move(init.source_key)), input(profile.bindings) {
  apply_visual_profile(std::move(init.modules.visual), init.visual_enabled);
  apply_automation_profile(std::move(init.modules.automation));
  apply_companion_profile(std::move(init.modules.companion));
  snapshot_buffer.reserve(profile.cooldowns.size());
  rebuild_input_configuration();
}

void Session::apply_visual_profile(
    ModuleSidecar<layerc::core::VisualProfile> updated,
    const bool visual_enabled) {
  modules.visual = std::move(updated);
  set_invalid_visual_sidecar(
      modules.visual.state == layerc::platform::ModuleSidecarState::invalid);
  last_visual_sequence = 0;
  if (modules.visual.usable()) {
    if (visual_triggers)
      visual_triggers->reconfigure(*modules.visual.profile);
    else
      visual_triggers = std::make_unique<layerc::core::VisualTriggerEngine>(
          *modules.visual.profile);
    visual_triggers->set_visual_enabled(visual_enabled);
  } else {
    visual_triggers.reset();
  }
}

void Session::apply_automation_profile(
    ModuleSidecar<layerc::core::AutomationProfile> updated) {
  if (!updated.usable()) updated.profile.reset();
  if (!updated.profile.has_value())
    automation_runtime.reset();
  modules.automation = std::move(updated);
  rebuild_input_configuration();
}

void Session::apply_companion_profile(
    ModuleSidecar<layerc::core::CompanionProfile> updated) {
  if (!updated.usable()) updated.profile.reset();
  modules.companion = std::move(updated);
}

bool Session::has_enabled_automations() const noexcept {
  return modules.automation.usable() &&
         layerc::core::has_enabled_automations(*modules.automation.profile);
}
bool Session::has_active_visual_rules() const noexcept {
  return visual_triggers != nullptr && modules.visual.usable() &&
         layerc::core::has_enabled_visual_assignments(*modules.visual.profile) &&
         visual_triggers->visual_enabled();
}

namespace {

// Seeds matcher state for keyboard/controller tokens and records which
// device classes and global keys this session needs at runtime.
void seed_binding_token(Session &session,
                        const layerc::core::InputToken &token) {
  bool pressed{};
  UINT virtual_key{};
  if (token.device == layerc::core::InputDevice::keyboard) {
    virtual_key = MapVirtualKeyW(token.code, MAPVK_VSC_TO_VK_EX);
    pressed =
        virtual_key != 0 &&
        (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
    session.input.seed(token, pressed);
  } else if (token.device == layerc::core::InputDevice::controller &&
             token.controller_index < XUSER_MAX_COUNT) {
    XINPUT_STATE state{};
    if (XInputGetState(token.controller_index, &state) == ERROR_SUCCESS) {
      if (token.code >= 1 && token.code <= 16) {
        const WORD mask = static_cast<WORD>(1U << (token.code - 1U));
        pressed = (state.Gamepad.wButtons & mask) != 0;
      } else if (token.code == 17) {
        pressed =
            state.Gamepad.bLeftTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
      } else if (token.code == 18) {
        pressed =
            state.Gamepad.bRightTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
      }
      session.input.seed(token, pressed);
    }
  }
  if (!session.is_global() ||
      token.device != layerc::core::InputDevice::keyboard || virtual_key == 0)
    return;
  const auto existing = std::find_if(
      session.global_keyboard.begin(), session.global_keyboard.end(),
      [&](const Session::PolledKeyState &key) { return key.token == token; });
  if (existing == session.global_keyboard.end())
    session.global_keyboard.push_back(
        {.token = token, .virtual_key = virtual_key, .pressed = pressed});
}

} // namespace

void Session::rebuild_input_configuration() {
  hid_states.assign(profile.hid_bindings.size(), {});
  global_keyboard.clear();
  for (const auto &binding : profile.bindings)
    for (const auto &token : binding.chord.tokens)
      seed_binding_token(*this, token);
  const auto demand = input_device_demand(
      profile, modules.automation.profile ? &*modules.automation.profile
                                          : nullptr);
  uses_raw_keyboard = demand.keyboard;
  uses_raw_mouse = demand.mouse;
  uses_controller = demand.controller;
  uses_hid = demand.hid;
}

void Session::apply_profile(const layerc::core::GameProfile &updated,
                            const bool capture_exclusion,
                            const layerc::core::TimePoint now) {
  const auto plan = layerc::core::plan_session_reconciliation(profile, updated);
  cooldowns.reconfigure(updated.cooldowns, plan.reset_cooldown_ids,
                        now, updated.cooldown_groups);
  profile = updated;
  if (plan.rebuild_input_matcher) {
    input.replace_bindings(profile.bindings);
    rebuild_input_configuration();
  }
  snapshot_buffer.clear();
  snapshot_buffer.reserve(profile.cooldowns.size());

  if (!plan.rebuild_overlays)
    return;
  if (!overlays.empty())
    overlays.rebuild(profile, capture_exclusion);
}

void Session::set_capture_exclusion(const bool enabled) {
  overlays.set_capture_exclusion(enabled);
}

void Session::clear_input_state() {
  input.clear();
  for (auto &key : global_keyboard) {
    key.pressed = false;
  }
}

namespace {

// Loads every sidecar module, plus persisted automation state, for a source
// whose path and base profile are already set.
void load_source_modules(ProfileSource &source) {
  source.modules.visual = module_sidecar_from(
      layerc::platform::load_visual_sidecar(source.path, source.profile));
  source.modules.automation = module_sidecar_from(
      layerc::platform::load_automation_sidecar(source.path, source.profile));
  source.modules.companion = module_sidecar_from(
      layerc::platform::load_companion_sidecar(source.path, source.profile));
}

} // namespace

void DaemonApp::reload_profiles() {
  std::vector<ProfileSource> loaded;
  const auto directory = layerc::platform::profile_directory();
  std::error_code error;
  if (!directory.empty()) {
    std::filesystem::create_directories(directory, error);
  }
  if (!error && !directory.empty()) {
    // Canonical enumeration shared with the Linux daemon and layerctl: the
    // same 1024-entry cap, case-folded .json gate, regular-file (symlinks
    // skipped) gate, and duplicate-id detection. The sidecar-loading pass
    // below stays daemon-specific. A scan error reconciles to an empty set
    // instead of half-loading a corrupt store, matching Linux; per-file
    // warnings ride the scan payload on Linux and are dropped here (the
    // editor surfaces them at the source).
    auto scan = layerc::platform::scan_profile_directory(directory);
    if (scan.error.empty()) {
      for (auto &stored : scan.profiles) {
        ProfileSource source;
        source.key = layerc::platform::canonical_profile_key(stored.path);
        source.path = std::move(stored.path);
        source.profile = std::move(stored.profile);
        source.revision = 0;
        load_source_modules(source);
        loaded.push_back(std::move(source));
      }
    }
  }

  reconcile_profile_sources(std::move(loaded));
  retry_quarantined_visual_captures();
}

void DaemonApp::apply_profile_update(
    std::string key, layerc::core::GameProfile profile,
    const layerc::core::ProfileRevision revision) {
  auto source_path = profile_path_for_key(key);
  if (source_path.empty()) return;
  key = layerc::platform::canonical_profile_key(source_path);
  auto loaded = session_registry_.sources;
  const auto found = std::ranges::find_if(
      loaded, [&](const ProfileSource &source) { return source.key == key; });
  if (found != loaded.end()) source_path = found->path;
  ProfileSource source;
  source.key = std::move(key);
  source.path = source_path;
  source.profile = std::move(profile);
  source.revision = revision;
  load_source_modules(source);
  if (found == loaded.end())
    loaded.push_back(std::move(source));
  else
    *found = std::move(source);
  reconcile_profile_sources(std::move(loaded));
  retry_quarantined_visual_captures();
}

void DaemonApp::reconcile_profile_sources(std::vector<ProfileSource> loaded) {
  loaded = dedupe_sources(std::move(loaded));
  advance_source_revisions(loaded, session_registry_.sources);
  const auto recreate_keys = compute_recreate_keys(loaded, session_registry_.sources);

  const auto should_recreate = [&](const std::string &key) {
    return std::ranges::find(recreate_keys, key) != recreate_keys.end();
  };
  std::erase_if(session_registry_.active_inputs, [&](const Session *session) {
    return should_recreate(session->source_key);
  });
  const auto reconcile_now = layerc::core::MonotonicClock::now();
  std::erase_if(session_registry_.sessions, [&](const std::unique_ptr<Session> &session) {
    const auto source =
        std::ranges::find_if(loaded, [&](const ProfileSource &candidate) {
          return candidate.key == session->source_key;
        });
    if (source == loaded.end() || should_recreate(session->source_key)) {
      automation_runtime_host_.retire_session(*session, reconcile_now);
      return true;
    }
    if (session->profile != source->profile)
      session->apply_profile(source->profile, overlay_runtime_.capture_exclusion, reconcile_now);
    if (session->modules.visual != source->modules.visual)
      session->apply_visual_profile(source->modules.visual,
                                    visual_capture_runtime_.recognition_enabled);
    if (session->modules.automation != source->modules.automation)
      session->apply_automation_profile(source->modules.automation);
    if (session->modules.companion != source->modules.companion)
      session->apply_companion_profile(source->modules.companion);
    return false;
  });
  for (const auto &key : recreate_keys)
    session_registry_.instances.erase_profile(key);

  session_registry_.sources = std::move(loaded);
  // A reconcile is an explicit reload: settled automation state is re-read
  // from its store, unsaved values stay with the owner until written.
  std::vector<std::filesystem::path> automation_paths;
  for (const auto &source : session_registry_.sources) {
    if (!source.modules.automation.usable())
      continue;
    (void)automation_runtime_host_.persistence().attach(
        source.path, *source.modules.automation.profile, reconcile_now, true);
    automation_paths.push_back(source.path);
  }
  automation_runtime_host_.persistence().retain(automation_paths);
  for (const auto &source : session_registry_.sources) {
    if (!source.profile.enabled || !source.profile.game.global)
      continue;
    const bool exists = std::ranges::any_of(
        session_registry_.sessions, [&](const std::unique_ptr<Session> &session) {
          return session->source_key == source.key && session->is_global();
        });
    if (!exists)
      session_registry_.sessions.push_back(std::make_unique<Session>(
          source.to_session_init(visual_capture_runtime_.recognition_enabled)));
  }

  const bool has_enabled_profile = std::any_of(
      session_registry_.sources.begin(), session_registry_.sources.end(),
      [](const ProfileSource &source) { return source.profile.enabled; });
  update_tooltip(has_enabled_profile ? L"LayerD - profiles loaded"
                                     : L"LayerD - no enabled profiles");
  refresh_game_windows();
}

bool DaemonApp::reload_settings_if_requested() {
  if (!settings_reload_event_.valid() ||
      WaitForSingleObject(settings_reload_event_.get(), 0) != WAIT_OBJECT_0) {
    return false;
  }
  ResetEvent(settings_reload_event_.get());
  load_platform_settings();
  for (auto &session : session_registry_.sessions) {
    session->set_capture_exclusion(overlay_runtime_.capture_exclusion);
    if (session->visual_triggers)
      session->visual_triggers->set_visual_enabled(visual_capture_runtime_.recognition_enabled);
  }
  if (live_edit_host_.session != nullptr) {
    live_edit_host_.session->overlays.set_capture_exclusion(overlay_runtime_.capture_exclusion);
  }
  reconcile_runtime_services();
  return true;
}

bool DaemonApp::reload_profiles_if_requested() {
  if (!reload_event_.valid() ||
      WaitForSingleObject(reload_event_.get(), 0) != WAIT_OBJECT_0) {
    return false;
  }
  ResetEvent(reload_event_.get());
  reload_profiles();
  return true;
}

void DaemonApp::refresh_game_windows() {
  using RuntimeKeys = std::vector<layerc::core::RuntimeInstanceKey>;
  RuntimeKeys desired;
  std::pair<DaemonApp *, RuntimeKeys *> context{this, &desired};
  // Only enabled window profiles can match; without one, skip opening every
  // top-level window's process once a second.
  const bool any_window_profile = std::ranges::any_of(
      session_registry_.sources, [](const ProfileSource &source) {
        return source.profile.enabled && !source.profile.game.global;
      });
  if (any_window_profile)
    EnumWindows(
        [](const HWND candidate, const LPARAM parameter) -> BOOL {
          auto &context =
              *reinterpret_cast<std::pair<DaemonApp *, RuntimeKeys *> *>(
                  parameter);
          if (IsIconic(candidate)) {
            return TRUE;
          }
          const auto described =
              layerc::platform::describe_attachable_window(candidate);
          if (!described.has_value()) {
            return TRUE;
          }
          if (described->process_id == GetCurrentProcessId() ||
              layerc::platform::is_layerc_editor_process(
                  described->process_id,
                  context.first->live_edit_host_.channel.peer_process_id())) {
            return TRUE;
          }
          for (const auto &source : context.first->session_registry_.sources) {
            if (!layerc::platform::profile_matches_window(*described,
                                                          source.profile)) {
              continue;
            }
            context.second->push_back(
                {.profile_key = source.key,
                 .window = {reinterpret_cast<std::uintptr_t>(candidate)}});
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
  const auto changes = session_registry_.instances.reconcile(std::move(desired));
  std::erase_if(session_registry_.active_inputs, [&](const Session *session) {
    return !session->is_global() &&
           !session_registry_.instances.contains(
               {.profile_key = session->source_key,
                .window = {reinterpret_cast<std::uintptr_t>(
                    session->game_window)}});
  });
  const auto refresh_now = layerc::core::MonotonicClock::now();
  std::erase_if(session_registry_.sessions, [&](const std::unique_ptr<Session> &session) {
    if (session->is_global())
      return false;
    const bool removed = !session_registry_.instances.contains(
        {.profile_key = session->source_key,
         .window = {reinterpret_cast<std::uintptr_t>(session->game_window)}});
    if (removed)
      automation_runtime_host_.retire_session(*session, refresh_now);
    return removed;
  });

  for (const auto &key : changes.added) {
    const auto source =
        std::find_if(session_registry_.sources.begin(), session_registry_.sources.end(),
                     [&](const ProfileSource &candidate) {
                       return candidate.key == key.profile_key;
                     });
    const auto window = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(key.window.opaque));
    if (source == session_registry_.sources.end() || !IsWindow(window)) {
      continue;
    }
    auto instance = std::make_unique<Session>(
        source->to_session_init(visual_capture_runtime_.recognition_enabled));
    instance->game_window = window;
    session_registry_.sessions.push_back(std::move(instance));
  }

  sync_foreground_state();
}
void DaemonApp::sync_foreground_state() {
  const HWND foreground_root = GetAncestor(GetForegroundWindow(), GA_ROOT);
  auto active_sessions = foreground_sessions();
  HWND active_window = nullptr;
  if (!active_sessions.empty())
    active_window = active_sessions.front()->is_global()
                        ? foreground_root
                        : active_sessions.front()->game_window;
  const bool used_controller = std::any_of(
      session_registry_.active_inputs.begin(), session_registry_.active_inputs.end(),
      [](const Session *session) { return session->uses_controller; });
  const bool uses_controller = std::any_of(
      active_sessions.begin(), active_sessions.end(),
      [](const Session *session) { return session->uses_controller; });
  if (active_window != session_registry_.input_foreground) {
    for (auto &session : session_registry_.sessions) {
      session->clear_input_state();
    }
    session_registry_.input_foreground = active_window;
    if (uses_controller) {
      seed_controller_states();
    } else {
      input_runtime_.reset_controllers();
    }
  } else {
    for (auto *previous : session_registry_.active_inputs) {
      if (std::find(active_sessions.begin(), active_sessions.end(), previous) ==
          active_sessions.end()) {
        previous->clear_input_state();
      }
    }
    if (uses_controller && !used_controller) {
      seed_controller_states();
    } else if (!uses_controller) {
      input_runtime_.reset_controllers();
    }
  }
  session_registry_.active_inputs = std::move(active_sessions);
  assert(session_registry_.invariant_holds());

  reconcile_runtime_services();
}

std::vector<Session *> DaemonApp::foreground_sessions() const {
  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  DWORD foreground_process_id{};
  if (foreground != nullptr)
    GetWindowThreadProcessId(foreground, &foreground_process_id);
  if (layerc::platform::is_layerc_editor_process(
          foreground_process_id,
          live_edit_host_.channel.peer_process_id()))
    return {};
  if (live_edit_host_.session != nullptr &&
      GetAncestor(live_edit_host_.session->target, GA_ROOT) == foreground)
    return {};
  const auto active = session_registry_.instances.foreground(
      {reinterpret_cast<std::uintptr_t>(foreground)});
  std::vector<Session *> matches;
  matches.reserve(active.size());
  for (auto &session : session_registry_.sessions) {
    if (session->is_global())
      continue;
    const auto found =
        std::find_if(active.begin(), active.end(),
                     [&](const layerc::core::RuntimeInstanceKey &key) {
                       return session->source_key == key.profile_key &&
                              layerc::core::WindowId{
                                  reinterpret_cast<std::uintptr_t>(
                                      session->game_window)} == key.window;
                     });
    if (found != active.end())
      matches.push_back(session.get());
  }
  if (!matches.empty())
    return matches;
  for (auto &session : session_registry_.sessions) {
    if (session->is_global())
      matches.push_back(session.get());
  }
  return matches;
}

std::vector<Session *> DaemonApp::current_companion_sessions() {
  const auto foreground = foreground_sessions();
  const HWND foreground_window = GetAncestor(GetForegroundWindow(), GA_ROOT);
  DWORD foreground_process_id{};
  if (foreground_window != nullptr)
    GetWindowThreadProcessId(foreground_window, &foreground_process_id);

  std::vector<Session *> all_sessions;
  all_sessions.reserve(session_registry_.sessions.size());
  for (const auto &session : session_registry_.sessions)
    all_sessions.push_back(session.get());

  const bool gamebar_foreground =
      platform::is_gamebar_overlay_process(foreground_process_id);
  Session *preferred{};
  if (!gamebar_foreground) {
    const auto game = std::ranges::find_if(
        foreground, [](const Session *session) {
          return session != nullptr && !session->is_global();
        });
    if (game != foreground.end())
      session_registry_.last_game_foreground = (*game)->game_window;
  } else {
    const auto game = std::ranges::find(
        all_sessions, session_registry_.last_game_foreground, &Session::game_window);
    if (game != all_sessions.end())
      preferred = *game;
  }
  auto selected =
      select_companion_sessions<Session>(foreground, all_sessions,
                                         gamebar_foreground);
  if (preferred != nullptr)
    std::ranges::rotate(selected, std::ranges::find(selected, preferred));
  companion_host_.set_active_session_count(selected.size());
  return selected;
}
} // namespace layerc::daemon

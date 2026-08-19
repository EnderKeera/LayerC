#include "daemon_app.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/windows.hpp"

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <variant>

namespace layerc::daemon {
namespace {
bool is_layerc_editor_process(const DWORD process_id,
                              const DWORD authenticated_editor_id) {
  if (process_id == 0)
    return false;
  if (authenticated_editor_id != 0 && process_id == authenticated_editor_id)
    return true;

  struct CachedResult {
    bool is_editor{};
    ULONGLONG checked_at{};
  };
  static std::unordered_map<DWORD, CachedResult> cache;
  const ULONGLONG now = GetTickCount64();
  if (const auto cached = cache.find(process_id);
      cached != cache.end() && now - cached->second.checked_at < 2'000U)
    return cached->second.is_editor;

  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr) {
    cache[process_id] = {.is_editor = false, .checked_at = now};
    return false;
  }
  std::array<wchar_t, 32'768> actual_buffer{};
  DWORD actual_length = static_cast<DWORD>(actual_buffer.size());
  const bool queried =
      QueryFullProcessImageNameW(process, 0, actual_buffer.data(),
                                 &actual_length) != FALSE;
  CloseHandle(process);
  if (!queried || actual_length == 0) {
    cache[process_id] = {.is_editor = false, .checked_at = now};
    return false;
  }

  std::array<wchar_t, 32'768> daemon_buffer{};
  const DWORD daemon_length = GetModuleFileNameW(
      nullptr, daemon_buffer.data(), static_cast<DWORD>(daemon_buffer.size()));
  if (daemon_length == 0 || daemon_length >= daemon_buffer.size()) {
    cache[process_id] = {.is_editor = false, .checked_at = now};
    return false;
  }
  const auto expected =
      std::filesystem::path(
          std::wstring_view(daemon_buffer.data(), daemon_length))
          .parent_path() /
      L"layere.exe";
  const auto actual = std::filesystem::path(
      std::wstring_view(actual_buffer.data(), actual_length));
  std::error_code error;
  const bool matches =
      std::filesystem::equivalent(expected, actual, error) && !error;
  if (cache.size() > 128U)
    cache.clear();
  cache[process_id] = {.is_editor = matches, .checked_at = now};
  return matches;
}

std::filesystem::path profile_path_for_key(const std::wstring_view key) {
  if (key.empty())
    return {};
  const std::filesystem::path candidate(key);
  std::error_code error;
  if (std::filesystem::exists(candidate, error) && !error)
    return candidate;
  return {};
}

} // namespace

Session::Session(SessionInit init)
    : source_key(std::move(init.source_key)),
      profile(std::move(init.profile)),
      cooldowns(profile.cooldowns, profile.cooldown_groups),
      input(profile.bindings) {
  apply_visual_profile(std::move(init.visual_profile), init.visual_health,
                       init.visual_enabled);
  apply_automation_profile(std::move(init.automation_profile),
                           init.automation_health);
  apply_companion_profile(std::move(init.companion_profile),
                          init.companion_health);
  snapshot_buffer.reserve(profile.cooldowns.size());
  rebuild_input_configuration();
  (void)init.capture_exclusion;
}

void Session::ensure_overlays(const bool capture_exclusion) {
  overlays.ensure(profile, capture_exclusion);
}

void Session::release_overlays() noexcept { overlays.release(); }

void Session::apply_visual_profile(
    std::optional<layerc::core::VisualProfile> updated_visual_profile,
    const layerc::platform::VisualSidecarState updated_health,
    const bool visual_enabled) {
  visual_profile = std::move(updated_visual_profile);
  visual_health = updated_health;
  last_visual_sequence = 0;
  if (visual_profile.has_value() &&
      visual_health != layerc::platform::VisualSidecarState::invalid) {
    if (visual_triggers)
      visual_triggers->reconfigure(*visual_profile);
    else
      visual_triggers =
          std::make_unique<layerc::core::VisualTriggerEngine>(*visual_profile);
    visual_triggers->set_visual_enabled(visual_enabled);
  } else {
    visual_triggers.reset();
  }
}

void Session::apply_automation_profile(
    std::optional<layerc::core::AutomationProfile> updated,
    const layerc::platform::AutomationSidecarState updated_health) {
  automation_health = updated_health;
  if (updated_health == layerc::platform::AutomationSidecarState::invalid)
    updated.reset();
  if (!updated.has_value())
    automation_runtime.reset();
  automation_profile = std::move(updated);
  rebuild_input_configuration();
}

void Session::apply_companion_profile(
    std::optional<layerc::core::CompanionProfile> updated,
    const layerc::platform::CompanionSidecarState updated_health) {
  companion_health = updated_health;
  if (updated_health == layerc::platform::CompanionSidecarState::invalid)
    updated.reset();
  companion_profile = std::move(updated);
}

bool Session::has_companion_slots() const noexcept {
  return companion_profile.has_value() && !companion_profile->slots.empty() &&
         companion_health != layerc::platform::CompanionSidecarState::invalid;
}

bool Session::has_enabled_automations() const noexcept {
  return automation_profile.has_value() &&
         automation_health !=
             layerc::platform::AutomationSidecarState::invalid &&
         std::ranges::any_of(
             automation_profile->automations, [](const auto &automation) {
               return automation.enabled && !automation.rules.empty();
             });
}
bool Session::has_active_visual_rules() const noexcept {
  return visual_triggers != nullptr && visual_profile.has_value() &&
         visual_health != layerc::platform::VisualSidecarState::invalid &&
         layerc::core::has_enabled_visual_assignments(*visual_profile) &&
         visual_triggers->visual_enabled();
}

void Session::rebuild_input_configuration() {
  uses_controller = false;
  uses_raw_input = false;
  uses_hid = false;
  hid_states.assign(profile.hid_bindings.size(), {});
  global_keyboard.clear();
  for (const auto &binding : profile.bindings) {
    for (const auto &token : binding.chord.tokens) {
      uses_controller = uses_controller ||
                        token.device == layerc::core::InputDevice::controller;
      uses_raw_input = uses_raw_input ||
                       token.device == layerc::core::InputDevice::keyboard ||
                       token.device == layerc::core::InputDevice::mouse;
      uses_hid =
          uses_hid || token.device == layerc::core::InputDevice::generic_hid;
      bool pressed{};
      if (token.device == layerc::core::InputDevice::keyboard) {
        const UINT virtual_key = MapVirtualKeyW(token.code, MAPVK_VSC_TO_VK_EX);
        pressed =
            virtual_key != 0 &&
            (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
        input.seed(token, pressed);
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
          input.seed(token, pressed);
        }
      }
      if (!is_global() || token.device != layerc::core::InputDevice::keyboard)
        continue;
      const auto existing = std::find_if(
          global_keyboard.begin(), global_keyboard.end(),
          [&](const PolledKeyState &key) { return key.token == token; });
      if (existing == global_keyboard.end()) {
        global_keyboard.push_back({.token = token, .pressed = pressed});
      }
    }
  }
  if (automation_profile.has_value()) {
    for (const auto &automation : automation_profile->automations) {
      if (!automation.enabled)
        continue;
      for (const auto &rule : automation.rules) {
        if (rule.event !=
            layerc::core::AutomationEventKind::input_value_changed)
          continue;
        if (rule.source_filter.empty() ||
            rule.source_filter.starts_with("controller:"))
          uses_controller = true;
        if (rule.source_filter.starts_with("hid:") &&
            !profile.hid_bindings.empty())
          uses_hid = true;
      }
    }
  }
}

void Session::apply_profile(const layerc::core::GameProfile &updated,
                            const bool capture_exclusion) {
  const auto plan = layerc::core::plan_session_reconciliation(profile, updated);
  cooldowns.reconfigure(updated.cooldowns, plan.reset_cooldown_ids,
                        layerc::core::MonotonicClock::now(),
                        updated.cooldown_groups);
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

void DaemonApp::load_visual_source(ProfileSource &source) {
  source.visual_profile.reset();
  source.visual_error.clear();
  const auto visual =
      layerc::platform::load_visual_sidecar(source.path, source.profile);
  source.visual_health = visual.state;
  if (visual.state == layerc::platform::VisualSidecarState::valid ||
      visual.state == layerc::platform::VisualSidecarState::recovered_backup) {
    source.visual_profile = visual.profile;
  }
  if (visual.state == layerc::platform::VisualSidecarState::recovered_backup ||
      visual.state == layerc::platform::VisualSidecarState::invalid) {
    source.visual_error = visual.error.message;
  }
}

void DaemonApp::load_automation_source(ProfileSource &source) {
  source.automation_profile.reset();
  source.automation_error.clear();
  source.automation_state.clear();
  source.automation_state_due.reset();
  const auto automation =
      layerc::platform::load_automation_sidecar(source.path, source.profile);
  source.automation_health = automation.state;
  if (automation.state == layerc::platform::AutomationSidecarState::valid ||
      automation.state ==
          layerc::platform::AutomationSidecarState::recovered_backup) {
    source.automation_profile = automation.profile;
    const auto state = layerc::platform::load_automation_state(
        source.path, *source.automation_profile);
    source.automation_state = state.values;
    if (state.error.has_value())
      source.automation_error = state.error->message;
  }
  if (automation.state ==
          layerc::platform::AutomationSidecarState::recovered_backup ||
      automation.state == layerc::platform::AutomationSidecarState::invalid)
    source.automation_error = automation.error.message;
}
void DaemonApp::load_companion_source(ProfileSource &source) {
  source.companion_profile.reset();
  source.companion_error.clear();
  const auto companion =
      layerc::platform::load_companion_sidecar(source.path, source.profile);
  source.companion_health = companion.state;
  if (companion.state == layerc::platform::CompanionSidecarState::valid ||
      companion.state == layerc::platform::CompanionSidecarState::recovered_backup) {
    source.companion_profile = companion.profile;
  }
  if (companion.state == layerc::platform::CompanionSidecarState::recovered_backup ||
      companion.state == layerc::platform::CompanionSidecarState::invalid) {
    source.companion_error = companion.error.message;
  }
}

void DaemonApp::reload_profiles() {
  std::vector<ProfileSource> loaded;
  const auto directory = layerc::platform::profile_directory();
  std::error_code error;
  if (!directory.empty()) {
    std::filesystem::create_directories(directory, error);
  }
  if (!error && !directory.empty()) {
    for (const auto &entry :
         std::filesystem::directory_iterator(directory, error)) {
      if (error || !entry.is_regular_file() ||
          layerc::platform::lower_copy(entry.path().extension().wstring()) !=
              L".json") {
        continue;
      }
      const auto contents = layerc::platform::read_profile_file(entry.path());
      if (!contents.has_value()) {
        continue;
      }
      auto parsed = layerc::core::parse_profile_json(*contents);
      if (auto *profile = std::get_if<layerc::core::GameProfile>(&parsed)) {
        ProfileSource source{
            .key = layerc::platform::canonical_profile_key(entry.path()),
            .path = entry.path(),
            .profile = std::move(*profile),
            .revision = 0};
        load_visual_source(source);
        load_automation_source(source);
        load_companion_source(source);
        loaded.push_back(std::move(source));
      }
    }
  }

  reconcile_profile_sources(std::move(loaded));
  retry_quarantined_visual_helpers();
}

void DaemonApp::apply_profile_update(
    std::wstring key, layerc::core::GameProfile profile,
    const layerc::core::ProfileRevision revision) {
  key = layerc::platform::canonical_profile_key(key);
  auto loaded = profile_sources_;
  const auto found = std::ranges::find_if(
      loaded, [&](const ProfileSource &source) { return source.key == key; });
  auto source_path =
      found == loaded.end() ? profile_path_for_key(key) : found->path;
  if (source_path.empty())
    source_path = std::filesystem::path(key);
  ProfileSource source{.key = std::move(key),
                       .path = source_path,
                       .profile = std::move(profile),
                       .revision = revision};
  load_visual_source(source);
  load_automation_source(source);
  load_companion_source(source);
  if (found == loaded.end())
    loaded.push_back(std::move(source));
  else
    *found = std::move(source);
  reconcile_profile_sources(std::move(loaded));
  retry_quarantined_visual_helpers();
}

void DaemonApp::reconcile_profile_sources(std::vector<ProfileSource> loaded) {
  std::vector<ProfileSource> unique_sources;
  unique_sources.reserve(loaded.size());
  for (auto &source : loaded) {
    source.key = layerc::platform::canonical_profile_key(source.key);
    const auto duplicate = std::ranges::find_if(
        unique_sources, [&](const ProfileSource &candidate) {
          return candidate.key == source.key;
        });
    if (duplicate == unique_sources.end()) {
      unique_sources.push_back(std::move(source));
    } else if (source.revision >= duplicate->revision) {
      *duplicate = std::move(source);
    }
  }
  loaded = std::move(unique_sources);

  std::vector<std::wstring> recreate_keys;
  for (auto &source : loaded) {
    const auto previous = std::ranges::find_if(
        profile_sources_, [&](const ProfileSource &candidate) {
          return candidate.key == source.key;
        });
    if (previous == profile_sources_.end()) {
      recreate_keys.push_back(source.key);
      continue;
    }
    if (source.revision == 0)
      source.revision = source.profile == previous->profile
                            ? previous->revision
                            : previous->revision + 1;
    source.automation_revision =
        (source.revision != previous->revision ||
         source.automation_profile != previous->automation_profile ||
         source.automation_health != previous->automation_health)
            ? previous->automation_revision + 1
            : previous->automation_revision;
    if (source.automation_profile.has_value() &&
        previous->automation_profile.has_value()) {
      bool carried_state{};
      for (const auto &variable : source.automation_profile->variables) {
        if (!variable.persistent)
          continue;
        const auto previous_declaration = std::ranges::find(
            previous->automation_profile->variables, variable.id,
            &layerc::core::AutomationVariable::id);
        const auto value = previous->automation_state.find(variable.id);
        if (previous_declaration ==
                previous->automation_profile->variables.end() ||
            previous_declaration->type != variable.type ||
            value == previous->automation_state.end())
          continue;
        source.automation_state[variable.id] = value->second;
        carried_state = true;
      }
      if (carried_state && previous->automation_state_due.has_value())
        source.automation_state_due = previous->automation_state_due;
    }
    if (layerc::core::plan_session_reconciliation(previous->profile,
                                                  source.profile)
            .recreate_instances)
      recreate_keys.push_back(source.key);
  }
  for (const auto &previous : profile_sources_) {
    if (std::ranges::none_of(loaded, [&](const ProfileSource &source) {
          return source.key == previous.key;
        }))
      recreate_keys.push_back(previous.key);
  }

  const auto should_recreate = [&](const std::wstring &key) {
    return std::ranges::find(recreate_keys, key) != recreate_keys.end();
  };
  std::erase_if(active_input_sessions_, [&](const Session *session) {
    return should_recreate(session->source_key);
  });
  const auto reconcile_now = layerc::core::MonotonicClock::now();
  std::erase_if(sessions_, [&](const std::unique_ptr<Session> &session) {
    const auto source =
        std::ranges::find_if(loaded, [&](const ProfileSource &candidate) {
          return candidate.key == session->source_key;
        });
    if (source == loaded.end() || should_recreate(session->source_key)) {
      retire_automation_session(*session, reconcile_now);
      return true;
    }
    if (session->profile != source->profile)
      session->apply_profile(source->profile, capture_exclusion_);
    if (session->visual_profile != source->visual_profile ||
        session->visual_health != source->visual_health)
      session->apply_visual_profile(source->visual_profile,
                                    source->visual_health,
                                    visual_recognition_enabled_);
    if (session->automation_profile != source->automation_profile ||
        session->automation_health != source->automation_health)
      session->apply_automation_profile(source->automation_profile,
                                        source->automation_health);
    if (session->companion_profile != source->companion_profile ||
        session->companion_health != source->companion_health)
      session->apply_companion_profile(source->companion_profile,
                                       source->companion_health);
    return false;
  });
  for (const auto &key : recreate_keys)
    runtime_instances_.erase_profile(key);

  profile_sources_ = std::move(loaded);
  for (const auto &source : profile_sources_) {
    if (!source.profile.enabled || !source.profile.game.global)
      continue;
    const bool exists = std::ranges::any_of(
        sessions_, [&](const std::unique_ptr<Session> &session) {
          return session->source_key == source.key && session->is_global();
        });
    if (!exists)
      sessions_.push_back(std::make_unique<Session>(SessionInit{
          .profile = source.profile,
          .source_key = source.key,
          .visual_profile = source.visual_profile,
          .visual_health = source.visual_health,
          .automation_profile = source.automation_profile,
          .automation_health = source.automation_health,
          .companion_profile = source.companion_profile,
          .companion_health = source.companion_health,
          .visual_enabled = visual_recognition_enabled_,
          .capture_exclusion = capture_exclusion_}));
  }

  const bool has_enabled_profile = std::any_of(
      profile_sources_.begin(), profile_sources_.end(),
      [](const ProfileSource &source) { return source.profile.enabled; });
  update_tooltip(has_enabled_profile ? L"LayerD - profiles loaded"
                                     : L"LayerD - no enabled profiles");
  refresh_game_windows();
}

bool DaemonApp::reload_settings_if_requested() {
  if (settings_reload_event_ == nullptr ||
      WaitForSingleObject(settings_reload_event_, 0) != WAIT_OBJECT_0) {
    return false;
  }
  ResetEvent(settings_reload_event_);
  capture_exclusion_ = layerc::platform::capture_exclusion_enabled();
  visual_recognition_enabled_ = layerc::platform::visual_recognition_enabled();
  for (auto &session : sessions_) {
    session->set_capture_exclusion(capture_exclusion_);
    if (session->visual_triggers)
      session->visual_triggers->set_visual_enabled(visual_recognition_enabled_);
  }
  if (edit_session_ != nullptr) {
    edit_session_->overlays.set_capture_exclusion(capture_exclusion_);
  }
  reconcile_runtime_services();
  return true;
}

bool DaemonApp::reload_profiles_if_requested() {
  if (reload_event_ == nullptr ||
      WaitForSingleObject(reload_event_, 0) != WAIT_OBJECT_0) {
    return false;
  }
  ResetEvent(reload_event_);
  reload_profiles();
  return true;
}

void DaemonApp::refresh_game_windows() {
  using RuntimeKeys = std::vector<layerc::core::RuntimeInstanceKey>;
  RuntimeKeys desired;
  std::pair<DaemonApp *, RuntimeKeys *> context{this, &desired};
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
            is_layerc_editor_process(
                described->process_id,
                context.first->live_channel_.peer_process_id())) {
          return TRUE;
        }
        for (const auto &source : context.first->profile_sources_) {
          if (!layerc::platform::profile_matches_window(*described,
                                                        source.profile)) {
            continue;
          }
          context.second->push_back(
              {.profile_key = source.key,
               .window = reinterpret_cast<std::uintptr_t>(candidate)});
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));
  const auto changes = runtime_instances_.reconcile(std::move(desired));
  std::erase_if(active_input_sessions_, [&](const Session *session) {
    return !session->is_global() &&
           !runtime_instances_.contains(
               {.profile_key = session->source_key,
                .window =
                    reinterpret_cast<std::uintptr_t>(session->game_window)});
  });
  const auto refresh_now = layerc::core::MonotonicClock::now();
  std::erase_if(sessions_, [&](const std::unique_ptr<Session> &session) {
    if (session->is_global())
      return false;
    const bool removed = !runtime_instances_.contains(
        {.profile_key = session->source_key,
         .window = reinterpret_cast<std::uintptr_t>(session->game_window)});
    if (removed)
      retire_automation_session(*session, refresh_now);
    return removed;
  });

  for (const auto &key : changes.added) {
    const auto source =
        std::find_if(profile_sources_.begin(), profile_sources_.end(),
                     [&](const ProfileSource &candidate) {
                       return candidate.key == key.profile_key;
                     });
    const auto window = reinterpret_cast<HWND>(key.window);
    if (source == profile_sources_.end() || !IsWindow(window)) {
      continue;
    }
    auto instance = std::make_unique<Session>(SessionInit{
        .profile = source->profile,
        .source_key = source->key,
        .visual_profile = source->visual_profile,
        .visual_health = source->visual_health,
        .automation_profile = source->automation_profile,
        .automation_health = source->automation_health,
        .companion_profile = source->companion_profile,
        .companion_health = source->companion_health,
        .visual_enabled = visual_recognition_enabled_,
        .capture_exclusion = capture_exclusion_});
    instance->game_window = window;
    sessions_.push_back(std::move(instance));
  }

  sync_foreground_state();
}
void DaemonApp::sync_foreground_state() {
  auto active_sessions = foreground_sessions();
  const HWND foreground_root = GetAncestor(GetForegroundWindow(), GA_ROOT);
  const HWND active_window = active_sessions.empty()
                                 ? nullptr
                                 : (active_sessions.front()->is_global()
                                        ? foreground_root
                                        : active_sessions.front()->game_window);
  const bool used_controller = std::any_of(
      active_input_sessions_.begin(), active_input_sessions_.end(),
      [](const Session *session) { return session->uses_controller; });
  const bool uses_controller = std::any_of(
      active_sessions.begin(), active_sessions.end(),
      [](const Session *session) { return session->uses_controller; });
  if (active_window != input_foreground_) {
    for (auto &session : sessions_) {
      session->clear_input_state();
    }
    input_foreground_ = active_window;
    if (uses_controller) {
      seed_controller_states();
    } else {
      controller_states_ = {};
    }
  } else {
    for (auto *previous : active_input_sessions_) {
      if (std::find(active_sessions.begin(), active_sessions.end(), previous) ==
          active_sessions.end()) {
        previous->clear_input_state();
      }
    }
    if (uses_controller && !used_controller) {
      seed_controller_states();
    } else if (!uses_controller) {
      controller_states_ = {};
    }
  }
  active_input_sessions_ = std::move(active_sessions);

  reconcile_runtime_services();
}

std::vector<Session *> DaemonApp::foreground_sessions() {
  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  DWORD foreground_process_id{};
  if (foreground != nullptr)
    GetWindowThreadProcessId(foreground, &foreground_process_id);
  if (is_layerc_editor_process(foreground_process_id,
                               live_channel_.peer_process_id()))
    return {};
  if (edit_session_ != nullptr &&
      GetAncestor(edit_session_->target, GA_ROOT) == foreground)
    return {};
  const auto active = runtime_instances_.foreground(
      reinterpret_cast<std::uintptr_t>(foreground));
  std::vector<Session *> matches;
  matches.reserve(active.size());
  for (auto &session : sessions_) {
    if (session->is_global())
      continue;
    const auto found =
        std::find_if(active.begin(), active.end(),
                     [&](const layerc::core::RuntimeInstanceKey &key) {
                       return session->source_key == key.profile_key &&
                              reinterpret_cast<std::uintptr_t>(
                                  session->game_window) == key.window;
                     });
    if (found != active.end())
      matches.push_back(session.get());
  }
  if (!matches.empty())
    return matches;
  for (auto &session : sessions_) {
    if (session->is_global())
      matches.push_back(session.get());
  }
  return matches;
}

} // namespace layerc::daemon

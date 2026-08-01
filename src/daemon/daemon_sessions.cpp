#include "daemon_app.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/windows.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>
#include <variant>

namespace layerc::daemon {
namespace {
std::vector<OverlayCooldown> make_overlay_cooldowns(
    const std::vector<layerc::core::CooldownDefinition> &definitions) {
  std::vector<OverlayCooldown> result;
  result.reserve(definitions.size());
  for (const auto &definition : definitions) {
    result.push_back({.id = definition.id,
                      .name = layerc::platform::utf8_to_wide(definition.name),
                      .duration = definition.duration});
  }
  return result;
}

} // namespace

DaemonApp::Session::Session(layerc::core::GameProfile source_profile,
                            std::wstring source_key_value,
                            const bool capture_exclusion)
    : source_key(std::move(source_key_value)),
      profile(std::move(source_profile)),
      overlay_cooldowns(make_overlay_cooldowns(profile.cooldowns)),
      cooldowns(profile.cooldowns), input(profile.bindings) {
  snapshot_buffer.reserve(profile.cooldowns.size());
  rebuild_input_configuration();
  overlays.reserve(profile.panels.size());
  for (const auto &panel : profile.panels) {
    auto overlay = std::make_unique<OverlayWindow>(panel, overlay_cooldowns);
    overlay->set_capture_exclusion(capture_exclusion);
    overlays.push_back(std::move(overlay));
  }
}

void DaemonApp::Session::rebuild_input_configuration() {
  uses_controller = false;
  global_keyboard.clear();
  for (const auto &binding : profile.bindings) {
    for (const auto &token : binding.chord.tokens) {
      uses_controller = uses_controller ||
                        token.device == layerc::core::InputDevice::controller;
      bool pressed{};
      if (token.device == layerc::core::InputDevice::keyboard) {
        const UINT virtual_key = MapVirtualKeyW(token.code, MAPVK_VSC_TO_VK_EX);
        pressed = virtual_key != 0 &&
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
            pressed = state.Gamepad.bLeftTrigger >=
                      XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
          } else if (token.code == 18) {
            pressed = state.Gamepad.bRightTrigger >=
                      XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
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
}

void DaemonApp::Session::apply_profile(
    const layerc::core::GameProfile &updated, const bool capture_exclusion) {
  const auto plan =
      layerc::core::plan_session_reconciliation(profile, updated);
  cooldowns.reconfigure(updated.cooldowns, plan.reset_cooldown_ids,
                        layerc::core::MonotonicClock::now());
  profile = updated;
  overlay_cooldowns = make_overlay_cooldowns(profile.cooldowns);
  input.replace_bindings(profile.bindings);
  rebuild_input_configuration();
  snapshot_buffer.clear();
  snapshot_buffer.reserve(profile.cooldowns.size());

  std::vector<std::unique_ptr<OverlayWindow>> next;
  next.reserve(profile.panels.size());
  for (const auto &panel : profile.panels) {
    const auto found = std::ranges::find_if(
        overlays, [&](const auto &candidate) {
          return candidate != nullptr && candidate->panel_id() == panel.id;
        });
    std::unique_ptr<OverlayWindow> overlay;
    if (found == overlays.end())
      overlay = std::make_unique<OverlayWindow>(panel, overlay_cooldowns);
    else {
      overlay = std::move(*found);
      overlay->update_snapshot(panel, overlay_cooldowns);
    }
    overlay->set_capture_exclusion(capture_exclusion);
    next.push_back(std::move(overlay));
  }
  overlays = std::move(next);
}

void DaemonApp::Session::set_capture_exclusion(const bool enabled) {
  for (auto &overlay : overlays) {
    overlay->set_capture_exclusion(enabled);
  }
}

void DaemonApp::Session::clear_input_state() {
  input.clear();
  for (auto &key : global_keyboard) {
    key.pressed = false;
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
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
      if (error || !entry.is_regular_file() ||
          layerc::platform::lower_copy(entry.path().extension().wstring()) != L".json") {
        continue;
      }
      const auto contents = layerc::platform::read_profile_file(entry.path());
      if (!contents.has_value()) {
        continue;
      }
      auto parsed = layerc::core::parse_profile_json(*contents);
      if (auto *profile = std::get_if<layerc::core::GameProfile>(&parsed)) {
        loaded.push_back({.key =
                              layerc::platform::canonical_profile_key(
                                  entry.path()),
                          .profile = std::move(*profile),
                          .revision = 0});
      }
    }
  }

  reconcile_profile_sources(std::move(loaded));
}

void DaemonApp::apply_profile_update(
    std::wstring key, layerc::core::GameProfile profile,
    const layerc::core::ProfileRevision revision) {
  key = layerc::platform::canonical_profile_key(key);
  auto loaded = profile_sources_;
  const auto found = std::ranges::find_if(
      loaded, [&](const ProfileSource &source) { return source.key == key; });
  ProfileSource source{.key = std::move(key),
                       .profile = std::move(profile),
                       .revision = revision};
  if (found == loaded.end())
    loaded.push_back(std::move(source));
  else
    *found = std::move(source);
  reconcile_profile_sources(std::move(loaded));
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
  std::erase_if(sessions_, [&](const std::unique_ptr<Session> &session) {
    const auto source = std::ranges::find_if(
        loaded, [&](const ProfileSource &candidate) {
          return candidate.key == session->source_key;
        });
    if (source == loaded.end() || should_recreate(session->source_key))
      return true;
    if (session->profile != source->profile)
      session->apply_profile(source->profile, capture_exclusion_);
    return false;
  });
  for (const auto &key : recreate_keys) runtime_instances_.erase_profile(key);

  profile_sources_ = std::move(loaded);
  for (const auto &source : profile_sources_) {
    if (!source.profile.enabled || !source.profile.game.global) continue;
    const bool exists = std::ranges::any_of(
        sessions_, [&](const std::unique_ptr<Session> &session) {
          return session->source_key == source.key && session->is_global();
        });
    if (!exists)
      sessions_.push_back(std::make_unique<Session>(
          source.profile, source.key, capture_exclusion_));
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
  for (auto &session : sessions_) {
    session->set_capture_exclusion(capture_exclusion_);
  }
  if (edit_session_ != nullptr) {
    for (auto &overlay : edit_session_->overlays)
      overlay->set_capture_exclusion(capture_exclusion_);
  }
  update_overlays();
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
                .window = reinterpret_cast<std::uintptr_t>(session->game_window)});
  });
  std::erase_if(sessions_, [&](const std::unique_ptr<Session> &session) {
    if (session->is_global()) {
      return false;
    }
    return !runtime_instances_.contains(
        {.profile_key = session->source_key,
         .window = reinterpret_cast<std::uintptr_t>(session->game_window)});
  });

  for (const auto &key : changes.added) {
    const auto source = std::find_if(
        profile_sources_.begin(), profile_sources_.end(), [&](const ProfileSource &candidate) {
          return candidate.key == key.profile_key;
        });
    const auto window = reinterpret_cast<HWND>(key.window);
    if (source == profile_sources_.end() || !IsWindow(window)) {
      continue;
    }
    auto instance = std::make_unique<Session>(source->profile, source->key,
                                               capture_exclusion_);
    instance->game_window = window;
    sessions_.push_back(std::move(instance));
  }

  sync_foreground_state();
}
void DaemonApp::sync_foreground_state() {
  auto active_sessions = foreground_sessions();
  const HWND foreground_root = GetAncestor(GetForegroundWindow(), GA_ROOT);
  const HWND active_window =
      active_sessions.empty()
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

  const bool needs_input_polling = std::any_of(
      active_input_sessions_.begin(), active_input_sessions_.end(),
      [](const Session *session) { return session->requires_input_polling(); });
  if (needs_input_polling && !input_timer_running_) {
    SetTimer(window_, input_poll_timer, 16, nullptr);
    input_timer_running_ = true;
  } else if (!needs_input_polling && input_timer_running_) {
    KillTimer(window_, input_poll_timer);
    input_timer_running_ = false;
    controller_states_ = {};
    for (auto &session : sessions_) {
      session->clear_input_state();
    }
  }
  update_overlays();
}

std::vector<DaemonApp::Session *> DaemonApp::foreground_sessions() {
  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  if (edit_session_ != nullptr &&
      GetAncestor(edit_session_->target, GA_ROOT) == foreground)
    return {};
  const auto active = runtime_instances_.foreground(
      reinterpret_cast<std::uintptr_t>(foreground));
  std::vector<Session *> matches;
  matches.reserve(active.size());
  for (auto &session : sessions_) {
    if (session->is_global()) continue;
    const auto found = std::find_if(
        active.begin(), active.end(),
        [&](const layerc::core::RuntimeInstanceKey &key) {
          return session->source_key == key.profile_key &&
                 reinterpret_cast<std::uintptr_t>(session->game_window) ==
                     key.window;
        });
    if (found != active.end()) matches.push_back(session.get());
  }
  if (!matches.empty()) return matches;
  for (auto &session : sessions_) {
    if (session->is_global()) matches.push_back(session.get());
  }
  return matches;
}

} // namespace layerc::daemon

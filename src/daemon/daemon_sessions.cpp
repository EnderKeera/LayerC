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
  for (const auto &binding : profile.bindings) {
    for (const auto &token : binding.chord.tokens) {
      uses_controller = uses_controller ||
                        token.device == layerc::core::InputDevice::controller;
      if (!is_global() || token.device != layerc::core::InputDevice::keyboard) {
        continue;
      }
      const auto existing = std::find_if(
          global_keyboard.begin(), global_keyboard.end(),
          [&](const PolledKeyState &key) { return key.token == token; });
      if (existing == global_keyboard.end()) {
        global_keyboard.push_back({.token = token});
      }
    }
  }

  overlays.reserve(profile.panels.size());
  for (const auto &panel : profile.panels) {
    auto overlay = std::make_unique<OverlayWindow>(panel, overlay_cooldowns);
    overlay->set_capture_exclusion(capture_exclusion);
    overlays.push_back(std::move(overlay));
  }
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
        loaded.push_back({.key = entry.path().lexically_normal().wstring(),
                          .profile = std::move(*profile)});
      }
    }
  }

  profile_sources_ = std::move(loaded);
  active_input_sessions_.clear();
  sessions_.clear();
  runtime_instances_.clear();
  for (const auto &source : profile_sources_) {
    if (source.profile.enabled && source.profile.game.global) {
      sessions_.push_back(std::make_unique<Session>(
          source.profile, source.key, capture_exclusion_));
    }
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

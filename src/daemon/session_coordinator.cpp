#include "session_coordinator.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/settings_store.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/window_query.hpp"
#include "layerc/platform/win32_error.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <utility>

namespace layerc::daemon {
namespace {
std::filesystem::path profile_path_for_key(const std::string_view key) {
  const auto decoded = layerc::core::try_utf8_to_wide(key);
  return decoded.has_value() && !decoded->empty()
             ? std::filesystem::path(*decoded)
             : std::filesystem::path{};
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
        source.modules.get<layerc::core::AutomationModule>() != previous->modules.get<layerc::core::AutomationModule>())
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


namespace {

// Storage, loading, and value-copy conversion use the same descriptor table.
void load_source_modules(ProfileSource &source) {
  source.modules = layerc::platform::module_sidecars(
      layerc::platform::load_profile_modules(source.path, source.profile));
}

} // namespace

void SessionCoordinator::reload_profiles() {
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
  visual_capture_runtime_.retry(true);
}

void SessionCoordinator::apply_profile_update(
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
  visual_capture_runtime_.retry(true);
}

void SessionCoordinator::reconcile_profile_sources(std::vector<ProfileSource> loaded) {
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
    if (session->automation.profile() != source->profile)
      session->apply_profile(source->profile, overlay_runtime_.capture_exclusion(), reconcile_now);
    session->apply_modules(source->modules,
                           visual_capture_runtime_.recognition_enabled());
    return false;
  });
  for (const auto &key : recreate_keys)
    session_registry_.instances.erase_profile(key);

  session_registry_.sources = std::move(loaded);
  // A reconcile is an explicit reload: settled automation state is re-read
  // from its store, unsaved values stay with the owner until written.
  std::vector<std::filesystem::path> automation_paths;
  for (const auto &source : session_registry_.sources) {
    if (!source.modules.get<layerc::core::AutomationModule>().usable())
      continue;
    (void)automation_runtime_host_.persistence().attach(
        source.path, *source.modules.get<layerc::core::AutomationModule>().profile, reconcile_now, true);
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
          source.to_session_init(visual_capture_runtime_.recognition_enabled())));
  }

  const bool has_enabled_profile = std::any_of(
      session_registry_.sources.begin(), session_registry_.sources.end(),
      [](const ProfileSource &source) { return source.profile.enabled; });
  notifications_.update_tooltip(has_enabled_profile ? L"LayerD - profiles loaded"
                                     : L"LayerD - no enabled profiles");
  refresh_game_windows();
}



void SessionCoordinator::refresh_game_windows() {
  using RuntimeKeys = std::vector<layerc::core::RuntimeInstanceKey>;
  RuntimeKeys desired;
  std::pair<SessionCoordinator *, RuntimeKeys *> context{this, &desired};
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
              *reinterpret_cast<std::pair<SessionCoordinator *, RuntimeKeys *> *>(
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
                  context.first->live_edit_host_.peer_process_id())) {
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
        source->to_session_init(visual_capture_runtime_.recognition_enabled()));
    instance->game_window = window;
    session_registry_.sessions.push_back(std::move(instance));
  }

  sync_foreground_state();
}
void SessionCoordinator::sync_foreground_state() {
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
      input_runtime_.seed_controller_states();
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
      input_runtime_.seed_controller_states();
    } else if (!uses_controller) {
      input_runtime_.reset_controllers();
    }
  }
  session_registry_.active_inputs = std::move(active_sessions);
  assert(session_registry_.invariant_holds());

  notifications_.reconcile_runtime_services();
}

std::vector<Session *> SessionCoordinator::foreground_sessions() const {
  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  DWORD foreground_process_id{};
  if (foreground != nullptr)
    GetWindowThreadProcessId(foreground, &foreground_process_id);
  if (layerc::platform::is_layerc_editor_process(
          foreground_process_id,
          live_edit_host_.peer_process_id()))
    return {};
  if (live_edit_host_.edit_session() != nullptr &&
      GetAncestor(live_edit_host_.edit_session()->target, GA_ROOT) == foreground)
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


void SessionCoordinator::load_settings() {
  settings_error_.clear();
  const auto append_error = [&](const std::string_view name,
                                const std::uint32_t error) {
    if (!settings_error_.empty())
      settings_error_ += "; ";
    settings_error_ += name;
    settings_error_ += ": ";
    settings_error_ += layerc::platform::wide_to_utf8(
        layerc::platform::windows_error_text(error));
  };
  // Both flags default to enabled when the setting is absent from the store.
  const auto apply = [&](bool &target, const auto &result,
                         const std::string_view name) {
    if (result.value)
      target = *result.value;
    else if (result.missing())
      target = true;
    else
      append_error(name, result.error);
  };

  bool capture_exclusion = overlay_runtime_.capture_exclusion();
  apply(capture_exclusion, layerc::platform::read_bool_setting(
          layerc::platform::BoolSetting::capture_exclusion),
        "capture exclusion");
  overlay_runtime_.set_capture_exclusion(capture_exclusion);
  bool recognition_enabled = visual_capture_runtime_.recognition_enabled();
  apply(recognition_enabled, layerc::platform::read_bool_setting(
          layerc::platform::BoolSetting::visual_recognition),
        "visual recognition");
  visual_capture_runtime_.set_recognition_enabled(recognition_enabled);
}

void SessionCoordinator::reload_settings() {
  load_settings();
  for (auto &session : session_registry_.sessions) {
    session->set_capture_exclusion(overlay_runtime_.capture_exclusion());
    if (session->automation.visual_triggers())
      session->automation.visual_triggers()->set_visual_enabled(
          visual_capture_runtime_.recognition_enabled());
  }
  live_edit_host_.set_capture_exclusion(overlay_runtime_.capture_exclusion());
  notifications_.reconcile_runtime_services();
}

layerc::runtime::CapabilityDemand SessionCoordinator::runtime_demand() {
  const auto &active = session_registry_.active_inputs;
  const auto companion =
      layerc::gamebar::GameBarTransport::current_process_is_expected_package()
          ? companion_host_.current_sessions()
          : std::vector<Session *>{};
  std::vector<layerc::runtime::SessionCapabilityRequest> requests;
  requests.reserve(session_registry_.sessions.size());
  for (const auto &owned : session_registry_.sessions) {
    const auto *session = owned.get();
    if (session == nullptr)
      continue;
    const bool visual = visual_capture_runtime_.recognition_enabled() && !session->is_global() &&
                        session->game_window != nullptr &&
                        session->has_active_visual_rules();
    requests.push_back({
        .window = {reinterpret_cast<std::uintptr_t>(session->game_window)},
        .active = std::ranges::find(active, session) != active.end(),
        .global = session->is_global(),
        .visual = visual,
        .generic_hid = session->uses_hid,
        .isolated_visual = session->automation.profile().visual_runtime ==
                           layerc::core::VisualRuntime::isolated,
        .automation = session->has_enabled_automations(),
        .companion = std::ranges::find(companion, session) != companion.end(),
        .profile_id = session->automation.profile().id,
    });
  }
  return layerc::runtime::calculate_capability_demand(
      requests, live_edit_host_.edit_session() != nullptr);
}

} // namespace layerc::daemon

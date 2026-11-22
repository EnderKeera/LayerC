#pragma once

#include "test_support.hpp"

#ifdef _WIN32
#include <windows.h>
#include <stdexcept>
#else
#include <sys/stat.h>
#endif

namespace layerc::test {

enum class EditorTestScope { store, autosave, widget };

#ifdef _WIN32
void push_editor_test_daemon(const std::filesystem::path &identity);
void pop_editor_test_daemon() noexcept;
#endif

class EditorTestEnvironment {
public:
  explicit EditorTestEnvironment(
      std::string_view prefix, EditorTestScope scope = EditorTestScope::store,
      std::string_view private_runtime_message = "fixture control namespace is private")
      : temp_(prefix)
#ifdef _WIN32
      , settings_key_(L"Software\\LayerC.EditorTests." + temp_.path().filename().native()),
        profiles_("LAYERC_PROFILE_DIR", temp_.path() / "profiles"),
        settings_("LAYERC_SETTINGS_KEY", settings_key_)
#endif
  {
#ifdef _WIN32
    (void)scope;
    (void)private_runtime_message;
    if (!profiles_.applied() || !settings_.applied())
      throw std::runtime_error("Could not isolate the Windows editor's profile and settings stores");
    std::filesystem::create_directories(temp_.path() / "profiles");
    push_editor_test_daemon(temp_.path().filename());
#else
    const auto &root = temp_.path();
    home_.emplace("HOME", root);
    config_.emplace("XDG_CONFIG_HOME", root / "config");
    data_.emplace("XDG_DATA_HOME", root / "data");
    runtime_.emplace("XDG_RUNTIME_DIR", root / "runtime");
    if (scope == EditorTestScope::widget) {
      cache_.emplace("XDG_CACHE_HOME", root / "cache");
      state_.emplace("XDG_STATE_HOME", root / "state");
      store_.emplace("LAYERC_STORE_ROOT", std::nullopt);
    }
    if (scope != EditorTestScope::store) {
      bus_.emplace("DBUS_SESSION_BUS_ADDRESS", std::nullopt);
      accessibility_.emplace("AT_SPI_BUS_ADDRESS", std::nullopt);
      backend_.emplace("FLTK_BACKEND", std::filesystem::path("x11"));
    }
    profiles_.emplace("LAYERC_PROFILE_DIR", std::nullopt);
    if (scope != EditorTestScope::store) require_x11();
    std::filesystem::create_directories(root / "runtime");
    expect(::chmod((root / "runtime").c_str(), 0700) == 0, private_runtime_message);
#endif
  }

#ifdef _WIN32
  ~EditorTestEnvironment() {
    pop_editor_test_daemon();
    const auto result = RegDeleteTreeW(HKEY_CURRENT_USER, settings_key_.c_str());
    expect(result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND,
           "the temporary Windows settings key was removed");
  }
#endif

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return temp_.path();
  }

private:
  TempDirectory temp_;
#ifdef _WIN32
  std::filesystem::path settings_key_;
  ScopedEnvironment profiles_, settings_;
#else
  std::optional<ScopedEnvironment> home_, config_, data_, runtime_, cache_, state_,
      store_, bus_, accessibility_, backend_, profiles_;
#endif
};

inline void require_editor_window() {
#ifndef _WIN32
  require_x11();
#endif
}

inline void require_fltk_editor_window() {
#ifndef _WIN32
  require_fltk_x11();
#endif
}

} // namespace layerc::test

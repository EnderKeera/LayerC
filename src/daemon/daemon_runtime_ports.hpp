#pragma once

#include "layerc/core/live_edit.hpp"

#include <string>
#include <vector>

namespace layerc::daemon {

// Notifications to the composition root, not access to its state. Services
// own input/capture/edit policy; the root routes reconciliation requests and
// presents tray status. Session coordination can implement the foreground
// notification independently of the message loop.
struct Session;

// Profile publication and game-window reconciliation, independently
// implemented by the session coordinator.
class SessionControl {
public:
  virtual void reload_profiles() = 0;
  virtual void apply_profile_update(std::string key,
                                    layerc::core::GameProfile profile,
                                    layerc::core::ProfileRevision revision) = 0;
  virtual void refresh_game_windows() = 0;

protected:
  ~SessionControl() = default;
};

// Foreground policy is shared through a query, not duplicated by companion
// publication or editor authentication.
class SessionQueries {
public:
  [[nodiscard]] virtual std::vector<Session *> foreground_sessions() const = 0;

protected:
  ~SessionQueries() = default;
};

class RuntimeDiagnosticsSource {
public:
  [[nodiscard]] virtual std::string runtime_diagnostics_json() const = 0;

protected:
  ~RuntimeDiagnosticsSource() = default;
};

class RuntimeNotifications {
public:
  virtual void sync_foreground_state() = 0;
  virtual void reconcile_runtime_services() = 0;
  virtual void update_tooltip(const std::wstring &status) = 0;

protected:
  ~RuntimeNotifications() = default;
};

} // namespace layerc::daemon

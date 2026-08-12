#include "daemon_app.hpp"

#include <algorithm>
#include <array>

namespace layerc::daemon {

class DaemonApp::InputRuntimeService final
    : public layerc::runtime::IRuntimeService {
public:
  explicit InputRuntimeService(DaemonApp &app) : app_(app) {}

  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::input;
  }

  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &) override {
    app_.reconcile_input_runtime();
    return {};
  }

  void quiesce() noexcept override { app_.quiesce_input_runtime(); }

  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    return {.timers = app_.input_timer_running_ ? 1U : 0U};
  }

private:
  DaemonApp &app_;
};

class DaemonApp::LiveEditRuntimeService final
    : public layerc::runtime::IRuntimeService {
public:
  explicit LiveEditRuntimeService(DaemonApp &app) : app_(app) {}

  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::live_edit;
  }

  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &) override {
    return {};
  }

  void quiesce() noexcept override {}

  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    return {.handles = app_.live_package_authenticated_ ? 1U : 0U};
  }

private:
  DaemonApp &app_;
};

class DaemonApp::OverlayRuntimeService final
    : public layerc::runtime::IRuntimeService {
public:
  explicit OverlayRuntimeService(DaemonApp &app) : app_(app) {}

  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::overlay;
  }

  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &) override {
    app_.reconcile_overlay_runtime();
    return {};
  }

  void quiesce() noexcept override { app_.quiesce_overlay_runtime(); }

  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    std::uint32_t objects{};
    std::uint32_t windows{};
    const auto count = [&](const OverlayCollection &overlays) {
      objects += static_cast<std::uint32_t>(overlays.object_count());
      windows += static_cast<std::uint32_t>(overlays.window_count());
    };
    for (const auto &session : app_.sessions_) count(session->overlays);
    if (app_.edit_session_) count(app_.edit_session_->overlays);
    return {.timers = app_.overlay_timer_running_ ? 1U : 0U,
            .windows = windows,
            .objects = objects};
  }

private:
  DaemonApp &app_;
};

class DaemonApp::VisualRuntimeService final
    : public layerc::runtime::IRuntimeService {
public:
  explicit VisualRuntimeService(DaemonApp &app) : app_(app) {}

  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::visual;
  }

  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &) override {
    app_.reconcile_visual_captures();
    return {};
  }

  void quiesce() noexcept override { app_.quiesce_inline_visual_captures(); }

  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    std::uint32_t workers{};
    for (const auto &[window, worker] : app_.visual_captures_) {
      (void)window;
      workers += worker != nullptr ? 1U : 0U;
    }
    return {.threads = workers,
            .handles = app_.visual_snapshot_event_ != nullptr ? 1U : 0U,
            .capture_sessions = workers};
  }

private:
  DaemonApp &app_;
};

class DaemonApp::TemplateRuntimeService final
    : public layerc::runtime::IRuntimeService {
public:
  explicit TemplateRuntimeService(DaemonApp &app) : app_(app) {}
  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::template_evaluation;
  }
  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &) override {
    return {};
  }
  void quiesce() noexcept override {}
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    std::uint32_t assets{};
    for (const auto *session : app_.active_input_sessions_) {
      if (!session || !session->visual_profile) continue;
      for (const auto &state : session->visual_profile->templates)
        assets += static_cast<std::uint32_t>(state.references.size());
    }
    return {.decoded_assets = assets};
  }
private:
  DaemonApp &app_;
};

class DaemonApp::IsolatedVisualRuntimeService final
    : public layerc::runtime::IRuntimeService {
public:
  explicit IsolatedVisualRuntimeService(DaemonApp &app) : app_(app) {}
  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::isolated_visual;
  }
  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &) override {
    app_.reconcile_visual_captures();
    return {};
  }
  void quiesce() noexcept override { app_.quiesce_isolated_visual_captures(); }
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    const auto count = static_cast<std::uint32_t>(
        app_.isolated_visual_captures_.size());
    return {.threads = count,
            .handles = count * 3U +
                       (app_.visual_captures_.empty() &&
                                app_.visual_snapshot_event_ != nullptr
                            ? 1U
                            : 0U),
            .objects = count,
            .capture_sessions = count,
            .queues = count};
  }
private:
  DaemonApp &app_;
};

class DaemonApp::HidRuntimeService final
    : public layerc::runtime::IRuntimeService {
public:
  explicit HidRuntimeService(DaemonApp &app) : app_(app) {}
  [[nodiscard]] layerc::runtime::Capability
  capability() const noexcept override {
    return layerc::runtime::Capability::generic_hid;
  }
  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &) override {
    return app_.reconcile_hid_runtime()
               ? layerc::runtime::ServiceReconcileResult{}
               : layerc::runtime::ServiceReconcileResult{
                     false, "Raw Input rejected a required HID usage"};
  }
  void quiesce() noexcept override { app_.quiesce_hid_runtime(); }
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    return app_.hid_runtime_resources();
  }

private:
  DaemonApp &app_;
};

DaemonApp *DaemonApp::foreground_hook_target_{};

std::unique_ptr<layerc::runtime::IRuntimeService>
DaemonApp::create_input_runtime_service(void *const context) {
  if (context == nullptr) return {};
  return std::make_unique<InputRuntimeService>(*static_cast<DaemonApp *>(context));
}

std::unique_ptr<layerc::runtime::IRuntimeService>
DaemonApp::create_live_edit_runtime_service(void *const context) {
  if (context == nullptr) return {};
  return std::make_unique<LiveEditRuntimeService>(
      *static_cast<DaemonApp *>(context));
}

std::unique_ptr<layerc::runtime::IRuntimeService>
DaemonApp::create_overlay_runtime_service(void *const context) {
  if (context == nullptr) return {};
  return std::make_unique<OverlayRuntimeService>(
      *static_cast<DaemonApp *>(context));
}

std::unique_ptr<layerc::runtime::IRuntimeService>
DaemonApp::create_visual_runtime_service(void *const context) {
  if (context == nullptr) return {};
  return std::make_unique<VisualRuntimeService>(
      *static_cast<DaemonApp *>(context));
}

std::unique_ptr<layerc::runtime::IRuntimeService>
DaemonApp::create_template_runtime_service(void *const context) {
  if (context == nullptr) return {};
  return std::make_unique<TemplateRuntimeService>(
      *static_cast<DaemonApp *>(context));
}

std::unique_ptr<layerc::runtime::IRuntimeService>
DaemonApp::create_isolated_visual_runtime_service(void *const context) {
  if (context == nullptr) return {};
  return std::make_unique<IsolatedVisualRuntimeService>(
      *static_cast<DaemonApp *>(context));
}

std::unique_ptr<layerc::runtime::IRuntimeService>
DaemonApp::create_hid_runtime_service(void *const context) {
  if (context == nullptr) return {};
  return std::make_unique<HidRuntimeService>(
      *static_cast<DaemonApp *>(context));
}

layerc::runtime::CapabilityDemand DaemonApp::runtime_demand() const {
  std::vector<layerc::runtime::SessionCapabilityRequest> requests;
  requests.reserve(active_input_sessions_.size());
  for (const auto *session : active_input_sessions_) {
    if (session == nullptr) continue;
    const bool visual = visual_recognition_enabled_ && !session->is_global() &&
                        session->game_window != nullptr &&
                        session->has_active_visual_rules();
    requests.push_back({
        .window = {reinterpret_cast<std::uintptr_t>(session->game_window)},
        .active = true,
        .global = session->is_global(),
        .visual = visual,
        .templates = visual && session->visual_profile.has_value() &&
                     !session->visual_profile->templates.empty(),
        .generic_hid = session->uses_hid,
        .isolated_visual =
            session->profile.visual_runtime ==
            layerc::core::VisualRuntime::isolated,
    });
  }
  return layerc::runtime::calculate_capability_demand(
      requests, live_package_authenticated_, edit_session_ != nullptr);
}

void DaemonApp::reconcile_runtime_services() {
  const std::array descriptors{
      layerc::runtime::RuntimeServiceDescriptor{
          .capability = layerc::runtime::Capability::input,
          .create = &DaemonApp::create_input_runtime_service,
          .context = this},
      layerc::runtime::RuntimeServiceDescriptor{
          .capability = layerc::runtime::Capability::overlay,
          .create = &DaemonApp::create_overlay_runtime_service,
          .context = this},
      layerc::runtime::RuntimeServiceDescriptor{
          .capability = layerc::runtime::Capability::visual,
          .create = &DaemonApp::create_visual_runtime_service,
          .context = this},
      layerc::runtime::RuntimeServiceDescriptor{
          .capability = layerc::runtime::Capability::template_evaluation,
          .create = &DaemonApp::create_template_runtime_service,
          .context = this},
      layerc::runtime::RuntimeServiceDescriptor{
          .capability = layerc::runtime::Capability::isolated_visual,
          .create = &DaemonApp::create_isolated_visual_runtime_service,
          .context = this},
      layerc::runtime::RuntimeServiceDescriptor{
          .capability = layerc::runtime::Capability::generic_hid,
          .create = &DaemonApp::create_hid_runtime_service,
          .context = this},
      layerc::runtime::RuntimeServiceDescriptor{
          .capability = layerc::runtime::Capability::live_edit,
          .create = &DaemonApp::create_live_edit_runtime_service,
          .context = this}};
  (void)runtime_kernel_.reconcile(runtime_demand(), descriptors);
}


} // namespace layerc::daemon

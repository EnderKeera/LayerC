#include "daemon_app.hpp"

#include <algorithm>
#include <array>

namespace layerc::daemon {
namespace {

// Runtime descriptors carry a void* owner context; every entry below is a
// captureless lambda that casts it back to DaemonApp.
[[nodiscard]] DaemonApp &app_of(void *const context) {
  return *static_cast<DaemonApp *>(context);
}

} // namespace

layerc::runtime::CapabilityDemand DaemonApp::runtime_demand() const {
  std::vector<layerc::runtime::SessionCapabilityRequest> requests;
  requests.reserve(active_input_sessions_.size());
  const bool companion_available =
      layerc::gamebar::GameBarTransport::current_process_is_expected_package();
  for (const auto *session : active_input_sessions_) {
    if (session == nullptr)
      continue;
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
        .isolated_visual = session->profile.visual_runtime ==
                           layerc::core::VisualRuntime::isolated,
    });
  }
  auto demand = layerc::runtime::calculate_capability_demand(
      requests, live_package_authenticated_, edit_session_ != nullptr);
  const auto automation_consumers = static_cast<std::uint32_t>(
      std::ranges::count_if(sessions_, [](const auto &session) {
        return session != nullptr && session->has_enabled_automations();
      }));
  demand.set(layerc::runtime::Capability::automation, automation_consumers);
  demand.set(layerc::runtime::Capability::gamebar,
             companion_available ? static_cast<std::uint32_t>(
                                       current_companion_sessions().size())
                                 : 0U);
  return demand;
}

void DaemonApp::reconcile_runtime_services() {
  using layerc::runtime::Capability;
  using layerc::runtime::RuntimeResourceStats;
  using layerc::runtime::RuntimeServiceDescriptor;
  using layerc::runtime::ServiceReconcileResult;

  const std::array descriptors{
      RuntimeServiceDescriptor{
          .capability = Capability::input,
          .context = this,
          .reconcile = [](void *const ctx) -> ServiceReconcileResult {
            app_of(ctx).reconcile_input_runtime();
            return {};
          },
          .quiesce = [](void *const ctx) noexcept {
            app_of(ctx).quiesce_input_runtime();
          },
          .resources = [](void *const ctx) noexcept -> RuntimeResourceStats {
            return {.timers = app_of(ctx).input_timer_running_ ? 1U : 0U};
          }},
      RuntimeServiceDescriptor{
          .capability = Capability::overlay,
          .context = this,
          .reconcile = [](void *const ctx) -> ServiceReconcileResult {
            app_of(ctx).reconcile_overlay_runtime();
            return {};
          },
          .quiesce = [](void *const ctx) noexcept {
            app_of(ctx).quiesce_overlay_runtime();
          },
          .resources = [](void *const ctx) noexcept -> RuntimeResourceStats {
            const auto &app = app_of(ctx);
            std::uint32_t objects{};
            std::uint32_t windows{};
            const auto count = [&](const OverlayCollection &overlays) {
              objects += static_cast<std::uint32_t>(overlays.object_count());
              windows += static_cast<std::uint32_t>(overlays.window_count());
            };
            for (const auto &session : app.sessions_)
              count(session->overlays);
            if (app.edit_session_)
              count(app.edit_session_->overlays);
            return {.timers = app.overlay_timer_running_ ? 1U : 0U,
                    .windows = windows,
                    .objects = objects};
          }},
      RuntimeServiceDescriptor{
          .capability = Capability::visual,
          .context = this,
          .reconcile = [](void *const ctx) -> ServiceReconcileResult {
            app_of(ctx).reconcile_visual_captures();
            return {};
          },
          .quiesce = [](void *const ctx) noexcept {
            app_of(ctx).quiesce_inline_visual_captures();
          },
          .resources = [](void *const ctx) noexcept -> RuntimeResourceStats {
            const auto &app = app_of(ctx);
            std::uint32_t workers{};
            for (const auto &[window, worker] : app.visual_captures_) {
              (void)window;
              workers += worker != nullptr ? 1U : 0U;
            }
            return {.threads = workers,
                    .handles =
                        app.visual_snapshot_event_ != nullptr ? 1U : 0U,
                    .capture_sessions = workers};
          }},
      RuntimeServiceDescriptor{
          .capability = Capability::template_evaluation,
          .context = this,
          .reconcile = [](void *) -> ServiceReconcileResult {
            return {};
          },
          .quiesce = [](void *) noexcept {},
          .resources = [](void *const ctx) noexcept -> RuntimeResourceStats {
            const auto &app = app_of(ctx);
            std::uint32_t assets{};
            for (const auto *session : app.active_input_sessions_) {
              if (!session || !session->visual_profile)
                continue;
              for (const auto &state : session->visual_profile->templates)
                assets +=
                    static_cast<std::uint32_t>(state.references.size());
            }
            return {.decoded_assets = assets};
          }},
      RuntimeServiceDescriptor{
          .capability = Capability::isolated_visual,
          .context = this,
          .reconcile = [](void *const ctx) -> ServiceReconcileResult {
            app_of(ctx).reconcile_visual_captures();
            return {};
          },
          .quiesce = [](void *const ctx) noexcept {
            app_of(ctx).quiesce_isolated_visual_captures();
          },
          .resources = [](void *const ctx) noexcept -> RuntimeResourceStats {
            const auto &app = app_of(ctx);
            const auto count = static_cast<std::uint32_t>(
                app.isolated_visual_captures_.size());
            // Three handles per helper; plus the shared snapshot event
            // handle, owned here only while no inline capture is running.
            const auto snapshot_handle =
                app.visual_captures_.empty() &&
                        app.visual_snapshot_event_ != nullptr
                    ? 1U
                    : 0U;
            return {.threads = count,
                    .handles = count * 3U + snapshot_handle,
                    .objects = count,
                    .capture_sessions = count,
                    .queues = count};
          }},
      RuntimeServiceDescriptor{
          .capability = Capability::generic_hid,
          .context = this,
          .reconcile = [](void *const ctx) -> ServiceReconcileResult {
            return app_of(ctx).reconcile_hid_runtime()
                       ? ServiceReconcileResult{}
                       : ServiceReconcileResult{
                             false,
                             "Raw Input rejected a required HID usage"};
          },
          .quiesce = [](void *const ctx) noexcept {
            app_of(ctx).quiesce_hid_runtime();
          },
          .resources = [](void *const ctx) noexcept -> RuntimeResourceStats {
            return app_of(ctx).hid_runtime_resources();
          }},
      RuntimeServiceDescriptor{
          .capability = Capability::live_edit,
          .context = this,
          .reconcile = [](void *) -> ServiceReconcileResult {
            return {};
          },
          .quiesce = [](void *) noexcept {},
          .resources = [](void *const ctx) noexcept -> RuntimeResourceStats {
            return {.handles =
                        app_of(ctx).live_package_authenticated_ ? 1U : 0U};
          }},
      RuntimeServiceDescriptor{
          .capability = Capability::automation,
          .context = this,
          .reconcile = [](void *const ctx) -> ServiceReconcileResult {
            app_of(ctx).reconcile_automation_runtime();
            return {};
          },
          .quiesce = [](void *const ctx) noexcept {
            app_of(ctx).quiesce_automation_runtime();
          },
          .resources = [](void *const ctx) noexcept -> RuntimeResourceStats {
            const auto &app = app_of(ctx);
            std::uint32_t runtimes{};
            std::uint64_t bytes{};
            for (const auto &session : app.sessions_) {
              if (!session->automation_runtime)
                continue;
              ++runtimes;
              const auto &snapshot = session->automation_runtime->snapshot();
              bytes +=
                  snapshot.variables.size() * sizeof(layerc::core::AutomationValue);
              bytes += snapshot.timers.size() * sizeof(layerc::core::AutomationTimer);
              bytes += snapshot.overlay_overrides.size() *
                       sizeof(layerc::core::OverlayPropertyOverride);
            }
            return {.timers = app.automation_timer_running_ ? 1U : 0U,
                    .objects = runtimes,
                    .cache_bytes = bytes};
          }},
      RuntimeServiceDescriptor{
          .capability = Capability::gamebar,
          .context = this,
          .reconcile = [](void *const ctx) -> ServiceReconcileResult {
            if (!app_of(ctx).gamebar_transport_.start())
              return {false, "Game Bar transport could not start"};
            return {};
          },
          .quiesce = [](void *const ctx) noexcept {
            app_of(ctx).gamebar_transport_.stop();
          },
          .resources = [](void *const ctx) noexcept -> RuntimeResourceStats {
            const auto &app = app_of(ctx);
            const auto active_companion_sessions = layerc::gamebar::
                GameBarTransport::current_process_is_expected_package()
                    ? static_cast<std::uint32_t>(
                          app.current_companion_sessions().size())
                    : 0U;
            return {.handles = static_cast<std::uint32_t>(
                        app.gamebar_transport_.resource_handle_count()),
                    .objects = active_companion_sessions};
          }}};
  (void)runtime_kernel_.reconcile(runtime_demand(), descriptors);
}

DaemonApp *DaemonApp::foreground_hook_target_{};

} // namespace layerc::daemon

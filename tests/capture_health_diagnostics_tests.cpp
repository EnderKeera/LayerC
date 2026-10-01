#include "capture_diagnostics.hpp"
#include "isolated_visual_protocol.hpp"
#include "visual_capture_worker.hpp"
#include "test_support.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <variant>

void snapshot_capture_health_round_trips() {
  using namespace layerc::daemon;
  VisualCaptureSnapshot source;
  source.sequence = 1;
  source.result.availability = layerc::core::CaptureAvailability::unavailable;
  source.result.diagnostic =
      "WGC: access denied \"target\"; Desktop Duplication: output lost\nretry";
  auto decoded = decode_isolated_snapshot(encode_isolated_snapshot(7, source), 7);
  const auto *value = std::get_if<VisualCaptureSnapshot>(&decoded);
  layerc::test::expect(value != nullptr &&
                          value->result.health() == source.result.health(),
                      "both backend fault reasons should survive the bounded wire");
  source.result.availability = layerc::core::CaptureAvailability::available;
  source.result.backend = layerc::core::CaptureBackendKind::desktop_duplication;
  source.result.diagnostic = "WGC fallback reason: access denied";
  decoded = decode_isolated_snapshot(encode_isolated_snapshot(8, source), 8);
  value = std::get_if<VisualCaptureSnapshot>(&decoded);
  layerc::test::expect(value != nullptr &&
                          value->result.health() == source.result.health(),
                      "available fallback health retains its authoritative reason");
  source.result.diagnostic.clear();
  decoded = decode_isolated_snapshot(encode_isolated_snapshot(9, source), 9);
  value = std::get_if<VisualCaptureSnapshot>(&decoded);
  layerc::test::expect(value != nullptr && value->result.diagnostic.empty(),
                      "recovered capture health replaces the old fault reason");
  source.result.diagnostic.assign(isolated_visual_max_message_bytes, 'x');
  layerc::test::expect(std::holds_alternative<IsolatedProtocolError>(
                          decode_isolated_snapshot(encode_isolated_snapshot(10, source), 10)),
                      "capture reasons remain subject to the complete message bound");
}

void capture_faults_do_not_fault_unrelated_services() {
  using layerc::core::CaptureAvailability;
  using layerc::core::CaptureBackendKind;
  using layerc::core::VisualRuntime;
  using layerc::runtime::Capability;
  using layerc::runtime::ServiceState;
  const auto baseline = [] {
    layerc::runtime::RuntimeDiagnostics diagnostics;
    for (const auto capability : {Capability::input, Capability::automation,
                                  Capability::visual, Capability::isolated_visual}) {
      layerc::runtime::RuntimeServiceStats service;
      service.capability = capability;
      service.state = ServiceState::active;
      diagnostics.services.push_back(service);
    }
    return diagnostics;
  }();
  for (const auto runtime : {VisualRuntime::inline_process, VisualRuntime::isolated}) {
    layerc::daemon::VisualCaptureSnapshot snapshot;
    snapshot.sequence = 1;
    for (const auto availability : {CaptureAvailability::unavailable,
                                    CaptureAvailability::available}) {
      snapshot.result.availability = availability;
      snapshot.result.backend = availability == CaptureAvailability::available
                                    ? CaptureBackendKind::desktop_duplication
                                    : CaptureBackendKind::none;
      snapshot.result.diagnostic = availability == CaptureAvailability::available
                                      ? "WGC fallback reason: access denied"
                                      : "WGC: unavailable; Desktop Duplication: output lost";
      const auto expected = snapshot.result.health();
      if (runtime == VisualRuntime::isolated) {
        const auto decoded = layerc::daemon::decode_isolated_snapshot(
            layerc::daemon::encode_isolated_snapshot(1, snapshot), 1);
        if (!layerc::test::expect(
                std::holds_alternative<layerc::daemon::VisualCaptureSnapshot>(decoded),
                "isolated host health must decode before presentation"))
          return;
        snapshot = std::get<layerc::daemon::VisualCaptureSnapshot>(decoded);
      }
      auto diagnostics = baseline;
      layerc::daemon::append_capture_diagnostics(
          diagnostics, {.window = 73, .runtime = runtime, .health = snapshot.result.health()});
      const auto parsed = layerc::runtime::parse_runtime_diagnostics_json(
          layerc::runtime::serialize_runtime_diagnostics_json(diagnostics));
      const auto *value = std::get_if<layerc::runtime::RuntimeDiagnostics>(&parsed);
      layerc::test::expect(value != nullptr && *value == diagnostics &&
                              value->captures.size() == 1 &&
                              value->captures.front().health == expected,
                          "per-window diagnostics retain inline/isolated capture health");
      const auto capability = runtime == VisualRuntime::isolated
                                  ? Capability::isolated_visual : Capability::visual;
      for (std::size_t index = 0; index < diagnostics.services.size(); ++index) {
        const auto &service = diagnostics.services[index];
        layerc::test::expect(service.state == ServiceState::active && service.fault_count == 0,
                            "backend unavailability does not fault resident services");
        if (service.capability == capability)
          layerc::test::expect(service.last_error.find(expected.diagnostic) != std::string::npos &&
                                  service.last_error.find("73") != std::string::npos,
                              "the existing user-facing summary includes the window and reason");
        else
          layerc::test::expect(service == baseline.services[index],
                              "capture faults leave unrelated input/automation diagnostics untouched");
      }
    }
    // Each runtime snapshot is a value copy, not a sticky fault accumulator.
    auto recovered = baseline;
    snapshot.result.diagnostic.clear();
    layerc::daemon::append_capture_diagnostics(
        recovered, {.window = 73, .runtime = runtime, .health = snapshot.result.health()});
    layerc::test::expect(recovered.captures.front().health.diagnostic.empty() &&
                            recovered.services == baseline.services,
                        "a cleared capture fault is absent from the next diagnostics snapshot");
  }
  auto legacy = layerc::runtime::serialize_runtime_diagnostics_json(baseline);
  const auto field = legacy.find(",\"captures\":[]");
  if (layerc::test::expect(field != std::string::npos, "empty capture rows have a stable wire form")) {
    legacy.erase(field, std::string(",\"captures\":[]").size());
    layerc::test::expect(std::holds_alternative<layerc::runtime::RuntimeDiagnostics>(
                            layerc::runtime::parse_runtime_diagnostics_json(legacy)),
                        "older schema-1 diagnostics without capture rows remain readable");
  }
}

void inline_capture_status_preserves_fault_reason() {
  using namespace std::chrono_literals;
  // An invalid window is a deterministic capture failure, not a hardware or
  // production-access fixture. The resident worker should still be healthy.
  layerc::daemon::VisualCaptureWorker worker(nullptr);
  worker.start();
  const auto deadline = layerc::core::MonotonicClock::now() + 3s;
  while (worker.snapshot().sequence == 0 && layerc::core::MonotonicClock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  const auto snapshot = worker.snapshot();
  const auto status = worker.status();
  worker.stop();
  layerc::test::expect(snapshot.sequence != 0 &&
                          snapshot.result.availability == layerc::core::CaptureAvailability::unavailable &&
                          !snapshot.result.diagnostic.empty() &&
                          status.capture_health == snapshot.result.health(),
                      "inline host status retains the authoritative capture fault");
  layerc::test::expect(status.running && !status.quarantined &&
                          status.fault_count == 0 && status.last_error.empty(),
                      "capture failure does not become a host/process fault");
}

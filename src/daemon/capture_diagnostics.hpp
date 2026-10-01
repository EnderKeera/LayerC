#pragma once

#include "layerc/runtime/diagnostics.hpp"

#include <utility>

namespace layerc::daemon {

// Copy authoritative health into the per-window snapshot. The existing editor
// summary consumes service last_error; augment that presentation copy only,
// without changing service activity, fault counts, or unrelated capabilities.
inline void append_capture_diagnostics(
    layerc::runtime::RuntimeDiagnostics &diagnostics,
    layerc::runtime::CaptureDiagnostics capture) {
  if (!capture.health.diagnostic.empty()) {
    const auto capability =
        capture.runtime == layerc::core::VisualRuntime::isolated
            ? layerc::runtime::Capability::isolated_visual
            : layerc::runtime::Capability::visual;
    for (auto &service : diagnostics.services) {
      if (service.capability != capability)
        continue;
      if (!service.last_error.empty())
        service.last_error += "; ";
      service.last_error += "capture window " + std::to_string(capture.window) +
                            ": " + capture.health.diagnostic;
      break;
    }
  }
  diagnostics.captures.push_back(std::move(capture));
}

} // namespace layerc::daemon

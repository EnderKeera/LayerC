#pragma once

#include "input_service.hpp"
#include "x11_capture.hpp"

#include "layerc/core/template_matcher.hpp"
#include "layerc/platform/visual_assets.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::daemon {

// One shared X11 capture per active window, sampled only while inline visual
// demand exists. Native X11 state remains outside core/runtime.
class X11VisualService final : public layerc::runtime::IRuntimeService {
public:
  void attach(layerc::platform::X11Display &display) noexcept;
  void refresh(std::span<InputSession> sessions,
               const layerc::runtime::CapabilityDemand &demand);
  [[nodiscard]] std::vector<std::string>
  poll(std::span<InputSession> sessions, layerc::core::TimePoint now);
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_deadline(layerc::core::TimePoint now) const noexcept;

  [[nodiscard]] std::vector<std::string> unavailable() const;
  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::visual;
  }
  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

private:
  struct TemplateEvaluation {
    layerc::core::TemplateState state;
    std::vector<layerc::core::TemplateImage> references;
    bool assets_available{};
  };

  struct Evaluation {
    std::filesystem::path source_path;
    std::string profile_id;
    layerc::core::VisualProfile profile;
    std::vector<TemplateEvaluation> templates;
    layerc::core::TimePoint next_sample{};
  };

  struct Target {
    std::uint64_t window{};
    layerc::core::CaptureQuality quality{layerc::core::CaptureQuality::low};
    std::unique_ptr<layerc::platform::X11CaptureSession> capture;
    std::vector<Evaluation> evaluations;
  };

  layerc::platform::X11Display *display_{};
  std::vector<Target> targets_;
  layerc::core::CpuTemplateMatcher template_matcher_;
  std::vector<std::string> asset_diagnostics_;
  std::vector<std::string> capture_diagnostics_;
  std::string fault_;
};

} // namespace layerc::daemon

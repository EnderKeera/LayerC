#pragma once

#include "input_service.hpp"
#include "x11_capture.hpp"

#include "layerc/core/template_matcher.hpp"
#include "layerc/platform/visual_assets_linux.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::daemon {

// One shared capture per active window, sampled only while inline visual
// demand exists. Native transports remain outside core/runtime.
class X11VisualService final : public layerc::runtime::IRuntimeService {
public:
  struct WorkStats {
    std::size_t asset_checks{};
    std::size_t asset_loads{};
    std::size_t evaluation_builds{};
    std::size_t template_preparations{};

    bool operator==(const WorkStats &) const = default;
  };

  void attach(layerc::platform::X11Display &display) noexcept;
  void use_native(NativeCaptureFactory *factory) noexcept;
  void refresh(std::span<InputSession> sessions,
               const layerc::runtime::CapabilityDemand &demand,
               bool configuration_changed = false,
               std::optional<std::span<const VisualConfiguration>>
                   configurations = std::nullopt);
  [[nodiscard]] std::vector<std::string>
  poll(std::span<InputSession> sessions, layerc::core::TimePoint now);
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_deadline(layerc::core::TimePoint now) const noexcept;

  [[nodiscard]] std::vector<std::string> unavailable() const;
  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;
  [[nodiscard]] WorkStats work_stats() const noexcept;

private:
  struct AssetStamp {
    std::string sha256;
    std::optional<std::string> contents_sha256;

    bool operator==(const AssetStamp &) const = default;
  };

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
    std::vector<AssetStamp> asset_stamps;
    std::vector<std::string> diagnostics;
    layerc::core::TimePoint next_sample{};
    std::uint64_t last_target_window{};
  };

  struct Target {
    std::uint64_t window{};
    layerc::core::CaptureQuality quality{layerc::core::CaptureQuality::low};
    // Transport chosen by create_capture(); the evaluator never branches on it.
    std::unique_ptr<VisualCaptureSession> capture;
    std::optional<std::uint64_t> x11_window;
    std::vector<Evaluation *> evaluations;
  };

  [[nodiscard]] std::unique_ptr<VisualCaptureSession>
  create_capture(const Target &target, std::string_view profile_id, std::uint8_t sample_rate);

  layerc::platform::X11Display *display_{};
  NativeCaptureFactory *native_{};
  std::vector<Target> targets_;
  // Matched-profile configuration survives foreground/capture quiescence.
  std::map<std::filesystem::path, Evaluation> evaluations_;
  layerc::core::CpuTemplateMatcher template_matcher_;
  std::size_t asset_checks_{};
  std::size_t asset_loads_{};
  std::size_t evaluation_builds_{};
  std::vector<std::string> asset_diagnostics_;
  std::vector<std::string> capture_diagnostics_;
  std::string fault_;
};

} // namespace layerc::daemon

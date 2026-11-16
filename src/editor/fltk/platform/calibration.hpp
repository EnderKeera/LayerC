#pragma once

#include "layerc/capture/capture.hpp"
#include "task_list.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace layerc::editor {

// Calibration exposes opaque selection keys, not native window handles.
// All calls run on the frontend loop; native resources are demand-opened.
struct CalibrationTarget {
  std::uint64_t key{};
  std::string label;
};
struct CalibrationScan {
  std::vector<CalibrationTarget> windows;
  std::string warning;
  std::optional<std::string> fault;
};
enum class CalibrationStartState { started, unavailable, identity_changed };
struct CalibrationStartResult {
  CalibrationStartState state{CalibrationStartState::unavailable};
  std::string diagnostic;
};
struct CalibrationSample {
  bool identity_changed{};
  capture::CapturePollResult result;
};

class CalibrationSource {
public:
  CalibrationSource();
  ~CalibrationSource();
  CalibrationSource(const CalibrationSource &) = delete;
  CalibrationSource &operator=(const CalibrationSource &) = delete;

  [[nodiscard]] CalibrationScan scan(const core::GameMatch &match);
  [[nodiscard]] CalibrationStartResult start(std::uint64_t key,
      const core::GameMatch &match, core::CaptureQuality quality);
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] CalibrationSample poll(std::uint64_t key, core::TimePoint now);
  void set_quality(core::CaptureQuality quality) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::editor

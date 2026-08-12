#pragma once

#include "isolated_visual_protocol.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace layerc::daemon {

struct IsolatedVisualStatus {
  bool running{};
  bool quarantined{};
  std::uint32_t fault_count{};
  DWORD helper_process_id{};
  std::string last_error;
};

class IsolatedVisualWorker {
public:
  explicit IsolatedVisualWorker(HWND window, HANDLE snapshot_event = nullptr);
  ~IsolatedVisualWorker();
  IsolatedVisualWorker(const IsolatedVisualWorker &) = delete;
  IsolatedVisualWorker &operator=(const IsolatedVisualWorker &) = delete;

  void start();
  void stop() noexcept;
  void configure(std::vector<VisualEvaluationRequest> requests,
                 layerc::core::CaptureQuality capture_quality);
  void set_excluded_screen_rects(std::span<const RECT> rectangles);
  void maintain();
  void retry();

  [[nodiscard]] VisualCaptureSnapshot snapshot() const;
  [[nodiscard]] IsolatedVisualStatus status() const;
  [[nodiscard]] HWND window() const noexcept { return window_; }

private:
  bool run_helper(std::string &error);
  void run();
  void terminate_owned_helper(DWORD exit_code) noexcept;
  void close_owned_helper_handles() noexcept;

  HWND window_{};
  HANDLE snapshot_event_{}; // non-owning, owned by IsolatedVisualService
  std::atomic_bool stopping_{};
  std::atomic_bool quarantined_{};
  std::atomic_bool running_{};
  std::atomic_uint64_t last_progress_ms_{};
  std::atomic_uint32_t fault_count_{};
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
  std::vector<VisualEvaluationRequest> requests_;
  std::vector<RECT> exclusions_;
  layerc::core::CaptureQuality capture_quality_{
      layerc::core::CaptureQuality::balanced};
  std::uint64_t configuration_revision_{};
  VisualCaptureSnapshot latest_;
  std::string last_error_;

  mutable std::mutex process_mutex_;
  HANDLE job_{};
  HANDLE pipe_{INVALID_HANDLE_VALUE};
  HANDLE process_{};
  DWORD process_id_{};
};

} // namespace layerc::daemon

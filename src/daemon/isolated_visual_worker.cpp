#include "isolated_visual_worker.hpp"

#include "layerc/platform/framed_pipe.hpp"
#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <bcrypt.h>
#include <sddl.h>

namespace layerc::daemon {
namespace {

std::string random_hex() {
  std::array<unsigned char, 24> bytes{};
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    return {};
  constexpr char digits[] = "0123456789abcdef";
  std::string result(bytes.size() * 2U, '0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result[index * 2U] = digits[bytes[index] >> 4U];
    result[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
  }
  return result;
}

std::filesystem::path sibling_path(const wchar_t *filename) {
  std::vector<wchar_t> module(32'768);
  const DWORD length = GetModuleFileNameW(nullptr, module.data(),
                                          static_cast<DWORD>(module.size()));
  if (length == 0 || length >= module.size())
    return {};
  return std::filesystem::path(std::wstring_view(module.data(), length))
             .parent_path() /
         filename;
}

std::wstring quoted(const std::wstring &value) {
  std::wstring output = L"\"";
  std::size_t slashes{};
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++slashes;
      continue;
    }
    if (character == L'\"') {
      output.append(slashes * 2U + 1U, L'\\');
      output.push_back(L'\"');
      slashes = 0;
      continue;
    }
    output.append(slashes, L'\\');
    slashes = 0;
    output.push_back(character);
  }
  output.append(slashes * 2U, L'\\');
  output.push_back(L'\"');
  return output;
}

bool same_rectangles(const std::span<const RECT> left,
                     const std::span<const RECT> right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const RECT &a, const RECT &b) {
                      return a.left == b.left && a.top == b.top &&
                             a.right == b.right && a.bottom == b.bottom;
                    });
}

template <typename NativeHandle>
HANDLE cancellable_thread_handle(const NativeHandle handle) noexcept {
  if constexpr (std::is_pointer_v<NativeHandle>)
    return reinterpret_cast<HANDLE>(handle);
  else
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle));
}

class OwnedHandle {
public:
  OwnedHandle() = default;
  explicit OwnedHandle(const HANDLE value) : value_(value) {}
  ~OwnedHandle() { reset(); }
  OwnedHandle(const OwnedHandle &) = delete;
  OwnedHandle &operator=(const OwnedHandle &) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HANDLE release() noexcept {
    const HANDLE value = value_;
    value_ = nullptr;
    return value;
  }
  void reset(const HANDLE value = nullptr) noexcept {
    if (valid())
      CloseHandle(value_);
    value_ = value;
  }

private:
  HANDLE value_{};
};

class OwnedLocalMemory {
public:
  explicit OwnedLocalMemory(const HLOCAL value) : value_(value) {}
  ~OwnedLocalMemory() {
    if (value_ != nullptr)
      LocalFree(value_);
  }
  OwnedLocalMemory(const OwnedLocalMemory &) = delete;
  OwnedLocalMemory &operator=(const OwnedLocalMemory &) = delete;

private:
  HLOCAL value_{};
};

struct HelperLaunch {
  OwnedHandle pipe;
  OwnedHandle job;
  OwnedHandle process;
  OwnedHandle thread;
  DWORD process_id{};
};

bool prepare_helper_launch(const std::filesystem::path &helper,
                           const std::wstring &pipe_name,
                           const std::string_view nonce, const HWND window,
                           HelperLaunch &launch, std::string &error) {
  PSECURITY_DESCRIPTOR descriptor{};
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor,
          nullptr) == FALSE) {
    error = "could not create helper pipe security descriptor";
    return false;
  }
  const OwnedLocalMemory owned_descriptor(descriptor);
  SECURITY_ATTRIBUTES security{.nLength = sizeof(SECURITY_ATTRIBUTES),
                               .lpSecurityDescriptor = descriptor,
                               .bInheritHandle = FALSE};
  launch.pipe.reset(CreateNamedPipeW(
      pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, static_cast<DWORD>(isolated_visual_max_message_bytes),
      static_cast<DWORD>(isolated_visual_max_message_bytes), 0, &security));
  if (!launch.pipe.valid()) {
    error = "could not create isolated helper pipe";
    return false;
  }

  launch.job.reset(CreateJobObjectW(nullptr, nullptr));
  if (!launch.job.valid()) {
    error = "could not create isolated helper job";
    return false;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                            JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                            JOB_OBJECT_LIMIT_PROCESS_MEMORY;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  limits.ProcessMemoryLimit = 256ULL * 1024ULL * 1024ULL;
  if (SetInformationJobObject(launch.job.get(),
                              JobObjectExtendedLimitInformation, &limits,
                              sizeof(limits)) == FALSE) {
    error = "could not apply isolated helper job limits";
    return false;
  }

  const std::wstring command =
      quoted(helper.wstring()) + L" --pipe=" + quoted(pipe_name) +
      L" --nonce=" + quoted(std::wstring(nonce.begin(), nonce.end())) +
      L" --parent=" + std::to_wstring(GetCurrentProcessId()) + L" --window=" +
      std::to_wstring(reinterpret_cast<std::uintptr_t>(window));
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process_info{};
  if (CreateProcessW(helper.c_str(), mutable_command.data(), nullptr, nullptr,
                     FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                     helper.parent_path().c_str(), &startup,
                     &process_info) == FALSE) {
    error = "could not launch layerw.exe";
    return false;
  }
  launch.process.reset(process_info.hProcess);
  launch.thread.reset(process_info.hThread);
  launch.process_id = process_info.dwProcessId;
  return true;
}

} // namespace

IsolatedHelperProcess::~IsolatedHelperProcess() { close(); }

HANDLE IsolatedHelperProcess::pipe() const noexcept {
  const std::scoped_lock lock(mutex_);
  return pipe_;
}

DWORD IsolatedHelperProcess::process_id() const noexcept {
  const std::scoped_lock lock(mutex_);
  return process_id_;
}

void IsolatedHelperProcess::terminate(const DWORD exit_code) noexcept {
  std::scoped_lock lock(mutex_);
  if (job_ != nullptr)
    (void)TerminateJobObject(job_, exit_code);
  // The process may not have reached AssignProcessToJobObject yet when a
  // foreground transition destroys this worker. Terminate it directly as
  // well so the CREATE_SUSPENDED launch window cannot leak an orphan.
  if (process_ != nullptr) {
    (void)TerminateProcess(process_, exit_code);
    (void)WaitForSingleObject(process_, 2'000);
  }
  if (pipe_ != INVALID_HANDLE_VALUE)
    (void)CancelIoEx(pipe_, nullptr);
}

void IsolatedHelperProcess::close() noexcept {
  const std::scoped_lock lock(mutex_);
  if (pipe_ != INVALID_HANDLE_VALUE) {
    (void)DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  if (process_ != nullptr) {
    CloseHandle(process_);
    process_ = nullptr;
  }
  if (job_ != nullptr) {
    CloseHandle(job_);
    job_ = nullptr;
  }
  process_id_ = 0;
}

bool IsolatedHelperProcess::validate_and_resume(
    const std::filesystem::path &helper, const HANDLE thread,
    const std::atomic_bool &stopping, std::string &error) {
  HANDLE job{};
  HANDLE process{};
  DWORD process_id{};
  {
    const std::scoped_lock lock(mutex_);
    job = job_;
    process = process_;
    process_id = process_id_;
  }
  const bool assigned = AssignProcessToJobObject(job, process) != FALSE;
  const bool trusted_path =
      layerc::platform::process_image_equals(process, helper);
  DWORD current_session{}, helper_session{};
  const bool same_session =
      ProcessIdToSessionId(GetCurrentProcessId(), &current_session) != FALSE &&
      ProcessIdToSessionId(process_id, &helper_session) != FALSE &&
      current_session == helper_session;
  if (stopping.load(std::memory_order_acquire) || !assigned || !trusted_path ||
      !same_session ||
      ResumeThread(thread) == static_cast<DWORD>(-1)) {
    terminate(stopping.load(std::memory_order_acquire) ? ERROR_OPERATION_ABORTED
                                                       : ERROR_ACCESS_DENIED);
    close();
    error = "isolated helper failed package, session, or job validation";
    return false;
  }
  return true;
}

bool IsolatedHelperProcess::connect_and_authenticate(
    const std::string_view nonce, std::string &error) {
  HANDLE pipe{};
  DWORD helper_process_id{};
  {
    const std::scoped_lock lock(mutex_);
    pipe = pipe_;
    helper_process_id = process_id_;
  }
  const BOOL connected = ConnectNamedPipe(pipe, nullptr);
  if (connected == FALSE && GetLastError() != ERROR_PIPE_CONNECTED) {
    error = "layerw did not connect to its owned pipe";
    terminate(ERROR_BROKEN_PIPE);
    return false;
  }

  ULONG client_pid{};
  if (GetNamedPipeClientProcessId(pipe, &client_pid) == FALSE ||
      client_pid != helper_process_id) {
    error = "isolated pipe client PID did not match layerw";
    terminate(ERROR_ACCESS_DENIED);
    return false;
  }

  auto hello = layerc::platform::read_framed_pipe(
      pipe, isolated_visual_max_message_bytes);
  if (!hello ||
      !validate_isolated_hello(hello.payload, nonce, "worker",
                               helper_process_id, error)) {
    if (error.empty())
      error = std::move(hello.error);
    terminate(ERROR_ACCESS_DENIED);
    return false;
  }

  std::string write_error;
  if (!layerc::platform::write_framed_pipe(
          pipe, isolated_hello_json(nonce, "daemon", GetCurrentProcessId()),
          isolated_visual_max_message_bytes, write_error)) {
    error = std::move(write_error);
    terminate(ERROR_BROKEN_PIPE);
    return false;
  }
  return true;
}

bool IsolatedHelperProcess::launch(const HWND window,
                                   const std::atomic_bool &stopping,
                                   std::string &error) {
  close();
  const auto helper = sibling_path(L"layerw.exe");
  const auto nonce = random_hex();
  const auto suffix = random_hex();
  if (helper.empty() || nonce.empty() || suffix.empty()) {
    error = "could not create isolated helper identity";
    return false;
  }
  const std::wstring pipe_name =
      LR"(\\.\pipe\LayerC.VisualWorker.)" +
      std::to_wstring(GetCurrentProcessId()) + L"." +
      std::wstring(suffix.begin(), suffix.end());

  HelperLaunch launch_state;
  if (!prepare_helper_launch(helper, pipe_name, nonce, window, launch_state,
                             error))
    return false;

  // Publish ownership before validation or ResumeThread. stop() can run from
  // the foreground hook between those operations.
  {
    const std::scoped_lock lock(mutex_);
    job_ = launch_state.job.release();
    pipe_ = launch_state.pipe.release();
    process_ = launch_state.process.release();
    process_id_ = launch_state.process_id;
  }
  if (!validate_and_resume(helper, launch_state.thread.get(), stopping, error))
    return false;
  launch_state.thread.reset();

  if (!connect_and_authenticate(nonce, error)) {
    close();
    return false;
  }
  return true;
}

bool IsolatedProtocolClient::apply_requests(
    std::vector<VisualEvaluationRequest> requests,
    const layerc::core::CaptureQuality quality) {
  const std::scoped_lock lock(mutex_);
  if (requests_ == requests && capture_quality_ == quality)
    return false;
  requests_ = std::move(requests);
  capture_quality_ = quality;
  ++configuration_revision_;
  return true;
}

bool IsolatedProtocolClient::apply_exclusions(
    const std::span<const RECT> rectangles) {
  const std::scoped_lock lock(mutex_);
  if (same_rectangles(exclusions_, rectangles))
    return false;
  exclusions_.assign(rectangles.begin(), rectangles.end());
  ++configuration_revision_;
  return true;
}

void IsolatedProtocolClient::clear_snapshot() {
  const std::scoped_lock lock(mutex_);
  latest_ = {};
}

VisualCaptureSnapshot IsolatedProtocolClient::snapshot() const {
  const std::scoped_lock lock(mutex_);
  return latest_;
}

void IsolatedProtocolClient::run(const HANDLE pipe,
                                 const std::atomic_bool &stopping,
                                 std::atomic_uint64_t &last_progress_ms,
                                 const HANDLE snapshot_event,
                                 std::string &error) {
  std::uint64_t applied_revision = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t packet_sequence{};

  while (error.empty() && !stopping.load(std::memory_order_acquire)) {
    IsolatedConfiguration configuration;
    bool configure{};
    {
      const std::scoped_lock lock(mutex_);
      configure = applied_revision != configuration_revision_;
      if (configure) {
        configuration.sequence = configuration_revision_;
        configuration.capture_quality = capture_quality_;
        configuration.requests = requests_;
        configuration.exclusions = exclusions_;
      }
    }
    if (configure) {
      std::string write_error;
      if (!layerc::platform::write_framed_pipe(
              pipe, encode_isolated_configuration(configuration),
              isolated_visual_max_message_bytes, write_error)) {
        error = std::move(write_error);
        break;
      }
      auto reply = layerc::platform::read_framed_pipe(
          pipe, isolated_visual_max_message_bytes);
      if (!reply ||
          !decode_isolated_ack(reply.payload, configuration.sequence, error)) {
        if (error.empty())
          error = std::move(reply.error);
        break;
      }
      applied_revision = configuration.sequence;
      last_progress_ms.store(GetTickCount64(), std::memory_order_release);
    }

    ++packet_sequence;
    std::string write_error;
    if (!layerc::platform::write_framed_pipe(
            pipe, encode_isolated_poll(packet_sequence),
            isolated_visual_max_message_bytes, write_error)) {
      error = std::move(write_error);
      break;
    }
    auto reply = layerc::platform::read_framed_pipe(
        pipe, isolated_visual_max_message_bytes);
    if (!reply) {
      error = std::move(reply.error);
      break;
    }
    auto decoded = decode_isolated_snapshot(reply.payload, packet_sequence);
    if (const auto *protocol_error =
            std::get_if<IsolatedProtocolError>(&decoded)) {
      error = protocol_error->message;
      break;
    }
    auto snapshot = std::move(std::get<VisualCaptureSnapshot>(decoded));
    const auto disposition = snapshot_validator_.validate(
        snapshot, layerc::core::MonotonicClock::now(), error);
    if (disposition == IsolatedSnapshotDisposition::rejected)
      break;
    // A poll may legitimately observe the same bounded capture snapshot. Do
    // not republish duplicate measurements into LayerD's authoritative state.
    if (disposition == IsolatedSnapshotDisposition::accepted) {
      const std::scoped_lock lock(mutex_);
      latest_ = std::move(snapshot);
      if (snapshot_event != nullptr)
        SetEvent(snapshot_event);
    }
    last_progress_ms.store(GetTickCount64(), std::memory_order_release);
  }
}

bool IsolatedVisualWorker::run_helper(std::string &error) {
  if (!helper_.launch(window_, stopping_, error))
    return false;
  client_.run(helper_.pipe(), stopping_, last_progress_ms_, snapshot_event_,
              error);
  helper_.close();
  return error.empty() || stopping_.load(std::memory_order_acquire);
}

IsolatedVisualWorker::IsolatedVisualWorker(const HWND window,
                                           const HANDLE snapshot_event)
    : window_(window), snapshot_event_(snapshot_event) {}

IsolatedVisualWorker::~IsolatedVisualWorker() { stop(); }

void IsolatedVisualWorker::start() {
  if (thread_.joinable())
    return;
  stopping_.store(false, std::memory_order_release);
  quarantined_.store(false, std::memory_order_release);
  last_progress_ms_.store(GetTickCount64(), std::memory_order_release);
  thread_ = std::thread(&IsolatedVisualWorker::run, this);
}

void IsolatedVisualWorker::stop() noexcept {
  stopping_.store(true, std::memory_order_release);
  wake_.notify_all();
  helper_.terminate(ERROR_OPERATION_ABORTED);
  if (thread_.joinable()) {
    (void)CancelSynchronousIo(
        cancellable_thread_handle(thread_.native_handle()));
    thread_.join();
  }
  running_.store(false, std::memory_order_release);
}

void IsolatedVisualWorker::configure(
    std::vector<VisualEvaluationRequest> requests,
    const layerc::core::CaptureQuality capture_quality) {
  if (client_.apply_requests(std::move(requests), capture_quality)) {
    wake_.notify_all();
    if (quarantined_.load(std::memory_order_acquire))
      retry();
  }
}

void IsolatedVisualWorker::set_excluded_screen_rects(
    const std::span<const RECT> rectangles) {
  if (client_.apply_exclusions(rectangles))
    wake_.notify_all();
}

void IsolatedVisualWorker::maintain() {
  if (!running_.load(std::memory_order_acquire) ||
      quarantined_.load(std::memory_order_acquire))
    return;
  const auto last = last_progress_ms_.load(std::memory_order_acquire);
  if (GetTickCount64() - last <= isolated_visual_watchdog_ms)
    return;
  {
    const std::scoped_lock lock(mutex_);
    last_error_ = "isolated visual helper made no progress for two seconds";
  }
  helper_.terminate(ERROR_TIMEOUT);
  if (thread_.joinable())
    (void)CancelSynchronousIo(
        cancellable_thread_handle(thread_.native_handle()));
}

void IsolatedVisualWorker::retry() {
  if (!quarantined_.load(std::memory_order_acquire))
    return;
  if (thread_.joinable())
    thread_.join();
  fault_count_.store(0, std::memory_order_release);
  quarantined_.store(false, std::memory_order_release);
  {
    const std::scoped_lock lock(mutex_);
    last_error_.clear();
  }
  start();
}

VisualCaptureSnapshot IsolatedVisualWorker::snapshot() const {
  return client_.snapshot();
}

VisualCaptureHostStatus IsolatedVisualWorker::status() const {
  VisualCaptureHostStatus status;
  status.running = running_.load(std::memory_order_acquire);
  status.quarantined = quarantined_.load(std::memory_order_acquire);
  status.fault_count = fault_count_.load(std::memory_order_acquire);
  {
    const std::scoped_lock lock(mutex_);
    status.last_error = last_error_;
  }
  status.helper_process_id = helper_.process_id();
  return status;
}

void IsolatedVisualWorker::run() {
  running_.store(true, std::memory_order_release);
  while (!stopping_.load(std::memory_order_acquire)) {
    std::string error;
    if (run_helper(error) || stopping_.load(std::memory_order_acquire))
      break;
    {
      const std::scoped_lock lock(mutex_);
      last_error_ = std::move(error);
    }
    client_.clear_snapshot();
    const auto faults =
        fault_count_.fetch_add(1, std::memory_order_acq_rel) + 1U;
    if (faults >= 2) {
      quarantined_.store(true, std::memory_order_release);
      if (snapshot_event_ != nullptr)
        SetEvent(snapshot_event_);
      break;
    }
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, std::chrono::seconds(1),
                   [&] { return stopping_.load(std::memory_order_acquire); });
    last_progress_ms_.store(GetTickCount64(), std::memory_order_release);
  }
  running_.store(false, std::memory_order_release);
}

} // namespace layerc::daemon

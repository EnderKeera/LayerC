#include "isolated_visual_worker.hpp"

#include "layerc/platform/bounded_helper.hpp"
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

template <typename NativeHandle>
HANDLE cancellable_thread_handle(const NativeHandle handle) noexcept {
  if constexpr (std::is_pointer_v<NativeHandle>)
    return reinterpret_cast<HANDLE>(handle);
  else
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle));
}

using layerc::platform::OwnedHandle;

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

bool prepare_helper_pipe(const std::wstring &pipe_name,
                         OwnedHandle &pipe, std::string &error) {
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
  pipe.reset(CreateNamedPipeW(
      pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, static_cast<DWORD>(isolated_visual_max_message_bytes),
      static_cast<DWORD>(isolated_visual_max_message_bytes), 0, &security));
  if (!pipe.valid()) {
    error = "could not create isolated helper pipe";
    return false;
  }
  return true;
}

} // namespace

IsolatedHelperProcess::~IsolatedHelperProcess() { close(); }

HANDLE IsolatedHelperProcess::pipe() const noexcept {
  const std::scoped_lock lock(mutex_);
  return pipe_.get();
}

DWORD IsolatedHelperProcess::process_id() const noexcept {
  const std::scoped_lock lock(mutex_);
  return child_.process_id;
}

void IsolatedHelperProcess::terminate(const DWORD exit_code) noexcept {
  std::scoped_lock lock(mutex_);
  // Terminates the Job and the process directly: the process may not have
  // reached AssignProcessToJobObject yet when a foreground transition destroys
  // this worker, and the CREATE_SUSPENDED launch window must not leak it.
  child_.terminate(exit_code);
  if (pipe_.valid())
    (void)CancelIoEx(pipe_.get(), nullptr);
}

void IsolatedHelperProcess::close() noexcept {
  const std::scoped_lock lock(mutex_);
  if (pipe_.valid())
    (void)DisconnectNamedPipe(pipe_.get());
  pipe_.reset();
  child_ = {};
}

bool IsolatedHelperProcess::connect_and_authenticate(
    const std::string_view nonce, std::string &error) {
  HANDLE pipe{};
  DWORD helper_process_id{};
  {
    const std::scoped_lock lock(mutex_);
    pipe = pipe_.get();
    helper_process_id = child_.process_id;
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
  if (!hello) {
    error = std::move(hello.error);
    terminate(ERROR_ACCESS_DENIED);
    return false;
  }
  const auto authenticated = validate_isolated_hello(
      hello.payload, nonce, "worker", helper_process_id);
  if (const auto *failure = std::get_if<IsolatedProtocolError>(&authenticated)) {
    error = failure->message;
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
  const auto helper = platform::sibling_image_path(L"layerw.exe");
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

  OwnedHandle launch_pipe;
  if (!prepare_helper_pipe(pipe_name, launch_pipe, error))
    return false;
  const std::wstring command =
      platform::quote_command_argument(helper.wstring()) + L" --pipe=" +
      platform::quote_command_argument(pipe_name) + L" --nonce=" +
      platform::quote_command_argument(
          std::wstring(nonce.begin(), nonce.end())) +
      L" --parent=" + std::to_wstring(GetCurrentProcessId()) + L" --window=" +
      std::to_wstring(reinterpret_cast<std::uintptr_t>(window));
  platform::HelperLaunchOptions options;
  options.creation_flags = CREATE_NO_WINDOW;
  options.job_flags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                      JOB_OBJECT_LIMIT_PROCESS_MEMORY;
  options.active_process_limit = 1;
  options.process_memory_limit = 256ULL * 1024ULL * 1024ULL;
  options.validate_suspended = [&](const HANDLE process, const DWORD id) {
    return !stopping.load(std::memory_order_acquire) &&
           platform::process_image_equals(process, helper) &&
           platform::same_session_as_current_process(id);
  };
  {
    // Serialize launch and ownership publication against stop(). The platform
    // helper assigns and validates the suspended process before resuming it.
    const std::scoped_lock lock(mutex_);
    auto launched = platform::launch_bounded_helper(helper, command, options);
    if (auto *failure = std::get_if<platform::HelperLaunchError>(&launched)) {
      error = failure->step == platform::HelperLaunchStep::create_process
                  ? "could not launch layerw.exe"
                  : "isolated helper failed package, session, or job validation";
      return false;
    }
    child_ = std::move(std::get<platform::BoundedHelper>(launched));
    pipe_ = std::move(launch_pipe);
  }

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

bool IsolatedProtocolClient::exchange_configuration(
    const HANDLE pipe, std::uint64_t &applied_revision,
    std::atomic_uint64_t &last_progress_ms, std::string &error) {
  IsolatedConfiguration configuration;
  {
    const std::scoped_lock lock(mutex_);
    if (applied_revision == configuration_revision_)
      return true;
    configuration.sequence = configuration_revision_;
    configuration.capture_quality = capture_quality_;
    configuration.requests = requests_;
    configuration.exclusions = exclusions_;
  }
  std::string write_error;
  if (!layerc::platform::write_framed_pipe(
          pipe, encode_isolated_configuration(configuration),
          isolated_visual_max_message_bytes, write_error)) {
    error = std::move(write_error);
    return false;
  }
  auto reply = layerc::platform::read_framed_pipe(
      pipe, isolated_visual_max_message_bytes);
  if (!reply) {
    error = std::move(reply.error);
    return false;
  }
  const auto acknowledged =
      decode_isolated_ack(reply.payload, configuration.sequence);
  if (const auto *failure = std::get_if<IsolatedProtocolError>(&acknowledged)) {
    error = failure->message;
    return false;
  }
  applied_revision = configuration.sequence;
  last_progress_ms.store(GetTickCount64(), std::memory_order_release);
  return true;
}

bool IsolatedProtocolClient::poll_snapshot(
    const HANDLE pipe, std::uint64_t &packet_sequence,
    std::atomic_uint64_t &last_progress_ms, const HANDLE snapshot_event,
    std::string &error) {
  ++packet_sequence;
  std::string write_error;
  if (!layerc::platform::write_framed_pipe(
          pipe, encode_isolated_poll(packet_sequence),
          isolated_visual_max_message_bytes, write_error)) {
    error = std::move(write_error);
    return false;
  }
  auto reply = layerc::platform::read_framed_pipe(
      pipe, isolated_visual_max_message_bytes);
  if (!reply) {
    error = std::move(reply.error);
    return false;
  }
  auto decoded = decode_isolated_snapshot(reply.payload, packet_sequence);
  if (const auto *protocol_error =
          std::get_if<IsolatedProtocolError>(&decoded)) {
    error = protocol_error->message;
    return false;
  }
  auto snapshot = std::move(std::get<VisualCaptureSnapshot>(decoded));
  const auto validated = snapshot_validator_.validate(
      snapshot, layerc::core::MonotonicClock::now());
  if (const auto *rejected = std::get_if<IsolatedProtocolError>(&validated)) {
    error = rejected->message;
    return false;
  }
  const auto disposition = std::get<IsolatedSnapshotDisposition>(validated);
  // A poll may legitimately observe the same bounded capture snapshot. Do
  // not republish duplicate measurements into LayerD's authoritative state.
  if (disposition == IsolatedSnapshotDisposition::accepted) {
    const std::scoped_lock lock(mutex_);
    latest_ = std::move(snapshot);
    if (snapshot_event != nullptr)
      SetEvent(snapshot_event);
  }
  last_progress_ms.store(GetTickCount64(), std::memory_order_release);
  return true;
}

void IsolatedProtocolClient::run(const HANDLE pipe,
                                 const std::atomic_bool &stopping,
                                 std::atomic_uint64_t &last_progress_ms,
                                 const HANDLE snapshot_event,
                                 std::string &error) {
  std::uint64_t applied_revision = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t packet_sequence{};
  // A replacement helper restarts its counters: validate it as a new epoch.
  snapshot_validator_.begin_connection();

  while (error.empty() && !stopping.load(std::memory_order_acquire)) {
    if (!exchange_configuration(pipe, applied_revision, last_progress_ms,
                                error))
      break;
    if (!poll_snapshot(pipe, packet_sequence, last_progress_ms, snapshot_event,
                       error))
      break;
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
  status.capture_health = client_.snapshot().result.health();
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

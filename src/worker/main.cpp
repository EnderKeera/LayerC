#include "isolated_visual_protocol.hpp"
#include "visual_capture_worker.hpp"

#include "layerc/platform/framed_pipe.hpp"
#include "layerc/platform/windows.hpp"
#include "layerc/core/json.hpp"

#include <array>
#include <charconv>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <shellapi.h>
#include <windows.h>

namespace {

std::optional<std::wstring> argument_value(const int argc, wchar_t **argv,
                                           const std::wstring_view name) {
  const std::wstring prefix = L"--" + std::wstring(name) + L"=";
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view value(argv[index]);
    if (value.starts_with(prefix))
      return std::wstring(value.substr(prefix.size()));
  }
  return std::nullopt;
}

std::optional<std::string> ascii_hex(const std::wstring &source) {
  std::string result;
  result.reserve(source.size());
  for (const wchar_t value : source) {
    const bool hex = (value >= L'0' && value <= L'9') ||
                     (value >= L'a' && value <= L'f');
    if (!hex) return std::nullopt;
    result.push_back(static_cast<char>(value));
  }
  return result;
}

template <typename Integer>
std::optional<Integer> decimal(const std::wstring &source) {
  std::string narrow;
  narrow.reserve(source.size());
  for (const wchar_t value : source) {
    if (value < L'0' || value > L'9') return std::nullopt;
    narrow.push_back(static_cast<char>(value));
  }
  Integer result{};
  const auto [end, error] = std::from_chars(
      narrow.data(), narrow.data() + narrow.size(), result);
  if (error != std::errc{} || end != narrow.data() + narrow.size())
    return std::nullopt;
  return result;
}

int worker_main(const std::wstring &pipe_name, const std::string &nonce,
                const DWORD parent_process_id, const HWND target_window) {
  if (pipe_name.empty() || nonce.size() != 48 || parent_process_id == 0 ||
      target_window == nullptr || !IsWindow(target_window))
    return 2;
  DWORD target_process_id{};
  GetWindowThreadProcessId(target_window, &target_process_id);
  if (target_process_id == 0 || target_process_id == parent_process_id ||
      target_process_id == GetCurrentProcessId())
    return 3;

  if (WaitNamedPipeW(pipe_name.c_str(), 5'000) == FALSE) return 4;
  const HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return 5;
  ULONG server_pid{};
  if (GetNamedPipeServerProcessId(pipe, &server_pid) == FALSE ||
      server_pid != parent_process_id) {
    CloseHandle(pipe);
    return 6;
  }
  DWORD current_session{}, parent_session{};
  const HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    parent_process_id);
  const bool trusted =
      parent != nullptr &&
      ProcessIdToSessionId(GetCurrentProcessId(), &current_session) != FALSE &&
      ProcessIdToSessionId(parent_process_id, &parent_session) != FALSE &&
      current_session == parent_session &&
      layerc::platform::same_user_as_current_process(parent) &&
      layerc::platform::is_sibling_process_image(parent_process_id,
                                                 L"layerd.exe");
  if (parent != nullptr) CloseHandle(parent);
  if (!trusted) {
    CloseHandle(pipe);
    return 7;
  }

  std::string error;
  if (!layerc::platform::write_framed_pipe(
          pipe,
          layerc::daemon::isolated_hello_json(
              nonce, "worker", GetCurrentProcessId()),
          layerc::daemon::isolated_visual_max_message_bytes, error)) {
    CloseHandle(pipe);
    return 8;
  }
  const auto hello = layerc::platform::read_framed_pipe(
      pipe, layerc::daemon::isolated_visual_max_message_bytes);
  if (!hello || !layerc::daemon::validate_isolated_hello(
                    hello.payload, nonce, "daemon", parent_process_id,
                    error)) {
    CloseHandle(pipe);
    return 9;
  }

  std::unique_ptr<layerc::daemon::VisualCaptureWorker> worker;
  const HANDLE capture_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (capture_event == nullptr) {
    CloseHandle(pipe);
    return 10;
  }
  layerc::core::CaptureQuality quality{};
  std::uint64_t last_configuration_sequence{};
  layerc::daemon::IsolatedSnapshotSequencer snapshot_sequencer;
  while (true) {
    auto packet = layerc::platform::read_framed_pipe(
        pipe, layerc::daemon::isolated_visual_max_message_bytes);
    if (!packet) break;
    const auto parsed = layerc::core::json::parse(packet.payload);
    const auto *root = std::get_if<layerc::core::json::Value>(&parsed);
    const auto *type = root == nullptr ? nullptr : root->find("type");
    if (type == nullptr || !type->is_string()) break;
    if (type->as_string() == "configure") {
      auto decoded =
          layerc::daemon::decode_isolated_configuration(packet.payload);
      const auto *configuration =
          std::get_if<layerc::daemon::IsolatedConfiguration>(&decoded);
      if (configuration == nullptr ||
          configuration->sequence <= last_configuration_sequence)
        break;
      if (configuration->requests.empty()) {
        worker.reset();
        ResetEvent(capture_event);
        snapshot_sequencer.begin_worker_epoch();
      } else {
        if (!worker || quality != configuration->capture_quality) {
          worker.reset();
          ResetEvent(capture_event);
          worker = std::make_unique<layerc::daemon::VisualCaptureWorker>(
              target_window, configuration->capture_quality, capture_event);
          quality = configuration->capture_quality;
          snapshot_sequencer.begin_worker_epoch();
          worker->configure(configuration->requests);
          worker->set_excluded_screen_rects(configuration->exclusions);
          worker->start();
        } else {
          worker->configure(configuration->requests);
          worker->set_excluded_screen_rects(configuration->exclusions);
        }
      }
      last_configuration_sequence = configuration->sequence;
      if (!layerc::platform::write_framed_pipe(
              pipe,
              layerc::daemon::encode_isolated_ack(
                  configuration->sequence),
              layerc::daemon::isolated_visual_max_message_bytes, error))
        break;
      continue;
    }
    if (type->as_string() == "waitSnapshot") {
      std::uint64_t sequence{};
      if (!layerc::daemon::decode_isolated_poll(packet.payload, sequence,
                                                error))
        break;
      const DWORD wait = WaitForSingleObject(
          capture_event, layerc::daemon::isolated_visual_heartbeat_ms);
      if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) break;
      auto snapshot = worker == nullptr
                          ? layerc::daemon::VisualCaptureSnapshot{}
                          : worker->snapshot();
      (void)snapshot_sequencer.rebase(snapshot);
      if (!layerc::platform::write_framed_pipe(
              pipe,
              layerc::daemon::encode_isolated_snapshot(sequence, snapshot),
              layerc::daemon::isolated_visual_max_message_bytes, error))
        break;
      continue;
    }
    break;
  }
  worker.reset();
  CloseHandle(capture_event);
  CloseHandle(pipe);
  return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc{};
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return 1;
  const auto pipe = argument_value(argc, argv, L"pipe");
  const auto nonce = argument_value(argc, argv, L"nonce");
  const auto nonce_ascii = nonce ? ascii_hex(*nonce) : std::nullopt;
  const auto parent_text = argument_value(argc, argv, L"parent");
  const auto window_text = argument_value(argc, argv, L"window");
  const auto parent = parent_text ? decimal<DWORD>(*parent_text) : std::nullopt;
  const auto window_value =
      window_text ? decimal<std::uintptr_t>(*window_text) : std::nullopt;
  const int result = pipe && nonce_ascii && parent && window_value
                          ? worker_main(
                                *pipe, *nonce_ascii, *parent,
                                reinterpret_cast<HWND>(*window_value))
                         : 1;
  LocalFree(argv);
  return result;
}

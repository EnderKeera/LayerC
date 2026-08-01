#include "layerc/core/profile.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/profile_store.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <variant>

#include <windows.h>

namespace {

bool expect(const bool condition, const char *message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}

layerc::core::GameProfile test_profile(const std::string &name) {
  using namespace std::chrono_literals;
  layerc::core::GameProfile profile;
  profile.id = "store-test";
  profile.name = name;
  profile.game.executable_name = "test.exe";
  profile.cooldowns.push_back(
      {.id = "dash", .name = "Dash", .duration = 1s});
  profile.panels.push_back({.id = "main",
                            .name = "Main",
                            .cooldown_ids = {"dash"}});
  profile.bindings.push_back(
      {.cooldown_id = "dash",
       .chord = {{{layerc::core::InputDevice::keyboard, 0x20, 0}}}});
  return profile;
}

} // namespace

int main() {
  bool passed = true;
  wchar_t temporary_root[MAX_PATH]{};
  const DWORD root_length = GetTempPathW(MAX_PATH, temporary_root);
  passed &= expect(root_length != 0 && root_length < MAX_PATH,
                   "temporary directory should be available");
  const auto directory =
      std::filesystem::path(temporary_root) /
      (L"LayerC-platform-test-" + std::to_wstring(GetCurrentProcessId()) +
       L"-" + std::to_wstring(GetTickCount64()));
  std::error_code filesystem_error;
  std::filesystem::create_directory(directory, filesystem_error);
  passed &= expect(!filesystem_error, "test directory should be created");

  const auto path = directory / L"profile.json";
  auto backup = path;
  backup += L".bak";
  const auto first = test_profile("First");
  const auto second = test_profile("Second");
  passed &= expect(layerc::platform::write_profile_file(
                       path, layerc::core::serialize_profile_json(first)),
                   "first atomic profile write should succeed");
  passed &= expect(layerc::platform::write_profile_file(
                       path, layerc::core::serialize_profile_json(second)),
                   "second atomic profile write should succeed");
  const auto backup_contents = layerc::platform::read_profile_file(backup);
  const auto backup_profile =
      backup_contents.has_value()
          ? layerc::core::parse_profile_json(*backup_contents)
          : layerc::core::ProfileResult(layerc::core::ProfileError{});
  const auto *stored = std::get_if<layerc::core::GameProfile>(&backup_profile);
  passed &= expect(stored != nullptr && stored->name == "First",
                   "the .bak file should retain the previous valid profile");

  const auto canonical = layerc::platform::canonical_profile_key(path);
  const auto dotted = directory / L"." / L"profile.json";
  passed &= expect(
      canonical == layerc::platform::canonical_profile_key(dotted),
      "canonical profile keys should collapse lexical aliases");
  std::wstring differently_cased = path.wstring();
  CharUpperBuffW(differently_cased.data(),
                 static_cast<DWORD>(differently_cased.size()));
  passed &= expect(
      canonical ==
          layerc::platform::canonical_profile_key(differently_cased),
      "canonical profile keys should ignore Windows path casing");

  const HANDLE locked_backup = CreateFileW(
      backup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  passed &= expect(locked_backup != INVALID_HANDLE_VALUE,
                   "the backup should be lockable for failure testing");
  const auto third = test_profile("Third");
  if (locked_backup != INVALID_HANDLE_VALUE) {
    passed &= expect(
        !layerc::platform::write_profile_file(
            path, layerc::core::serialize_profile_json(third)),
        "a locked backup replacement should fail the protected write");
    CloseHandle(locked_backup);
  }
  const auto live_after_failure = layerc::platform::read_profile_file(path);
  const auto backup_after_failure = layerc::platform::read_profile_file(backup);
  const auto live_parsed =
      live_after_failure.has_value()
          ? layerc::core::parse_profile_json(*live_after_failure)
          : layerc::core::ProfileResult(layerc::core::ProfileError{});
  const auto backup_parsed =
      backup_after_failure.has_value()
          ? layerc::core::parse_profile_json(*backup_after_failure)
          : layerc::core::ProfileResult(layerc::core::ProfileError{});
  const auto *live_stored =
      std::get_if<layerc::core::GameProfile>(&live_parsed);
  const auto *backup_stored =
      std::get_if<layerc::core::GameProfile>(&backup_parsed);
  passed &= expect(live_stored != nullptr && live_stored->name == "Second",
                   "failed backup protection must leave the live file intact");
  passed &= expect(backup_stored != nullptr &&
                       backup_stored->name == "First",
                   "failed backup protection must preserve the older backup");

  const std::wstring spoof_pipe_name =
      L"\\\\.\\pipe\\LayerC.PlatformSpoofTest." +
      std::to_wstring(GetCurrentProcessId()) + L"." +
      std::to_wstring(GetTickCount64());
  {
    layerc::platform::NamedPipeEditorDaemonChannel spoof_server(
        layerc::platform::NamedPipeEditorDaemonChannel::Role::server,
        spoof_pipe_name);
    layerc::platform::NamedPipeEditorDaemonChannel guarded_client(
        layerc::platform::NamedPipeEditorDaemonChannel::Role::client,
        spoof_pipe_name, GetCurrentProcessId() + 1U);
    for (int attempt = 0; attempt < 50; ++attempt) {
      (void)guarded_client.poll();
      (void)spoof_server.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    passed &= expect(!guarded_client.connected() &&
                         !guarded_client.last_error().empty(),
                     "the client should reject an unexpected pipe server PID");
  }

  const std::wstring pipe_name =
      L"\\\\.\\pipe\\LayerC.PlatformTest." +
      std::to_wstring(GetCurrentProcessId()) + L"." +
      std::to_wstring(GetTickCount64());
  layerc::platform::NamedPipeEditorDaemonChannel server(
      layerc::platform::NamedPipeEditorDaemonChannel::Role::server, pipe_name);
  layerc::platform::NamedPipeEditorDaemonChannel client(
      layerc::platform::NamedPipeEditorDaemonChannel::Role::client, pipe_name,
      GetCurrentProcessId());
  for (int attempt = 0;
       attempt < 1000 && (!server.connected() || !client.connected()); ++attempt) {
    (void)client.poll();
    (void)server.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  passed &= expect(server.connected() && client.connected(),
                   "same-user overlapped pipe peers should connect");
  passed &= expect(client.send({
                       .type = layerc::core::LiveMessageType::handshake,
                       .revision = 77,
                       .text = "platform-test",
                   }),
                   "client should queue a live message");

  bool server_received{};
  for (int attempt = 0; attempt < 1000 && !server_received; ++attempt) {
    (void)client.poll();
    for (auto &result : server.poll()) {
      const auto *message = std::get_if<layerc::core::LiveMessage>(&result);
      server_received = message != nullptr && message->revision == 77 &&
                        message->text == "platform-test";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  passed &= expect(server_received,
                   "framed UTF-8 messages should cross the named pipe");

  (void)DeleteFileW(path.c_str());
  (void)DeleteFileW(backup.c_str());
  auto temporary = path;
  temporary += L".tmp";
  (void)DeleteFileW(temporary.c_str());
  auto backup_temporary = backup;
  backup_temporary += L".tmp";
  (void)DeleteFileW(backup_temporary.c_str());
  (void)RemoveDirectoryW(directory.c_str());
  return passed ? 0 : 1;
}

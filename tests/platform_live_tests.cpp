#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/framed_pipe.hpp"
#include "layerc/platform/hid_device.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets.hpp"
#include "layerc/platform/windows.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <variant>

#include <windows.h>

namespace {

using layerc::test::expect;

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

layerc::core::VisualProfile test_visual_profile() {
  using namespace layerc::core;
  VisualProfile profile;
  profile.profile_id = "store-test";
  profile.rules.push_back({.id = "ready",
                           .name = "Ready",
                           .region = {0.1, 0.2, 0.3, 0.4},
                           .predicate = AverageColorPredicate{0x123456, 10}});
  profile.triggers.push_back({.cooldown_id = "dash",
                              .observation_id = "ready",
                              .mode = VisualTriggerMode::pixel_only});
  return profile;
}

int run_authenticated_peer(const bool server, const std::wstring &pipe_name) {
  using Channel = layerc::platform::NamedPipeEditorDaemonChannel;
  Channel channel(server ? Channel::Role::server : Channel::Role::client,
                  pipe_name);
  bool sent{};
  for (int attempt = 0; attempt < 5'000; ++attempt) {
    for (auto &result : channel.poll()) {
      const auto *message = std::get_if<layerc::core::LiveMessage>(&result);
      if (message == nullptr) continue;
      if (server && message->type ==
                        layerc::core::LiveMessageType::handshake) {
        (void)channel.send({
            .type = layerc::core::LiveMessageType::acknowledgement,
            .revision = message->revision,
            .text = "authenticated",
        });
        sent = true;
      } else if (!server && message->type ==
                               layerc::core::LiveMessageType::acknowledgement &&
                 message->text == "authenticated") {
        return 0;
      }
    }
    if (!server && channel.connected() && !sent) {
      sent = channel.send({.type = layerc::core::LiveMessageType::handshake,
                           .revision = 1,
                           .text = "sibling-auth-test"});
    }
    if (server && sent) {
      // Give the overlapped acknowledgement write time to complete.
      for (int flush = 0; flush < 20; ++flush) {
        (void)channel.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 1;
}

bool launch_auth_peer(const std::filesystem::path &executable,
                      const wchar_t *const mode,
                      const std::wstring &pipe_name,
                      PROCESS_INFORMATION &process) {
  std::wstring command = L"\"" + executable.wstring() + L"\" " + mode +
                         L" \"" + pipe_name + L"\"";
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');
  STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
  return CreateProcessW(executable.c_str(), command_buffer.data(), nullptr,
                        nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process) != FALSE;
}

} // namespace

int main(const int argc, char *argv[]) {
  if (argc == 3 &&
      (std::string_view(argv[1]) == "--auth-server" ||
       std::string_view(argv[1]) == "--auth-client")) {
    const std::string pipe_utf8(argv[2]);
    return run_authenticated_peer(
        std::string_view(argv[1]) == "--auth-server",
        std::wstring(pipe_utf8.begin(), pipe_utf8.end()));
  }
  bool passed = true;
  passed &= expect(std::wstring_view(layerc::platform::live_edit_pipe_name) ==
                       L"\\\\.\\pipe\\LayerC.LayerD.LiveEdit.v2",
                   "the tested production pipe name should match the protocol contract");
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

  HANDLE abandoned_mutex = CreateMutexW(
      nullptr, FALSE, L"Local\\LayerC.ProfileTransaction.v1");
  bool mutex_owned{};
  std::thread abandoned_owner([&] {
    mutex_owned =
        WaitForSingleObject(abandoned_mutex, 5'000) == WAIT_OBJECT_0;
    // Exiting without ReleaseMutex intentionally creates WAIT_ABANDONED.
  });
  abandoned_owner.join();
  passed &= expect(
      mutex_owned &&
          layerc::platform::recover_profile_transactions(directory),
      "transaction recovery should accept and release ownership of an abandoned mutex");
  if (abandoned_mutex != nullptr) CloseHandle(abandoned_mutex);

  const layerc::platform::ProfileFileMutation colliding_mutations[] = {
      {.target = directory / L"Caf\u00c9.json",
       .contents = std::vector<std::byte>{std::byte{'a'}}},
      {.target = directory / L"caf\u00e9.json",
       .contents = std::vector<std::byte>{std::byte{'b'}}},
  };
  passed &= expect(
      !layerc::platform::commit_profile_transaction(
          directory, colliding_mutations),
      "transaction target validation should reject invariant-casefolded Unicode collisions");

  layerc::platform::HidRawDevice system_keyboard{
      .handle = reinterpret_cast<HANDLE>(0x1234),
      .raw_type = RIM_TYPEKEYBOARD};
  RAWINPUT keyboard_input{};
  keyboard_input.header.dwType = RIM_TYPEKEYBOARD;
  keyboard_input.header.hDevice = system_keyboard.handle;
  keyboard_input.data.keyboard.MakeCode = 0x36;
  layerc::core::HidControlBinding system_key{
      .usage_page = 0x07,
      .usage = 0x36,
      .activation = layerc::core::HidActivationMode::button,
      .allow_system_class = true};
  const auto key_down = layerc::platform::sample_hid_control(
      system_keyboard, system_key, keyboard_input);
  keyboard_input.data.keyboard.Flags = RI_KEY_BREAK;
  const auto key_up = layerc::platform::sample_hid_control(
      system_keyboard, system_key, keyboard_input);
  passed &= expect(key_down.has_value() && key_down->button_active &&
                       key_up.has_value() && !key_up->button_active,
                   "Advanced system keyboard bindings should retain device-specific press and release samples");

  layerc::platform::HidRawDevice system_mouse{
      .handle = reinterpret_cast<HANDLE>(0x5678),
      .raw_type = RIM_TYPEMOUSE};
  RAWINPUT mouse_input{};
  mouse_input.header.dwType = RIM_TYPEMOUSE;
  mouse_input.header.hDevice = system_mouse.handle;
  mouse_input.data.mouse.lLastX = 4;
  layerc::core::HidControlBinding relative_mouse{
      .usage_page = 0x01,
      .usage = 0x30,
      .activation = layerc::core::HidActivationMode::relative_positive,
      .allow_system_class = true};
  const auto first_movement = layerc::platform::sample_hid_control(
      system_mouse, relative_mouse, mouse_input);
  const auto second_movement = layerc::platform::sample_hid_control(
      system_mouse, relative_mouse, mouse_input);
  passed &= expect(first_movement.has_value() &&
                       second_movement.has_value() &&
                       first_movement->relative && first_movement->value == 4,
                   "each successive relative system-mouse report should remain an independently available movement sample");

  const std::array<std::byte, 2> report_bytes{std::byte{0x7f},
                                               std::byte{0x20}};
  layerc::platform::HidRawDevice reportless_device{};
  passed &= expect(
      layerc::platform::effective_hid_report_id(reportless_device,
                                                report_bytes) == 0,
      "report-ID-less collections should persist report ID zero instead of the first data byte");
  reportless_device.uses_report_ids = true;
  passed &= expect(
      layerc::platform::effective_hid_report_id(reportless_device,
                                                report_bytes) == 0x7f,
      "collections with report IDs should preserve the leading report ID");
  std::array<wchar_t, 32'768> previous_profile_directory{};
  const DWORD previous_profile_directory_length = GetEnvironmentVariableW(
      L"LAYERC_PROFILE_DIR", previous_profile_directory.data(),
      static_cast<DWORD>(previous_profile_directory.size()));
  const bool had_profile_directory =
      previous_profile_directory_length > 0 &&
      previous_profile_directory_length < previous_profile_directory.size();
  passed &= expect(SetEnvironmentVariableW(L"LAYERC_PROFILE_DIR",
                                           directory.c_str()) != FALSE,
                   "asset tests should use an isolated profile directory");

  layerc::core::TemplateImage asset{
      .width = 8,
      .height = 8,
      .pixels = std::vector<layerc::core::RgbPixel>(64),
      .alpha = std::vector<std::uint8_t>(64, 255)};
  for (std::size_t index = 0; index < asset.pixels.size(); ++index) {
    asset.pixels[index] = {
        static_cast<std::uint8_t>(index * 3U),
        static_cast<std::uint8_t>(255U - index * 2U),
        static_cast<std::uint8_t>(index * 4U)};
  }
  auto flat_asset = asset;
  std::ranges::fill(flat_asset.pixels,
                    layerc::core::RgbPixel{80, 186, 200});
  passed &= expect(
      std::holds_alternative<layerc::platform::VisualAssetError>(
          layerc::platform::store_template_asset(flat_asset)),
      "flat template assets should be rejected before entering content-addressed storage");
  auto stored_asset = layerc::platform::store_template_asset(asset);
  auto *stored_asset_info =
      std::get_if<layerc::platform::ImportedTemplateAsset>(&stored_asset);
  if (const auto *error =
          std::get_if<layerc::platform::VisualAssetError>(&stored_asset))
    std::cerr << "Asset store error: " << error->message << '\n';
  passed &= expect(stored_asset_info != nullptr &&
                       stored_asset_info->reference.asset_sha256.size() == 64 &&
                       std::filesystem::exists(stored_asset_info->path),
                   "template pixels should be sanitized and stored by SHA-256");
  if (stored_asset_info != nullptr) {
    auto loaded = layerc::platform::load_template_asset(
        stored_asset_info->reference.asset_sha256);
    const auto *image = std::get_if<layerc::core::TemplateImage>(&loaded);
    passed &= expect(image != nullptr && image->pixels == asset.pixels &&
                         image->alpha == asset.alpha,
                     "stored template assets should verify and round-trip");
    {
      std::ofstream corrupt(stored_asset_info->path,
                            std::ios::binary | std::ios::trunc);
      corrupt << "not a png";
    }
    passed &= expect(
        std::holds_alternative<layerc::platform::VisualAssetError>(
            layerc::platform::load_template_asset(
                stored_asset_info->reference.asset_sha256)),
        "a replaced template asset must fail content-hash verification");
    stored_asset = layerc::platform::store_template_asset(asset);
    stored_asset_info =
        std::get_if<layerc::platform::ImportedTemplateAsset>(&stored_asset);
    passed &= expect(stored_asset_info != nullptr,
                     "storing known pixels should repair a corrupt asset");
  }

  auto second_asset = asset;
  second_asset.pixels.front().red ^= 0x7FU;
  const auto second_result =
      layerc::platform::store_template_asset(second_asset);
  const auto *second_asset_info =
      std::get_if<layerc::platform::ImportedTemplateAsset>(&second_result);
  if (stored_asset_info != nullptr && second_asset_info != nullptr) {
    auto gc_base = test_profile("Asset GC");
    layerc::core::VisualProfile gc_visual;
    gc_visual.profile_id = gc_base.id;
    gc_visual.templates.push_back(
        {.id = "template",
         .name = "Template",
         .search_region = {0.0, 0.0, 1.0, 1.0},
         .expected_width = 0.25,
         .expected_height = 0.25,
         .references = {stored_asset_info->reference}});
    gc_visual.triggers.push_back(
        {.cooldown_id = "dash", .observation_id = "template"});
    const auto gc_profile_path = directory / L"asset-gc.json";
    const auto stale_asset_temporary =
        layerc::platform::visual_asset_directory() /
        (std::wstring(64, L'd') + L".png.1.1.tmp");
    {
      std::ofstream output(stale_asset_temporary,
                           std::ios::binary | std::ios::trunc);
      output << "stale";
    }
    std::filesystem::last_write_time(
        stale_asset_temporary,
        std::filesystem::file_time_type::clock::now() -
            std::chrono::hours(48),
        filesystem_error);
    passed &= expect(!filesystem_error,
                     "stale asset temporary timestamp should be set");
    passed &= expect(
        layerc::platform::write_profile_bundle(gc_profile_path, gc_base,
                                                gc_visual) &&
            layerc::platform::garbage_collect_unreferenced_visual_assets() &&
            std::filesystem::exists(stored_asset_info->path) &&
            !std::filesystem::exists(second_asset_info->path) &&
            !std::filesystem::exists(stale_asset_temporary),
        "a complete profile scan should retain references and remove orphans and stale asset temporaries");

    auto third_asset = asset;
    third_asset.pixels.back().blue ^= 0x55U;
    const auto third_result =
        layerc::platform::store_template_asset(third_asset);
    const auto *third_asset_info =
        std::get_if<layerc::platform::ImportedTemplateAsset>(&third_result);
    auto stale_backup =
        layerc::platform::visual_sidecar_path(gc_profile_path);
    stale_backup += L".bak";
    if (third_asset_info != nullptr) {
      auto recovery_visual = gc_visual;
      recovery_visual.templates.front().references = {
          third_asset_info->reference};
      std::ofstream output(stale_backup,
                           std::ios::binary | std::ios::trunc);
      output << layerc::core::serialize_visual_profile_json(
          recovery_visual);
    }
    passed &= expect(
        third_asset_info != nullptr &&
            layerc::platform::garbage_collect_unreferenced_visual_assets() &&
            std::filesystem::exists(third_asset_info->path),
        "assets referenced only by a valid recovery sidecar should be retained");

    const auto malformed = directory / L"unreadable.json";
    {
      std::ofstream output(malformed, std::ios::binary | std::ios::trunc);
      output << "{not-json";
    }
    {
      std::ofstream output(stale_backup,
                           std::ios::binary | std::ios::trunc);
      output << "{corrupt-stale-backup";
    }
    passed &= expect(
        third_asset_info != nullptr &&
            layerc::platform::garbage_collect_unreferenced_visual_assets() &&
            !std::filesystem::exists(third_asset_info->path) &&
            std::filesystem::exists(stored_asset_info->path),
        "an unrelated malformed base and corrupt stale backup should not block safe collection");
    std::filesystem::remove(malformed, filesystem_error);
    filesystem_error.clear();

    auto fourth_asset = asset;
    fourth_asset.pixels[1].green ^= 0x33U;
    const auto fourth_result =
        layerc::platform::store_template_asset(fourth_asset);
    const auto *fourth_asset_info =
        std::get_if<layerc::platform::ImportedTemplateAsset>(&fourth_result);
    const auto active_sidecar =
        layerc::platform::visual_sidecar_path(gc_profile_path);
    {
      std::ofstream output(active_sidecar,
                           std::ios::binary | std::ios::trunc);
      output << "{invalid-active-sidecar";
    }
    passed &= expect(
        fourth_asset_info != nullptr &&
            !layerc::platform::garbage_collect_unreferenced_visual_assets() &&
            std::filesystem::exists(fourth_asset_info->path),
        "an invalid active sidecar bundle should conservatively abort collection");
  } else {
    passed = false;
  }

  // Exercise the production image-name policy without touching the fixed
  // production pipe, which may legitimately be in use by the user's daemon.
  std::array<wchar_t, 32'768> own_executable{};
  const DWORD own_length = GetModuleFileNameW(
      nullptr, own_executable.data(),
      static_cast<DWORD>(own_executable.size()));
  const auto sibling_server = directory / L"layerd.exe";
  const auto sibling_client = directory / L"layere.exe";
  const bool copied_peers =
      own_length != 0 && own_length < own_executable.size() &&
      CopyFileW(own_executable.data(), sibling_server.c_str(), FALSE) != FALSE &&
      CopyFileW(own_executable.data(), sibling_client.c_str(), FALSE) != FALSE;
  passed &= expect(copied_peers,
                   "sibling-image authentication test peers should be created");
  if (copied_peers) {
    const std::wstring sibling_pipe =
        L"\\\\.\\pipe\\LayerC.SiblingAuthTest." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    PROCESS_INFORMATION server_process{};
    PROCESS_INFORMATION client_process{};
    const bool server_launched = launch_auth_peer(
        sibling_server, L"--auth-server", sibling_pipe, server_process);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool client_launched = launch_auth_peer(
        sibling_client, L"--auth-client", sibling_pipe, client_process);
    passed &= expect(server_launched && client_launched,
                     "sibling layerd/layere authentication peers should launch");
    if (server_launched && client_launched) {
      const DWORD server_wait = WaitForSingleObject(server_process.hProcess, 10'000);
      const DWORD client_wait = WaitForSingleObject(client_process.hProcess, 10'000);
      DWORD server_exit{1};
      DWORD client_exit{1};
      if (server_wait == WAIT_OBJECT_0)
        (void)GetExitCodeProcess(server_process.hProcess, &server_exit);
      if (client_wait == WAIT_OBJECT_0)
        (void)GetExitCodeProcess(client_process.hProcess, &client_exit);
      passed &= expect(server_wait == WAIT_OBJECT_0 &&
                           client_wait == WAIT_OBJECT_0 && server_exit == 0 &&
                           client_exit == 0,
                       "same-user, same-session sibling images should authenticate bidirectionally");
      if (server_wait != WAIT_OBJECT_0) {
        TerminateProcess(server_process.hProcess, 1);
        WaitForSingleObject(server_process.hProcess, 1'000);
      }
      if (client_wait != WAIT_OBJECT_0) {
        TerminateProcess(client_process.hProcess, 1);
        WaitForSingleObject(client_process.hProcess, 1'000);
      }
    } else {
      if (server_launched) {
        TerminateProcess(server_process.hProcess, 1);
        WaitForSingleObject(server_process.hProcess, 1'000);
      }
      if (client_launched) {
        TerminateProcess(client_process.hProcess, 1);
        WaitForSingleObject(client_process.hProcess, 1'000);
      }
    }
    for (auto *process : {&client_process, &server_process}) {
      if (process->hThread != nullptr) CloseHandle(process->hThread);
      if (process->hProcess != nullptr) CloseHandle(process->hProcess);
    }
  }

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

  const auto expected_sidecar = directory / L"_visual" / L"profile.json";
  passed &= expect(layerc::platform::visual_sidecar_path(path) ==
                       expected_sidecar,
                   "visual sidecars should pair by base profile filename");
  const auto absent_visual =
      layerc::platform::load_visual_sidecar(path, second);
  passed &= expect(absent_visual.state ==
                       layerc::platform::VisualSidecarState::absent,
                   "profiles without a visual sidecar should remain input-only");
  auto visual = test_visual_profile();
  passed &= expect(layerc::platform::write_visual_sidecar(path, visual, second),
                   "the first visual sidecar write should succeed");
  visual.sample_rate_hz = 15;
  passed &= expect(layerc::platform::write_visual_sidecar(path, visual, second),
                   "a replacement visual sidecar should create a protected backup");
  auto visual_backup = expected_sidecar;
  visual_backup += L".bak";
  passed &= expect(std::filesystem::exists(visual_backup),
                   "a valid previous visual sidecar should be backed up");
  auto visual_state = expected_sidecar;
  visual_state += L".state";
  const HANDLE locked_visual_state = CreateFileW(
      visual_state.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  passed &= expect(locked_visual_state != INVALID_HANDLE_VALUE,
                   "the visual state marker should be lockable");
  if (locked_visual_state != INVALID_HANDLE_VALUE) {
    visual.sample_rate_hz = 5;
    passed &= expect(
        !layerc::platform::write_visual_sidecar(path, visual, second),
        "a state-publication failure should fail the visual write");
    CloseHandle(locked_visual_state);
    const auto after_state_failure =
        layerc::platform::load_visual_sidecar(path, second);
    passed &= expect(
        after_state_failure.state ==
                layerc::platform::VisualSidecarState::valid &&
            after_state_failure.profile.has_value() &&
            after_state_failure.profile->sample_rate_hz == 15,
        "a state-publication failure must restore the previous visual and backup");
    visual.sample_rate_hz = 15;
  }
  {
    std::ofstream corrupt(expected_sidecar,
                          std::ios::binary | std::ios::trunc);
    corrupt << "{broken";
  }
  const auto recovered_visual =
      layerc::platform::load_visual_sidecar(path, second);
  passed &= expect(
      recovered_visual.state ==
              layerc::platform::VisualSidecarState::recovered_backup &&
          recovered_visual.profile.has_value() &&
          recovered_visual.profile->sample_rate_hz == 10,
      "an invalid visual sidecar should recover its last valid backup");
  {
    std::ofstream corrupt(visual_backup,
                          std::ios::binary | std::ios::trunc);
    corrupt << "{also-broken";
  }
  const auto invalid_visual =
      layerc::platform::load_visual_sidecar(path, second);
  passed &= expect(invalid_visual.state ==
                       layerc::platform::VisualSidecarState::invalid &&
                       !invalid_visual.error.message.empty(),
                   "two invalid visual copies should produce a blocking diagnostic");
  visual.sample_rate_hz = 5;
  passed &= expect(
      layerc::platform::write_visual_sidecar(path, visual, second),
      "repairing an invalid visual sidecar should replace its stale backup");
  {
    std::ofstream corrupt(expected_sidecar,
                          std::ios::binary | std::ios::trunc);
    corrupt << "{broken-again";
  }
  const auto repaired_backup =
      layerc::platform::load_visual_sidecar(path, second);
  passed &= expect(
      repaired_backup.state ==
              layerc::platform::VisualSidecarState::recovered_backup &&
          repaired_backup.profile.has_value() &&
          repaired_backup.profile->sample_rate_hz == 5,
      "a second corruption should recover the repaired visual definition");

  const auto collision_path = directory / L"collision.json";
  auto legacy_temporary = collision_path;
  legacy_temporary += L".tmp";
  {
    std::ofstream sentinel(legacy_temporary, std::ios::binary);
    sentinel << "do-not-replace";
  }
  passed &= expect(
      layerc::platform::write_profile_file(
          collision_path, layerc::core::serialize_profile_json(first)),
      "profile writes should not collide with a legacy deterministic .tmp file");
  std::ifstream sentinel(legacy_temporary, std::ios::binary);
  const std::string sentinel_contents{std::istreambuf_iterator<char>(sentinel),
                                      std::istreambuf_iterator<char>()};
  sentinel.close();
  passed &= expect(sentinel_contents == "do-not-replace",
                    "unique atomic-write temporaries should leave unrelated .tmp files untouched");

  const auto bundle_path = directory / L"bundle.json";
  auto old_bundle_profile = test_profile("Old bundle");
  old_bundle_profile.id = "bundle-old";
  auto old_bundle_visual = test_visual_profile();
  old_bundle_visual.profile_id = old_bundle_profile.id;
  passed &= expect(
      layerc::platform::write_profile_file(
          bundle_path,
          layerc::core::serialize_profile_json(old_bundle_profile)) &&
          layerc::platform::write_profile_file(
              bundle_path,
              layerc::core::serialize_profile_json(old_bundle_profile)) &&
          layerc::platform::write_visual_sidecar(
              bundle_path, old_bundle_visual, old_bundle_profile),
      "the old profile bundle should be created for rollback testing");
  auto new_bundle_profile = old_bundle_profile;
  new_bundle_profile.id = "bundle-new";
  new_bundle_profile.name = "New bundle";
  auto new_bundle_visual = old_bundle_visual;
  new_bundle_visual.profile_id = new_bundle_profile.id;
  auto bundle_backup = bundle_path;
  bundle_backup += L".bak";
  const HANDLE locked_bundle_backup = CreateFileW(
      bundle_backup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  passed &= expect(locked_bundle_backup != INVALID_HANDLE_VALUE,
                    "the bundle backup should be lockable for rollback testing");
  if (locked_bundle_backup != INVALID_HANDLE_VALUE) {
    passed &= expect(
        !layerc::platform::write_profile_bundle(
            bundle_path, new_bundle_profile, new_bundle_visual),
        "a failed second bundle replacement should report failure");
    CloseHandle(locked_bundle_backup);
  }
  const auto rolled_back_profile_json =
      layerc::platform::read_profile_file(bundle_path);
  const auto rolled_back_profile =
      rolled_back_profile_json
          ? layerc::core::parse_profile_json(*rolled_back_profile_json)
          : layerc::core::ProfileResult(layerc::core::ProfileError{});
  const auto *rolled_back =
      std::get_if<layerc::core::GameProfile>(&rolled_back_profile);
  layerc::platform::VisualSidecarLoad rolled_back_visual;
  if (rolled_back == nullptr)
    rolled_back_visual.state =
        layerc::platform::VisualSidecarState::invalid;
  else
    rolled_back_visual =
        layerc::platform::load_visual_sidecar(bundle_path, *rolled_back);
  passed &= expect(
      rolled_back != nullptr && rolled_back->id == "bundle-old" &&
          rolled_back_visual.profile.has_value() &&
          rolled_back_visual.profile->profile_id == "bundle-old",
      "a partial bundle failure must restore a matching profile and sidecar");
  passed &= expect(
      layerc::platform::write_profile_bundle(
          bundle_path, new_bundle_profile, new_bundle_visual),
      "a complete bundle replacement should succeed after the lock clears");
  const auto committed_visual =
      layerc::platform::load_visual_sidecar(bundle_path, new_bundle_profile);
  passed &= expect(
      committed_visual.profile.has_value() &&
          committed_visual.profile->profile_id == "bundle-new",
      "a successful bundle replacement should commit the matching sidecar");
  auto committed_visual_backup =
      layerc::platform::visual_sidecar_path(bundle_path);
  committed_visual_backup += L".bak";
  const HANDLE locked_visual_backup = CreateFileW(
      committed_visual_backup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  passed &= expect(locked_visual_backup != INVALID_HANDLE_VALUE,
                   "the visual recovery copy should be lockable");
  if (locked_visual_backup != INVALID_HANDLE_VALUE) {
    auto rejected_profile = new_bundle_profile;
    rejected_profile.id = "bundle-rejected";
    auto rejected_visual = new_bundle_visual;
    rejected_visual.profile_id = rejected_profile.id;
    passed &= expect(
        !layerc::platform::write_profile_bundle(
            bundle_path, rejected_profile, rejected_visual),
        "a locked visual backup refresh should reject the bundle before publishing it");
    CloseHandle(locked_visual_backup);
    const auto unchanged_profile_json =
        layerc::platform::read_profile_file(bundle_path);
    const auto unchanged_profile =
        unchanged_profile_json
            ? layerc::core::parse_profile_json(*unchanged_profile_json)
            : layerc::core::ProfileResult(layerc::core::ProfileError{});
    const auto *unchanged =
        std::get_if<layerc::core::GameProfile>(&unchanged_profile);
    const auto unchanged_visual =
        unchanged != nullptr
            ? layerc::platform::load_visual_sidecar(bundle_path, *unchanged)
            : layerc::platform::VisualSidecarLoad{};
    passed &= expect(
        unchanged != nullptr && unchanged->id == "bundle-new" &&
            unchanged_visual.profile.has_value() &&
            unchanged_visual.profile->profile_id == "bundle-new",
        "a backup-refresh failure must leave the active profile bundle unchanged");
  }
  const auto committed_visual_path =
      layerc::platform::visual_sidecar_path(bundle_path);
  const HANDLE locked_visual = CreateFileW(
      committed_visual_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  passed &= expect(locked_visual != INVALID_HANDLE_VALUE,
                   "the committed visual sidecar should be lockable");
  if (locked_visual != INVALID_HANDLE_VALUE) {
    passed &= expect(
        layerc::platform::write_profile_bundle(
            bundle_path, new_bundle_profile, std::nullopt),
        "a durable removal marker should commit even while stale visual bytes are locked");
    const auto removed_while_locked =
        layerc::platform::load_visual_sidecar(bundle_path,
                                              new_bundle_profile);
    passed &= expect(
        removed_while_locked.state ==
            layerc::platform::VisualSidecarState::absent,
        "locked stale visual bytes must remain inactive after removal commits");
    CloseHandle(locked_visual);
  }
  passed &= expect(
      layerc::platform::write_profile_bundle(
          bundle_path, new_bundle_profile, std::nullopt),
      "an input-only bundle should replace the base and remove visual data");
  passed &= expect(
      !std::filesystem::exists(
          layerc::platform::visual_sidecar_path(bundle_path)) &&
          !std::filesystem::exists(committed_visual_backup) &&
          layerc::platform::load_visual_sidecar(bundle_path,
                                                new_bundle_profile)
                  .state ==
              layerc::platform::VisualSidecarState::absent,
      "an input-only overwrite must keep both stale visual copies inactive and clean them when unlocked");

  HANDLE framed_read{};
  HANDLE framed_write{};
  passed &= expect(CreatePipe(&framed_read, &framed_write, nullptr, 4'096) != FALSE,
                   "anonymous calibration test pipe should be created");
  if (framed_read != nullptr && framed_write != nullptr) {
    std::string framed_error;
    passed &= expect(layerc::platform::write_framed_pipe(
                         framed_write, "{\"calibration\":true}", 1'024,
                         framed_error),
                     "bounded calibration frame should be written");
    const auto framed =
        layerc::platform::read_framed_pipe(framed_read, 1'024);
    passed &= expect(framed && framed.payload == "{\"calibration\":true}",
                     "bounded calibration frame should be read exactly");
    CloseHandle(framed_write);
    CloseHandle(framed_read);
  }

  std::array<wchar_t, 32'768> previous_directory{};
  const DWORD previous_directory_length = GetCurrentDirectoryW(
      static_cast<DWORD>(previous_directory.size()), previous_directory.data());
  const bool changed_directory =
      previous_directory_length != 0 &&
      previous_directory_length < previous_directory.size() &&
      SetCurrentDirectoryW(directory.c_str()) != FALSE;
  bool relative_write{};
  if (changed_directory) {
    relative_write = layerc::platform::write_profile_file(
        L"relative.json", layerc::core::serialize_profile_json(first));
    (void)SetCurrentDirectoryW(previous_directory.data());
  }
  passed &= expect(changed_directory && relative_write,
                   "atomic profile writes should support a path with an empty parent component");

  layerc::platform::WindowCandidate candidate{
      .title = L"Game Window",
      .executable_path = L"C:\\Games\\I.EXE",
      .executable_name = L"I.EXE",
      .window_class = L"GameClass"};
  layerc::core::GameProfile matcher_profile;
  matcher_profile.game.executable_name = "i.exe";
  passed &= expect(layerc::platform::profile_matches_window(
                       candidate, matcher_profile),
                   "window matching should use locale-invariant case folding");
  matcher_profile.game.executable_name = std::string(1, '\xFF');
  matcher_profile.game.window_title = "Game";
  passed &= expect(!layerc::platform::profile_matches_window(
                       candidate, matcher_profile),
                   "invalid UTF-8 matcher fields must reject instead of weakening to title-only matching");

  const auto corrupt_path = directory / L"corrupt.json";
  {
    std::ofstream corrupt(corrupt_path, std::ios::binary | std::ios::trunc);
    corrupt << R"({"schemaVersion":1,"name":)";
  }
  const auto corrupt_contents =
      layerc::platform::read_profile_file(corrupt_path);
  passed &= expect(
      corrupt_contents.has_value() &&
          std::holds_alternative<layerc::core::ProfileError>(
              layerc::core::parse_profile_json(*corrupt_contents)),
      "an externally corrupted profile should return a bounded validation error");
  passed &= expect(
      layerc::platform::write_profile_file(
          corrupt_path, layerc::core::serialize_profile_json(first)),
      "a valid save should safely replace an externally corrupted profile");
  auto corrupt_backup = corrupt_path;
  corrupt_backup += L".bak";
  passed &= expect(!std::filesystem::exists(corrupt_backup),
                   "an invalid previous profile must not be promoted to backup");

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
      layerc::platform::NamedPipeEditorDaemonChannel::Role::server, pipe_name,
      GetCurrentProcessId());
  layerc::platform::NamedPipeEditorDaemonChannel client(
      layerc::platform::NamedPipeEditorDaemonChannel::Role::client, pipe_name,
      GetCurrentProcessId());
  std::array<HANDLE, 3> wait_handles{};
  passed &= expect(server.wait_handles(wait_handles) == 1,
                   "a listening server should expose one waitable connect event");
  for (int attempt = 0;
       attempt < 1000 && (!server.connected() || !client.connected()); ++attempt) {
    (void)client.poll();
    (void)server.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  passed &= expect(server.connected() && client.connected(),
                   "same-user overlapped pipe peers should connect");
  passed &= expect(server.wait_handles(wait_handles) >= 1,
                   "a connected server should expose waitable overlapped I/O");
  passed &= expect(server.peer_process_id() == GetCurrentProcessId() &&
                       client.peer_process_id() == GetCurrentProcessId(),
                   "both authenticated channel ends should retain the peer PID");
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

  (void)SetEnvironmentVariableW(
      L"LAYERC_PROFILE_DIR",
      had_profile_directory ? previous_profile_directory.data() : nullptr);
  filesystem_error.clear();
  std::filesystem::remove_all(directory, filesystem_error);
  passed &= expect(!filesystem_error,
                   "the platform test directory should always be cleaned up");
  return passed ? 0 : 1;
}

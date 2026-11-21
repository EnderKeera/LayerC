#include "layerc/core/profile.hpp"
#include "layerc/platform/bounded_helper.hpp"
#include "layerc/platform/framed_pipe.hpp"
#include "layerc/platform/hid_device.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_visual_assets.hpp"
#include "layerc/platform/settings_store.hpp"
#include "layerc/platform/window_query.hpp"
#include "platform_live_scenarios.hpp"
#include "test_support.hpp"
#include "visual_asset_load_contract.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <windows.h>

namespace layerc::platform {

struct NamedPipeEditorDaemonChannelTestAccess {
  using Channel = NamedPipeEditorDaemonChannel;
  static bool pending_connect(const Channel &channel) {
    return channel.connect_pending_ &&
           !HasOverlappedIoCompleted(&channel.connect_overlapped_);
  }
  static bool pending_read(const Channel &channel) {
    return channel.read_pending_ &&
           !HasOverlappedIoCompleted(&channel.read_overlapped_);
  }
  static bool pending_write(const Channel &channel) {
    return channel.write_pending_ &&
           !HasOverlappedIoCompleted(&channel.write_overlapped_);
  }
  static bool close_and_verify(Channel &channel) {
    const std::array pending{channel.connect_pending_, channel.read_pending_,
                             channel.write_pending_};
    const std::array operations{&channel.connect_overlapped_,
                                &channel.read_overlapped_,
                                &channel.write_overlapped_};
    // Exercise precisely the destructor's boundary while its OVERLAPPED
    // storage is still inspectable; observing freed storage would be UB.
    channel.close_handles();
    for (std::size_t index{}; index < operations.size(); ++index)
      if (pending[index] && !HasOverlappedIoCompleted(operations[index]))
        return false;
    return !channel.connect_pending_ && !channel.read_pending_ &&
           !channel.write_pending_ && channel.pipe_ == INVALID_HANDLE_VALUE;
  }
};

} // namespace layerc::platform

namespace {

using layerc::test::expect;

int run_authenticated_peer(const bool server, const std::wstring &pipe_name) {
  using Channel = layerc::platform::NamedPipeEditorDaemonChannel;
  const auto channel = std::make_unique<Channel>(
      server ? Channel::Role::server : Channel::Role::client, pipe_name);
  bool sent{};
  for (int attempt = 0; attempt < 5'000; ++attempt) {
    for (auto &result : channel->poll()) {
      const auto *message = std::get_if<layerc::core::LiveMessage>(&result);
      if (message == nullptr)
        continue;
      if (server && message->type == layerc::core::LiveMessageType::handshake) {
        (void)channel->send({
            .type = layerc::core::LiveMessageType::acknowledgement,
            .revision = message->revision,
            .text = "authenticated",
        });
        sent = true;
      } else if (!server &&
                 message->type ==
                     layerc::core::LiveMessageType::acknowledgement &&
                 message->text == "authenticated") {
        return 0;
      }
    }
    if (!server && channel->connected() && !sent) {
      sent = channel->send({.type = layerc::core::LiveMessageType::handshake,
                           .revision = 1,
                           .text = "sibling-auth-test"});
    }
    if (server && sent) {
      // Give the overlapped acknowledgement write time to complete.
      for (int flush = 0; flush < 20; ++flush) {
        (void)channel->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 1;
}

void visual_asset_contract_case() {
  layerc::test::TempDirectory temp("LayerC-asset-contract");
  expect(std::filesystem::exists(temp.path()),
         "asset contract directory should be created");
  layerc::test::visual_asset_load_contract(temp.path());
}

void profile_template_asset_snapshot() {
  namespace assets = layerc::platform;
  namespace core = layerc::core;
  layerc::test::TempDirectory temp("LayerC-asset-snapshot");
  const auto profile_path = temp.path() / "profile.json";
  const auto image = layerc::test::visual_asset_contract::contract_image(true);
  auto prepared = assets::prepare_profile_template_assets(
      profile_path, std::span(&image, 1));
  const auto *owned = std::get_if<std::vector<assets::EncodedTemplateAsset>>(&prepared);
  if (!expect(owned && owned->size() == 1, "calibration produces one owned image")) return;
  const auto &asset = owned->front();
  const auto destination = assets::profile_visual_asset_path(profile_path, asset.asset_sha256);
  expect(destination == temp.path() / "_visual" / "assets" / (asset.asset_sha256 + ".png") &&
             !std::filesystem::exists(destination),
         "Windows layout is unified and preparing a template does not write");
  expect(assets::commit_profile_transaction(temp.path(), [&](auto &mutations) {
           return assets::plan_profile_template_copies(profile_path, *owned, mutations);
         }).ok() && std::filesystem::exists(destination),
         "Windows assets publish through the recovered profile transaction");
  core::VisualProfile visual;
  core::TemplateState observation;
  observation.references.push_back({.asset_sha256 = asset.asset_sha256});
  visual.observations.push_back(std::move(observation));
  const auto snapshot = assets::snapshot_profile_template_assets(profile_path, visual);
  const auto *copied = std::get_if<std::vector<assets::EncodedTemplateAsset>>(&snapshot);
  expect(copied && copied->size() == 1 && copied->front().bytes == asset.bytes,
         "a duplicate snapshots the original encoded bytes");
  const auto loaded = assets::load_profile_template_asset(profile_path, asset.asset_sha256);
  const auto *decoded = std::get_if<core::TemplateImage>(&loaded);
  expect(decoded && decoded->pixels == image.pixels, "the verified loader keeps the pixels");
  std::error_code error;
  expect(std::filesystem::remove(destination, error) && !error &&
             std::holds_alternative<assets::VisualAssetError>(
                 assets::snapshot_profile_template_assets(profile_path, visual)),
         "missing bytes refuse a snapshot");
  expect(std::holds_alternative<core::TemplateImage>(
             assets::load_profile_template_asset({}, asset.asset_sha256, *owned)),
         "a draft can still load its owned bytes");
}

void pipe_name_contract() {
  expect(std::wstring_view(layerc::platform::live_edit_pipe_name) ==
             L"\\\\.\\pipe\\LayerC.LayerD.LiveEdit.v3",
         "the tested production pipe name should match the protocol contract");
}

void bounded_helper_failure() {
  // A forced Job-assignment failure must terminate the still-suspended child.
  std::vector<wchar_t> image_path(32'768);
  const DWORD image_length = GetModuleFileNameW(
      nullptr, image_path.data(), static_cast<DWORD>(image_path.size()));
  expect(image_length != 0 && image_length < image_path.size(),
         "the bounded-helper test image path should be available");
  if (image_length != 0 && image_length < image_path.size()) {
    layerc::platform::OwnedHandle observed;
    layerc::platform::HelperLaunchOptions options;
    options.fail_assignment_for_test = [&](HANDLE process) {
      HANDLE duplicate{};
      if (DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(),
                          &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE)
        observed.reset(duplicate);
      return true;
    };
    const std::filesystem::path image(image_path.data());
    const auto launched = layerc::platform::launch_bounded_helper(
        image,
        layerc::platform::quote_command_argument(image.wstring()) +
            L" --bounded-helper-probe",
        options);
    const auto *failure =
        std::get_if<layerc::platform::HelperLaunchError>(&launched);
    DWORD exit_code{};
    expect(failure != nullptr &&
               failure->step == layerc::platform::HelperLaunchStep::assign_job &&
               observed.valid() &&
               WaitForSingleObject(observed.get(), 2'000) == WAIT_OBJECT_0 &&
               GetExitCodeProcess(observed.get(), &exit_code) != FALSE &&
               exit_code == ERROR_ACCESS_DENIED,
           "Job assignment failure must stop the suspended helper before it runs");
  }
}

void pipe_server_spoof() {
  const std::wstring spoof_pipe_name =
      L"\\\\.\\pipe\\LayerC.PlatformSpoofTest." +
      std::to_wstring(GetCurrentProcessId()) + L"." +
      std::to_wstring(GetTickCount64());
  const auto spoof_server =
      std::make_unique<layerc::platform::NamedPipeEditorDaemonChannel>(
          layerc::platform::NamedPipeEditorDaemonChannel::Role::server,
          spoof_pipe_name);
  const auto guarded_client =
      std::make_unique<layerc::platform::NamedPipeEditorDaemonChannel>(
          layerc::platform::NamedPipeEditorDaemonChannel::Role::client,
          spoof_pipe_name, GetCurrentProcessId() + 1U);
  for (int attempt = 0; attempt < 50; ++attempt) {
    (void)guarded_client->poll();
    (void)spoof_server->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(!guarded_client->connected() &&
             !guarded_client->last_error().empty(),
         "the client should reject an unexpected pipe server PID");
}

void named_pipe_round_trip() {
  const std::wstring pipe_name = L"\\\\.\\pipe\\LayerC.PlatformTest." +
                                 std::to_wstring(GetCurrentProcessId()) + L"." +
                                 std::to_wstring(GetTickCount64());
  const auto server =
      std::make_unique<layerc::platform::NamedPipeEditorDaemonChannel>(
          layerc::platform::NamedPipeEditorDaemonChannel::Role::server,
          pipe_name, GetCurrentProcessId());
  const auto client =
      std::make_unique<layerc::platform::NamedPipeEditorDaemonChannel>(
          layerc::platform::NamedPipeEditorDaemonChannel::Role::client,
          pipe_name, GetCurrentProcessId());
  std::array<HANDLE, 3> wait_handles{};
  expect(server->wait_handles(wait_handles) == 1,
         "a listening server should expose one waitable connect event");
  for (int attempt = 0;
       attempt < 1000 && (!server->connected() || !client->connected());
       ++attempt) {
    (void)client->poll();
    (void)server->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(server->connected() && client->connected(),
         "same-user overlapped pipe peers should connect");
  expect(server->wait_handles(wait_handles) >= 1,
         "a connected server should expose waitable overlapped I/O");
  expect(server->peer_process_id() == GetCurrentProcessId() &&
             client->peer_process_id() == GetCurrentProcessId(),
         "both authenticated channel ends should retain the peer PID");
  expect(client->send({.type = layerc::core::LiveMessageType::handshake,
                       .revision = 77,
                       .text = "platform-test"}),
         "client should queue a live message");

  bool server_received{};
  for (int attempt = 0; attempt < 1000 && !server_received; ++attempt) {
    (void)client->poll();
    for (auto &result : server->poll()) {
      const auto *message = std::get_if<layerc::core::LiveMessage>(&result);
      server_received = message != nullptr && message->revision == 77 &&
                        message->text == "platform-test";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(server_received, "framed UTF-8 messages should cross the named pipe");
}

bool connect_test_peers(layerc::platform::NamedPipeEditorDaemonChannel &server,
                        layerc::platform::NamedPipeEditorDaemonChannel &client) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while ((!server.connected() || !client.connected()) &&
         std::chrono::steady_clock::now() < deadline) {
    (void)client.poll();
    (void)server.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return server.connected() && client.connected();
}

std::vector<layerc::platform::OwnedHandle> retain_completion_events(
    const layerc::platform::NamedPipeEditorDaemonChannel &channel) {
  std::array<HANDLE, 3> events{};
  const auto count = channel.wait_handles(events);
  std::vector<layerc::platform::OwnedHandle> retained;
  for (std::size_t index{}; index < count; ++index) {
    HANDLE duplicate{};
    if (expect(DuplicateHandle(GetCurrentProcess(), events[index],
                               GetCurrentProcess(), &duplicate, 0, FALSE,
                               DUPLICATE_SAME_ACCESS) != FALSE,
               "teardown test should retain completion events, not freed OVERLAPPED storage"))
      retained.emplace_back(duplicate);
  }
  return retained;
}

void named_pipe_cancel_drain() {
  using Channel = layerc::platform::NamedPipeEditorDaemonChannel;
  using Access = layerc::platform::NamedPipeEditorDaemonChannelTestAccess;
  const auto prefix = L"\\\\.\\pipe\\LayerC.CancelDrainTest." +
                      std::to_wstring(GetCurrentProcessId()) + L"." +
                      std::to_wstring(GetTickCount64());
  for (int iteration{}; iteration < 4; ++iteration) {
    const auto pipe_name = prefix + L"." + std::to_wstring(iteration);
    auto server = std::make_unique<Channel>(Channel::Role::server, pipe_name,
                                            GetCurrentProcessId());
    if (!expect(Access::pending_connect(*server),
                "teardown fixture must force a pending overlapped connect")) return;
    expect(Access::close_and_verify(*server),
           "destructor teardown must drain a pending connect before closing or reusing its state");
    (void)server->poll();
    auto client = std::make_unique<Channel>(Channel::Role::client, pipe_name,
                                            GetCurrentProcessId());
    if (!expect(connect_test_peers(*server, *client),
                "server should reconnect after draining an unaccepted connect")) return;
    if (!expect(Access::pending_read(*client) && Access::pending_read(*server),
                "idle peers must have real pending reads")) return;
    const layerc::core::LiveMessage large{
        .type = layerc::core::LiveMessageType::handshake,
        .revision = 900,
        .text = std::string(400U * 1024U, 'x')};
    // Do not poll the receiving peer: 400 KiB exceeds its pipe quota plus the
    // outstanding 64 KiB read, forcing WriteFile to remain in flight.
    if (!expect(client->send(large) && Access::pending_write(*client),
                "teardown fixture must force a pending write with live frame storage")) return;
    expect(Access::close_and_verify(*client),
           "destructor teardown must drain pending read/write before releasing the frame or events");
    client->disconnect_peer();
    client.reset();
    server->disconnect_peer();
    (void)server->poll();
    client = std::make_unique<Channel>(Channel::Role::client, pipe_name,
                                      GetCurrentProcessId());
    if (!expect(connect_test_peers(*server, *client),
                "server must reconnect after cancelling a blocked write")) return;
    expect(client->send({.type = layerc::core::LiveMessageType::handshake,
                         .revision = 77, .text = "after-cancel"}),
           "reconnected channel should accept a fresh frame");
    bool received{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!received && std::chrono::steady_clock::now() < deadline) {
      (void)client->poll();
      for (const auto &result : server->poll()) {
        const auto *message = std::get_if<layerc::core::LiveMessage>(&result);
        expect(message == nullptr || message->revision != 900,
               "a cancelled frame must not leak into the reconnected stream");
        received = message != nullptr && message->revision == 77 &&
                   message->text == "after-cancel";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(received, "reconnected channel should decode only the new connection's frame");
    if (!expect(client->send(large) && Access::pending_write(*client) &&
                    Access::pending_read(*client),
                "destruction fixture should retain actual pending read and write")) return;
    const auto events = retain_completion_events(*client);
    expect(events.size() == 2, "pending read/write destruction should expose two events");
    client.reset();
    for (const auto &event : events)
      expect(WaitForSingleObject(event.get(), 0) == WAIT_OBJECT_0,
             "destruction must return only after cancelled read/write completion");
    server.reset();
  }
  auto listening = std::make_unique<Channel>(Channel::Role::server,
                                             prefix + L".destruction");
  expect(Access::pending_connect(*listening),
         "connect destruction fixture should still be pending");
  const auto events = retain_completion_events(*listening);
  listening.reset();
  for (const auto &event : events)
    expect(WaitForSingleObject(event.get(), 0) == WAIT_OBJECT_0,
           "destruction must return only after cancelled connect completion");
}

void hid_sampling() {
  layerc::platform::HidRawDevice system_keyboard{};
  system_keyboard.handle = reinterpret_cast<HANDLE>(0x1234);
  system_keyboard.raw_type = RIM_TYPEKEYBOARD;
  RAWINPUT keyboard_input{};
  keyboard_input.header.dwType = RIM_TYPEKEYBOARD;
  keyboard_input.header.hDevice = system_keyboard.handle;
  keyboard_input.data.keyboard.MakeCode = 0x36;
  layerc::core::HidControlBinding system_key{};
  system_key.usage_page = 0x07;
  system_key.usage = 0x36;
  system_key.activation = layerc::core::HidActivationMode::button;
  system_key.allow_system_class = true;
  const auto key_down = layerc::platform::sample_hid_control(
      system_keyboard, system_key, keyboard_input);
  keyboard_input.data.keyboard.Flags = RI_KEY_BREAK;
  const auto key_up = layerc::platform::sample_hid_control(
      system_keyboard, system_key, keyboard_input);
  expect(key_down.has_value() && key_down->button_active &&
             key_up.has_value() && !key_up->button_active,
         "Advanced system keyboard bindings should retain "
         "device-specific press and release samples");

  layerc::platform::HidRawDevice system_mouse{};
  system_mouse.handle = reinterpret_cast<HANDLE>(0x5678);
  system_mouse.raw_type = RIM_TYPEMOUSE;
  RAWINPUT mouse_input{};
  mouse_input.header.dwType = RIM_TYPEMOUSE;
  mouse_input.header.hDevice = system_mouse.handle;
  mouse_input.data.mouse.lLastX = 4;
  layerc::core::HidControlBinding relative_mouse{};
  relative_mouse.usage_page = 0x01;
  relative_mouse.usage = 0x30;
  relative_mouse.activation =
      layerc::core::HidActivationMode::relative_positive;
  relative_mouse.allow_system_class = true;
  const auto first_movement = layerc::platform::sample_hid_control(
      system_mouse, relative_mouse, mouse_input);
  const auto second_movement = layerc::platform::sample_hid_control(
      system_mouse, relative_mouse, mouse_input);
  expect(first_movement.has_value() && second_movement.has_value() &&
             first_movement->relative && first_movement->value == 4,
         "each successive relative system-mouse report should remain "
         "an independently available movement sample");

  const std::array<std::byte, 2> report_bytes{std::byte{0x7f}, std::byte{0x20}};
  layerc::platform::HidRawDevice reportless_device{};
  expect(layerc::platform::effective_hid_report_id(reportless_device,
                                                    report_bytes) == 0,
         "report-ID-less collections should persist report ID zero "
         "instead of the first data byte");
  reportless_device.uses_report_ids = true;
  expect(layerc::platform::effective_hid_report_id(reportless_device,
                                                    report_bytes) == 0x7f,
         "collections with report IDs should preserve the leading report ID");
}

void framed_calibration_pipe() {
  HANDLE framed_read{};
  HANDLE framed_write{};
  expect(CreatePipe(&framed_read, &framed_write, nullptr, 4'096) != FALSE,
         "anonymous calibration test pipe should be created");
  if (framed_read != nullptr && framed_write != nullptr) {
    std::string framed_error;
    expect(layerc::platform::write_framed_pipe(
               framed_write, "{\"calibration\":true}", 1'024, framed_error),
           "bounded calibration frame should be written");
    const auto framed = layerc::platform::read_framed_pipe(framed_read, 1'024);
    expect(framed && framed.payload == "{\"calibration\":true}",
           "bounded calibration frame should be read exactly");
    CloseHandle(framed_write);
    CloseHandle(framed_read);
  }
}

void window_matcher_policy() {
  layerc::platform::WindowCandidate candidate{.title = L"Game Window",
                                               .executable_path =
                                                   L"C:\\Games\\I.EXE",
                                               .executable_name = L"I.EXE",
                                               .window_class = L"GameClass"};
  layerc::core::GameProfile matcher_profile;
  matcher_profile.game.executable_name = "i.exe";
  expect(layerc::platform::profile_matches_window(candidate, matcher_profile),
         "window matching should use locale-invariant case folding");
  matcher_profile.game.executable_name = std::string(1, '\xFF');
  matcher_profile.game.window_title = "Game";
  expect(!layerc::platform::profile_matches_window(candidate, matcher_profile),
         "invalid UTF-8 matcher fields must reject instead of weakening to "
         "title-only matching");
}

// Runs entirely in a disposable HKCU key; the user's Software\LayerC is never
// read or written. The read check runs first, so a broken override fails
// before the write could reach the user's key.
void settings_key_override() {
  const std::wstring parent = L"Software\\LayerC.Test";
  const std::wstring key = parent + L"\\settings-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64());
  constexpr DWORD probe = 0x5e771e;
  expect(RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"OverrideProbe",
                         REG_DWORD, &probe, sizeof(probe)) == ERROR_SUCCESS,
         "disposable settings key should be created");
  expect(SetEnvironmentVariableW(L"LAYERC_SETTINGS_KEY", key.c_str()) != FALSE,
         "settings override should be set");
  const auto read =
      layerc::platform::read_layerc_registry_dword(L"OverrideProbe");
  const bool redirected = read.value == probe;
  expect(redirected, "settings reads should come from LAYERC_SETTINGS_KEY");
  if (redirected) {
    expect(static_cast<bool>(layerc::platform::write_bool_setting(
               layerc::platform::BoolSetting::visual_recognition, true)),
           "redirected settings write should succeed");
    DWORD stored{};
    DWORD size = sizeof(stored);
    expect(RegGetValueW(HKEY_CURRENT_USER, key.c_str(),
                        L"VisualRecognitionEnabled", RRF_RT_REG_DWORD, nullptr,
                        &stored, &size) == ERROR_SUCCESS &&
               stored == 1,
           "settings writes should land in LAYERC_SETTINGS_KEY");
    // The editor's Options check box writes the DWORD the launcher reads
    // (launcher_win_tests pins the launcher's side of the same value).
    expect(static_cast<bool>(layerc::platform::write_bool_setting(
               layerc::platform::BoolSetting::launcher_updates, true)),
           "the launcher update preference should be written");
    stored = 0;
    size = sizeof(stored);
    expect(RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"Updates",
                        RRF_RT_REG_DWORD, nullptr, &stored, &size) ==
                   ERROR_SUCCESS &&
               stored == 1,
           "the launcher update preference should be the Updates DWORD");
    expect(static_cast<bool>(layerc::platform::write_bool_setting(
               layerc::platform::BoolSetting::launcher_updates, false)) &&
               layerc::platform::read_bool_setting(
                   layerc::platform::BoolSetting::launcher_updates)
                       .value == false,
           "the launcher update preference should turn off");
  }
  SetEnvironmentVariableW(L"LAYERC_SETTINGS_KEY", nullptr);
  RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
  RegDeleteKeyW(HKEY_CURRENT_USER, parent.c_str()); // only when now empty
}

// Absent-or-present DWORDs that can fail one chosen read, write or restore,
// driving commit_settings through each failure path (review CQ-01).
struct FaultingSettingsStore {
  using Value = std::optional<std::uint32_t>;
  std::vector<Value> values;
  std::vector<std::uint32_t> next;
  std::optional<std::size_t> fail_read;
  std::optional<std::size_t> fail_write;
  std::optional<std::size_t> fail_restore;
  std::size_t writes{};

  std::uint32_t read(const std::size_t index,
                     layerc::platform::StoredSetting &prior) const {
    if (fail_read == index)
      return ERROR_ACCESS_DENIED;
    prior = {};
    if (const auto &value = values[index]; value) {
      prior.type = REG_DWORD;
      prior.data.resize(sizeof(*value));
      std::memcpy(prior.data.data(), &*value, sizeof(*value));
    }
    return 0;
  }
  std::uint32_t write(const std::size_t index) {
    ++writes;
    if (fail_write == index)
      return ERROR_ACCESS_DENIED;
    values[index] = next[index];
    return 0;
  }
  std::uint32_t restore(const std::size_t index,
                        const layerc::platform::StoredSetting &prior) {
    if (fail_restore == index)
      return ERROR_WRITE_FAULT;
    if (!prior.type) {
      values[index].reset();
      return 0;
    }
    std::uint32_t value{};
    std::memcpy(&value, prior.data.data(), sizeof(value));
    values[index] = value;
    return 0;
  }
};

void settings_commit_policy() {
  using layerc::platform::SettingsCommitState;
  using Value = FaultingSettingsStore::Value;
  const std::vector<Value> before{7U, std::nullopt, 3U, 9U};
  const auto store = [&] {
    return FaultingSettingsStore{.values = before, .next = {1, 1, 1, 1}};
  };
  const auto commit = [](FaultingSettingsStore &target) {
    return layerc::platform::commit_settings(target, target.values.size());
  };

  auto clean = store();
  const auto committed = commit(clean);
  expect(static_cast<bool>(committed) && committed.error == 0 &&
             clean.values == std::vector<Value>{1U, 1U, 1U, 1U},
         "a commit without faults writes every value");

  auto failed_write = store();
  failed_write.fail_write = 2;
  const auto undone = commit(failed_write);
  expect(!undone && undone.state == SettingsCommitState::unchanged &&
             undone.error == ERROR_ACCESS_DENIED,
         "a failed write reports the commit unchanged with its error");
  expect(failed_write.values == before && failed_write.writes == 3,
         "a failed write restores earlier values, an absent one included, "
         "and writes nothing after it");

  auto failed_restore = store();
  failed_restore.fail_write = 2;
  failed_restore.fail_restore = 0;
  const auto partial = commit(failed_restore);
  expect(!partial && partial.state == SettingsCommitState::partial &&
             partial.error == ERROR_ACCESS_DENIED,
         "an undo that fails reports a partial commit and the write error");
  expect(failed_restore.values ==
             std::vector<Value>{1U, std::nullopt, 3U, 9U},
         "a partial commit still restores every other written value");

  auto failed_read = store();
  failed_read.fail_read = 3;
  const auto unread = commit(failed_read);
  expect(!unread && unread.state == SettingsCommitState::unchanged &&
             failed_read.writes == 0 && failed_read.values == before,
         "a failed read of any prior value writes nothing");

  auto first_write = store();
  first_write.fail_write = 0;
  const auto nothing = commit(first_write);
  expect(!nothing && nothing.state == SettingsCommitState::unchanged &&
             first_write.values == before,
         "a failed first write leaves every value as it was");
}

// Options > Apply's snapshot against a disposable registry key: every value
// lands as the DWORD its reader expects, whatever type was there before.
void options_settings_commit() {
  const std::wstring parent = L"Software\\LayerC.Test";
  const std::wstring key = parent + L"\\options-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64());
  constexpr wchar_t stale[] = L"stale";
  expect(RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"ExcludeFromCapture",
                         REG_SZ, stale, sizeof(stale)) == ERROR_SUCCESS,
         "disposable options key should be created");
  expect(RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(),
                         L"ExcludeEditorFromCapture", REG_BINARY, nullptr, 0) ==
             ERROR_SUCCESS,
         "a zero-byte prior option should be readable before commit");
  expect(SetEnvironmentVariableW(L"LAYERC_SETTINGS_KEY", key.c_str()) != FALSE,
         "settings override should be set");
  using layerc::platform::BoolSetting;
  using layerc::platform::read_bool_setting;
  // The override is in effect before anything is written through it.
  if (expect(read_bool_setting(BoolSetting::capture_exclusion).failed(),
             "the seeded string reads from the disposable key")) {
    const auto committed = layerc::platform::write_options_settings(
        {.capture_exclusion = true,
         .editor_capture_exclusion = false,
         .visual_recognition = true,
         .launcher_updates = true,
         .editor_text_scale = 130});
    expect(static_cast<bool>(committed) && committed.error == 0,
           "the options snapshot commits");
    expect(read_bool_setting(BoolSetting::capture_exclusion).value == true &&
               read_bool_setting(BoolSetting::editor_capture_exclusion)
                       .value == false &&
               read_bool_setting(BoolSetting::visual_recognition).value ==
                   true &&
               read_bool_setting(BoolSetting::launcher_updates).value ==
                   true &&
               layerc::platform::read_editor_text_scale().value == 130U,
           "every option reads back as committed");
  }
  SetEnvironmentVariableW(L"LAYERC_SETTINGS_KEY", nullptr);
  RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
  RegDeleteKeyW(HKEY_CURRENT_USER, parent.c_str()); // only when now empty
}

} // namespace

int main(const int argc, char *argv[]) {
  if (argc == 3 && (std::string_view(argv[1]) == "--auth-server" ||
                    std::string_view(argv[1]) == "--auth-client")) {
    const std::string pipe_utf8(argv[2]);
    return run_authenticated_peer(
        std::string_view(argv[1]) == "--auth-server",
        std::wstring(pipe_utf8.begin(), pipe_utf8.end()));
  }
  namespace live = layerc::test::platform_live;
  return layerc::test::run({
      LAYERC_TEST_CASE(visual_asset_contract_case),
      LAYERC_TEST_CASE(profile_template_asset_snapshot),
      LAYERC_TEST_CASE(pipe_name_contract),
      LAYERC_TEST_CASE(live::platform_transactions),
      LAYERC_TEST_CASE(live::journal_read_status_recovery),
      LAYERC_TEST_CASE(live::profile_planning_after_recovery),
      LAYERC_TEST_CASE(hid_sampling),
      LAYERC_TEST_CASE(live::platform_visual_assets),
      LAYERC_TEST_CASE(live::platform_visual_asset_writer_ownership),
      LAYERC_TEST_CASE(live::platform_sibling_authentication),
      LAYERC_TEST_CASE(live::platform_profile_writes),
      LAYERC_TEST_CASE(live::platform_visual_sidecar_recovery),
      LAYERC_TEST_CASE(live::platform_atomic_temporary_collision),
      LAYERC_TEST_CASE(live::platform_bundle_recovery),
      LAYERC_TEST_CASE(live::platform_automation_state_recovery),
      LAYERC_TEST_CASE(framed_calibration_pipe),
      LAYERC_TEST_CASE(live::platform_relative_profile_write),
      LAYERC_TEST_CASE(window_matcher_policy),
      LAYERC_TEST_CASE(settings_key_override),
      LAYERC_TEST_CASE(settings_commit_policy),
      LAYERC_TEST_CASE(options_settings_commit),
      LAYERC_TEST_CASE(live::platform_corrupt_profile_recovery),
      LAYERC_TEST_CASE(bounded_helper_failure),
      LAYERC_TEST_CASE(pipe_server_spoof),
      LAYERC_TEST_CASE(named_pipe_round_trip),
      LAYERC_TEST_CASE(named_pipe_cancel_drain),
  });
}

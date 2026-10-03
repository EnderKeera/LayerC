#include "platform_live_scenarios.hpp"
#include "platform_live_test_support.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_file_guard.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets_windows.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <windows.h>

namespace {

using layerc::test::expect;
using layerc::test::read_file;
using layerc::test::write_file;
using layerc::test::platform_live::completed_visual_gc;
using layerc::test::platform_live::durable_read_contract;
using layerc::test::platform_live::launch_auth_peer;
using layerc::test::platform_live::test_automation_profile;
using layerc::test::platform_live::test_profile;
using layerc::test::platform_live::test_visual_profile;
using layerc::test::platform_live::visual_gc_blocked_by;

class ScopedCurrentDirectory {
public:
  explicit ScopedCurrentDirectory(const std::filesystem::path &directory)
      : previous_(std::filesystem::current_path()) {
    std::error_code error;
    std::filesystem::current_path(directory, error);
    changed_ = !error;
  }
  ~ScopedCurrentDirectory() {
    if (changed_) {
      std::error_code ignored;
      std::filesystem::current_path(previous_, ignored);
    }
  }
  ScopedCurrentDirectory(const ScopedCurrentDirectory &) = delete;
  ScopedCurrentDirectory &operator=(const ScopedCurrentDirectory &) = delete;
  [[nodiscard]] bool changed() const noexcept { return changed_; }

private:
  std::filesystem::path previous_;
  bool changed_{};
};

} // namespace

namespace layerc::test::platform_live {

void platform_transactions() {
  layerc::test::TempDirectory temp("LayerC-platform-test");
  const auto &directory = temp.path();
  expect(!directory.empty(), "temporary directory should be available");
  expect(std::filesystem::exists(directory),
         "test directory should be created");
  std::error_code filesystem_error;
  const auto transaction_scope =
      layerc::platform::profile_transaction_scope(directory);
  expect(transaction_scope.has_value(),
         "test transaction scope should resolve");
  if (!transaction_scope.has_value())
    return;

  expect(durable_read_contract(directory),
         "bounded reads should distinguish missing, oversize, and successful "
         "files");

  HANDLE abandoned_mutex =
      CreateMutexW(nullptr, FALSE, L"Local\\LayerC.ProfileTransaction.v1");
  bool mutex_owned{};
  std::thread abandoned_owner([&] {
    mutex_owned = WaitForSingleObject(abandoned_mutex, 5'000) == WAIT_OBJECT_0;
    // Exiting without ReleaseMutex intentionally creates WAIT_ABANDONED.
  });
  abandoned_owner.join();
  expect(mutex_owned &&
             layerc::platform::recover_profile_transactions(*transaction_scope),
         "transaction recovery should accept and release ownership of an "
         "abandoned mutex");
  if (abandoned_mutex != nullptr)
    CloseHandle(abandoned_mutex);

  const layerc::platform::ProfileFileMutation colliding_mutations[] = {
      {.target = directory / L"Caf\u00c9.json",
       .contents = std::vector<std::byte>{std::byte{'a'}}},
      {.target = directory / L"caf\u00e9.json",
       .contents = std::vector<std::byte>{std::byte{'b'}}},
  };
  expect(layerc::platform::commit_profile_transaction(*transaction_scope,
                                                      colliding_mutations)
                 .error() == layerc::platform::ProfileWriteError::invalid_input,
         "transaction target validation should reject "
         "invariant-casefolded Unicode collisions");
  const layerc::platform::ProfileFileMutation outside = {
      .target = directory.parent_path() / L"layerc-outside-scope.json",
      .contents = std::vector<std::byte>{std::byte{'x'}}};
  expect(layerc::platform::commit_profile_transaction(*transaction_scope,
                                                      std::span(&outside, 1))
                 .error() ==
             layerc::platform::ProfileWriteError::scope_rejected,
         "a target outside the transaction scope should report "
         "scope_rejected");
  const layerc::platform::ProfileFileMutation create = {
      .target = directory / L"create-only.json",
      .contents = std::vector<std::byte>{std::byte{'x'}}};
  expect(static_cast<bool>(layerc::platform::commit_profile_transaction(
             *transaction_scope, std::span(&create, 1))),
         "the first create-only write should publish");
  auto create_again = create;
  create_again.must_be_absent = true;
  expect(layerc::platform::commit_profile_transaction(
             *transaction_scope, std::span(&create_again, 1))
                 .error() == layerc::platform::ProfileWriteError::target_exists,
         "a create-only write over an existing file should report "
         "target_exists");

  const auto recovery_first = directory / L"recovery-first.bin";
  const auto recovery_second = directory / L"recovery-second.bin";
  const auto transaction =
      directory / L"_transactions" / L"published-upper-bound";
  expect(
      write_file(recovery_first, "new-first") &&
          write_file(recovery_second, "old-second") &&
          write_file(transaction / L"backup" / L"0.bin", "old-first") &&
          write_file(transaction / L"backup" / L"1.bin", "old-second") &&
          write_file(transaction / L"journal.json",
                     "{\"schemaVersion\":2,\"operations\":["
                     "{\"root\":\"config\",\"target\":\"recovery-first.bin\","
                     "\"existed\":true},"
                     "{\"root\":\"config\",\"target\":\"recovery-second.bin\","
                     "\"existed\":true}]}\n") &&
          write_file(transaction / L"published", "2\n") &&
          layerc::platform::recover_profile_transactions(*transaction_scope) &&
          read_file(recovery_first) == "old-first" &&
          read_file(recovery_second) == "old-second",
      "recovery should roll back the durable published upper bound when a "
      "crash occurs before the next replacement");

  const auto committed_target = directory / L"recovery-committed.bin";
  const auto committed_transaction =
      directory / L"_transactions" / L"committed-cleanup";
  expect(
      write_file(committed_target, "published") &&
          write_file(committed_transaction / L"backup" / L"0.bin", "old") &&
          write_file(committed_transaction / L"journal.json",
                     "{\"schemaVersion\":2,\"operations\":["
                     "{\"root\":\"config\",\"target\":"
                     "\"recovery-committed.bin\",\"existed\":true}]}\n") &&
          write_file(committed_transaction / L"published", "1\n") &&
          write_file(committed_transaction / L"committed", "committed\n") &&
          layerc::platform::recover_profile_transactions(*transaction_scope) &&
          read_file(committed_target) == "published",
      "recovery should retain a committed publication and clean only its "
      "journal residue");
  filesystem_error.clear();
  std::filesystem::remove_all(directory, filesystem_error);
  expect(!filesystem_error,
         "the platform test directory should always be cleaned up");
}

void platform_visual_assets() {
  layerc::test::TempDirectory temp("LayerC-assets");
  const auto &directory = temp.path();
  std::error_code filesystem_error;
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", directory);
  if (!expect(profile_dir.applied(),
              "asset tests should use an isolated profile directory"))
    return;

  layerc::core::TemplateImage asset{
      .asset_sha256 = {},
      .width = 8,
      .height = 8,
      .pixels = std::vector<layerc::core::RgbPixel>(64),
      .alpha = std::vector<std::uint8_t>(64, 255)};
  for (std::size_t index = 0; index < asset.pixels.size(); ++index) {
    asset.pixels[index] = {static_cast<std::uint8_t>(index * 3U),
                           static_cast<std::uint8_t>(255U - index * 2U),
                           static_cast<std::uint8_t>(index * 4U)};
  }
  auto flat_asset = asset;
  std::ranges::fill(flat_asset.pixels, layerc::core::RgbPixel{80, 186, 200});
  expect(std::holds_alternative<layerc::platform::VisualAssetError>(
             layerc::platform::store_template_asset(flat_asset)),
         "flat template assets should be rejected before entering "
         "content-addressed storage");
  auto stored_asset = layerc::platform::store_template_asset(asset);
  auto *stored_asset_info =
      std::get_if<layerc::platform::ImportedTemplateAsset>(&stored_asset);
  if (const auto *error =
          std::get_if<layerc::platform::VisualAssetError>(&stored_asset))
    std::cerr << "Asset store error: " << error->message << '\n';
  expect(stored_asset_info != nullptr &&
             stored_asset_info->reference.asset_sha256.size() == 64 &&
             std::filesystem::exists(stored_asset_info->path),
         "template pixels should be sanitized and stored by SHA-256");
  if (stored_asset_info != nullptr) {
    auto loaded = layerc::platform::load_template_asset(
        stored_asset_info->reference.asset_sha256);
    const auto *image = std::get_if<layerc::core::TemplateImage>(&loaded);
    expect(image != nullptr && image->pixels == asset.pixels &&
               image->alpha == asset.alpha,
           "stored template assets should verify and round-trip");
    write_file(stored_asset_info->path, "not a png");
    expect(std::holds_alternative<layerc::platform::VisualAssetError>(
               layerc::platform::load_template_asset(
                   stored_asset_info->reference.asset_sha256)),
           "a replaced template asset must fail content-hash verification");
    stored_asset = layerc::platform::store_template_asset(asset);
    stored_asset_info =
        std::get_if<layerc::platform::ImportedTemplateAsset>(&stored_asset);
    expect(stored_asset_info != nullptr,
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
    gc_visual.observations.push_back(
        layerc::core::TemplateState{.id = "template",
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
    write_file(stale_asset_temporary, "stale");
    std::filesystem::last_write_time(
        stale_asset_temporary,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(48),
        filesystem_error);
    expect(!filesystem_error, "stale asset temporary timestamp should be set");
    const bool gc_bundle_written = layerc::platform::write_profile_bundle(
        gc_profile_path, gc_base, gc_visual);
    const bool first_gc_completed = completed_visual_gc(2U, 1U);
    expect(
        gc_bundle_written && first_gc_completed &&
            std::filesystem::exists(stored_asset_info->path) &&
            !std::filesystem::exists(second_asset_info->path) &&
            !std::filesystem::exists(stale_asset_temporary),
        "a complete profile scan should retain references and remove orphans "
        "and stale asset temporaries");

    auto third_asset = asset;
    third_asset.pixels.back().blue ^= 0x55U;
    const auto third_result =
        layerc::platform::store_template_asset(third_asset);
    const auto *third_asset_info =
        std::get_if<layerc::platform::ImportedTemplateAsset>(&third_result);
    auto stale_backup = layerc::platform::visual_sidecar_path(gc_profile_path);
    stale_backup += L".bak";
    if (third_asset_info != nullptr) {
      auto recovery_visual = gc_visual;
      std::get<layerc::core::TemplateState>(recovery_visual.observations.front()).references = {
          third_asset_info->reference};
      write_file(stale_backup,
                 layerc::core::serialize_visual_profile_json(recovery_visual));
    }
    const bool recovery_gc_completed = completed_visual_gc(0U, 2U);
    expect(third_asset_info != nullptr && recovery_gc_completed &&
               std::filesystem::exists(third_asset_info->path),
           "assets referenced only by a valid recovery sidecar should be "
           "retained");

    const auto malformed = directory / L"unreadable.json";
    write_file(malformed, "{not-json");
    write_file(stale_backup, "{corrupt-stale-backup");
    const bool unrelated_malformed_gc_completed = completed_visual_gc(0U, 1U);
    expect(
        third_asset_info != nullptr && unrelated_malformed_gc_completed &&
            !std::filesystem::exists(third_asset_info->path) &&
            std::filesystem::exists(stored_asset_info->path),
        "an unrelated malformed base and corrupt stale backup should not block "
        "safe collection");
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
    write_file(active_sidecar, "{invalid-active-sidecar");
    const bool gc_was_blocked = visual_gc_blocked_by(active_sidecar);
    expect(fourth_asset_info != nullptr && gc_was_blocked &&
               std::filesystem::exists(fourth_asset_info->path),
           "an invalid active sidecar bundle should conservatively abort "
           "collection "
           "with its exact fault path");
  } else {
    expect(false, "both GC test assets should be stored");
  }
}

void platform_visual_asset_writer_ownership() {
  layerc::test::TempDirectory fixture("LayerC-asset-writer");
  layerc::test::ScopedEnvironment root("LAYERC_PROFILE_DIR", fixture.path());
  if (!expect(root.applied(), "writer tests must use an isolated profile store"))
    return;
  layerc::core::TemplateImage pixels{
      .width = 8, .height = 8,
      .pixels = std::vector<layerc::core::RgbPixel>(64),
      .alpha = std::vector<std::uint8_t>(64, 255)};
  for (std::size_t index{}; index < pixels.pixels.size(); ++index)
    pixels.pixels[index] = {static_cast<std::uint8_t>(index * 3U),
                            static_cast<std::uint8_t>(255U - index * 2U),
                            static_cast<std::uint8_t>(index * 4U)};
  const auto first = layerc::platform::store_template_asset(pixels);
  pixels.pixels.front().red ^= 0x7FU;
  const auto pending = layerc::platform::store_template_asset(pixels);
  const auto *a = std::get_if<layerc::platform::ImportedTemplateAsset>(&first);
  const auto *b = std::get_if<layerc::platform::ImportedTemplateAsset>(&pending);
  if (a == nullptr || b == nullptr) {
    expect(false, "both writer-boundary asset fixtures must publish");
    return;
  }
  const auto base = test_profile("Asset writer ownership");
  layerc::core::VisualProfile visual;
  visual.profile_id = base.id;
  visual.observations.push_back(layerc::core::TemplateState{.id = "template", .name = "Template",
                              .search_region = {0.0, 0.0, 1.0, 1.0},
                              .expected_width = 0.25, .expected_height = 0.25,
                              .references = {a->reference}});
  visual.triggers.push_back({.cooldown_id = "dash", .observation_id = "template"});
  const auto path = fixture.path() / "writer.json";
  if (!expect(layerc::platform::write_profile_bundle(path, base, visual),
              "the first writer fixture must commit"))
    return;
  const auto roots = layerc::platform::profile_transaction_scope(fixture.path());
  if (!expect(roots.has_value(), "writer fixture scope must resolve")) return;
  std::future<layerc::platform::VisualAssetGcReport> collection;
  {
    layerc::platform::TransactionFileLock writer(*roots);
    if (!expect(writer.locked(), "the fixture must own the real writer boundary"))
      return;
    std::promise<void> announced;
    auto began = announced.get_future();
    collection = std::async(std::launch::async,
                            [signal = std::move(announced)]() mutable {
      signal.set_value();
      return layerc::platform::garbage_collect_unreferenced_visual_assets();
    });
    expect(began.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
           "the maintenance worker must start under fixture writer ownership");
    expect(collection.wait_for(std::chrono::milliseconds(250)) ==
               std::future_status::timeout && std::filesystem::exists(b->path),
           "maintenance must not scan/delete while another writer owns the store");
    std::get<layerc::core::TemplateState>(visual.observations.front()).references = {
        b->reference};
    expect(layerc::platform::write_profile_bundle(path, base, visual),
           "the owning writer must publish its new reference before releasing");
  }
  const auto report = collection.get();
  expect(report.completed() && report.deleted == 0 && report.retained == 2 &&
             std::filesystem::exists(a->path) && std::filesystem::exists(b->path),
         "collection must scan the committed B and recovery A after ownership releases");
}

void platform_sibling_authentication() {
  layerc::test::TempDirectory temp("LayerC-sibling-auth");
  const auto &directory = temp.path();
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", directory);
  if (!expect(profile_dir.applied(),
              "sibling peers should use an isolated profile directory"))
    return;
  // Exercise the production image-name policy without touching the fixed
  // production pipe, which may legitimately be in use by the user's daemon.
  std::vector<wchar_t> own_executable(32'768);
  const DWORD own_length =
      GetModuleFileNameW(nullptr, own_executable.data(),
                         static_cast<DWORD>(own_executable.size()));
  const auto sibling_server = directory / L"layerd.exe";
  const auto sibling_client = directory / L"layere.exe";
  const bool copied_peers =
      own_length != 0 && own_length < own_executable.size() &&
      CopyFileW(own_executable.data(), sibling_server.c_str(), FALSE) !=
          FALSE &&
      CopyFileW(own_executable.data(), sibling_client.c_str(), FALSE) != FALSE;
  expect(copied_peers,
         "sibling-image authentication test peers should be created");
  if (copied_peers) {
    const std::wstring sibling_pipe = L"\\\\.\\pipe\\LayerC.SiblingAuthTest." +
                                      std::to_wstring(GetCurrentProcessId()) +
                                      L"." + std::to_wstring(GetTickCount64());
    PROCESS_INFORMATION server_process{};
    PROCESS_INFORMATION client_process{};
    const bool server_launched = launch_auth_peer(
        sibling_server, L"--auth-server", sibling_pipe, server_process);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool client_launched = launch_auth_peer(
        sibling_client, L"--auth-client", sibling_pipe, client_process);
    expect(server_launched && client_launched,
           "sibling layerd/layere authentication peers should launch");
    // Guard on the handles themselves: a failed launch leaves them null.
    const HANDLE server = server_process.hProcess;
    const HANDLE client = client_process.hProcess;
    if (server != nullptr && client != nullptr) {
      const DWORD server_wait = WaitForSingleObject(server, 10'000);
      const DWORD client_wait = WaitForSingleObject(client, 10'000);
      DWORD server_exit{1};
      DWORD client_exit{1};
      if (server_wait == WAIT_OBJECT_0)
        (void)GetExitCodeProcess(server, &server_exit);
      if (client_wait == WAIT_OBJECT_0)
        (void)GetExitCodeProcess(client, &client_exit);
      expect(server_wait == WAIT_OBJECT_0 && client_wait == WAIT_OBJECT_0 &&
                 server_exit == 0 && client_exit == 0,
             "same-user, same-session sibling images should authenticate "
             "bidirectionally");
      if (server_wait != WAIT_OBJECT_0) {
        TerminateProcess(server, 1);
        WaitForSingleObject(server, 1'000);
      }
      if (client_wait != WAIT_OBJECT_0) {
        TerminateProcess(client, 1);
        WaitForSingleObject(client, 1'000);
      }
    } else {
      if (server != nullptr) {
        TerminateProcess(server, 1);
        WaitForSingleObject(server, 1'000);
      }
      if (client != nullptr) {
        TerminateProcess(client, 1);
        WaitForSingleObject(client, 1'000);
      }
    }
    for (auto *process : {&client_process, &server_process}) {
      if (process->hThread != nullptr)
        CloseHandle(process->hThread);
      if (process->hProcess != nullptr)
        CloseHandle(process->hProcess);
    }
  }
}

void platform_profile_writes() {
  layerc::test::TempDirectory temp("LayerC-profile-writes");
  const auto &directory = temp.path();
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", directory);
  if (!expect(profile_dir.applied(),
              "profile writes should use an isolated directory"))
    return;
  const auto path = directory / L"profile.json";
  auto backup = path;
  backup += L".bak";
  const auto first = test_profile("First");
  const auto second = test_profile("Second");
  expect(layerc::platform::write_profile_file(
             path, layerc::core::serialize_profile_json(first)),
         "first atomic profile write should succeed");
  expect(layerc::platform::write_profile_file(
             path, layerc::core::serialize_profile_json(second)),
         "second atomic profile write should succeed");
  const auto backup_contents = layerc::platform::read_profile_file(backup);
  const auto backup_profile =
      backup_contents.has_value()
          ? layerc::core::parse_profile_json(*backup_contents)
          : layerc::core::ProfileResult(layerc::core::ProfileError{});
  const auto *stored = std::get_if<layerc::core::GameProfile>(&backup_profile);
  expect(stored != nullptr && stored->name == "First",
         "the .bak file should retain the previous valid profile");

  const auto canonical = layerc::platform::canonical_profile_key(path);
  const auto dotted = directory / L"." / L"profile.json";
  expect(canonical == layerc::platform::canonical_profile_key(dotted),
         "canonical profile keys should collapse lexical aliases");
  std::wstring differently_cased = path.wstring();
  CharUpperBuffW(differently_cased.data(),
                 static_cast<DWORD>(differently_cased.size()));
  expect(canonical ==
             layerc::platform::canonical_profile_key(differently_cased),
         "canonical profile keys should ignore Windows path casing");

  const HANDLE locked_backup =
      CreateFileW(backup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  expect(locked_backup != INVALID_HANDLE_VALUE,
         "the backup should be lockable for failure testing");
  const auto third = test_profile("Third");
  if (locked_backup != INVALID_HANDLE_VALUE) {
    expect(!layerc::platform::write_profile_file(
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
  expect(live_stored != nullptr && live_stored->name == "Second",
         "failed backup protection must leave the live file intact");
  expect(backup_stored != nullptr && backup_stored->name == "First",
         "failed backup protection must preserve the older backup");
}

void platform_visual_sidecar_recovery() {
  layerc::test::TempDirectory temp("LayerC-visual-recovery");
  const auto &directory = temp.path();
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", directory);
  if (!expect(profile_dir.applied(),
              "visual recovery should use an isolated directory"))
    return;
  const auto path = directory / L"profile.json";
  const auto second = test_profile("Second");
  expect(layerc::platform::write_profile_file(
             path, layerc::core::serialize_profile_json(second)),
         "visual recovery base profile should be created");
  const auto expected_sidecar = directory / L"_visual" / L"profile.json";
  expect(layerc::platform::visual_sidecar_path(path) == expected_sidecar,
         "visual sidecars should pair by base profile filename");
  const auto absent_visual =
      layerc::platform::load_visual_sidecar(path, second);
  expect(absent_visual.state == layerc::platform::ModuleSidecarState::absent,
         "profiles without a visual sidecar should remain input-only");
  auto visual = test_visual_profile();
  expect(layerc::platform::write_profile_bundle(path, second, visual),
         "the first visual sidecar write should succeed");
  visual.sample_rate_hz = 15;
  expect(layerc::platform::write_profile_bundle(path, second, visual),
         "a replacement visual sidecar should create a protected backup");
  auto visual_backup = expected_sidecar;
  visual_backup += L".bak";
  expect(std::filesystem::exists(visual_backup),
         "a valid previous visual sidecar should be backed up");
  auto visual_state = expected_sidecar;
  visual_state += L".state";
  const HANDLE locked_visual_state =
      CreateFileW(visual_state.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  expect(locked_visual_state != INVALID_HANDLE_VALUE,
         "the visual state marker should be lockable");
  if (locked_visual_state != INVALID_HANDLE_VALUE) {
    visual.sample_rate_hz = 5;
    expect(!layerc::platform::write_profile_bundle(path, second, visual),
           "a state-publication failure should fail the visual write");
    CloseHandle(locked_visual_state);
    const auto after_state_failure =
        layerc::platform::load_visual_sidecar(path, second);
    expect(after_state_failure.state ==
                   layerc::platform::ModuleSidecarState::valid &&
               after_state_failure.profile.has_value() &&
               after_state_failure.profile->sample_rate_hz == 15,
           "a state-publication failure must restore the previous "
           "visual and backup");
    visual.sample_rate_hz = 15;
  }
  write_file(expected_sidecar, "{broken");
  const auto recovered_visual =
      layerc::platform::load_visual_sidecar(path, second);
  expect(recovered_visual.state ==
                 layerc::platform::ModuleSidecarState::recovered_backup &&
             recovered_visual.profile.has_value() &&
             recovered_visual.profile->sample_rate_hz == 10,
         "an invalid visual sidecar should recover its last valid backup");
  write_file(visual_backup, "{also-broken");
  const auto invalid_visual =
      layerc::platform::load_visual_sidecar(path, second);
  expect(invalid_visual.state ==
                 layerc::platform::ModuleSidecarState::invalid &&
             !invalid_visual.error.message.empty(),
         "two invalid visual copies should produce a blocking diagnostic");
  visual.sample_rate_hz = 5;
  expect(layerc::platform::write_profile_bundle(path, second, visual),
         "repairing an invalid visual sidecar should replace its stale backup");
  write_file(expected_sidecar, "{broken-again");
  const auto repaired_backup =
      layerc::platform::load_visual_sidecar(path, second);
  expect(repaired_backup.state ==
                 layerc::platform::ModuleSidecarState::recovered_backup &&
             repaired_backup.profile.has_value() &&
             repaired_backup.profile->sample_rate_hz == 5,
         "a second corruption should recover the repaired visual definition");
}

void platform_atomic_temporary_collision() {
  layerc::test::TempDirectory temp("LayerC-atomic-collision");
  const auto &directory = temp.path();
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", directory);
  if (!expect(profile_dir.applied(),
              "atomic writes should use an isolated directory"))
    return;
  const auto first = test_profile("First");
  const auto collision_path = directory / L"collision.json";
  auto legacy_temporary = collision_path;
  legacy_temporary += L".tmp";
  write_file(legacy_temporary, "do-not-replace");
  expect(layerc::platform::write_profile_file(
             collision_path, layerc::core::serialize_profile_json(first)),
         "profile writes should not collide with a legacy deterministic "
         ".tmp file");
  expect(read_file(legacy_temporary) == "do-not-replace",
         "unique atomic-write temporaries should leave unrelated "
         ".tmp files untouched");
}

void platform_bundle_recovery() {
  layerc::test::TempDirectory temp("LayerC-bundle-recovery");
  const auto &directory = temp.path();
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", directory);
  if (!expect(profile_dir.applied(),
              "bundle recovery should use an isolated directory"))
    return;
  const auto bundle_path = directory / L"bundle.json";
  auto old_bundle_profile = test_profile("Old bundle");
  old_bundle_profile.id = "bundle-old";
  auto old_bundle_visual = test_visual_profile();
  old_bundle_visual.profile_id = old_bundle_profile.id;
  expect(layerc::platform::write_profile_file(
             bundle_path,
             layerc::core::serialize_profile_json(old_bundle_profile)) &&
             layerc::platform::write_profile_file(
                 bundle_path,
                 layerc::core::serialize_profile_json(old_bundle_profile)) &&
             layerc::platform::write_profile_bundle(
                 bundle_path, old_bundle_profile, old_bundle_visual),
         "the old profile bundle should be created for rollback testing");
  auto new_bundle_profile = old_bundle_profile;
  new_bundle_profile.id = "bundle-new";
  new_bundle_profile.name = "New bundle";
  auto new_bundle_visual = old_bundle_visual;
  new_bundle_visual.profile_id = new_bundle_profile.id;
  auto bundle_backup = bundle_path;
  bundle_backup += L".bak";
  const HANDLE locked_bundle_backup =
      CreateFileW(bundle_backup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  expect(locked_bundle_backup != INVALID_HANDLE_VALUE,
         "the bundle backup should be lockable for rollback testing");
  if (locked_bundle_backup != INVALID_HANDLE_VALUE) {
    expect(!layerc::platform::write_profile_bundle(
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
    rolled_back_visual.state = layerc::platform::ModuleSidecarState::invalid;
  else
    rolled_back_visual =
        layerc::platform::load_visual_sidecar(bundle_path, *rolled_back);
  expect(
      rolled_back != nullptr && rolled_back->id == "bundle-old" &&
          rolled_back_visual.profile.has_value() &&
          rolled_back_visual.profile->profile_id == "bundle-old",
      "a partial bundle failure must restore a matching profile and sidecar");
  expect(layerc::platform::write_profile_bundle(bundle_path, new_bundle_profile,
                                                new_bundle_visual),
         "a complete bundle replacement should succeed after the lock clears");
  const auto committed_visual =
      layerc::platform::load_visual_sidecar(bundle_path, new_bundle_profile);
  expect(committed_visual.profile.has_value() &&
             committed_visual.profile->profile_id == "bundle-new",
         "a successful bundle replacement should commit the matching sidecar");
  auto committed_visual_backup =
      layerc::platform::visual_sidecar_path(bundle_path);
  committed_visual_backup += L".bak";
  const HANDLE locked_visual_backup = CreateFileW(
      committed_visual_backup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  expect(locked_visual_backup != INVALID_HANDLE_VALUE,
         "the visual recovery copy should be lockable");
  if (locked_visual_backup != INVALID_HANDLE_VALUE) {
    auto rejected_profile = new_bundle_profile;
    rejected_profile.id = "bundle-rejected";
    auto rejected_visual = new_bundle_visual;
    rejected_visual.profile_id = rejected_profile.id;
    expect(!layerc::platform::write_profile_bundle(
               bundle_path, rejected_profile, rejected_visual),
           "a locked visual backup refresh should reject the bundle "
           "before publishing it");
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
    expect(unchanged != nullptr && unchanged->id == "bundle-new" &&
               unchanged_visual.profile.has_value() &&
               unchanged_visual.profile->profile_id == "bundle-new",
           "a backup-refresh failure must leave the active profile "
           "bundle unchanged");
  }
  const auto committed_visual_path =
      layerc::platform::visual_sidecar_path(bundle_path);
  const HANDLE locked_visual =
      CreateFileW(committed_visual_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  expect(locked_visual != INVALID_HANDLE_VALUE,
         "the committed visual sidecar should be lockable");
  if (locked_visual != INVALID_HANDLE_VALUE) {
    expect(layerc::platform::write_profile_bundle(
               bundle_path, new_bundle_profile, std::nullopt),
           "a durable removal marker should commit even while stale "
           "visual bytes are locked");
    const auto removed_while_locked =
        layerc::platform::load_visual_sidecar(bundle_path, new_bundle_profile);
    expect(
        removed_while_locked.state ==
            layerc::platform::ModuleSidecarState::absent,
        "locked stale visual bytes must remain inactive after removal commits");
    CloseHandle(locked_visual);
  }
  expect(layerc::platform::write_profile_bundle(bundle_path, new_bundle_profile,
                                                std::nullopt),
         "an input-only bundle should replace the base and remove visual data");
  expect(
      std::filesystem::exists(
          layerc::platform::visual_sidecar_path(bundle_path)) &&
          std::filesystem::exists(committed_visual_backup) &&
          layerc::platform::load_visual_sidecar(bundle_path, new_bundle_profile)
                  .state == layerc::platform::ModuleSidecarState::absent,
      "a durable removal marker must keep retained visual recovery bytes "
      "inactive");
}

void platform_automation_state_recovery() {
  layerc::test::TempDirectory temp("LayerC-automation-recovery");
  const auto &directory = temp.path();
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", directory);
  if (!expect(profile_dir.applied(),
              "automation recovery should use an isolated directory"))
    return;
  const auto bundle_path = directory / L"bundle.json";
  auto new_bundle_profile = test_profile("New bundle");
  new_bundle_profile.id = "bundle-new";
  auto new_bundle_visual = test_visual_profile();
  new_bundle_visual.profile_id = new_bundle_profile.id;
  expect(layerc::platform::write_profile_bundle(bundle_path, new_bundle_profile,
                                                new_bundle_visual),
         "automation test base and visual should be created");
  auto automation_bundle = test_automation_profile(new_bundle_profile.id);
  expect(layerc::platform::write_profile_bundle(bundle_path, new_bundle_profile,
                                                new_bundle_visual,
                                                automation_bundle),
         "a base, visual, and automation module should commit as one bundle");
  const auto committed_automation = layerc::platform::load_automation_sidecar(
      bundle_path, new_bundle_profile);
  expect(committed_automation.profile.has_value() &&
             committed_automation.profile->profile_id == new_bundle_profile.id,
         "the committed automation sidecar should match its base profile");
  const layerc::core::AutomationPersistentValues persistent_values{
      {"count", layerc::core::AutomationValue(std::int64_t{7})}};
  const auto reloaded_state = [&] {
    if (!layerc::platform::write_automation_state(
            bundle_path, automation_bundle, persistent_values))
      return layerc::platform::AutomationStateLoad{};
    return layerc::platform::load_automation_state(bundle_path,
                                                   automation_bundle);
  }();
  const auto saved_count = reloaded_state.values.find("count");
  expect(
      saved_count != reloaded_state.values.end() &&
          saved_count->second == layerc::core::AutomationValue(std::int64_t{7}),
      "machine-local persistent automation values should checkpoint and load");

  const auto automation_state_path =
      layerc::platform::automation_state_path(bundle_path);
  const HANDLE locked_state =
      CreateFileW(automation_state_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  expect(locked_state != INVALID_HANDLE_VALUE,
         "the committed automation state should be lockable");
  if (locked_state != INVALID_HANDLE_VALUE) {
    const layerc::core::AutomationPersistentValues replacement_values{
        {"count", layerc::core::AutomationValue(std::int64_t{8})}};
    expect(!layerc::platform::write_automation_state(
               bundle_path, automation_bundle, replacement_values),
           "a blocked atomic state replacement should report failure");
    const auto retained =
        layerc::platform::load_automation_state(bundle_path, automation_bundle);
    expect(
        retained.values.contains("count") &&
            retained.values.at("count") ==
                layerc::core::AutomationValue(std::int64_t{7}),
        "a failed state replacement must retain the prior durable checkpoint");
    CloseHandle(locked_state);
  }
  expect(write_file(automation_state_path, "{malformed") &&
             layerc::platform::load_automation_state(bundle_path,
                                                     automation_bundle)
                 .error.has_value(),
         "corrupt machine-local automation state should fail closed");
  expect(layerc::platform::write_automation_state(
             bundle_path, automation_bundle, persistent_values),
         "a valid checkpoint should recover after corrupt state is rejected");
  const auto automation_path =
      layerc::platform::automation_sidecar_path(bundle_path);
  auto automation_backup = automation_path;
  automation_backup += L".bak";
  const HANDLE locked_automation =
      CreateFileW(automation_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  expect(locked_automation != INVALID_HANDLE_VALUE,
         "the committed automation sidecar should be lockable");
  if (locked_automation != INVALID_HANDLE_VALUE) {
    expect(
        layerc::platform::write_profile_bundle(bundle_path, new_bundle_profile,
                                               new_bundle_visual, std::nullopt),
        "a durable removal marker should commit while stale automation bytes "
        "are locked");
    expect(layerc::platform::load_automation_sidecar(bundle_path,
                                                     new_bundle_profile)
                   .state == layerc::platform::ModuleSidecarState::absent,
           "locked stale automation bytes must remain inactive after removal "
           "commits");
    CloseHandle(locked_automation);
  }
  expect(layerc::platform::write_profile_bundle(bundle_path, new_bundle_profile,
                                                new_bundle_visual,
                                                std::nullopt) &&
             std::filesystem::exists(automation_path) &&
             std::filesystem::exists(automation_backup) &&
             layerc::platform::load_automation_sidecar(bundle_path,
                                                       new_bundle_profile)
                     .state == layerc::platform::ModuleSidecarState::absent,
         "automation removal must keep retained recovery bytes inactive after "
         "unlock");
}

void platform_relative_profile_write() {
  layerc::test::TempDirectory temp("LayerC-relative-write");
  const auto &directory = temp.path();
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", directory);
  if (!expect(profile_dir.applied(),
              "relative writes should use an isolated directory"))
    return;
  const auto first = test_profile("First");
  bool relative_write{};
  {
    ScopedCurrentDirectory cwd(directory);
    if (cwd.changed())
      relative_write = layerc::platform::write_profile_file(
          L"relative.json", layerc::core::serialize_profile_json(first));
    expect(cwd.changed() && relative_write,
           "atomic profile writes should support a path with an empty "
           "parent component");
  }
}

void platform_corrupt_profile_recovery() {
  layerc::test::TempDirectory temp("LayerC-corrupt-profile");
  const auto &directory = temp.path();
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", directory);
  if (!expect(profile_dir.applied(),
              "corrupt profile recovery should use an isolated directory"))
    return;
  const auto first = test_profile("First");
  const auto corrupt_path = directory / L"corrupt.json";
  write_file(corrupt_path, R"({"schemaVersion":1,"name":)");
  const auto corrupt_contents =
      layerc::platform::read_profile_file(corrupt_path);
  expect(corrupt_contents.has_value() &&
             std::holds_alternative<layerc::core::ProfileError>(
                 layerc::core::parse_profile_json(*corrupt_contents)),
         "an externally corrupted profile should return a bounded "
         "validation error");
  expect(layerc::platform::write_profile_file(
             corrupt_path, layerc::core::serialize_profile_json(first)),
         "a valid save should safely replace an externally corrupted profile");
  auto corrupt_backup = corrupt_path;
  corrupt_backup += L".bak";
  expect(!std::filesystem::exists(corrupt_backup),
         "an invalid previous profile must not be promoted to backup");
}

} // namespace layerc::test::platform_live

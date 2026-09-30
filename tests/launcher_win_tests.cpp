#include "install.hpp"
#include "release_client.hpp"

#include "launcher_signature_vectors.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <filesystem>
#include <string>

using namespace layerc;
using layerc::test::expect;
namespace fs = std::filesystem;
namespace vectors = layerc::test::signature_vectors;

namespace {

void bcrypt_verifies_openssl_signatures() {
  const auto key = launcher::parse_p256_public_key_hex(vectors::public_key_hex);
  expect(key.has_value(), "the test key parses");
  if (!key) return;
  launcher::win32::BcryptSignatureVerifier verifier(*key);
  const auto signature = vectors::from_hex(vectors::der_signature_hex);
  expect(verifier.verify(vectors::message, signature),
         "an openssl signature over the manifest verifies with CNG");

  std::string changed(vectors::message);
  changed.replace(changed.find("\"sequence\":1"), 12, "\"sequence\":2");
  expect(!verifier.verify(changed, signature), "a changed manifest fails verification");

  auto flipped = signature;
  flipped[10] = static_cast<char>(flipped[10] ^ 0x01);
  expect(!verifier.verify(vectors::message, flipped), "a changed signature fails");
  expect(!verifier.verify(vectors::message, "not der"), "garbage is rejected");

  auto other_key = *key;
  other_key[5] = static_cast<std::uint8_t>(other_key[5] ^ 0x01);
  launcher::win32::BcryptSignatureVerifier wrong(other_key);
  expect(!wrong.verify(vectors::message, signature), "another key rejects the signature");
}

// Every registry write goes to a per-process key under HKCU\Software that is
// deleted afterwards; the real Uninstall and Run keys are never touched.
struct TestKeys {
  std::wstring base = L"Software\\LayerC.LauncherTests\\" + std::to_wstring(GetCurrentProcessId());
  launcher::win32::RegistryKeys keys{base + L"\\Uninstall\\LayerC", base + L"\\Run", L"LayerC",
                                     base + L"\\Settings"};
  ~TestKeys() { RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str()); }
};

void registration_round_trips() {
  const TestKeys test_keys;
  const auto &keys = test_keys.keys;
  test::TempDirectory directory("layerc-launcher-win");
  launcher::win32::InstallPaths paths{directory.path() / L"Programs" / L"LayerC",
                                      directory.path() / L"Programs" / L"LayerC" / L"layerc.exe",
                                      directory.path() / L"StartMenu" / L"LayerC.lnk",
                                      directory.path() / L"Data"};

  expect(launcher::win32::write_uninstall_entry(keys, paths, L"12-abcdef012345", 4096 * 3),
         "the uninstall entry is written");
  expect(launcher::win32::read_string(keys.uninstall, L"DisplayVersion") ==
                 std::wstring(L"12-abcdef012345") &&
             launcher::win32::read_string(keys.uninstall, L"UninstallString") ==
                 L"\"" + paths.launcher.wstring() + L"\" --uninstall",
         "the uninstall entry names the build and the launcher's uninstall command");
  DWORD size_kb = 0;
  DWORD bytes = sizeof(size_kb);
  expect(RegGetValueW(HKEY_CURRENT_USER, keys.uninstall.c_str(), L"EstimatedSize",
                      RRF_RT_REG_DWORD, nullptr, &size_kb, &bytes) == ERROR_SUCCESS &&
             size_kb == 12,
         "EstimatedSize is recorded in KiB");
  expect(launcher::win32::remove_uninstall_entry(keys) &&
             !launcher::win32::read_string(keys.uninstall, L"DisplayName") &&
             launcher::win32::remove_uninstall_entry(keys),
         "removing the uninstall entry is idempotent");

  expect(!launcher::win32::autostart_enabled(keys), "autostart is off by default");
  expect(launcher::win32::set_autostart(keys, true, paths.launcher) &&
             launcher::win32::autostart_enabled(keys),
         "autostart can be turned on");
  expect(launcher::win32::set_autostart(keys, false, paths.launcher) &&
             !launcher::win32::autostart_enabled(keys) &&
             launcher::win32::set_autostart(keys, false, paths.launcher),
         "autostart can be turned off repeatedly");
}

void updates_require_explicit_opt_in() {
  const TestKeys test_keys;
  const auto &keys = test_keys.keys;
  expect(!launcher::win32::updates_enabled(keys),
         "updates are off when the settings key is missing");

  HKEY settings = nullptr;
  const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, keys.settings.c_str(), 0, nullptr, 0,
                                      KEY_SET_VALUE, nullptr, &settings, nullptr);
  expect(status == ERROR_SUCCESS, "the disposable settings key is created");
  if (status != ERROR_SUCCESS) return;
  expect(!launcher::win32::updates_enabled(keys), "updates are off when the preference is unset");

  const DWORD on = 1;
  expect(RegSetValueExW(settings, L"Updates", 0, REG_DWORD,
                        reinterpret_cast<const BYTE *>(&on), sizeof(on)) == ERROR_SUCCESS &&
             launcher::win32::updates_enabled(keys),
         "Updates=1 opts in to update checks");
  const DWORD off = 0;
  expect(RegSetValueExW(settings, L"Updates", 0, REG_DWORD,
                        reinterpret_cast<const BYTE *>(&off), sizeof(off)) == ERROR_SUCCESS &&
             !launcher::win32::updates_enabled(keys),
         "Updates=0 turns update checks off");
  expect(RegDeleteValueW(settings, L"Updates") == ERROR_SUCCESS &&
             !launcher::win32::updates_enabled(keys),
         "deleting the preference restores the offline default");

  const wchar_t invalid[] = L"1";
  expect(RegSetValueExW(settings, L"Updates", 0, REG_SZ,
                        reinterpret_cast<const BYTE *>(invalid), sizeof(invalid)) == ERROR_SUCCESS &&
             !launcher::win32::updates_enabled(keys),
         "a malformed preference does not enable network updates");
  RegCloseKey(settings);
}

void shortcut_points_at_the_launcher() {
  test::TempDirectory directory("layerc-launcher-lnk");
  const auto target = directory.path() / L"layerc.exe";
  test::write_file(target, "stub");
  const auto shortcut = directory.path() / L"Menu" / L"LayerC.lnk";
  expect(launcher::win32::create_shortcut(shortcut, target, L"LayerC") &&
             fs::is_regular_file(shortcut),
         "the Start menu shortcut is created");
  expect(launcher::win32::directory_size(directory.path()) >= 4,
         "directory_size counts file bytes");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(bcrypt_verifies_openssl_signatures),
                 LAYERC_TEST_CASE(registration_round_trips),
                 LAYERC_TEST_CASE(updates_require_explicit_opt_in),
                 LAYERC_TEST_CASE(shortcut_points_at_the_launcher))

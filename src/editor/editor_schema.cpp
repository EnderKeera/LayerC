#include "editor_window.hpp"

#include "layerc/core/json.hpp"

#include <variant>

namespace layerc::editor {

bool source_uses_schema_v1(const std::string_view source) {
  const auto parsed = layerc::core::json::parse(source);
  const auto *root = std::get_if<layerc::core::json::Value>(&parsed);
  if (root == nullptr || !root->is_object()) return false;
  const auto *version = root->find("schemaVersion");
  return version != nullptr && version->is_number() &&
         version->as_number() == 1.0;
}

bool confirm_v2_schema_upgrade(const HWND owner) {
  return MessageBoxW(
             owner,
             L"Saving this legacy profile upgrades its base profile and visual sidecar to schema version 2. Older LayerC builds cannot reopen the upgraded files. Continue?",
             L"Upgrade profile to LayerC V2", MB_YESNO | MB_ICONWARNING) ==
         IDYES;
}

} // namespace layerc::editor

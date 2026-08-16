#include "editor_window.hpp"

#include "layerc/core/json.hpp"

#include <variant>

namespace layerc::editor {

namespace {

bool source_schema_before(const std::string_view source,
                          const double current_schema) {
  const auto parsed = layerc::core::json::parse(source);
  const auto *root = std::get_if<layerc::core::json::Value>(&parsed);
  if (root == nullptr || !root->is_object()) return false;
  const auto *version = root->find("schemaVersion");
  return version != nullptr && version->is_number() &&
         version->as_number() < current_schema;
}

} // namespace

bool source_uses_legacy_base_schema(const std::string_view source) {
  return source_schema_before(source, 4.0);
}

bool source_uses_legacy_visual_schema(const std::string_view source) {
  return source_schema_before(source, 2.0);
}

bool confirm_schema_upgrade(const HWND owner) {
  return MessageBoxW(
             owner,
             L"Saving this legacy profile upgrades its base profile to schema version 4 and all declared module sidecars to their current schemas. Older LayerC builds cannot reopen the upgraded files. Continue?",
             L"Upgrade profile to LayerC V2.3", MB_YESNO | MB_ICONWARNING) ==
         IDYES;
}

} // namespace layerc::editor

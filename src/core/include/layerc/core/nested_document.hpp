#pragma once

#include "layerc/core/json_reader.hpp"

#include <stdexcept>

namespace layerc::core::json {

// Both local IPC protocols carry JSON documents as nested objects, never
// strings/base64. Optional absent sidecars use JSON null. Materializing the
// tree canonicalizes whitespace; limits still apply to the source and the
// compact decoded document, not to its enclosing packet.
[[nodiscard]] inline Value
nested_document_value(const std::string_view source, const std::size_t maximum,
                      const bool optional = false) {
  if (source.empty() && optional)
    return Value{};
  auto parsed = parse_bounded(source, maximum);
  if (const auto *error = std::get_if<FieldError>(&parsed))
    throw std::invalid_argument(error->describe());
  auto value = std::get<Value>(std::move(parsed));
  if (!value.is_object())
    throw std::invalid_argument("nested document must be an object");
  return value;
}

[[nodiscard]] inline bool
read_nested_document(Reader &reader, const Value &parent,
                     const std::string_view key, std::string &output,
                     const std::size_t maximum, const bool optional = false) {
  const auto *value = reader.field(parent, key, "$", Required::yes);
  if (!value)
    return false;
  if (optional && value->is_null()) {
    output.clear();
    return true;
  }
  const auto path = "$." + std::string(key);
  if (!value->is_object())
    return reader.fail(path, "expected nested document object");
  std::string document;
  append_value(document, *value);
  if (document.size() > maximum)
    return reader.fail(path, "nested document exceeds its byte limit");
  output = std::move(document);
  return true;
}

} // namespace layerc::core::json

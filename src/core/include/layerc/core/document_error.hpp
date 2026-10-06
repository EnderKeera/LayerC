#pragma once

#include <string>

namespace layerc::core {

// The one error of every JSON document reader and validator: a JSON path
// ("$.cooldowns[0].id", or "$@<byte offset>" for syntax errors) and a
// message. Message-only protocol and I/O errors stay separate types.
struct DocumentError {
  std::string path;
  std::string message;

  // "path: message"; empty when there is no error message.
  [[nodiscard]] std::string describe() const {
    return message.empty() ? std::string{} : path + ": " + message;
  }
  bool operator==(const DocumentError &) const = default;
};

} // namespace layerc::core

#include "utf8.hpp"

namespace layerc::core::detail {

bool is_valid_utf8(const std::string_view text) {
  std::size_t index{};
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t length{};
    std::uint32_t codepoint{};
    if (lead >= 0xC2U && lead <= 0xDFU) {
      length = 2;
      codepoint = lead & 0x1FU;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      length = 3;
      codepoint = lead & 0x0FU;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      length = 4;
      codepoint = lead & 0x07U;
    } else {
      return false;
    }

    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t continuation = 1; continuation < length;
         ++continuation) {
      const auto byte =
          static_cast<unsigned char>(text[index + continuation]);
      if ((byte & 0xC0U) != 0x80U) {
        return false;
      }
      codepoint = (codepoint << 6U) | (byte & 0x3FU);
    }

    if ((length == 2 && codepoint < 0x80U) ||
        (length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return false;
    }
    index += length;
  }
  return true;
}

void append_utf8(std::string &output, const std::uint32_t codepoint) {
  if (codepoint <= 0x7FU) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFU) {
    output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0xFFFFU) {
    output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    output.push_back(
        static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else {
    output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
    output.push_back(
        static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    output.push_back(
        static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
}

} // namespace layerc::core::detail
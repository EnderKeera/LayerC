#include "layerc/core/utf8.hpp"

#include <cstdint>

namespace layerc::core {
namespace {

bool decode_next(const std::string_view text, std::size_t &index,
                 std::uint32_t &codepoint) noexcept {
  const auto lead = static_cast<unsigned char>(text[index]);
  std::size_t length{};
  if (lead <= 0x7FU) {
    length = 1;
    codepoint = lead;
  } else if (lead >= 0xC2U && lead <= 0xDFU) {
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

  if (index + length > text.size()) return false;
  for (std::size_t continuation = 1; continuation < length; ++continuation) {
    const auto byte = static_cast<unsigned char>(text[index + continuation]);
    if ((byte & 0xC0U) != 0x80U) return false;
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
  return true;
}

// Shared comparison fold (identical on Windows and Linux): ASCII A-Z plus
// the cased rows of the Latin-1 supplement (U+00C0-U+00D6, U+00D8-U+00DE,
// each +0x20). U+00D7 (multiplication sign) has no case and is excluded.
// Codepoints outside these rows compare byte-exact. This replaces the
// former Windows LCMapStringEx invariant mapping with one deterministic
// table so both platforms agree on collisions and matches.
std::uint32_t fold_codepoint(const std::uint32_t codepoint) noexcept {
  if (codepoint >= 'A' && codepoint <= 'Z') return codepoint + ('a' - 'A');
  if (codepoint >= 0x00C0U && codepoint <= 0x00D6U) return codepoint + 0x20U;
  if (codepoint >= 0x00D8U && codepoint <= 0x00DEU) return codepoint + 0x20U;
  return codepoint;
}

} // namespace

bool is_valid_utf8(const std::string_view text) noexcept {
  std::size_t index{};
  std::uint32_t codepoint{};
  while (index < text.size()) {
    if (!decode_next(text, index, codepoint)) return false;
  }
  return true;
}

namespace detail {

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

} // namespace detail

std::optional<std::wstring> try_utf8_to_wide(const std::string_view text) {
  static_assert(sizeof(wchar_t) == 2 || sizeof(wchar_t) == 4);
  std::wstring output;
  output.reserve(text.size());
  std::size_t index{};
  while (index < text.size()) {
    std::uint32_t codepoint{};
    if (!decode_next(text, index, codepoint)) return std::nullopt;
    if constexpr (sizeof(wchar_t) == 2) {
      if (codepoint <= 0xFFFFU) {
        output.push_back(static_cast<wchar_t>(codepoint));
      } else {
        codepoint -= 0x10000U;
        output.push_back(static_cast<wchar_t>(0xD800U + (codepoint >> 10U)));
        output.push_back(static_cast<wchar_t>(0xDC00U + (codepoint & 0x3FFU)));
      }
    } else {
      output.push_back(static_cast<wchar_t>(codepoint));
    }
  }
  return output;
}

std::optional<std::string> try_wide_to_utf8(const std::wstring_view text) {
  static_assert(sizeof(wchar_t) == 2 || sizeof(wchar_t) == 4);
  std::string output;
  output.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    auto codepoint = static_cast<std::uint32_t>(text[index]);
    if constexpr (sizeof(wchar_t) == 2) {
      if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
        if (++index >= text.size()) return std::nullopt;
        const auto low = static_cast<std::uint32_t>(text[index]);
        if (low < 0xDC00U || low > 0xDFFFU) return std::nullopt;
        codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                    (low - 0xDC00U);
      } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
        return std::nullopt;
      }
    } else if ((codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
               codepoint > 0x10FFFFU) {
      return std::nullopt;
    }
    detail::append_utf8(output, codepoint);
  }
  return output;
}

std::optional<std::string> try_casefold_utf8(const std::string_view text) {
  std::string output;
  output.reserve(text.size());
  std::size_t index{};
  while (index < text.size()) {
    std::uint32_t codepoint{};
    if (!decode_next(text, index, codepoint)) return std::nullopt;
    detail::append_utf8(output, fold_codepoint(codepoint));
  }
  return output;
}

} // namespace layerc::core

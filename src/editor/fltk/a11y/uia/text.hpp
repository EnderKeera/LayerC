#pragma once

#include "context.hpp"

#include <string>
#include <vector>

namespace layerc::editor::a11y::uia {
struct Span { std::size_t begin{}, end{}; };
struct Glyph {
  std::size_t begin{}, end{};
  Bounds bounds;
};
struct TextInfo {
  Node node;
  std::wstring value;
  std::size_t caret{}, anchor{};
  std::vector<Glyph> glyphs;
};
[[nodiscard]] std::optional<TextInfo> text_info(
    const std::shared_ptr<Context> &context, NodeId id);
[[nodiscard]] ITextRangeProvider *make_range(
    const std::shared_ptr<Context> &context, NodeId id, Span span);
[[nodiscard]] Span clipped(Span span, const std::wstring &text);
[[nodiscard]] HRESULT select(const std::shared_ptr<Context> &context,
                              NodeId id, Span span, const TextInfo &info);
} // namespace layerc::editor::a11y::uia

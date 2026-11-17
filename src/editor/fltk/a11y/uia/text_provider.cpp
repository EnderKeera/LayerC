#include "text.hpp"

#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace layerc::editor::a11y::uia {
namespace {
HRESULT range_array(const std::shared_ptr<Context> &context, const NodeId id,
                    const std::vector<Span> &spans, SAFEARRAY **result) {
  auto *array = SafeArrayCreateVector(VT_UNKNOWN, 0, static_cast<ULONG>(spans.size()));
  if (array == nullptr) return E_OUTOFMEMORY;
  for (LONG index = 0; index < static_cast<LONG>(spans.size()); ++index) {
    auto *range = make_range(context, id, spans[static_cast<std::size_t>(index)]);
    if (range == nullptr) { SafeArrayDestroy(array); return E_OUTOFMEMORY; }
    const auto status = SafeArrayPutElement(array, &index, range);
    range->Release();
    if (FAILED(status)) { SafeArrayDestroy(array); return status; }
  }
  *result = array;
  return S_OK;
}

class TextProvider final : public ITextProvider {
public:
  TextProvider(std::shared_ptr<Context> context, NodeId id)
      : context_(std::move(context)), id_(id) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IAgileObject) &&
        !IsEqualIID(iid, IID_ITextProvider)) return E_NOINTERFACE;
    *result = static_cast<ITextProvider *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = --references_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      return range_array(context_, id_,
          {{std::min(info->caret, info->anchor), std::max(info->caret, info->anchor)}},
          result);
    });
  }
  HRESULT STDMETHODCALLTYPE GetVisibleRanges(SAFEARRAY **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      std::vector<Span> spans;
      const auto &b = info->node.bounds;
      if (info->node.states.showing) {
        for (const auto &glyph : info->glyphs) {
          const auto &g = glyph.bounds;
          if (g.left >= b.left + b.width || g.top >= b.top + b.height ||
              g.left + g.width <= b.left || g.top + g.height <= b.top) continue;
          if (!spans.empty() && spans.back().end == glyph.begin)
            spans.back().end = glyph.end;
          else spans.push_back({glyph.begin, glyph.end});
        }
        if (info->value.empty()) spans.push_back({0, 0});
      }
      return range_array(context_, id_, spans, result);
    });
  }
  HRESULT STDMETHODCALLTYPE RangeFromChild(IRawElementProviderSimple *,
                                            ITextRangeProvider **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    // A plain input contains no embedded child objects.
    return context_->connected.load() ? E_INVALIDARG : UIA_E_ELEMENTNOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE RangeFromPoint(UiaPoint point,
                                            ITextRangeProvider **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return E_INVALIDARG;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      double distance = std::numeric_limits<double>::max();
      auto position = info->caret;
      for (const auto &glyph : info->glyphs) {
        const auto &g = glyph.bounds;
        const double dx = point.x - std::clamp(point.x, g.left, g.left + g.width);
        const double dy = point.y - std::clamp(point.y, g.top, g.top + g.height);
        const double candidate = dx * dx + dy * dy;
        if (candidate < distance) {
          distance = candidate;
          position = point.x < g.left + g.width / 2 ? glyph.begin : glyph.end;
        }
      }
      *result = make_range(context_, id_, {position, position});
      return *result != nullptr ? S_OK : E_OUTOFMEMORY;
    });
  }
  HRESULT STDMETHODCALLTYPE get_DocumentRange(ITextRangeProvider **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      *result = make_range(context_, id_, {0, info->value.size()});
      return *result != nullptr ? S_OK : E_OUTOFMEMORY;
    });
  }
  HRESULT STDMETHODCALLTYPE get_SupportedTextSelection(
      SupportedTextSelection *result) override {
    if (result == nullptr) return E_POINTER;
    *result = SupportedTextSelection_Single;
    return context_->connected.load() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }
private:
  std::atomic<ULONG> references_{1};
  std::shared_ptr<Context> context_;
  NodeId id_;
};
} // namespace

std::optional<TextInfo> text_info(const std::shared_ptr<Context> &context,
                                  const NodeId id) {
  auto current = node(context, id);
  if (!current || !current->text || current->states.password) return std::nullopt;
  const auto wide = core::try_utf8_to_wide(current->text->value);
  if (!wide) return std::nullopt;
  TextInfo info;
  info.value = *wide;
  const auto offset = [&](std::size_t byte) {
    return core::try_utf8_to_wide(
        std::string_view(current->text->value).substr(0, byte));
  };
  const auto caret = offset(current->text->caret);
  const auto anchor = offset(current->text->anchor);
  if (!caret || !anchor) return std::nullopt;
  info.caret = caret->size();
  info.anchor = anchor->size();
  // Visible runs may omit offscreen text, so each glyph's UTF-16 offsets come
  // from the document prefix before its own run, never from accumulated runs.
  for (const auto &run : current->text->runs) {
    const auto begin = offset(run.begin);
    const auto end = offset(run.end);
    if (!begin || !end || begin->size() >= end->size()) return std::nullopt;
    info.glyphs.push_back({begin->size(), end->size(), run.bounds});
  }
  info.node = std::move(*current);
  return info;
}

HRESULT make_text_pattern(const std::shared_ptr<Context> &context, const NodeId id,
                           IUnknown **result) {
  *result = new (std::nothrow) TextProvider(context, id);
  return *result != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT select(const std::shared_ptr<Context> &context, const NodeId id,
                Span span, const TextInfo &info) {
  span = clipped(span, info.value);
  const auto begin = core::try_wide_to_utf8(
      std::wstring_view(info.value).substr(0, span.begin));
  const auto end = core::try_wide_to_utf8(
      std::wstring_view(info.value).substr(0, span.end));
  if (!begin || !end) return E_INVALIDARG;
  return context->dispatch({id, ActionKind::select_text, {},
                             end->size(), begin->size()});
}
} // namespace layerc::editor::a11y::uia

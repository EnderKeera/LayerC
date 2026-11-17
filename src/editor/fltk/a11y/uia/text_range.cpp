#include "text.hpp"

#include <algorithm>
#include <climits>
#include <cwctype>
#include <iterator>
#include <vector>

namespace layerc::editor::a11y::uia {
namespace {
std::size_t aligned(std::size_t offset, const std::wstring &text) {
  offset = std::min(offset, text.size());
  if (offset != 0 && offset < text.size() &&
      text[offset] >= 0xdc00 && text[offset] <= 0xdfff) --offset;
  return offset;
}

std::vector<std::size_t> boundaries(const std::wstring &text, TextUnit unit) {
  std::vector<std::size_t> result{0};
  if (unit == TextUnit_Character) {
    for (std::size_t i = 0; i < text.size();) {
      i += text[i] >= 0xd800 && text[i] <= 0xdbff ? 2 : 1;
      result.push_back(i);
    }
  } else if (unit == TextUnit_Word) {
    for (std::size_t i = 1; i < text.size(); ++i)
      if (std::iswspace(text[i - 1]) && !std::iswspace(text[i]) &&
          aligned(i, text) == i) result.push_back(i);
  } else if (unit == TextUnit_Line || unit == TextUnit_Paragraph) {
    for (std::size_t i = 1; i < text.size(); ++i)
      if (text[i - 1] == L'\n') result.push_back(i);
  }
  if (result.back() != text.size()) result.push_back(text.size());
  return result;
}

bool valid_unit(TextUnit unit) {
  return unit >= TextUnit_Character && unit <= TextUnit_Document;
}

bool valid_endpoint(TextPatternRangeEndpoint endpoint) {
  return endpoint == TextPatternRangeEndpoint_Start ||
         endpoint == TextPatternRangeEndpoint_End;
}

HRESULT attribute(const TextInfo &info, TEXTATTRIBUTEID id, VARIANT *result) {
  VariantInit(result);
  if (id == UIA_IsReadOnlyAttributeId || id == UIA_IsHiddenAttributeId) {
    result->vt = VT_BOOL;
    const bool value = id == UIA_IsReadOnlyAttributeId
                           ? info.node.states.read_only : !info.node.states.showing;
    result->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
  }
  result->vt = VT_UNKNOWN;
  return UiaGetReservedNotSupportedValue(&result->punkVal);
}

std::vector<Bounds> rectangles(const TextInfo &info, const Span span) {
  std::vector<Bounds> result;
  if (!info.node.states.showing) return result;
  const auto append = [&](const Bounds &rectangle) {
    const auto &clip = info.node.bounds;
    const double left = std::max(rectangle.left, clip.left);
    const double top = std::max(rectangle.top, clip.top);
    const double right = std::min(rectangle.left + rectangle.width, clip.left + clip.width);
    const double bottom = std::min(rectangle.top + rectangle.height, clip.top + clip.height);
    if (right <= left || bottom <= top) return;
    if (!result.empty()) {
      auto &last = result.back();
      if (last.top == top && last.height == bottom - top &&
          left <= last.left + last.width + 1) {
        last.width = std::max(last.left + last.width, right) - last.left;
        return;
      }
    }
    result.push_back({left, top, right - left, bottom - top});
  };
  if (span.begin == span.end) {
    auto caret = info.node.text->caret_bounds;
    if (span.begin != info.caret) {
      for (const auto &glyph : info.glyphs) {
        if (glyph.begin == span.begin) {
          caret = glyph.bounds; caret.width = 1; break;
        }
        if (glyph.end == span.begin) {
          caret = glyph.bounds; caret.left += caret.width; caret.width = 1;
        }
      }
    }
    append(caret);
  } else {
    for (const auto &glyph : info.glyphs)
      if (glyph.end > span.begin && glyph.begin < span.end) append(glyph.bounds);
  }
  return result;
}

class TextRange final : public ITextRangeProvider {
public:
  TextRange(std::shared_ptr<Context> context, NodeId id, Span span)
      : context_(std::move(context)), id_(id), span_(span) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IAgileObject) &&
        !IsEqualIID(iid, IID_ITextRangeProvider)) return E_NOINTERFACE;
    *result = static_cast<ITextRangeProvider *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = --references_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE Clone(ITextRangeProvider **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      *result = make_range(context_, id_, clipped(read(), info->value));
      return *result != nullptr ? S_OK : E_OUTOFMEMORY;
    });
  }
  HRESULT STDMETHODCALLTYPE Compare(ITextRangeProvider *other, BOOL *result) override {
    if (result == nullptr) return E_POINTER;
    *result = FALSE;
    return guarded([&]() -> HRESULT {
      auto *range = compatible(other);
      if (range == nullptr) return E_INVALIDARG;
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto first = clipped(read(), info->value);
      const auto second = clipped(range->read(), info->value);
      *result = first.begin == second.begin && first.end == second.end;
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE CompareEndpoints(TextPatternRangeEndpoint endpoint,
      ITextRangeProvider *other, TextPatternRangeEndpoint target, int *result) override {
    if (result == nullptr) return E_POINTER;
    *result = 0;
    if (!valid_endpoint(endpoint) || !valid_endpoint(target)) return E_INVALIDARG;
    return guarded([&]() -> HRESULT {
      auto *range = compatible(other);
      if (range == nullptr) return E_INVALIDARG;
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto first = clipped(read(), info->value);
      const auto second = clipped(range->read(), info->value);
      const auto a = endpoint == TextPatternRangeEndpoint_Start ? first.begin : first.end;
      const auto b = target == TextPatternRangeEndpoint_Start ? second.begin : second.end;
      *result = static_cast<int>(std::clamp(
          static_cast<long long>(a) - static_cast<long long>(b),
          static_cast<long long>(INT_MIN), static_cast<long long>(INT_MAX)));
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(TextUnit unit) override {
    if (!valid_unit(unit)) return E_INVALIDARG;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto points = boundaries(info->value, unit);
      auto span = clipped(read(), info->value);
      auto next = std::upper_bound(points.begin(), points.end(), span.begin);
      if (next == points.end() && points.size() > 1) next = std::prev(points.end());
      if (next == points.begin() || points.size() < 2) span = {0, 0};
      else span = {*std::prev(next), *next};
      write(span);
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID id, VARIANT value,
      BOOL backwards, ITextRangeProvider **result) override {
    (void)backwards; // Every supported plain-input attribute is uniform.
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      VARIANT actual;
      const auto status = attribute(*info, id, &actual);
      if (FAILED(status)) return status;
      const bool matches = actual.vt == VT_BOOL && value.vt == VT_BOOL &&
                           actual.boolVal == value.boolVal;
      VariantClear(&actual);
      if (matches) {
        *result = make_range(context_, id_, clipped(read(), info->value));
        if (*result == nullptr) return E_OUTOFMEMORY;
      }
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE FindText(BSTR text, BOOL backwards, BOOL ignore_case,
                                      ITextRangeProvider **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      // Validated where the value is used: /analyze cannot carry the caller's
      // null fact into the closure, and a null BSTR must never reach
      // CompareStringOrdinal.
      if (text == nullptr || SysStringLen(text) == 0) return E_INVALIDARG;
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto span = clipped(read(), info->value);
      const auto length = static_cast<std::size_t>(SysStringLen(text));
      if (length > static_cast<std::size_t>(INT_MAX)) return E_INVALIDARG;
      if (length > span.end - span.begin) return S_OK;
      auto position = backwards ? span.end - length : span.begin;
      for (;;) {
        if (aligned(position, info->value) == position &&
            aligned(position + length, info->value) == position + length &&
            CompareStringOrdinal(info->value.data() + position, static_cast<int>(length),
                                 text, static_cast<int>(length), ignore_case) == CSTR_EQUAL) {
          *result = make_range(context_, id_, {position, position + length});
          return *result != nullptr ? S_OK : E_OUTOFMEMORY;
        }
        if (backwards) { if (position == span.begin) break; --position; }
        else { if (position + length == span.end) break; ++position; }
      }
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID id, VARIANT *result) override {
    if (result == nullptr) return E_POINTER;
    VariantInit(result);
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      return info ? attribute(*info, id, result) : UIA_E_ELEMENTNOTAVAILABLE;
    });
  }
  HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto boxes = rectangles(*info, clipped(read(), info->value));
      auto *array = SafeArrayCreateVector(VT_R8, 0, static_cast<ULONG>(boxes.size() * 4));
      if (array == nullptr) return E_OUTOFMEMORY;
      LONG index{};
      for (const auto &box : boxes) {
        for (double value : {box.left, box.top, box.width, box.height}) {
          const auto status = SafeArrayPutElement(array, &index, &value);
          if (FAILED(status)) { SafeArrayDestroy(array); return status; }
          ++index;
        }
      }
      *result = array;
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE GetEnclosingElement(IRawElementProviderSimple **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      if (!node(context_, id_)) return UIA_E_ELEMENTNOTAVAILABLE;
      *result = make_provider(context_, id_);
      return *result != nullptr ? S_OK : E_OUTOFMEMORY;
    });
  }
  HRESULT STDMETHODCALLTYPE GetText(int maximum, BSTR *result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (maximum < -1) return E_INVALIDARG;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      auto span = clipped(read(), info->value);
      if (maximum >= 0) span.end = aligned(
          std::min(span.end, span.begin + static_cast<std::size_t>(maximum)), info->value);
      *result = SysAllocStringLen(info->value.data() + span.begin,
                                  static_cast<UINT>(span.end - span.begin));
      return *result != nullptr ? S_OK : E_OUTOFMEMORY;
    });
  }
  HRESULT STDMETHODCALLTYPE Move(TextUnit unit, int count, int *moved) override {
    if (moved == nullptr) return E_POINTER;
    *moved = 0;
    if (!valid_unit(unit)) return E_INVALIDARG;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      if (count == 0) return S_OK;
      auto span = clipped(read(), info->value);
      const auto points = boundaries(info->value, unit);
      if (points.size() < 2) return S_OK;
      const bool degenerate = span.begin == span.end;
      const auto next = std::upper_bound(points.begin(), points.end(), span.begin);
      auto index = static_cast<long long>(std::distance(points.begin(), next)) - 1;
      const auto last = static_cast<long long>(points.size()) - (degenerate ? 1 : 2);
      index = std::min(index, last);
      const auto destination = std::clamp(index + count, 0LL, last);
      *moved = static_cast<int>(destination - index);
      const auto start = static_cast<std::size_t>(destination);
      write({points[start], degenerate ? points[start] : points[start + 1]});
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(TextPatternRangeEndpoint endpoint,
      TextUnit unit, int count, int *moved) override {
    if (moved == nullptr) return E_POINTER;
    *moved = 0;
    if (!valid_endpoint(endpoint) || !valid_unit(unit)) return E_INVALIDARG;
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      if (count == 0) return S_OK;
      auto span = clipped(read(), info->value);
      const auto points = boundaries(info->value, unit);
      auto &position = endpoint == TextPatternRangeEndpoint_Start ? span.begin : span.end;
      const auto cursor = count > 0
          ? std::prev(std::upper_bound(points.begin(), points.end(), position))
          : std::lower_bound(points.begin(), points.end(), position);
      const auto index = static_cast<long long>(std::distance(points.begin(), cursor));
      const auto destination = std::clamp(index + count, 0LL,
                                           static_cast<long long>(points.size()) - 1);
      *moved = static_cast<int>(destination - index);
      position = points[static_cast<std::size_t>(destination)];
      if (span.begin > span.end) {
        if (endpoint == TextPatternRangeEndpoint_Start) span.end = span.begin;
        else span.begin = span.end;
      }
      write(span);
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE MoveEndpointByRange(TextPatternRangeEndpoint endpoint,
      ITextRangeProvider *other, TextPatternRangeEndpoint target) override {
    if (!valid_endpoint(endpoint) || !valid_endpoint(target)) return E_INVALIDARG;
    return guarded([&]() -> HRESULT {
      auto *range = compatible(other);
      if (range == nullptr) return E_INVALIDARG;
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      auto span = clipped(read(), info->value);
      const auto destination = clipped(range->read(), info->value);
      const auto position = target == TextPatternRangeEndpoint_Start
                                ? destination.begin : destination.end;
      if (endpoint == TextPatternRangeEndpoint_Start) {
        span.begin = position;
        span.end = std::max(span.end, span.begin);
      } else {
        span.end = position;
        span.begin = std::min(span.begin, span.end);
      }
      write(span);
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE Select() override {
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      return info ? select(context_, id_, read(), *info) : UIA_E_ELEMENTNOTAVAILABLE;
    });
  }
  HRESULT STDMETHODCALLTYPE AddToSelection() override {
    // Single-selection fields cannot add a disjoint second selection.
    return context_->connected.load() ? UIA_E_INVALIDOPERATION : UIA_E_ELEMENTNOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE RemoveFromSelection() override {
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto remove = clipped(read(), info->value);
      auto selection = Span{std::min(info->caret, info->anchor),
                             std::max(info->caret, info->anchor)};
      if (remove.end <= selection.begin || remove.begin >= selection.end) return S_OK;
      if (remove.begin <= selection.begin && remove.end >= selection.end)
        selection = {info->caret, info->caret};
      else if (remove.begin <= selection.begin) selection.begin = remove.end;
      else if (remove.end >= selection.end) selection.end = remove.begin;
      else return UIA_E_INVALIDOPERATION;
      return select(context_, id_, selection, *info);
    });
  }
  HRESULT STDMETHODCALLTYPE ScrollIntoView(BOOL) override {
    return guarded([&]() -> HRESULT {
      const auto info = text_info(context_, id_);
      if (!info) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto span = clipped(read(), info->value);
      const auto &clip = info->node.bounds;
      bool visible = info->node.states.showing;
      for (const auto &glyph : info->glyphs) {
        if (glyph.end <= span.begin || glyph.begin >= span.end) continue;
        const auto &g = glyph.bounds;
        visible = visible && g.left >= clip.left && g.top >= clip.top &&
            g.left + g.width <= clip.left + clip.width &&
            g.top + g.height <= clip.top + clip.height;
      }
      // Stock FLTK inputs have no non-selection horizontal scroll operation.
      // Never move the caret/selection, synthesize input, or claim an offscreen
      // range was revealed when the widget cannot perform that operation.
      return visible ? S_OK : UIA_E_NOTSUPPORTED;
    });
  }
  HRESULT STDMETHODCALLTYPE GetChildren(SAFEARRAY **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    *result = SafeArrayCreateVector(VT_UNKNOWN, 0, 0);
    return *result != nullptr ? S_OK : E_OUTOFMEMORY;
  }
private:
  TextRange *compatible(ITextRangeProvider *other) const {
    auto *range = dynamic_cast<TextRange *>(other);
    return range != nullptr && range->context_ == context_ && range->id_ == id_
               ? range : nullptr;
  }
  Span read() const { const std::lock_guard lock(mutex_); return span_; }
  void write(Span span) { const std::lock_guard lock(mutex_); span_ = span; }
  std::atomic<ULONG> references_{1};
  std::shared_ptr<Context> context_;
  NodeId id_;
  mutable std::mutex mutex_;
  Span span_;
};
} // namespace

Span clipped(Span span, const std::wstring &text) {
  span.begin = aligned(span.begin, text);
  span.end = std::max(span.begin, aligned(span.end, text));
  return span;
}
ITextRangeProvider *make_range(const std::shared_ptr<Context> &context,
                               const NodeId id, const Span span) {
  return new (std::nothrow) TextRange(context, id, span);
}
} // namespace layerc::editor::a11y::uia

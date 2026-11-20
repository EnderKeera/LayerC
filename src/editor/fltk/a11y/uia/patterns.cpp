#include "context.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <vector>

// Tear-off pattern objects. UI Automation asks a provider for one pattern at a
// time, so each object below carries only the pattern it was made for, owns its
// own reference count, and reads the published node on every call: an element
// that stopped publishing the pattern fails with the automation error instead of
// answering from a stale copy.

namespace layerc::editor::a11y::uia {
namespace {
// UI Automation's UIA_ScrollPatternNoScroll, spelled out because not every SDK
// header set declares the constant.
constexpr double no_scroll = -1.0;
#ifdef UIA_ScrollPatternNoScroll
static_assert(UIA_ScrollPatternNoScroll == no_scroll);
#endif
// Toggle: check boxes, radio buttons and toggle menu items publish a check
// state, and the action runs the widget's own toggle through the application.
class TogglePattern final : public IToggleProvider {
public:
  TogglePattern(std::shared_ptr<Context> context, const NodeId id)
      : context_(std::move(context)), id_(id) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IToggleProvider))
      *result = static_cast<IToggleProvider *>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = --references_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState *result) override {
    if (result == nullptr) return E_POINTER;
    *result = ToggleState_Off;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      *result = current->states.mixed   ? ToggleState_Indeterminate
                : current->states.checked ? ToggleState_On
                                          : ToggleState_Off;
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE Toggle() override {
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      if (current->states.disabled) return UIA_E_ELEMENTNOTENABLED;
      // The widget's own callback flips the state from FLTK's loop, exactly as
      // a click does; nothing here writes the value behind the widget's back.
      return context_->dispatch({id_, ActionKind::toggle});
    });
  }

private:
  std::atomic<ULONG> references_{1};
  std::shared_ptr<Context> context_;
  NodeId id_;
};

// Range value: a valuator's own limits, step and read-only state.
class RangeValue final : public IRangeValueProvider {
public:
  RangeValue(std::shared_ptr<Context> context, const NodeId id)
      : context_(std::move(context)), id_(id) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IRangeValueProvider))
      *result = static_cast<IRangeValueProvider *>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = --references_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE SetValue(double value) override {
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      if (!current->range) return UIA_E_ELEMENTNOTAVAILABLE;
      if (current->states.disabled) return UIA_E_ELEMENTNOTENABLED;
      if (current->states.read_only) return UIA_E_INVALIDOPERATION;
      // A value outside the widget's own range is the client's mistake, not a
      // clamp: UI Automation reports E_INVALIDARG for it.
      if (value < current->range->minimum || value > current->range->maximum ||
          !std::isfinite(value))
        return E_INVALIDARG;
      return context_->dispatch({id_, ActionKind::set_number, {}, 0, 0, false, value});
    });
  }
  HRESULT STDMETHODCALLTYPE get_Value(double *result) override { return read(result, &Range::value); }
  HRESULT STDMETHODCALLTYPE get_Maximum(double *result) override {
    return read(result, &Range::maximum);
  }
  HRESULT STDMETHODCALLTYPE get_Minimum(double *result) override {
    return read(result, &Range::minimum);
  }
  HRESULT STDMETHODCALLTYPE get_LargeChange(double *result) override {
    return read(result, &Range::step);
  }
  HRESULT STDMETHODCALLTYPE get_SmallChange(double *result) override {
    return read(result, &Range::step);
  }
  HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL *result) override {
    if (result == nullptr) return E_POINTER;
    *result = VARIANT_FALSE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      *result = current->states.read_only ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
    });
  }

private:
  HRESULT read(double *result, double Range::*member) {
    if (result == nullptr) return E_POINTER;
    *result = 0;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current || !current->range) return UIA_E_ELEMENTNOTAVAILABLE;
      *result = (*current->range).*member;
      return S_OK;
    });
  }
  std::atomic<ULONG> references_{1};
  std::shared_ptr<Context> context_;
  NodeId id_;
};

// Selection: a list, tab list or radio group hands out its chosen children.
class Selection final : public ISelectionProvider {
public:
  Selection(std::shared_ptr<Context> context, const NodeId id)
      : context_(std::move(context)), id_(id) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ISelectionProvider))
      *result = static_cast<ISelectionProvider *>(this);
    else return E_NOINTERFACE;
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
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      std::vector<IRawElementProviderSimple *> chosen;
      for (const auto child : current->children) {
        const auto candidate = node(context_, child);
        if (!candidate || !selection_member(current->role, *candidate) ||
            !selected_item(*candidate))
          continue;
        if (auto *provider = make_provider(context_, child)) chosen.push_back(provider);
      }
      auto *array = SafeArrayCreateVector(VT_UNKNOWN, 0, static_cast<ULONG>(chosen.size()));
      if (array == nullptr) {
        for (auto *provider : chosen) provider->Release();
        return E_OUTOFMEMORY;
      }
      auto *slots = static_cast<IUnknown **>(array->pvData);
      for (std::size_t index = 0; index < chosen.size(); ++index)
        slots[index] = chosen[index]; // SafeArrayDestroy releases them.
      *result = array;
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL *result) override {
    if (result == nullptr) return E_POINTER;
    *result = VARIANT_FALSE;
    return connected() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL *result) override {
    if (result == nullptr) return E_POINTER;
    *result = VARIANT_FALSE;
    return connected() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }

private:
  [[nodiscard]] bool connected() const { return context_->connected.load(); }
  std::atomic<ULONG> references_{1};
  std::shared_ptr<Context> context_;
  NodeId id_;
};

// Selection item: a row, tab or radio button. Selecting an already chosen item
// is the same selection again; a second item in a single-selection list is a
// client error, which the widget's own behaviour would silently resolve.
class SelectionItem final : public ISelectionItemProvider {
public:
  SelectionItem(std::shared_ptr<Context> context, const NodeId id)
      : context_(std::move(context)), id_(id) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ISelectionItemProvider))
      *result = static_cast<ISelectionItemProvider *>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = --references_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE Select() override {
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      if (current->states.disabled) return UIA_E_ELEMENTNOTENABLED;
      return context_->dispatch({id_, ActionKind::invoke});
    });
  }
  HRESULT STDMETHODCALLTYPE AddToSelection() override {
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      if (current->states.disabled) return UIA_E_ELEMENTNOTENABLED;
      // These lists hold one selection, so adding to any existing selection is
      // not expressible, not even the item's own; only an empty one becomes Select.
      // A chosen radio is checked rather than selected, and only items of the
      // same kind share a selection, so a checked check box beside radios does not.
      std::size_t chosen = 0;
      for (const auto child : siblings(*current)) {
        const auto candidate = node(context_, child);
        if (candidate && candidate->role == current->role && selected_item(*candidate))
          ++chosen;
      }
      if (chosen > 0) return UIA_E_INVALIDOPERATION;
      return context_->dispatch({id_, ActionKind::invoke});
    });
  }
  HRESULT STDMETHODCALLTYPE RemoveFromSelection() override {
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      if (current->states.disabled) return UIA_E_ELEMENTNOTENABLED;
      // A selected item cannot be deselected in a list that always keeps one
      // choice; an unselected item already satisfies the request.
      if (selected_item(*current)) return UIA_E_INVALIDOPERATION;
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL *result) override {
    if (result == nullptr) return E_POINTER;
    *result = VARIANT_FALSE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      *result = selected_item(*current) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE get_SelectionContainer(
      IRawElementProviderSimple **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current || current->parent == no_node) return UIA_E_ELEMENTNOTAVAILABLE;
      *result = make_provider(context_, current->parent);
      return *result != nullptr ? S_OK : E_OUTOFMEMORY;
    });
  }

private:
  [[nodiscard]] std::vector<NodeId> siblings(const Node &current) const {
    const auto parent = current.parent == no_node ? std::optional<Node>{}
                                                  : node(context_, current.parent);
    return parent ? parent->children : std::vector<NodeId>{};
  }
  std::atomic<ULONG> references_{1};
  std::shared_ptr<Context> context_;
  NodeId id_;
};

// Expand/collapse: only a node that publishes an expansion state gets one, so
// a leaf still reports the pattern as absent.
class ExpandCollapse final : public IExpandCollapseProvider {
public:
  ExpandCollapse(std::shared_ptr<Context> context, const NodeId id)
      : context_(std::move(context)), id_(id) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IExpandCollapseProvider))
      *result = static_cast<IExpandCollapseProvider *>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = --references_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE Expand() override { return change(ActionKind::expand, false); }
  HRESULT STDMETHODCALLTYPE Collapse() override { return change(ActionKind::collapse, true); }
  HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState *result) override {
    if (result == nullptr) return E_POINTER;
    *result = ExpandCollapseState_Collapsed;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      *result = current->states.expanded ? ExpandCollapseState_Expanded
                                         : ExpandCollapseState_Collapsed;
      return S_OK;
    });
  }

private:
  // Expanding a menu bar title opens a popup that runs its own loop, so the
  // action lands from FLTK's loop; collapsing closes it in the call.
  HRESULT change(const ActionKind kind, const bool collapse) {
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      if (current->states.disabled) return UIA_E_ELEMENTNOTENABLED;
      const bool expanded = current->states.expanded;
      if (collapse ? !expanded : expanded) return UIA_E_INVALIDOPERATION;
      return context_->dispatch({id_, kind});
    });
  }
  std::atomic<ULONG> references_{1};
  std::shared_ptr<Context> context_;
  NodeId id_;
};

// Scroll: a list or scroll pane in percentages, with -1 for an axis that
// cannot move, which UI Automation calls UIA_ScrollPatternNoScroll.
class ScrollPattern final : public IScrollProvider {
public:
  ScrollPattern(std::shared_ptr<Context> context, const NodeId id)
      : context_(std::move(context)), id_(id) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IScrollProvider))
      *result = static_cast<IScrollProvider *>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = --references_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE Scroll(ScrollAmount horizontal, ScrollAmount vertical) override {
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current || !current->scroll) return UIA_E_ELEMENTNOTAVAILABLE;
      if (current->states.disabled) return UIA_E_ELEMENTNOTENABLED;
      const auto move = [](double position, ScrollAmount amount) -> double {
        const double step = amount == ScrollAmount_LargeIncrement ? 10.0
                            : amount == ScrollAmount_SmallIncrement ? 1.0
                            : amount == ScrollAmount_LargeDecrement ? -10.0
                            : amount == ScrollAmount_SmallDecrement ? -1.0
                                                                    : 0.0;
        return step == 0.0 ? -1.0 : std::clamp(position + step, 0.0, 100.0);
      };
      const auto &scroll = *current->scroll;
      const double h = scroll.horizontal < 0 ? -1.0 : move(scroll.horizontal, horizontal);
      const double v = scroll.vertical < 0 ? -1.0 : move(scroll.vertical, vertical);
      if (h < 0 && v < 0) return S_OK; // NoScroll on both axes: nothing to do.
      return context_->dispatch({id_, ActionKind::scroll, {}, 0, 0, false, 0, h, v});
    });
  }
  HRESULT STDMETHODCALLTYPE SetScrollPercent(double horizontal, double vertical) override {
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current || !current->scroll) return UIA_E_ELEMENTNOTAVAILABLE;
      if (current->states.disabled) return UIA_E_ELEMENTNOTENABLED;
      const auto valid = [](double percent) {
        return percent == no_scroll ||
               (std::isfinite(percent) && percent >= 0.0 && percent <= 100.0);
      };
      if (!valid(horizontal) || !valid(vertical)) return E_INVALIDARG;
      if (current->scroll->horizontal < 0 && horizontal != no_scroll)
        return E_INVALIDARG;
      if (current->scroll->vertical < 0 && vertical != no_scroll)
        return E_INVALIDARG;
      if (horizontal == no_scroll && vertical == no_scroll)
        return S_OK;
      return context_->dispatch({id_, ActionKind::scroll, {}, 0, 0, false, 0, horizontal,
                                 vertical});
    });
  }
  HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double *result) override {
    return axis(result, &Scroll::horizontal);
  }
  HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double *result) override {
    return axis(result, &Scroll::vertical);
  }
  HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double *result) override {
    return axis(result, &Scroll::horizontal_view);
  }
  HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double *result) override {
    return axis(result, &Scroll::vertical_view);
  }
  HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(BOOL *result) override {
    return scrollable(result, true);
  }
  HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(BOOL *result) override {
    return scrollable(result, false);
  }

private:
  HRESULT axis(double *result, double Scroll::*member) {
    if (result == nullptr) return E_POINTER;
    *result = no_scroll;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current || !current->scroll) return UIA_E_ELEMENTNOTAVAILABLE;
      const double value = (*current->scroll).*member;
      *result = value < 0 ? no_scroll : value;
      return S_OK;
    });
  }
  HRESULT scrollable(BOOL *result, const bool horizontal) {
    if (result == nullptr) return E_POINTER;
    *result = VARIANT_FALSE;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current || !current->scroll) return UIA_E_ELEMENTNOTAVAILABLE;
      const double value = horizontal ? current->scroll->horizontal : current->scroll->vertical;
      *result = value < 0 ? VARIANT_FALSE : VARIANT_TRUE;
      return S_OK;
    });
  }
  std::atomic<ULONG> references_{1};
  std::shared_ptr<Context> context_;
  NodeId id_;
};

}

HRESULT make_toggle_pattern(const std::shared_ptr<Context> &context, const NodeId id,
                            IUnknown **result) {
  *result = new (std::nothrow) TogglePattern(context, id);
  return *result != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT make_range_value_pattern(const std::shared_ptr<Context> &context, const NodeId id,
                                 IUnknown **result) {
  *result = new (std::nothrow) RangeValue(context, id);
  return *result != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT make_selection_pattern(const std::shared_ptr<Context> &context, const NodeId id,
                               IUnknown **result) {
  *result = new (std::nothrow) Selection(context, id);
  return *result != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT make_selection_item_pattern(const std::shared_ptr<Context> &context, const NodeId id,
                                    IUnknown **result) {
  *result = new (std::nothrow) SelectionItem(context, id);
  return *result != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT make_expand_collapse_pattern(const std::shared_ptr<Context> &context, const NodeId id,
                                     IUnknown **result) {
  *result = new (std::nothrow) ExpandCollapse(context, id);
  return *result != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT make_scroll_pattern(const std::shared_ptr<Context> &context, const NodeId id,
                            IUnknown **result) {
  *result = new (std::nothrow) ScrollPattern(context, id);
  return *result != nullptr ? S_OK : E_OUTOFMEMORY;
}
} // namespace layerc::editor::a11y::uia

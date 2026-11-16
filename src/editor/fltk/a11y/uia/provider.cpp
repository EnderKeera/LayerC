#include "context.hpp"

#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <string>

namespace layerc::editor::a11y::uia {
namespace {
HRESULT string_value(const std::string &text, BSTR *result) {
  const auto wide = core::try_utf8_to_wide(text);
  if (!wide) return E_INVALIDARG;
  *result = SysAllocStringLen(wide->data(), static_cast<UINT>(wide->size()));
  return *result != nullptr ? S_OK : E_OUTOFMEMORY;
}

CONTROLTYPEID control_type(const Role role) {
  switch (role) {
  case Role::application: case Role::window: case Role::dialog:
    return UIA_WindowControlTypeId;
  case Role::button: return UIA_ButtonControlTypeId;
  case Role::check_box: return UIA_CheckBoxControlTypeId;
  case Role::radio_button: return UIA_RadioButtonControlTypeId;
  case Role::text_field: return UIA_EditControlTypeId;
  case Role::number_field: return UIA_SpinnerControlTypeId;
  case Role::drop_down: return UIA_ComboBoxControlTypeId;
  case Role::list: return UIA_ListControlTypeId;
  case Role::list_row: return UIA_ListItemControlTypeId;
  case Role::menu_bar: return UIA_MenuBarControlTypeId;
  case Role::menu: return UIA_MenuControlTypeId;
  case Role::menu_item: return UIA_MenuItemControlTypeId;
  case Role::label: case Role::status: return UIA_TextControlTypeId;
  case Role::image: return UIA_ImageControlTypeId;
  case Role::group: return UIA_GroupControlTypeId;
  }
  return UIA_CustomControlTypeId;
}

class Provider final : public IRawElementProviderSimple,
                       public IRawElementProviderFragment,
                       public IRawElementProviderFragmentRoot,
                       public IInvokeProvider, public IValueProvider {
public:
  Provider(std::shared_ptr<Context> context, const NodeId id)
      : context_(std::move(context)), id_(id) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) ||
        IsEqualIID(iid, IID_IAgileObject) ||
        IsEqualIID(iid, IID_IRawElementProviderSimple))
      *result = static_cast<IRawElementProviderSimple *>(this);
    else if (IsEqualIID(iid, IID_IRawElementProviderFragment))
      *result = static_cast<IRawElementProviderFragment *>(this);
    else if (IsEqualIID(iid, IID_IRawElementProviderFragmentRoot) && is_root())
      *result = static_cast<IRawElementProviderFragmentRoot *>(this);
    else if (IsEqualIID(iid, IID_IInvokeProvider))
      *result = static_cast<IInvokeProvider *>(this);
    else if (IsEqualIID(iid, IID_IValueProvider))
      *result = static_cast<IValueProvider *>(this);
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

  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions *result) override {
    if (result == nullptr) return E_POINTER;
    *result = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider |
                                           ProviderOptions_UseComThreading);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern,
                                                 IUnknown **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      if (pattern == UIA_InvokePatternId &&
          (current->role == Role::button || current->role == Role::menu_item))
        return QueryInterface(IID_IInvokeProvider, reinterpret_cast<void **>(result));
      if (pattern == UIA_ValuePatternId && !current->states.password &&
          (current->text || current->role == Role::drop_down))
        return QueryInterface(IID_IValueProvider, reinterpret_cast<void **>(result));
      if (pattern == UIA_TextPatternId && current->text &&
          !current->states.password)
        return make_text_pattern(context_, id_, result);
      return S_OK; // Unsupported patterns are not advertised.
    });
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property,
                                               VARIANT *result) override {
    if (result == nullptr) return E_POINTER;
    VariantInit(result);
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto boolean = [&](bool value) {
        result->vt = VT_BOOL;
        result->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
      };
      const auto text = [&](const std::string &value) {
        result->vt = VT_BSTR;
        return string_value(value, &result->bstrVal);
      };
      switch (property) {
      case UIA_ControlTypePropertyId:
        result->vt = VT_I4;
        result->lVal = control_type(current->role);
        break;
      case UIA_NamePropertyId: return text(current->name);
      case UIA_HelpTextPropertyId: return text(current->description);
      case UIA_AutomationIdPropertyId:
        return text("layerc-" + std::to_string(id_));
      case UIA_FrameworkIdPropertyId: return text("LayerC.FLTK");
      case UIA_ValueValuePropertyId:
        if (!current->states.password)
          return text(current->text ? current->text->value : current->value);
        break;
      case UIA_IsKeyboardFocusablePropertyId:
        boolean(current->states.focusable && !current->states.disabled); break;
      case UIA_HasKeyboardFocusPropertyId: boolean(current->states.focused); break;
      case UIA_IsEnabledPropertyId: boolean(!current->states.disabled); break;
      case UIA_IsOffscreenPropertyId: boolean(!current->states.showing); break;
      case UIA_IsPasswordPropertyId: boolean(current->states.password); break;
      case UIA_IsControlElementPropertyId:
      case UIA_IsContentElementPropertyId: boolean(true); break;
      case UIA_ValueIsReadOnlyPropertyId: boolean(current->states.read_only); break;
      case UIA_LiveSettingPropertyId:
        if (current->role == Role::status) {
          result->vt = VT_I4;
          result->lVal = Polite;
        }
        break;
      case UIA_LabeledByPropertyId:
        if (!current->labelled_by.empty()) {
          result->vt = VT_UNKNOWN;
          result->punkVal = make_provider(context_, current->labelled_by.front());
          if (result->punkVal == nullptr) return E_OUTOFMEMORY;
        }
        break;
      default: break; // VT_EMPTY asks UIA for its default property value.
      }
      return S_OK;
    });
  }

  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
      IRawElementProviderSimple **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    return is_root() ? UiaHostProviderFromHwnd(context_->window, result) : S_OK;
  }

  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                       IRawElementProviderFragment **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      NodeId destination{};
      switch (direction) {
      case NavigateDirection_Parent: destination = current->parent; break;
      case NavigateDirection_FirstChild:
        if (!current->children.empty()) destination = current->children.front();
        break;
      case NavigateDirection_LastChild:
        if (!current->children.empty()) destination = current->children.back();
        break;
      case NavigateDirection_NextSibling:
      case NavigateDirection_PreviousSibling: {
        const auto parent = node(context_, current->parent);
        if (!parent) break;
        const auto found = std::find(parent->children.begin(),
                                     parent->children.end(), id_);
        if (found == parent->children.end()) break;
        if (direction == NavigateDirection_NextSibling &&
            std::next(found) != parent->children.end()) destination = *std::next(found);
        if (direction == NavigateDirection_PreviousSibling &&
            found != parent->children.begin()) destination = *std::prev(found);
        break;
      }
      default: return E_INVALIDARG;
      }
      return destination == no_node ? S_OK : make_fragment(context_, destination, result);
    });
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    if (is_root()) return S_OK;
    auto *array = SafeArrayCreateVector(VT_I4, 0, 3);
    if (array == nullptr) return E_OUTOFMEMORY;
    const int values[]{UiaAppendRuntimeId, static_cast<int>(id_ & 0x7fffffffU),
                       static_cast<int>(id_ >> 31)};
    for (LONG index = 0; index < 3; ++index) {
      int value = values[index];
      const auto status = SafeArrayPutElement(array, &index, &value);
      if (FAILED(status)) { SafeArrayDestroy(array); return status; }
    }
    *result = array;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect *result) override {
    if (result == nullptr) return E_POINTER;
    *result = {};
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto &b = current->bounds;
      *result = {b.left, b.top, b.width, b.height};
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return context_->connected.load() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE SetFocus() override {
    return guarded([&] { return context_->dispatch({id_, ActionKind::focus}); });
  }
  HRESULT STDMETHODCALLTYPE get_FragmentRoot(
      IRawElementProviderFragmentRoot **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto snapshot = context_->model.read();
      auto *provider = make_provider(context_, snapshot.root);
      if (provider == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto status = provider->QueryInterface(
          IID_IRawElementProviderFragmentRoot, reinterpret_cast<void **>(result));
      provider->Release();
      return status;
    });
  }
  HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(
      double x, double y, IRawElementProviderFragment **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto snapshot = context_->model.read();
      NodeId found{};
      std::size_t deepest{};
      for (const auto &[id, item] : snapshot.nodes) {
        const auto &b = item.bounds;
        if (!item.states.showing || x < b.left || y < b.top ||
            x >= b.left + b.width || y >= b.top + b.height) continue;
        std::size_t depth{};
        for (auto parent = item.parent; parent != no_node;
             parent = snapshot.nodes.at(parent).parent) ++depth;
        if (found == no_node || depth >= deepest) { found = id; deepest = depth; }
      }
      return found == no_node ? S_OK : make_fragment(context_, found, result);
    });
  }
  HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      if (!context_->connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
      const auto snapshot = context_->model.read();
      for (const auto &[id, item] : snapshot.nodes)
        if (item.states.focused) return make_fragment(context_, id, result);
      return S_OK;
    });
  }

  HRESULT STDMETHODCALLTYPE Invoke() override {
    return guarded([&] { return context_->dispatch({id_, ActionKind::invoke}); });
  }
  HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override {
    if (value == nullptr) return E_INVALIDARG;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      if (current->states.disabled) return UIA_E_ELEMENTNOTENABLED;
      if (current->states.read_only || current->states.password) return E_ACCESSDENIED;
      const auto utf8 = core::try_wide_to_utf8(value);
      if (!utf8) return E_INVALIDARG;
      return context_->dispatch({id_, ActionKind::set_value, *utf8});
    });
  }
  HRESULT STDMETHODCALLTYPE get_Value(BSTR *result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      if (current->states.password) return E_ACCESSDENIED;
      return string_value(current->text ? current->text->value : current->value, result);
    });
  }
  HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL *result) override {
    if (result == nullptr) return E_POINTER;
    return guarded([&]() -> HRESULT {
      const auto current = node(context_, id_);
      if (!current) return UIA_E_ELEMENTNOTAVAILABLE;
      *result = current->states.read_only ? TRUE : FALSE;
      return S_OK;
    });
  }
private:
  bool is_root() const noexcept { return id_ == 1; }
  std::atomic<ULONG> references_{1};
  std::shared_ptr<Context> context_;
  NodeId id_;
};
} // namespace

IRawElementProviderSimple *make_provider(const std::shared_ptr<Context> &context,
                                         const NodeId id) {
  const std::lock_guard lock(context->providers_mutex_);
  if (!context->connected.load() || id == no_node) return nullptr;
  if (const auto found = context->providers_.find(id);
      found != context->providers_.end()) {
    found->second->AddRef();
    return found->second;
  }
  auto *provider = new (std::nothrow) Provider(context, id);
  if (provider == nullptr) return nullptr;
  try { context->providers_.emplace(id, provider); }
  catch (...) { provider->Release(); throw; }
  provider->AddRef(); // The cache owns the original reference until disconnect.
  return provider;
}

HRESULT make_fragment(const std::shared_ptr<Context> &context, const NodeId id,
                       IRawElementProviderFragment **result) {
  auto *provider = make_provider(context, id);
  if (provider == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;
  const auto status = provider->QueryInterface(
      IID_IRawElementProviderFragment, reinterpret_cast<void **>(result));
  provider->Release();
  return status;
}
} // namespace layerc::editor::a11y::uia

#include "context.hpp"

#include "layerc/core/utf8.hpp"

#include <cstring>
#include <memory>
#include <optional>
#include <string>

namespace layerc::editor::a11y::uia {
namespace {
void property(IRawElementProviderSimple *provider, PROPERTYID id,
               const std::string &before, const std::string &after) {
  if (before == after) return;
  const auto old_text = core::try_utf8_to_wide(before);
  const auto new_text = core::try_utf8_to_wide(after);
  if (!old_text || !new_text) return;
  VARIANT old_value{}, new_value{};
  old_value.vt = new_value.vt = VT_BSTR;
  old_value.bstrVal = SysAllocStringLen(old_text->data(), static_cast<UINT>(old_text->size()));
  new_value.bstrVal = SysAllocStringLen(new_text->data(), static_cast<UINT>(new_text->size()));
  if (old_value.bstrVal != nullptr && new_value.bstrVal != nullptr)
    (void)UiaRaiseAutomationPropertyChangedEvent(provider, id, old_value, new_value);
  VariantClear(&old_value);
  VariantClear(&new_value);
}

void integer(IRawElementProviderSimple *provider, PROPERTYID id, int before, int after) {
  if (before == after) return;
  VARIANT old_value{}, new_value{};
  old_value.vt = new_value.vt = VT_I4;
  old_value.lVal = before;
  new_value.lVal = after;
  (void)UiaRaiseAutomationPropertyChangedEvent(provider, id, old_value, new_value);
}
void real(IRawElementProviderSimple *provider, PROPERTYID id, double before, double after) {
  if (before == after) return;
  VARIANT old_value{}, new_value{};
  old_value.vt = new_value.vt = VT_R8;
  old_value.dblVal = before;
  new_value.dblVal = after;
  (void)UiaRaiseAutomationPropertyChangedEvent(provider, id, old_value, new_value);
}

using Notify = HRESULT(WINAPI *)(IRawElementProviderSimple *, NotificationKind,
                                  NotificationProcessing, BSTR, BSTR);
Notify notification_function() {
  static const auto function = [] {
    const auto module = GetModuleHandleW(L"uiautomationcore.dll");
    const auto procedure = module == nullptr ? nullptr
        : GetProcAddress(module, "UiaRaiseNotificationEvent");
    Notify result{};
    static_assert(sizeof(result) == sizeof(procedure));
    std::memcpy(&result, &procedure, sizeof(result));
    return result;
  }();
  return function;
}
} // namespace

void raise_changes(const std::shared_ptr<Context> &context,
                     const Snapshot &before, const Snapshot &after) {
  if (!UiaClientsAreListening() || !context->connected.load()) return;
  for (const auto &[id, current] : after.nodes) {
    const auto old = before.nodes.find(id);
    auto *provider = make_provider(context, id);
    if (provider == nullptr) continue;
    if (old == before.nodes.end() || old->second.children != current.children)
      (void)UiaRaiseStructureChangedEvent(provider, StructureChangeType_ChildrenInvalidated,
                                           nullptr, 0);
    if (old != before.nodes.end()) {
      property(provider, UIA_NamePropertyId, old->second.name, current.name);
      property(provider, UIA_HelpTextPropertyId, old->second.description, current.description);
      property(provider, UIA_ValueValuePropertyId,
          old->second.text ? old->second.text->value : old->second.value,
          current.text ? current.text->value : current.value);
      if (current.text && old->second.text) {
        if (current.text->value != old->second.text->value)
          (void)UiaRaiseAutomationEvent(provider, UIA_Text_TextChangedEventId);
        if (current.text->caret != old->second.text->caret ||
            current.text->anchor != old->second.text->anchor)
          (void)UiaRaiseAutomationEvent(provider, UIA_Text_TextSelectionChangedEventId);
      }
      const auto toggle = [](const States &states) {
        return states.mixed ? 2 : states.checked ? 1 : 0;
      };
      integer(provider, UIA_ToggleToggleStatePropertyId, toggle(old->second.states),
              toggle(current.states));
      integer(provider, UIA_ExpandCollapseExpandCollapseStatePropertyId,
              old->second.states.expanded ? 1 : 0, current.states.expanded ? 1 : 0);
      if (current.range && old->second.range)
        real(provider, UIA_RangeValueValuePropertyId, old->second.range->value,
             current.range->value);
      const auto scroll_percent = [](const std::optional<Scroll> &scroll, bool horizontal) {
        if (!scroll) return -1.0;
        return horizontal ? scroll->horizontal : scroll->vertical;
      };
      real(provider, UIA_ScrollHorizontalScrollPercentPropertyId,
           scroll_percent(old->second.scroll, true), scroll_percent(current.scroll, true));
      real(provider, UIA_ScrollVerticalScrollPercentPropertyId,
           scroll_percent(old->second.scroll, false), scroll_percent(current.scroll, false));
      // Radio selection follows the shared checked-state rule, not Toggle.
      if ((current.role == Role::list_row || current.role == Role::tab ||
           current.role == Role::radio_button) &&
          selected_item(current) && !selected_item(old->second))
        (void)UiaRaiseAutomationEvent(provider, UIA_SelectionItem_ElementSelectedEventId);
    }
    if (current.states.focused &&
        (old == before.nodes.end() || !old->second.states.focused))
      (void)UiaRaiseAutomationEvent(provider, UIA_AutomationFocusChangedEventId);
    provider->Release();
  }
  if (!after.announcement.empty() &&
      before.announcement_sequence != after.announcement_sequence) {
    auto *root = make_provider(context, after.root);
    const auto notify = notification_function();
    const auto text = core::try_utf8_to_wide(after.announcement);
    if (root != nullptr && notify != nullptr && text) {
      auto message = SysAllocStringLen(text->data(), static_cast<UINT>(text->size()));
      auto activity = SysAllocString(L"LayerE status");
      if (message != nullptr && activity != nullptr)
        (void)notify(root, NotificationKind_Other,
                       NotificationProcessing_ImportantMostRecent, message, activity);
      SysFreeString(message);
      SysFreeString(activity);
    }
    if (root != nullptr) root->Release();
  }
}
} // namespace layerc::editor::a11y::uia

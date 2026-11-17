#include "bridge.hpp"

#include "context.hpp"
#include "../input.hpp"
#include "layerc/core/utf8.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace layerc::editor::a11y::uia {
namespace {
constexpr UINT_PTR subclass_id = 1;
struct Binding {
  NodeId id{};
  Role role{Role::group};
  std::string name, description;
  bool explicit_name{};
};
Role role_of(Fl_Widget &widget) {
  if (dynamic_cast<Fl_Window *>(&widget)) return Role::window;
  if (dynamic_cast<Fl_Input_ *>(&widget)) return Role::text_field;
  if (dynamic_cast<Fl_Button *>(&widget)) return Role::button;
  if (dynamic_cast<Fl_Group *>(&widget)) return Role::group;
  return Role::label;
}
} // namespace

struct Bridge::Impl : std::enable_shared_from_this<Impl> {
  explicit Impl(Fl_Window &host)
      : window(host), context(std::make_shared<Context>(fl_xid(&host))) {
    bindings.emplace(&host, Binding{1, Role::window, {}, {}, false});
  }
  Fl_Window &window;
  std::shared_ptr<Context> context;
  std::map<Fl_Widget *, Binding> bindings;
  std::map<NodeId, Fl_Widget *> widgets;
  NodeId next_node{1};
  std::optional<BridgeError> fault;
  std::string announcement;
  std::uint64_t announcement_sequence{};
  bool attached{}, caret_created{};
  int caret_height{};

  static void check(void *data) noexcept {
    auto *self = static_cast<Impl *>(data);
    if (!self->context->connected.load()) return;
    try {
      const auto hold = self->shared_from_this();
      (void)self->sync();
    } catch (...) {
      self->context->disconnect();
      OutputDebugStringW(L"LayerE: UI Automation tree update failed.\n");
    }
  }

  static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam,
                                      LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto *self = reinterpret_cast<Impl *>(data);
    const auto hold = self->shared_from_this();
    if (message == WM_GETOBJECT &&
        lparam == static_cast<LPARAM>(UiaRootObjectId) &&
        self->context->connected.load()) {
      auto *provider = make_provider(self->context, 1);
      if (provider == nullptr) return 0;
      const auto result = UiaReturnRawElementProvider(hwnd, wparam, lparam, provider);
      provider->Release();
      return result;
    }
    if (message == self->context->message &&
        static_cast<ULONG_PTR>(lparam) == self->context->cookie) {
      auto request = self->context->take(static_cast<UINT_PTR>(wparam));
      if (request) request->result.store(guarded([&]() -> HRESULT {
        if (std::chrono::steady_clock::now() >= request->deadline)
          return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        return self->apply(request->action);
      }));
      return 0;
    }
    if (message == WM_NCDESTROY) self->close();
    return DefSubclassProc(hwnd, message, wparam, lparam);
  }

  void attach() {
    if (!window.shown() || context->window == nullptr || context->message == 0) {
      fault = BridgeError{"UI Automation requires a shown FLTK window"};
      context->disconnect();
      return;
    }
    DWORD_PTR existing{};
    if (GetWindowSubclass(context->window, procedure, subclass_id, &existing)) {
      fault = BridgeError{"UI Automation bridge is already attached"};
      context->disconnect();
      return;
    }
    if (sync()) { context->disconnect(); return; }
    if (!SetWindowSubclass(context->window, procedure, subclass_id,
                             reinterpret_cast<DWORD_PTR>(this))) {
      fault = BridgeError{"UI Automation window subclass is unavailable"};
      context->disconnect();
      return;
    }
    attached = true;
    Fl::add_check(check, this);
  }

  void close() {
    Fl::remove_check(check, this);
    context->disconnect();
    if (caret_created) { DestroyCaret(); caret_created = false; }
    if (attached) {
      RemoveWindowSubclass(context->window, procedure, subclass_id);
      attached = false;
    }
  }

  void collect(Fl_Widget &widget, NodeId parent, Snapshot &tree,
               const POINT origin, double sx, double sy) {
    auto found = bindings.find(&widget);
    if (found == bindings.end()) found = bindings.emplace(
        &widget, Binding{++next_node, role_of(widget), {}, {}, false}).first;
    const auto &binding = found->second;
    Node item;
    item.id = binding.id;
    item.parent = parent;
    item.role = binding.role;
    item.name = binding.explicit_name ? binding.name
                                      : (widget.label() ? widget.label() : "");
    item.description = binding.description;
    item.states.showing = widget.visible_r() != 0;
    item.states.disabled = widget.active_r() == 0;
    item.states.focusable = dynamic_cast<Fl_Input_ *>(&widget) != nullptr ||
                            dynamic_cast<Fl_Button *>(&widget) != nullptr ||
                            &widget == &window;
    item.states.focused = ::GetFocus() == context->window &&
        (Fl::focus() == &widget || (&widget == &window && Fl::focus() == nullptr));
    item.bounds = {origin.x + widget.x() * sx, origin.y + widget.y() * sy,
                    widget.w() * sx, widget.h() * sy};
    if (&widget == &window) {
      RECT rectangle{};
      if (GetWindowRect(context->window, &rectangle))
        item.bounds = {static_cast<double>(rectangle.left),
                        static_cast<double>(rectangle.top),
                        static_cast<double>(rectangle.right - rectangle.left),
                        static_cast<double>(rectangle.bottom - rectangle.top)};
      item.states.modal = window.modal() != 0;
    }
    if (auto *input = dynamic_cast<Fl_Input_ *>(&widget)) {
      item.states.read_only = input->readonly() != 0;
      item.states.password = input->input_type() == FL_SECRET_INPUT;
      if (!item.states.password) {
        item.value.assign(input->value(), static_cast<std::size_t>(input->size()));
        if (auto *accessible = dynamic_cast<Input *>(input)) {
          item.text = accessible->accessible_text();
          const auto screen = [&](Bounds &b) {
            b = {origin.x + b.left * sx, origin.y + b.top * sy,
                  b.width * sx, b.height * sy};
          };
          screen(item.text->caret_bounds);
          for (auto &run : item.text->runs) screen(run.bounds);
        }
      }
    }
    if (auto *button = dynamic_cast<Fl_Button *>(&widget))
      item.states.checked = button->value() != 0;
    widgets.emplace(item.id, &widget);
    if (auto *group = dynamic_cast<Fl_Group *>(&widget)) {
      for (int index = 0; index < group->children(); ++index) {
        auto &child = *group->child(index);
        collect(child, item.id, tree, origin, sx, sy);
        item.children.push_back(bindings.at(&child).id);
      }
    }
    tree.nodes.emplace(item.id, std::move(item));
  }

  std::optional<BridgeError> sync() {
    if (!context->connected.load())
      return fault ? fault : std::optional<BridgeError>{{"UI Automation window disconnected"}};
    POINT origin{};
    RECT client{};
    if (!ClientToScreen(context->window, &origin) ||
        !GetClientRect(context->window, &client) || window.w() <= 0 || window.h() <= 0) {
      fault = BridgeError{"UI Automation cannot read window geometry"};
      return fault;
    }
    Fl::flush(); // Scroll/caret geometry is settled by FLTK's own draw pass.
    window.make_current();
    const double sx = static_cast<double>(client.right) / window.w();
    const double sy = static_cast<double>(client.bottom) / window.h();
    Snapshot next;
    next.root = 1;
    next.announcement = announcement;
    next.announcement_sequence = announcement_sequence;
    widgets.clear();
    collect(window, no_node, next, origin, sx, sy);
    std::set<Fl_Widget *> live;
    for (const auto &[id, widget] : widgets) { (void)id; live.insert(widget); }
    std::erase_if(bindings, [&](const auto &entry) { return !live.contains(entry.first); });
    const auto before = context->model.read();
    if (const auto error = context->model.publish(next)) {
      fault = BridgeError{error->message};
      return fault;
    }
    context->prune(next);
    update_caret(next, origin);
    raise_changes(context, before, next);
    fault.reset();
    return std::nullopt;
  }

  void update_caret(const Snapshot &tree, POINT origin) {
    const Node *focused{};
    for (const auto &[id, item] : tree.nodes) {
      (void)id;
      if (item.states.focused && item.text) { focused = &item; break; }
    }
    if (focused == nullptr) {
      if (caret_created) { DestroyCaret(); caret_created = false; }
      return;
    }
    const auto &b = focused->text->caret_bounds;
    const auto height = std::max(1, static_cast<int>(std::lround(b.height)));
    if (!caret_created || height != caret_height) {
      if (caret_created) DestroyCaret();
      caret_created = CreateCaret(context->window, nullptr, 1, height) != 0;
      caret_height = height;
    }
    if (caret_created)
      (void)SetCaretPos(static_cast<int>(std::lround(b.left - origin.x)),
                         static_cast<int>(std::lround(b.top - origin.y)));
    // No ShowCaret: FLTK draws the visible caret; Windows exposes only its
    // matching insertion-point location to UIA and Magnifier.
  }

  HRESULT apply(const Action &action) {
    if (sync()) return UIA_E_ELEMENTNOTAVAILABLE;
    const auto found = widgets.find(action.node);
    if (found == widgets.end()) return UIA_E_ELEMENTNOTAVAILABLE;
    auto *widget = found->second;
    if (!widget->active_r()) return UIA_E_ELEMENTNOTENABLED;
    switch (action.kind) {
    case ActionKind::focus:
      (void)SetForegroundWindow(context->window);
      (void)::SetFocus(context->window);
      if (!widget->take_focus()) return UIA_E_INVALIDOPERATION;
      break;
    case ActionKind::invoke:
      if (bindings.at(widget).role != Role::button &&
          bindings.at(widget).role != Role::menu_item) return E_INVALIDARG;
      widget->do_callback();
      break;
    case ActionKind::set_value: {
      auto *input = dynamic_cast<Fl_Input_ *>(widget);
      if (input == nullptr) return E_INVALIDARG;
      if (input->readonly()) return E_ACCESSDENIED;
      if (!core::is_valid_utf8(action.value)) return E_INVALIDARG;
      input->value(action.value.c_str());
      input->insert_position(input->size());
      input->set_changed();
      input->do_callback();
      break;
    }
    case ActionKind::select_text: {
      auto *input = dynamic_cast<Fl_Input_ *>(widget);
      if (input == nullptr) return E_INVALIDARG;
      const auto value = std::string_view(input->value(),
                                          static_cast<std::size_t>(input->size()));
      if (action.caret > value.size() || action.anchor > value.size() ||
          !core::is_valid_utf8(value.substr(0, action.caret)) ||
          !core::is_valid_utf8(value.substr(0, action.anchor))) return E_INVALIDARG;
      input->insert_position(static_cast<int>(action.caret),
                               static_cast<int>(action.anchor));
      break;
    }
    }
    if (!context->connected.load()) return S_OK; // Invoke may close its window.
    return sync() ? E_FAIL : S_OK;
  }
};

Bridge::Bridge(Fl_Window &window) : impl_(std::make_shared<Impl>(window)) {
  impl_->attach();
}
Bridge::~Bridge() { impl_->close(); }
void Bridge::bind(Fl_Widget &widget, Role role, std::string name,
                   std::string description) {
  auto found = impl_->bindings.find(&widget);
  if (found == impl_->bindings.end()) found = impl_->bindings.emplace(
      &widget, Binding{++impl_->next_node, role, {}, {}, true}).first;
  found->second.role = role;
  found->second.name = std::move(name);
  found->second.description = std::move(description);
  found->second.explicit_name = true;
}
std::optional<BridgeError> Bridge::sync() { return impl_->sync(); }
std::optional<BridgeError> Bridge::error() const { return impl_->fault; }
void Bridge::announce(std::string text) {
  impl_->announcement = std::move(text);
  // Every call is its own occurrence, including a repeated identical message.
  ++impl_->announcement_sequence;
  (void)impl_->sync();
}
} // namespace layerc::editor::a11y::uia

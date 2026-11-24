// UI Automation publication of the shared FLTK tree: one walk feeds every
// hosted window, and nothing walks until a client asks for the root object.
#include "adaptor.hpp"

#include "a11y/fltk_actions.hpp"
#include "a11y/fltk_tree.hpp"
#include "context.hpp"
#include "steady_timeout.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace layerc::editor::a11y::uia {
namespace {

constexpr UINT_PTR subclass_id = 1;
// A client polling in a tight loop never pays for more than one walk per
// interval, exactly like the AT-SPI adaptor's cadence.
constexpr auto kRefreshInterval = std::chrono::milliseconds{100};
// A client that stops asking for this long lets the walks stop again.
constexpr auto kDemandWindow = std::chrono::nanoseconds{std::chrono::seconds{5}};

// Clears the walk guard on every return path, including the rejected publish.
struct WalkGuard {
  explicit WalkGuard(bool &flag) : flag(flag) { flag = true; }
  ~WalkGuard() { flag = false; }
  WalkGuard(const WalkGuard &) = delete;
  WalkGuard &operator=(const WalkGuard &) = delete;
  bool &flag;
};

// One shown top-level FLTK window: its own provider context, holding only this
// window's subtree, attached to its own native window procedure.
struct Host {
  Fl_Window *window{};
  HWND handle{};
  std::shared_ptr<Context> context;
};

// The window node and its descendants, as this host publishes them: the window
// becomes the root, and relations that name a node outside this window are
// dropped rather than rejected by the model.
[[nodiscard]] std::optional<Snapshot> subtree(const Snapshot &full, const NodeId root) {
  if (root == no_node || !full.nodes.contains(root)) return std::nullopt;
  Snapshot part;
  part.root = root;
  std::vector<NodeId> queue{root};
  for (std::size_t index = 0; index < queue.size(); ++index) {
    const auto found = full.nodes.find(queue[index]);
    if (found == full.nodes.end()) return std::nullopt;
    Node node = found->second;
    queue.insert(queue.end(), node.children.begin(), node.children.end());
    part.nodes.emplace(node.id, std::move(node));
  }
  for (auto &[id, node] : part.nodes) {
    (void)id;
    std::erase_if(node.labelled_by,
                  [&](const NodeId related) { return !part.nodes.contains(related); });
    std::erase_if(node.described_by,
                  [&](const NodeId related) { return !part.nodes.contains(related); });
  }
  part.nodes.at(root).parent = no_node;
  return part;
}

[[nodiscard]] Bounds window_bounds(const HWND handle) {
  RECT rectangle{};
  if (!GetWindowRect(handle, &rectangle)) return {};
  return {static_cast<double>(rectangle.left), static_cast<double>(rectangle.top),
          static_cast<double>(rectangle.right - rectangle.left),
          static_cast<double>(rectangle.bottom - rectangle.top)};
}
} // namespace

struct Adaptor::Impl : std::enable_shared_from_this<Impl> {
  explicit Impl(FltkTree &exported) : tree(exported) {}
  ~Impl() { close(); }

  FltkTree &tree;
  std::map<HWND, Host> hosts;
  std::optional<AdaptorError> fault;
  std::string announcement;
  std::uint64_t announcement_sequence{};
  Fl_Window *announcement_window{};
  Fl_Widget *announcement_source{};
  // No client has asked for the root object yet, so nothing is exported.
  bool exposed{};
  // An action of the client's own took effect: publish without waiting out the
  // interval, never inside the notification that reported it.
  bool stale{};
  bool walking{};
  std::atomic<std::uint64_t> walks{};
  // Read from test threads while the FLTK thread hosts windows.
  std::atomic<std::size_t> hosted{};
  std::chrono::steady_clock::time_point next_refresh{};
  SteadyTimeout refresh_timer{[this] { on_timer(); }};
  // Set by a walk that ran from the refresh timer; the check that follows sees it
  // and stays quiet, so an idle loop performs no periodic walk at all.
  bool walked_in_check{};
  HWND caret_handle{};
  bool caret_created{};
  int caret_height{};

  void start() {
    set_changed_handler(&Impl::on_changed, this);
    Fl::add_check(on_check, this);
  }

  void close() {
    Fl::remove_check(on_check, this);
    refresh_timer.cancel();
    cancel_deferred();
    set_changed_handler(nullptr, nullptr);
    for (auto &[handle, host] : hosts) {
      (void)handle;
      detach(host);
    }
    hosts.clear();
    hosted.store(0);
    if (caret_created) {
      DestroyCaret();
      caret_created = false;
    }
    caret_handle = nullptr;
  }

  static void on_changed(void *data) { static_cast<Impl *>(data)->stale = true; }

  static void on_check(void *data) { static_cast<Impl *>(data)->tick(); }

  void on_timer() {
    (void)refresh();
    // A walk from this timer is the loop's only activity in this pass: the next
    // check runs before anything else can have changed the tree, so it stays
    // quiet instead of re-arming and walking again.
    walked_in_check = true;
  }

  void tick() {
    discover();
    if (walked_in_check) {
      walked_in_check = false;
      return;
    }
    if (!demand()) return;
    const auto now = std::chrono::steady_clock::now();
    if (!stale && now < next_refresh) {
      // Checks run once per loop iteration, so a change made inside the window
      // waits for unrelated input unless the remaining interval is armed.
      if (!refresh_timer.owed()) refresh_timer.start(next_refresh - now);
      return;
    }
    (void)refresh();
  }

  [[nodiscard]] bool demand() const {
    if (!exposed) return false;
    if (UiaClientsAreListening()) return true;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    for (const auto &[handle, host] : hosts) {
      (void)handle;
      if (host.context == nullptr) continue;
      const auto touched = host.context->last_touch.load();
      if (touched != 0 && std::chrono::nanoseconds{now - touched} < kDemandWindow)
        return true;
    }
    return false;
  }

  void discover() {
    std::vector<Fl_Window *> shown;
    for (auto *window = Fl::first_window(); window != nullptr;
         window = Fl::next_window(window))
      shown.push_back(window);
    for (auto it = hosts.begin(); it != hosts.end();) {
      auto *window = it->second.window;
      const bool alive = window != nullptr && window->shown() &&
                         window == it->second.window &&
                         std::find(shown.begin(), shown.end(), window) != shown.end() &&
                         fl_xid(window) == it->first;
      if (alive) {
        ++it;
        continue;
      }
      auto host = std::move(it->second);
      it = hosts.erase(it);
      detach(host);
    }
    for (auto *window : shown) {
      auto *handle = window->shown() ? fl_xid(window) : nullptr;
      if (handle == nullptr || hosts.contains(handle)) continue;
      attach(*window, handle);
    }
    hosted.store(hosts.size());
  }

  void attach(Fl_Window &window, const HWND handle) {
    Host host;
    host.window = &window;
    host.handle = handle;
    host.context = std::make_shared<Context>(handle);
    if (!SetWindowSubclass(handle, procedure, subclass_id,
                           reinterpret_cast<DWORD_PTR>(this))) {
      host.context->disconnect();
      fault = AdaptorError{"UI Automation window subclass is unavailable"};
      return;
    }
    hosts.emplace(handle, std::move(host));
    hosted.store(hosts.size());
  }

  void detach(Host &host) {
    if (host.handle != nullptr)
      RemoveWindowSubclass(host.handle, procedure, subclass_id);
    if (host.context) host.context->disconnect();
    if (caret_handle == host.handle && caret_created) {
      DestroyCaret();
      caret_created = false;
      caret_handle = nullptr;
    }
  }

  static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto *self = reinterpret_cast<Impl *>(data);
    const auto hold = self->shared_from_this();
    const auto found = self->hosts.find(hwnd);
    Host host;
    if (found != self->hosts.end()) host = found->second;
    if (message == WM_GETOBJECT &&
        lparam == static_cast<LPARAM>(UiaRootObjectId)) {
      if (host.context == nullptr) return 0;
      // The client is asking for this window's root object: publish now, never
      // serve the generation that preceded the client's own change.
      self->exposed = true;
      if (self->sync()) return 0;
      auto *provider = make_provider(host.context, host.context->root.load());
      if (provider == nullptr) return 0;
      const auto result = UiaReturnRawElementProvider(hwnd, wparam, lparam, provider);
      provider->Release();
      return result;
    }
    if (host.context != nullptr && message == host.context->message &&
        static_cast<ULONG_PTR>(lparam) == host.context->cookie) {
      auto request = host.context->take(static_cast<UINT_PTR>(wparam));
      if (request)
        request->result.store(guarded([&]() -> HRESULT {
          if (std::chrono::steady_clock::now() >= request->deadline)
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
          return self->apply(host.context, request->action);
        }));
      return 0;
    }
    if (message == WM_NCDESTROY && found != self->hosts.end()) {
      auto closing = std::move(host);
      self->hosts.erase(found);
      self->detach(closing);
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
  }

  [[nodiscard]] std::optional<AdaptorError> sync() {
    discover();
    return refresh();
  }

  [[nodiscard]] std::optional<AdaptorError> refresh() {
    if (walking) return fault;
    WalkGuard guard{walking};
    ++walks;
    const auto now = std::chrono::steady_clock::now();
    next_refresh = now + kRefreshInterval;
    refresh_timer.cancel();
    stale = false;
    // Scroll and caret geometry settle in FLTK's own draw pass.
    const Snapshot full = tree.snapshot();
    std::optional<AdaptorError> failure;
    HWND caret_owner{};
    Bounds caret_bounds{};
    for (auto &[handle, host] : hosts) {
      if (!host.context || !host.context->connected.load() || host.window == nullptr)
        continue;
      const auto root = tree.id_for(*host.window);
      auto part = subtree(full, root);
      if (!part) continue;
      host.context->root.store(root);
      part->nodes.at(root).bounds = window_bounds(handle);
      if (!announcement.empty() && announcement_window == host.window) {
        part->announcement = announcement;
        part->announcement_sequence = announcement_sequence;
        auto source = announcement_source != nullptr ? tree.id_for(*announcement_source)
                                                     : no_node;
        if (source == no_node || !part->nodes.contains(source)) {
          source = no_node;
          for (const auto &[id, item] : part->nodes)
            if (item.states.focused) {
              source = id;
              break;
            }
        }
        part->announcement_source = source == no_node ? root : source;
      }
      const Snapshot before = host.context->model.read();
      if (const auto error = host.context->model.publish(*part)) {
        failure = AdaptorError{error->message};
        continue;
      }
      host.context->prune(*part);
      raise_changes(host.context, before, *part);
      if (caret_owner == nullptr)
        for (const auto &[id, item] : part->nodes) {
          (void)id;
          if (!item.states.focused || !item.text) continue;
          caret_owner = handle;
          caret_bounds = item.text->caret_bounds;
          break;
        }
    }
    update_caret(caret_owner, caret_bounds);
    fault = failure;
    return failure;
  }

  void update_caret(const HWND handle, const Bounds &bounds) {
    if (handle == nullptr) {
      if (caret_created) {
        DestroyCaret();
        caret_created = false;
      }
      caret_handle = nullptr;
      return;
    }
    const auto height = std::max(1, static_cast<int>(std::lround(bounds.height)));
    if (!caret_created || handle != caret_handle || height != caret_height) {
      if (caret_created) DestroyCaret();
      caret_created = CreateCaret(handle, nullptr, 1, height) != 0;
      caret_height = height;
      caret_handle = handle;
    }
    POINT origin{};
    if (!caret_created || !ClientToScreen(handle, &origin)) return;
    (void)SetCaretPos(static_cast<int>(std::lround(bounds.left - origin.x)),
                       static_cast<int>(std::lround(bounds.top - origin.y)));
    // No ShowCaret: FLTK draws the visible caret; Windows exposes only its
    // matching insertion-point location to UI Automation and Magnifier.
  }

  [[nodiscard]] HRESULT apply(const std::shared_ptr<Context> &context,
                              const Action &action) {
    if (sync()) return UIA_E_ELEMENTNOTAVAILABLE;
    const auto target = tree.target(action.node);
    if (!target) return UIA_E_ELEMENTNOTAVAILABLE;
    const auto current = context->model.read(action.node);
    if (current && current->states.disabled) return UIA_E_ELEMENTNOTENABLED;
    HRESULT status = S_OK;
    switch (action.kind) {
    case ActionKind::focus: {
      const auto found = hosts.find(context->window);
      const HWND handle = found != hosts.end() ? found->second.handle : nullptr;
      if (handle != nullptr) {
        (void)SetForegroundWindow(handle);
        (void)::SetFocus(handle);
      }
      if (!grab_focus(*target)) status = UIA_E_INVALIDOPERATION;
      break;
    }
    case ActionKind::invoke:
      // Invoke must return without blocking, so the application callback runs
      // from FLTK's loop; the action layer drops it if the widget went away.
      if (!activate(*target)) status = E_INVALIDARG;
      break;
    case ActionKind::set_value:
      if (current && (current->states.read_only || current->states.password)) {
        status = E_ACCESSDENIED;
        break;
      }
      if (!set_text(*target, action.value)) {
        status = E_INVALIDARG;
        break;
      }
      // SetValue leaves the caret after the new text, as typing would.
      (void)set_caret(*target, action.value.size());
      break;
    case ActionKind::select_text:
      if (!select_text(*target, action.anchor, action.caret)) status = E_INVALIDARG;
      break;
    case ActionKind::set_number:
      if (!set_number(*target, action.number)) status = E_INVALIDARG;
      break;
    case ActionKind::toggle:
      // A check state flips through the widget's own toggle, from FLTK's loop.
      if (!activate(*target)) status = E_INVALIDARG;
      break;
    case ActionKind::expand:
      // The popup runs its own modal loop, so opening lands from FLTK's loop;
      // an already-open or popup-less node cannot expand again.
      if (!expand(*target)) status = UIA_E_INVALIDOPERATION;
      break;
    case ActionKind::collapse:
      if (!collapse(*target)) status = UIA_E_INVALIDOPERATION;
      break;
    case ActionKind::scroll:
      if (!scroll_to(*target, action.horizontal, action.vertical)) status = E_INVALIDARG;
      break;
    }
    if (!context->connected.load()) return status; // Invoke may close its window.
    return sync() ? E_FAIL : status;
  }
};

Adaptor::Adaptor(FltkTree &tree) : impl_(std::make_shared<Impl>(tree)) { impl_->start(); }
Adaptor::~Adaptor() = default;

std::optional<AdaptorError> Adaptor::sync() { return impl_->sync(); }
std::optional<AdaptorError> Adaptor::error() const { return impl_->fault; }

void Adaptor::announce(Fl_Widget *source, std::string text) {
  // Only a client that is listening can hear it. Keeping an unheard message
  // would replay it, stale, to the first client that connects later.
  if (!impl_->demand()) {
    impl_->announcement.clear();
    impl_->announcement_source = nullptr;
    impl_->announcement_window = nullptr;
    return;
  }
  impl_->announcement = std::move(text);
  // Every call is its own occurrence, including a repeated identical message.
  ++impl_->announcement_sequence;
  impl_->announcement_source = source;
  impl_->announcement_window =
      source != nullptr ? source->top_window() : nullptr;
  (void)impl_->sync();
}

std::uint64_t Adaptor::walks_for_testing() const { return impl_->walks.load(); }

std::size_t Adaptor::hosted_windows_for_testing() const { return impl_->hosted.load(); }
} // namespace layerc::editor::a11y::uia

#pragma once

#include "../model.hpp"

#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace layerc::editor::a11y::uia {

template <typename Function> HRESULT guarded(Function &&function) noexcept {
  try { return std::forward<Function>(function)(); }
  catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  catch (...) { return E_FAIL; }
}

struct Request {
  Action action;
  std::chrono::steady_clock::time_point deadline;
  std::atomic<HRESULT> result{static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE)};
};

struct Context {
  explicit Context(HWND window);
  const HWND window;
  const UINT message;
  const ULONG_PTR cookie;
  std::atomic<bool> connected{true};
  Model model;

  [[nodiscard]] HRESULT dispatch(Action action);
  [[nodiscard]] std::shared_ptr<Request> take(UINT_PTR token);
  void disconnect();
  void prune(const Snapshot &snapshot);

private:
  friend IRawElementProviderSimple *make_provider(
      const std::shared_ptr<Context> &context, NodeId id);
  std::mutex providers_mutex_;
  std::map<NodeId, IRawElementProviderSimple *> providers_;
  std::mutex mutex_;
  UINT_PTR next_request_{};
  std::map<UINT_PTR, std::shared_ptr<Request>> requests_;
};

[[nodiscard]] std::optional<Node> node(const std::shared_ptr<Context> &context,
                                      NodeId id);
[[nodiscard]] IRawElementProviderSimple *make_provider(
    const std::shared_ptr<Context> &context, NodeId id);
[[nodiscard]] HRESULT make_fragment(
    const std::shared_ptr<Context> &context, NodeId id,
    IRawElementProviderFragment **result);
[[nodiscard]] HRESULT make_text_pattern(
    const std::shared_ptr<Context> &context, NodeId id, IUnknown **result);
void raise_changes(const std::shared_ptr<Context> &context,
                   const Snapshot &before, const Snapshot &after);

} // namespace layerc::editor::a11y::uia

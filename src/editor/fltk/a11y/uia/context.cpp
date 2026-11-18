#include "context.hpp"

namespace layerc::editor::a11y::uia {
namespace {
std::atomic<ULONG_PTR> next_cookie{1};
}

Context::Context(const HWND handle)
    : window(handle),
      message(RegisterWindowMessageW(L"LayerC.FLTK.Accessibility.Action.v1")),
      cookie(next_cookie.fetch_add(1)) {}

HRESULT Context::dispatch(Action action) {
  if (!connected.load() || message == 0) return UIA_E_ELEMENTNOTAVAILABLE;
  auto request = std::make_shared<Request>();
  request->action = std::move(action);
  request->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  UINT_PTR token{};
  {
    const std::lock_guard lock(mutex_);
    if (!connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
    token = ++next_request_;
    requests_.emplace(token, request);
  }
  DWORD_PTR ignored{};
  const bool sent = SendMessageTimeoutW(
      window, message, static_cast<WPARAM>(token), static_cast<LPARAM>(cookie),
      SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored) != 0;
  const std::lock_guard lock(mutex_);
  requests_.erase(token);
  if (!connected.load()) return UIA_E_ELEMENTNOTAVAILABLE;
  return sent ? request->result.load() : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

std::shared_ptr<Request> Context::take(const UINT_PTR token) {
  const std::lock_guard lock(mutex_);
  const auto found = requests_.find(token);
  if (found == requests_.end()) return {};
  auto request = std::move(found->second);
  requests_.erase(found);
  return request;
}

void Context::disconnect() {
  connected.store(false);
  {
    const std::lock_guard lock(mutex_);
    requests_.clear();
  }
  std::map<NodeId, IRawElementProviderSimple *> providers;
  {
    const std::lock_guard lock(providers_mutex_);
    providers.swap(providers_);
  }
  for (const auto &[id, provider] : providers) {
    (void)id;
    (void)UiaDisconnectProvider(provider);
    provider->Release();
  }
  model.clear();
}

void Context::prune(const Snapshot &snapshot) {
  std::map<NodeId, IRawElementProviderSimple *> retired;
  {
    const std::lock_guard lock(providers_mutex_);
    for (auto it = providers_.begin(); it != providers_.end();) {
      if (snapshot.nodes.contains(it->first)) { ++it; continue; }
      auto current = it++;
      retired.insert(providers_.extract(current));
    }
  }
  for (const auto &[id, provider] : retired) {
    (void)id;
    (void)UiaDisconnectProvider(provider);
    provider->Release();
  }
}

std::optional<Node> node(const std::shared_ptr<Context> &context,
                         const NodeId id) {
  if (!context->connected.load()) return std::nullopt;
  context->last_touch.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
  return context->model.read(id);
}
} // namespace layerc::editor::a11y::uia

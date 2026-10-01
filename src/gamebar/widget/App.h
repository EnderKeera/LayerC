#pragma once

#include "App.xaml.g.h"

#include <winrt/Microsoft.Gaming.XboxGameBar.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace winrt::LayerC::GameBar::implementation {

// Extension ids are "PanelSlot<1..8>" (bounds duplicated in the manifest) or
// "Settings". is_panel_extension is the id predicate; panel_slot maps a
// panel extension to its zero-based slot index.
inline bool is_panel_extension(const std::wstring_view id) {
  constexpr std::wstring_view panel_prefix = L"PanelSlot";
  return id.starts_with(panel_prefix) &&
         id.size() == panel_prefix.size() + 1U && id.back() >= L'1' &&
         id.back() <= L'8';
}

inline std::optional<std::uint32_t> panel_slot(const std::wstring_view id) {
  if (!is_panel_extension(id))
    return std::nullopt;
  return static_cast<std::uint32_t>(id.back() - L'1');
}

struct App : AppT<App> {
  App();
  ~App();

  void OnActivated(
      Windows::ApplicationModel::Activation::IActivatedEventArgs const &args);
  void OnBackgroundActivated(
      Windows::ApplicationModel::Activation::BackgroundActivatedEventArgs const
          &args);

private:
  struct WidgetWindow {
    Windows::UI::Xaml::Controls::Frame frame{nullptr};
    Microsoft::Gaming::XboxGameBar::XboxGameBarWidget widget{nullptr};
    Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    Windows::UI::Xaml::Window::Closed_revoker closed;
    void close() noexcept;
  };
  void close_widget(std::wstring const &extension_id,
                    const std::shared_ptr<WidgetWindow> &owner);
  void clear_widgets() noexcept;
  void navigation_failed(
      Windows::Foundation::IInspectable const &,
      Windows::UI::Xaml::Navigation::NavigationFailedEventArgs const &args);
  fire_and_forget ensure_daemon();

  std::mutex widgets_mutex_;
  std::map<std::wstring, std::shared_ptr<WidgetWindow>, std::less<>> widgets_;
};

} // namespace winrt::LayerC::GameBar::implementation

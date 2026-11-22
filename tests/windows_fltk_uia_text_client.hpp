#pragma once

#include "test_support.hpp"

#include <ole2.h>
#include <uiautomation.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace layerc::test::uia_text {

template <typename T> class Ref {
public:
  Ref() = default;
  ~Ref() { if (value_) value_->Release(); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  Ref(Ref &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  Ref &operator=(Ref &&other) noexcept {
    if (this != &other) {
      if (value_) value_->Release();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  T *operator->() const { return value_; }
  T &operator*() const { return *value_; }
  T *get() const { return value_; }
  T **put() { if (value_) value_->Release(); value_ = nullptr; return &value_; }
  explicit operator bool() const { return value_ != nullptr; }
private:
  T *value_{};
};

struct String {
  BSTR value{};
  String() = default;
  explicit String(const wchar_t *text) : value(SysAllocString(text)) {}
  ~String() { SysFreeString(value); }
  String(const String &) = delete;
  String &operator=(const String &) = delete;
  std::wstring text() const {
    return value ? std::wstring(value, SysStringLen(value)) : std::wstring{};
  }
};

struct Apartment {
  explicit Apartment(DWORD model = COINIT_MULTITHREADED)
      : result{CoInitializeEx(nullptr, model)} {}
  ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
  HRESULT result;
};

inline bool ok(HRESULT result, const char *message) {
  if (FAILED(result))
    std::cerr << message << ": HRESULT=" << std::hex
              << static_cast<unsigned long>(result) << std::dec << '\n';
  return expect(SUCCEEDED(result), message);
}

template <typename Predicate> bool wait_for(Predicate done, int milliseconds = 3000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return done();
}

struct Client {
  Apartment apartment;
  Ref<IUIAutomation> automation;
  Client() {
    if (!ok(apartment.result, "initialize client MTA")) return;
    if (!ok(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                             IID_IUIAutomation,
                             reinterpret_cast<void **>(automation.put())),
             "create real IUIAutomation client"))
      return;
    Ref<IUIAutomation2> timeouts;
    if (ok(automation->QueryInterface(IID_IUIAutomation2,
                                      reinterpret_cast<void **>(timeouts.put())),
           "UIA timeout interface")) {
      ok(timeouts->put_ConnectionTimeout(2000), "bound connection timeout");
      ok(timeouts->put_TransactionTimeout(2000), "bound transaction timeout");
    }
  }
  explicit operator bool() const { return static_cast<bool>(automation); }
  [[nodiscard]] Ref<IUIAutomationElement> root(const HWND hwnd) {
    Ref<IUIAutomationElement> element;
    if (static_cast<bool>(automation))
      ok(automation->ElementFromHandle(hwnd, element.put()), "client finds FLTK HWND");
    return element;
  }
};

inline Ref<IUIAutomationElement> find(IUIAutomation &automation, IUIAutomationElement &root,
                                const wchar_t *name) {
  VARIANT value{};
  value.vt = VT_BSTR;
  value.bstrVal = SysAllocString(name);
  Ref<IUIAutomationCondition> condition;
  const auto result = automation.CreatePropertyCondition(UIA_NamePropertyId, value,
                                                         condition.put());
  VariantClear(&value);
  Ref<IUIAutomationElement> element;
  if (ok(result, "build name condition") && condition)
    ok(root.FindFirst(TreeScope_Descendants, condition.get(), element.put()),
       "find real UIA descendant");
  expect(static_cast<bool>(element), "fixture element is in the client tree");
  return element;
}

inline Ref<IUIAutomationTextPattern> text_of(IUIAutomationElement &element) {
  Ref<IUIAutomationTextPattern> text;
  ok(element.GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                 reinterpret_cast<void **>(text.put())),
     "Text pattern");
  return text;
}

inline std::wstring range_text(IUIAutomationTextRange &range) {
  String text;
  ok(range.GetText(-1, &text.value), "read range text");
  return text.text();
}

inline Ref<IUIAutomationTextRange> document_of(IUIAutomationTextPattern &text) {
  Ref<IUIAutomationTextRange> document;
  ok(text.get_DocumentRange(document.put()), "document range");
  return document;
}

} // namespace layerc::test::uia_text

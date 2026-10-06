#include "automation_editor.hpp"
#include "editor_automation_internal.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"

#include "layerc/core/profile.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <commdlg.h>
#include <windows.h>

namespace layerc::editor {
namespace {
using namespace std::chrono_literals;

std::optional<std::filesystem::path> choose_trace_path(const HWND owner,
                                                       const bool save) {
  std::vector<wchar_t> buffer(32'768);
  if (save)
    wcscpy_s(buffer.data(), buffer.size(), L"automation.layerctrace");
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter =
      L"LayerC traces (*.layerctrace)\0*.layerctrace\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"layerctrace";
  dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                 (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  const BOOL selected =
      save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
  if (selected == FALSE)
    return std::nullopt;
  auto path = std::filesystem::path(buffer.data());
  if (save && _wcsicmp(path.extension().c_str(), L".layerctrace") != 0)
    path.replace_extension(L".layerctrace");
  return path;
}

std::optional<std::string>
read_bounded_trace(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > layerc::core::max_automation_trace_bytes)
    return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  std::string result(static_cast<std::size_t>(size), '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  return input.gcount() == static_cast<std::streamsize>(result.size())
             ? std::optional(std::move(result))
             : std::nullopt;
}

std::string
trace_from_events(const std::span<const layerc::core::AutomationEvent> events) {
  std::vector<layerc::core::AutomationTraceEntry> entries;
  entries.reserve(events.size());
  for (std::size_t index{}; index < events.size(); ++index)
    entries.push_back(layerc::core::make_automation_root_trace_entry(
        events[index], index + 1U));
  return layerc::core::export_layerc_trace(entries);
}

// Visits each ';'-separated item until visit returns false. Spaces after a
// separator are skipped and a trailing separator yields no empty item.
template <typename Visit>
void for_each_list_item(const std::string_view text, Visit &&visit) {
  std::size_t cursor{};
  while (cursor < text.size()) {
    const auto end = text.find(';', cursor);
    const auto item = text.substr(
        cursor, end == std::string_view::npos ? std::string_view::npos
                                              : end - cursor);
    if (!visit(item) || end == std::string_view::npos)
      return;
    cursor = end + 1U;
    while (cursor < text.size() && text[cursor] == ' ')
      ++cursor;
  }
}

UINT simulator_interval_ms(const int speed) {
  switch (speed) {
  case 0:
    return 400U;
  case 2:
    return 25U;
  default:
    return 100U;
  }
}
} // namespace

void AutomationEditor::update_simulator_view(const HWND dialog) {
  std::string output = "Events: " + std::to_string(simulator_index_) + "/" +
                       std::to_string(simulator_events_.size());
  if (!simulator_) {
    output += "\r\nReset the simulator to compile the current graph.";
  } else {
    output +=
        "    Timers: " + std::to_string(simulator_->snapshot().timers.size()) +
        "\r\nVariables:";
    for (const auto &[id, value] : simulator_->snapshot().variables)
      output +=
          " " + id + "=" + layerc::core::automation_value_json(value) + ";";
    const auto assertions = wide_to_utf8(
        window_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_ASSERTIONS)));
    if (!assertions.empty()) {
      bool all_passed = true;
      for_each_list_item(assertions, [&](const std::string_view item) {
        const auto equal = item.find('=');
        if (equal == std::string_view::npos || equal == 0) {
          all_passed = false;
          return false;
        }
        const auto id = std::string(item.substr(0, equal));
        const auto &profile = simulator_->profile();
        const auto declaration = std::ranges::find(
            profile.variables, id, &layerc::core::AutomationVariable::id);
        if (declaration == profile.variables.end()) {
          all_passed = false;
          return false;
        }
        const auto expected = layerc::core::parse_automation_value_json(
            item.substr(equal + 1U), declaration->type);
        const auto *value =
            std::get_if<layerc::core::AutomationValue>(&expected);
        const auto actual = simulator_->snapshot().variables.find(id);
        if (value == nullptr ||
            actual == simulator_->snapshot().variables.end() ||
            actual->second != *value)
          all_passed = false;
        return true;
      });
      output += all_passed ? "\r\nAssertions: PASS" : "\r\nAssertions: FAIL";
    }
  }
  trace_display_ = std::move(output);
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
           to_wide(trace_display_));
}

void AutomationEditor::reset_simulator(const HWND dialog) {
  KillTimer(dialog, automation_simulator_timer);
  simulator_playing_ = false;
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_PLAY), L"Pla&y");
  const auto parsed = layerc::core::parse_automation_profile_json(
      wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_JSON))),
      document_.profile());
  const auto *graph = std::get_if<layerc::core::AutomationProfile>(&parsed);
  if (graph == nullptr) {
    const auto &error = std::get<layerc::core::AutomationError>(parsed);
    trace_display_ =
        "Simulator reset failed: " + error.describe();
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
             to_wide(trace_display_));
    return;
  }
  simulator_ = std::make_unique<layerc::core::AutomationRuntime>(
      *graph, layerc::core::AutomationPersistentValues{},
      layerc::core::CooldownGroupTopology(document_.profile().cooldown_groups));
  simulator_->set_trace_enabled(true);
  simulator_index_ = 0;
  if (simulator_events_.empty()) {
    layerc::core::AutomationEvent event;
    event.kind = layerc::core::AutomationEventKind::session_started;
    event.timestamp = layerc::core::TimePoint{};
    event.origin_token = 1;
    trace_events_.push_back(event);
    simulator_events_.push_back(std::move(event));
  }
  update_simulator_view(dialog);
}

void AutomationEditor::play_simulator(const HWND dialog, const bool playing) {
  KillTimer(dialog, automation_simulator_timer);
  simulator_playing_ = playing;
  if (playing) {
    if (!simulator_)
      reset_simulator(dialog);
    if (!simulator_ || simulator_index_ >= simulator_events_.size()) {
      simulator_playing_ = false;
    } else {
      const int speed = static_cast<int>(SendDlgItemMessageW(
          dialog, IDC_AUTOMATION_SIM_SPEED, CB_GETCURSEL, 0, 0));
      SetTimer(dialog, automation_simulator_timer,
               simulator_interval_ms(speed), nullptr);
    }
  }
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_PLAY),
           simulator_playing_ ? L"Pa&use" : L"Pla&y");
}

void AutomationEditor::step_simulator(const HWND dialog) {
  if (!simulator_)
    reset_simulator(dialog);
  if (!simulator_ || simulator_index_ >= simulator_events_.size()) {
    play_simulator(dialog, false);
    update_simulator_view(dialog);
    return;
  }
  const auto &event = simulator_events_[simulator_index_++];
  const auto trace_start = simulator_->trace().size();
  (void)simulator_->expire_overlay_overrides(event.timestamp);
  for (const auto &timer_event : simulator_->due_timer_events(event.timestamp))
    (void)simulator_->process(timer_event);
  const auto result = simulator_->process(event);
  if (!result.committed)
    simulator_playing_ = false;
  const auto breakpoint =
      wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_BREAKPOINT)));
  if (!breakpoint.empty()) {
    const auto &trace = simulator_->trace();
    if (std::any_of(trace.begin() + static_cast<std::ptrdiff_t>(trace_start),
                    trace.end(), [&](const auto &entry) {
                      return entry.rule_id == breakpoint;
                    }))
      simulator_playing_ = false;
  }
  if (!simulator_playing_ || simulator_index_ >= simulator_events_.size())
    play_simulator(dialog, false);
  update_simulator_view(dialog);
}
void AutomationEditor::add_simulator_event(const HWND dialog) {
  constexpr auto &kinds = layerc::core::automation_event_kinds;
  const int selected = static_cast<int>(SendDlgItemMessageW(
      dialog, IDC_AUTOMATION_SIM_EVENT, CB_GETCURSEL, 0, 0));
  if (selected < 0 || static_cast<std::size_t>(selected) >= kinds.size())
    return;
  layerc::core::AutomationEvent event;
  event.kind = kinds[static_cast<std::size_t>(selected)];
  event.source_id =
      wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_SOURCE)));
  const auto timestamp =
      simulator_events_.empty()
          ? 0ms
          : std::chrono::duration_cast<layerc::core::Milliseconds>(
                simulator_events_.back().timestamp.time_since_epoch()) +
                100ms;
  event.timestamp = layerc::core::TimePoint(timestamp);
  event.origin_token = simulator_events_.size() + 1U;
  auto fields =
      wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_FIELDS)));
  if (!fields.empty()) {
    bool valid = true;
    for_each_list_item(fields, [&](const std::string_view item) {
      // name=JSON for a field the event kind types; name:type=JSON otherwise.
      const auto equal = item.find('=');
      const auto colon = item.substr(0, equal).find(':');
      const auto name = item.substr(0, std::min(colon, equal));
      const auto *spec =
          layerc::core::find_automation_event_field(event.kind, name);
      auto type = spec != nullptr ? spec->type : std::nullopt;
      if (equal == std::string_view::npos || name.empty() ||
          (colon == std::string_view::npos && !type.has_value())) {
        themed_message_box(
            dialog,
            L"Fields use name=JSON, or name:type=JSON for fields this event "
            L"does not define; for example value=0.5",
            L"Synthetic event", MB_OK | MB_ICONWARNING);
        valid = false;
        return false;
      }
      if (colon != std::string_view::npos) {
        const auto declared = layerc::core::parse_automation_value_type_name(
            item.substr(colon + 1U, equal - colon - 1U));
        if (!declared.has_value() || (type.has_value() && type != declared)) {
          themed_message_box(dialog,
                             L"Unsupported synthetic field type for this event.",
                             L"Synthetic event", MB_OK | MB_ICONWARNING);
          valid = false;
          return false;
        }
        type = declared;
      }
      const auto parsed = layerc::core::parse_automation_value_json(
          item.substr(equal + 1U), *type);
      const auto *value = std::get_if<layerc::core::AutomationValue>(&parsed);
      if (value == nullptr) {
        const auto &error = std::get<layerc::core::AutomationError>(parsed);
        themed_message_box(dialog, to_wide(error.describe()).c_str(),
                    L"Synthetic event", MB_OK | MB_ICONWARNING);
        valid = false;
        return false;
      }
      event.fields.emplace(std::string(name), *value);
      return true;
    });
    if (!valid)
      return;
  }
  simulator_events_.push_back(event);
  trace_events_.push_back(std::move(event));
  update_simulator_view(dialog);
}

void AutomationEditor::load_trace(const HWND dialog) {
  const auto path = choose_trace_path(dialog, false);
  if (!path.has_value())
    return;
  const auto contents = read_bounded_trace(*path);
  if (!contents.has_value()) {
    themed_message_box(dialog, L"The trace could not be read or exceeds 4 MiB.",
                L"Load trace", MB_OK | MB_ICONERROR);
    return;
  }
  const auto parsed = layerc::core::parse_layerc_trace(*contents);
  const auto *events =
      std::get_if<std::vector<layerc::core::AutomationEvent>>(&parsed);
  if (events == nullptr) {
    const auto &error = std::get<layerc::core::AutomationError>(parsed);
    themed_message_box(dialog, to_wide(error.describe()).c_str(),
                L"Invalid trace", MB_OK | MB_ICONWARNING);
    return;
  }
  trace_json_ = *contents;
  trace_events_ = *events;
  simulator_events_ = *events;
  reset_simulator(dialog);
}

bool AutomationEditor::handle_live_message(
    const layerc::core::LiveMessage &message) {
  if (message.type != layerc::core::LiveMessageType::automation_trace_event)
    return false;

  const auto parsed = layerc::core::parse_layerc_trace(message.text);
  if (const auto *events =
          std::get_if<std::vector<layerc::core::AutomationEvent>>(&parsed)) {
    constexpr std::size_t max_trace_events = 2048U;
    const std::size_t incoming = std::min(events->size(), max_trace_events);
    const std::size_t retained = max_trace_events - incoming;
    if (trace_events_.size() > retained)
      trace_events_.erase(trace_events_.begin(),
                          trace_events_.end() -
                              static_cast<std::ptrdiff_t>(retained));
    trace_events_.insert(trace_events_.end(),
                         events->end() - static_cast<std::ptrdiff_t>(incoming),
                         events->end());
    trace_json_ = message.text;
    trace_display_ =
        "Live trace events: " + std::to_string(trace_events_.size()) +
        "\r\nLast authenticated packet: " +
        std::to_string(message.text.size()) + " bytes";
  } else {
    const auto &error = std::get<layerc::core::AutomationError>(parsed);
    trace_display_ =
        "Rejected malformed LayerD trace: " + error.describe();
  }
  if (dialog_ != nullptr && IsWindow(dialog_))
    set_text(GetDlgItem(dialog_, IDC_AUTOMATION_TRACE_TEXT),
             to_wide(trace_display_));
  return true;
}

void AutomationEditor::restore_live_subscription() {
  if (!trace_enabled_)
    return;
  live_edit_.advance_revision();
  (void)(void)live_edit_.channel().send({
      .type = layerc::core::LiveMessageType::automation_trace_subscribe,
      .revision = live_edit_.revision(),
  });
}
void AutomationEditor::set_trace(const HWND dialog, const bool enabled) {
  trace_enabled_ = enabled;
  set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_TRACE), enabled);
  if (enabled) {
    trace_json_.clear();
    trace_events_.clear();
    trace_display_ = "Waiting for authenticated LayerD events...";
  }
  if (!live_edit_.channel().connected() ||
      !live_edit_.package_authenticated()) {
    trace_display_ =
        "LayerD is not authenticated; start or restart LayerD and try again.";
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
             to_wide(trace_display_));
    trace_enabled_ = false;
    set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_TRACE), false);
    return;
  }
  live_edit_.advance_revision();
  layerc::core::LiveMessage message;
  message.type =
      enabled ? layerc::core::LiveMessageType::automation_trace_subscribe
              : layerc::core::LiveMessageType::automation_trace_unsubscribe;
  message.revision = live_edit_.revision();
  (void)(void)live_edit_.channel().send(message);
}

void AutomationEditor::save_trace(const HWND dialog) {
  if (trace_events_.empty() && trace_json_.empty()) {
    themed_message_box(dialog, L"There is no trace or simulator output to save.",
                L"Automation trace", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (themed_message_box(
          dialog,
          L"Trace files retain typed source-field values for exact replay and "
          L"may include user-authored strings or maps. Save this trace?",
          L"Automation trace privacy", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
    return;
  const auto path = choose_trace_path(dialog, true);
  if (!path.has_value())
    return;
  const auto trace =
      trace_events_.empty() ? trace_json_ : trace_from_events(trace_events_);
  std::ofstream output(*path, std::ios::binary | std::ios::trunc);
  output.write(trace.data(), static_cast<std::streamsize>(trace.size()));
  if (!output)
    themed_message_box(dialog, L"Could not save the trace file.", L"Automation trace",
                MB_OK | MB_ICONERROR);
}

void AutomationEditor::retry_selected(const HWND dialog) {
  const auto *automation = selected_automation(dialog);
  if (automation == nullptr)
    return;
  live_edit_.advance_revision();
  layerc::core::LiveMessage message;
  message.type = layerc::core::LiveMessageType::retry_automation;
  message.revision = live_edit_.revision();
  message.profile_key =
      layerc::platform::canonical_profile_key(document_.path());
  message.text = automation->id;
  (void)(void)live_edit_.channel().send(message);
}
} // namespace layerc::editor

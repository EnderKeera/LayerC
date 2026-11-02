#include "automation_editor.hpp"
#include "editor_automation_internal.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"

#include "layerc/core/profile.hpp"

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <commdlg.h>
#include <windows.h>

namespace layerc::editor {
namespace {

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

} // namespace

std::wstring trace_window_text(const std::string_view text) {
  std::string output;
  output.reserve(text.size());
  for (std::size_t index{}; index < text.size(); ++index) {
    if (text[index] == '\n' && (index == 0 || text[index - 1] != '\r'))
      output += '\r';
    output += text[index];
  }
  return to_wide(output);
}

std::optional<SimulatorPanelPreview>
AutomationEditor::panel_preview(const std::string_view panel_id) const {
  if (dialog_ == nullptr || !IsWindow(dialog_)) return std::nullopt;
  return simulator_.panel_preview(panel_id);
}

void AutomationEditor::update_simulator_view(const HWND dialog) {
  trace_display_ = simulator_.describe(wide_to_utf8(
      window_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_ASSERTIONS))));
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
           trace_window_text(trace_display_));
  RedrawWindow(shell_.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void AutomationEditor::reset_simulator(const HWND dialog) {
  play_simulator(dialog, false);
  const auto reset = simulator_.reset(
      wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_JSON))),
      document_.profile());
  if (reset.error) {
    trace_display_ = *reset.error;
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
             trace_window_text(trace_display_));
    return;
  }
  if (reset.seeded)
    trace_log_.record(*reset.seeded);
  update_simulator_view(dialog);
}

void AutomationEditor::play_simulator(const HWND dialog, const bool playing) {
  KillTimer(dialog, automation_simulator_timer);
  simulator_playing_ = false;
  if (playing) {
    if (!simulator_.compiled())
      reset_simulator(dialog);
    if (simulator_.compiled() &&
        simulator_.position() < simulator_.events().size()) {
      const int speed = static_cast<int>(SendDlgItemMessageW(
          dialog, IDC_AUTOMATION_SIM_SPEED, CB_GETCURSEL, 0, 0));
      simulator_playing_ = SetTimer(
          dialog, automation_simulator_timer,
          static_cast<UINT>(simulator_interval(speed).count()), nullptr) != 0;
    }
  }
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_PLAY),
           simulator_playing_ ? L"Pa&use" : L"Pla&y");
}

void AutomationEditor::step_simulator(const HWND dialog) {
  if (!simulator_.compiled())
    reset_simulator(dialog);
  const auto breakpoint = wide_to_utf8(
      window_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_BREAKPOINT)));
  if (!simulator_.step(breakpoint) || !simulator_playing_)
    play_simulator(dialog, false);
  update_simulator_view(dialog);
}

void AutomationEditor::add_simulator_event(const HWND dialog) {
  constexpr auto &kinds = layerc::core::automation_event_kinds;
  const int selected = static_cast<int>(SendDlgItemMessageW(
      dialog, IDC_AUTOMATION_SIM_EVENT, CB_GETCURSEL, 0, 0));
  if (selected < 0 || static_cast<std::size_t>(selected) >= kinds.size())
    return;
  const auto added = simulator_.add_event(
      kinds[static_cast<std::size_t>(selected)],
      wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_SOURCE))),
      wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_FIELDS))));
  if (const auto *refusal = std::get_if<std::string>(&added)) {
    themed_message_box(dialog, to_wide(*refusal).c_str(), L"Synthetic event",
                       MB_OK | MB_ICONWARNING);
    return;
  }
  trace_log_.record(std::get<layerc::core::AutomationEvent>(added));
  update_simulator_view(dialog);
}

void AutomationEditor::load_trace(const HWND dialog) {
  const auto path = choose_trace_path(dialog, false);
  if (!path)
    return;
  auto loaded = read_trace_file(*path);
  if (const auto *error = std::get_if<TraceLoadError>(&loaded)) {
    themed_message_box(dialog, to_wide(error->message).c_str(),
                       error->invalid ? L"Invalid trace" : L"Load trace",
                       MB_OK | (error->invalid ? MB_ICONWARNING : MB_ICONERROR));
    return;
  }
  auto &trace = std::get<LoadedTrace>(loaded);
  trace_log_.assign(trace.events, std::move(trace.text));
  simulator_.load(std::move(trace.events));
  reset_simulator(dialog);
}

bool AutomationEditor::handle_live_message(
    const layerc::core::LiveMessage &message) {
  if (message.type != layerc::core::LiveMessageType::automation_trace_event)
    return false;

  trace_display_ = trace_log_.append_packet(message.text);
  if (dialog_ != nullptr && IsWindow(dialog_))
    set_text(GetDlgItem(dialog_, IDC_AUTOMATION_TRACE_TEXT),
             trace_window_text(trace_display_));
  return true;
}

void AutomationEditor::restore_live_subscription() {
  if (!trace_enabled_)
    return;
  (void)live_edit_.send_authenticated(
      {.type = layerc::core::LiveMessageType::automation_trace_subscribe});
}
void AutomationEditor::set_trace(const HWND dialog, const bool enabled) {
  trace_enabled_ = enabled;
  set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_TRACE), enabled);
  if (enabled) {
    trace_log_.clear();
    trace_display_ = "Waiting for authenticated LayerD events...";
  }
  if (!live_edit_.connected() || !live_edit_.authenticated()) {
    trace_display_ =
        "LayerD is not authenticated; start or restart LayerD and try again.";
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
             to_wide(trace_display_));
    trace_enabled_ = false;
    set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_TRACE), false);
    return;
  }
  (void)live_edit_.send_authenticated(
      {.type = enabled
                   ? layerc::core::LiveMessageType::automation_trace_subscribe
                   : layerc::core::LiveMessageType::automation_trace_unsubscribe});
}

void AutomationEditor::save_trace(const HWND dialog) {
  if (trace_log_.empty()) {
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
  if (!write_trace_file(*path, trace_log_.export_text()))
    themed_message_box(dialog, L"Could not save the trace file.", L"Automation trace",
                       MB_OK | MB_ICONERROR);
}

void AutomationEditor::retry_selected(const HWND dialog) {
  const auto *automation = selected_automation(dialog);
  if (automation == nullptr)
    return;
  (void)live_edit_.send_authenticated(
      {.type = layerc::core::LiveMessageType::retry_automation,
       .profile_key = layerc::platform::canonical_profile_key(document_.path()),
       .text = automation->id});
}
} // namespace layerc::editor

#include "profile_edit.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <system_error>

namespace layerc::editor {

layerc::core::GameProfile make_new_profile() {
  using namespace std::chrono_literals;
  using namespace layerc::core;
  GameProfile profile;
  profile.id = "new-profile";
  profile.name = "New Profile";
  profile.enabled = true;
  profile.cooldowns.push_back({.id = "ability",
                               .name = "Ability",
                               .duration = 5s,
                               .max_charges = 1,
                               .retrigger = RetriggerPolicy::ignore});
  profile.panels.push_back({.id = "main",
                            .name = "Main",
                            .style = PanelStyle::bar,
                            .bar = {},
                            .anchor_x = 0.5,
                            .anchor_y = 0.75,
                            .cooldown_ids = {"ability"}});
  profile.bindings.push_back(
      {.cooldown_id = "ability",
       .chord = InputChord{{{InputDevice::keyboard, 0x12, 0}}}});
  return profile;
}

std::string profile_list_text(const layerc::core::GameProfile &profile) {
  std::string label = profile.name;
  if (!profile.game.executable_name.empty())
    label += "  \u2014  " + profile.game.executable_name;
  if (!profile.enabled)
    label += "  (off)";
  return label;
}

std::string unique_panel_id(
    const std::string_view base,
    const std::vector<layerc::core::Panel> &existing,
    const std::vector<layerc::core::Panel> *generated) {
  return next_unique_id(base, [&](const std::string_view candidate) {
    const auto contains = [&](const std::vector<layerc::core::Panel> &panels) {
      return std::ranges::any_of(panels, [&](const layerc::core::Panel &panel) {
        return panel.id == candidate;
      });
    };
    return contains(existing) ||
           (generated != nullptr && contains(*generated));
  });
}

std::optional<std::size_t>
split_grouped_bars(layerc::core::GameProfile &profile, const long target_height) {
  using layerc::core::Panel;
  std::vector<Panel> split;
  split.reserve(profile.panels.size() + profile.cooldowns.size());
  std::optional<std::size_t> first_split;
  const auto height = static_cast<double>(std::max(1L, target_height));
  for (const auto &panel : profile.panels) {
    if (panel.style != layerc::core::PanelStyle::bar || panel.cooldown_ids.size() <= 1) {
      split.push_back(panel);
      continue;
    }
    const double offset =
        (static_cast<double>(panel.bar.height) + layerc::core::overlay_metrics.spacing) / height;
    const auto count = static_cast<double>(panel.cooldown_ids.size());
    for (std::size_t index = 0; index < panel.cooldown_ids.size(); ++index) {
      auto bar = panel;
      const auto &cooldown = panel.cooldown_ids[index];
      bar.id = unique_panel_id(panel.id + "-" + cooldown, profile.panels, &split);
      bar.name = panel.name + " " + cooldown;
      bar.cooldown_ids = {cooldown};
      bar.anchor_y = std::clamp(
          panel.anchor_y + (static_cast<double>(index) - (count - 1.0) / 2.0) * offset, 0.0, 1.0);
      if (!first_split) first_split = split.size();
      split.push_back(std::move(bar));
    }
  }
  if (first_split) profile.panels = std::move(split);
  return first_split;
}

std::optional<layerc::core::Milliseconds>
parse_duration_seconds(const std::string_view seconds) {
  if (seconds.empty())
    return std::nullopt;
  double parsed{};
  const auto [end, code] =
      std::from_chars(seconds.data(), seconds.data() + seconds.size(), parsed);
  if (code != std::errc{} || end != seconds.data() + seconds.size() ||
      !std::isfinite(parsed) || parsed <= 0.0 ||
      parsed * 1000.0 >
          static_cast<double>(layerc::core::max_cooldown_duration.count()))
    return std::nullopt;
  const auto milliseconds = std::llround(parsed * 1000.0);
  if (milliseconds < 1 ||
      milliseconds > layerc::core::max_cooldown_duration.count())
    return std::nullopt;
  return layerc::core::Milliseconds(milliseconds);
}

std::string profile_error_line(const layerc::core::ProfileError &error) {
  return error.path.empty() ? error.message : error.path + ": " + error.message;
}

void erase_visual_assignment(layerc::core::VisualProfile &visual,
                             const std::string_view cooldown_id) {
  std::string old_rule;
  const auto trigger = std::ranges::find(
      visual.triggers, cooldown_id, &layerc::core::VisualTrigger::cooldown_id);
  if (trigger != visual.triggers.end())
    old_rule = trigger->observation_id;
  std::erase_if(visual.triggers, [&](const auto &t) {
    return t.cooldown_id == cooldown_id;
  });
  gc_orphan_visual_observation(visual, old_rule);
}

void gc_orphan_visual_observation(layerc::core::VisualProfile &visual,
                                  const std::string_view observation_id) {
  if (observation_id.empty()) return;
  if (std::ranges::any_of(visual.triggers, [&](const auto &t) {
        return t.observation_id == observation_id;
      }))
    return;
  std::erase_if(visual.observations,
                [&](const layerc::core::VisualObservation &item) {
                  return layerc::core::observation_id(item) == observation_id;
                });
}

} // namespace layerc::editor

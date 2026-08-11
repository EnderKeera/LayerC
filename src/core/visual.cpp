#include "layerc/core/visual.hpp"

#include "layerc/core/json.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace layerc::core {
namespace {

class Reader {
public:
  Reader(const json::Value &root, const GameProfile &base)
      : root_(root), base_(base) {}

  VisualResult read() {
    if (!root_.is_object()) return error("$", "visual root must be an object");
    const auto version = integer(root_, "schemaVersion", "$", 1, 2);
    if (!version) return error_;
    if (*version == 1) {
      if (!fields(root_, {"schemaVersion", "profileId", "sampleRateHz",
                          "quality", "rules", "triggers"}, "$"))
        return error_;
    } else if (!fields(root_, {"schemaVersion", "profileId", "sampleRateHz",
                               "quality", "observations", "triggers"}, "$")) {
      return error_;
    }

    VisualProfile result;
    if (!version || !string(root_, "profileId", "$", result.profile_id))
      return error_;
    // Schema 1 is migrated in memory. Every subsequent save emits schema 2.
    result.schema_version = 2;
    const auto rate = integer(root_, "sampleRateHz", "$", 5, 15);
    std::string quality;
    if (!rate || !string(root_, "quality", "$", quality)) return error_;
    if (*rate != 5 && *rate != 10 && *rate != 15)
      return error("$.sampleRateHz", "sample rate must be 5, 10, or 15 Hz");
    result.sample_rate_hz = static_cast<std::uint8_t>(*rate);
    if (quality == "low")
      result.quality = CaptureQuality::low;
    else if (quality == "balanced")
      result.quality = CaptureQuality::balanced;
    else if (quality == "exact")
      result.quality = CaptureQuality::exact;
    else
      return error("$.quality", "expected low, balanced, or exact");

    if (*version == 1) {
      const auto *rules = array(root_, "rules", "$", true);
      if (rules == nullptr) return error_;
      if (rules->size() > max_pixel_rules)
        return error("$.rules", "at most four pixel rules are supported");
      for (std::size_t index = 0; index < rules->size(); ++index) {
        if (!read_rule((*rules)[index], index, result.rules, false))
          return error_;
      }
    } else {
      const auto *observations = array(root_, "observations", "$", true);
      if (observations == nullptr) return error_;
      if (observations->size() > max_pixel_rules + max_template_states)
        return error("$.observations",
                     "at most eight visual observations are supported");
      for (std::size_t index = 0; index < observations->size(); ++index) {
        if (!read_observation((*observations)[index], index, result))
          return error_;
      }
    }

    const auto *triggers = array(root_, "triggers", "$", true);
    if (triggers == nullptr) return error_;
    for (std::size_t index = 0; index < triggers->size(); ++index) {
      if (!read_trigger((*triggers)[index], index, *version, result.triggers))
        return error_;
    }
    const auto validation = validate_visual_profile(result, base_);
    if (!validation.message.empty()) return validation;
    return result;
  }

private:
  bool read_rule(const json::Value &value, const std::size_t index,
                 std::vector<PixelRule> &output, const bool typed) {
    const auto path = std::string(typed ? "$.observations[" : "$.rules[") +
                      std::to_string(index) + "]";
    if (!value.is_object()) return set(path, "pixel rule must be an object");
    if ((typed &&
         !fields(value, {"type", "id", "name", "region", "predicate",
                         "stableSamples", "repeat"}, path)) ||
        (!typed &&
         !fields(value, {"id", "name", "region", "predicate",
                         "stableSamples", "repeat"}, path)))
      return false;
    PixelRule rule;
    if (!string(value, "id", path, rule.id) ||
        !string(value, "name", path, rule.name))
      return false;
    const auto *region = object(value, "region", path);
    if (region == nullptr ||
        !fields(*region, {"x", "y", "width", "height"}, path + ".region"))
      return false;
    const auto x = number(*region, "x", path + ".region", 0.0, 1.0);
    const auto y = number(*region, "y", path + ".region", 0.0, 1.0);
    const auto width = number(*region, "width", path + ".region", 0.0, 1.0);
    const auto height = number(*region, "height", path + ".region", 0.0, 1.0);
    if (!x || !y || !width || !height) return false;
    rule.region = {*x, *y, *width, *height};

    const auto *predicate = object(value, "predicate", path);
    if (predicate == nullptr) return false;
    std::string kind;
    if (!string(*predicate, "type", path + ".predicate", kind)) return false;
    if (kind == "averageColor") {
      if (!fields(*predicate, {"type", "target", "tolerance"},
                  path + ".predicate"))
        return false;
      const auto target = color(*predicate, "target", path + ".predicate");
      const auto tolerance = integer(*predicate, "tolerance",
                                     path + ".predicate", 0, 255);
      if (!target || !tolerance) return false;
      rule.predicate = AverageColorPredicate{
          *target, static_cast<std::uint8_t>(*tolerance)};
    } else if (kind == "brightness") {
      if (!fields(*predicate, {"type", "comparison", "threshold"},
                  path + ".predicate"))
        return false;
      std::string comparison;
      const auto threshold = integer(*predicate, "threshold",
                                     path + ".predicate", 0, 255);
      if (!threshold || !string(*predicate, "comparison",
                                path + ".predicate", comparison))
        return false;
      BrightnessComparison parsed{};
      if (comparison == "above")
        parsed = BrightnessComparison::above;
      else if (comparison == "below")
        parsed = BrightnessComparison::below;
      else
        return set(path + ".predicate.comparison",
                   "expected above or below");
      rule.predicate = BrightnessPredicate{
          static_cast<std::uint8_t>(*threshold), parsed};
    } else if (kind == "colorCoverage") {
      if (!fields(*predicate,
                  {"type", "target", "tolerance", "minimumPercent"},
                  path + ".predicate"))
        return false;
      const auto target = color(*predicate, "target", path + ".predicate");
      const auto tolerance = integer(*predicate, "tolerance",
                                     path + ".predicate", 0, 255);
      const auto minimum = number(*predicate, "minimumPercent",
                                  path + ".predicate", 0.0, 100.0);
      if (!target || !tolerance || !minimum) return false;
      rule.predicate = ColorCoveragePredicate{
          *target, static_cast<std::uint8_t>(*tolerance), *minimum / 100.0};
    } else {
      return set(path + ".predicate.type", "unsupported pixel predicate");
    }

    const auto stable = integer(value, "stableSamples", path, 1, 15);
    std::string repeat;
    if (!stable || !string(value, "repeat", path, repeat)) return false;
    rule.stable_samples = static_cast<std::uint8_t>(*stable);
    if (repeat == "edge")
      rule.repeat = PixelRepeatPolicy::edge;
    else if (repeat == "whenFullyReady")
      rule.repeat = PixelRepeatPolicy::when_fully_ready;
    else
      return set(path + ".repeat", "expected edge or whenFullyReady");
    output.push_back(std::move(rule));
    return true;
  }

  bool read_observation(const json::Value &value, const std::size_t index,
                        VisualProfile &output) {
    const auto path = "$.observations[" + std::to_string(index) + "]";
    if (!value.is_object())
      return set(path, "visual observation must be an object");
    std::string type;
    if (!string(value, "type", path, type)) return false;
    if (type == "pixel") {
      if (output.rules.size() >= max_pixel_rules)
        return set(path, "at most four pixel observations are supported");
      return read_rule(value, index, output.rules, true);
    }
    if (type != "template")
      return set(path + ".type", "expected pixel or template");
    if (output.templates.size() >= max_template_states)
      return set(path, "at most four template observations are supported");
    return read_template(value, index, output.templates);
  }

  bool read_template(const json::Value &value, const std::size_t index,
                     std::vector<TemplateState> &output) {
    const auto path = "$.observations[" + std::to_string(index) + "]";
    if (!fields(value,
                {"type", "id", "name", "searchRegion", "expectedSize",
                 "references", "confidence", "scaleTolerancePercent",
                 "colorTolerance", "stableSamples", "repeat"},
                path))
      return false;
    TemplateState state;
    if (!string(value, "id", path, state.id) ||
        !string(value, "name", path, state.name))
      return false;

    const auto *region = object(value, "searchRegion", path);
    if (region == nullptr ||
        !fields(*region, {"x", "y", "width", "height"},
                path + ".searchRegion"))
      return false;
    const auto x = number(*region, "x", path + ".searchRegion", 0.0, 1.0);
    const auto y = number(*region, "y", path + ".searchRegion", 0.0, 1.0);
    const auto width =
        number(*region, "width", path + ".searchRegion", 0.0, 1.0);
    const auto height =
        number(*region, "height", path + ".searchRegion", 0.0, 1.0);
    if (!x || !y || !width || !height) return false;
    state.search_region = {*x, *y, *width, *height};

    const auto *expected = object(value, "expectedSize", path);
    if (expected == nullptr ||
        !fields(*expected, {"width", "height"}, path + ".expectedSize"))
      return false;
    const auto expected_width =
        number(*expected, "width", path + ".expectedSize", 0.000001, 1.0);
    const auto expected_height =
        number(*expected, "height", path + ".expectedSize", 0.000001, 1.0);
    if (!expected_width || !expected_height) return false;
    state.expected_width = *expected_width;
    state.expected_height = *expected_height;

    const auto *references = array(value, "references", path, true);
    if (references == nullptr || references->empty() ||
        references->size() > max_template_references)
      return set(path + ".references",
                 "one to four template references are required");
    for (std::size_t reference_index = 0;
         reference_index < references->size(); ++reference_index) {
      const auto reference_path =
          path + ".references[" + std::to_string(reference_index) + "]";
      const auto &reference = (*references)[reference_index];
      if (!reference.is_object() ||
          !fields(reference, {"asset"}, reference_path))
        return set(reference_path, "template reference must be an object");
      TemplateReference parsed;
      if (!string(reference, "asset", reference_path, parsed.asset_sha256))
        return false;
      state.references.push_back(std::move(parsed));
    }

    const auto confidence =
        number(value, "confidence", path, 0.0, 1.0);
    const auto scale =
        number(value, "scaleTolerancePercent", path, 0.0, 20.0);
    const auto color_tolerance =
        integer(value, "colorTolerance", path, 0, 255);
    const auto stable = integer(value, "stableSamples", path, 1, 15);
    std::string repeat;
    if (!confidence || !scale || !color_tolerance || !stable ||
        !string(value, "repeat", path, repeat))
      return false;
    state.confidence_threshold = *confidence;
    state.scale_tolerance = *scale / 100.0;
    state.color_tolerance = static_cast<std::uint8_t>(*color_tolerance);
    state.stable_samples = static_cast<std::uint8_t>(*stable);
    if (repeat == "edge")
      state.repeat = PixelRepeatPolicy::edge;
    else if (repeat == "whenFullyReady")
      state.repeat = PixelRepeatPolicy::when_fully_ready;
    else
      return set(path + ".repeat", "expected edge or whenFullyReady");
    output.push_back(std::move(state));
    return true;
  }

  bool read_trigger(const json::Value &value, const std::size_t index,
                    const std::int64_t schema_version,
                    std::vector<VisualTrigger> &output) {
    const auto path = "$.triggers[" + std::to_string(index) + "]";
    if (!value.is_object()) return set(path, "visual trigger must be an object");
    if ((schema_version == 1 &&
         !fields(value,
                 {"cooldownId", "ruleId", "enabled", "mode", "keepInput",
                  "confirmationTimeoutMs", "onUnavailable"},
                 path)) ||
        (schema_version == 2 &&
         !fields(value,
                 {"cooldownId", "observationId", "enabled", "mode",
                  "keepInput", "confirmationTimeoutMs", "onUnavailable"},
                 path)))
      return false;
    VisualTrigger trigger;
    if (!string(value, "cooldownId", path, trigger.cooldown_id) ||
        !string(value, schema_version == 1 ? "ruleId" : "observationId", path,
                trigger.observation_id))
      return false;
    if (!boolean(value, "enabled", path, trigger.enabled, true)) return false;
    std::string mode;
    if (!string(value, "mode", path, mode)) return false;
    if (mode == "pixelOnly") {
      trigger.mode = VisualTriggerMode::pixel_only;
      if (value.find("confirmationTimeoutMs") != nullptr)
        return set(path + ".confirmationTimeoutMs",
                   "field is only valid for confirmInput");
      if (!boolean(value, "keepInput", path, trigger.keep_input, false))
        return false;
    } else if (mode == "confirmInput") {
      trigger.mode = VisualTriggerMode::confirm_input;
      if (value.find("keepInput") != nullptr)
        return set(path + ".keepInput",
                   "field is only valid for pixelOnly");
      const auto timeout = integer(value, "confirmationTimeoutMs", path,
                                   50, 5'000);
      if (!timeout)
        return false;
      trigger.confirmation_timeout = Milliseconds(*timeout);
    } else {
      return set(path + ".mode", "expected pixelOnly or confirmInput");
    }
    std::string unavailable = "ignore";
    if (const auto *policy_value = value.find("onUnavailable");
        policy_value != nullptr) {
      if (!policy_value->is_string())
        return set(path + ".onUnavailable", "expected string");
      unavailable = policy_value->as_string();
    }
    if (unavailable == "ignore")
      trigger.unavailable_policy = CaptureUnavailablePolicy::ignore;
    else if (unavailable == "triggerFromInput")
      trigger.unavailable_policy =
          CaptureUnavailablePolicy::trigger_from_input;
    else
      return set(path + ".onUnavailable",
                 "expected ignore or triggerFromInput");
    output.push_back(std::move(trigger));
    return true;
  }

  bool fields(const json::Value &value,
              const std::initializer_list<std::string_view> allowed,
              const std::string &path) {
    if (!value.is_object()) return set(path, "expected object");
    for (const auto &[key, ignored] : value.as_object()) {
      (void)ignored;
      if (std::ranges::find(allowed, key) == allowed.end())
        return set(path + "." + key, "unknown field");
    }
    return true;
  }

  const json::Value *object(const json::Value &parent, std::string_view key,
                            const std::string &path) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      set(path + "." + std::string(key), "required value is missing");
      return nullptr;
    }
    if (!value->is_object()) {
      set(path + "." + std::string(key), "expected object");
      return nullptr;
    }
    return value;
  }

  const json::Value::Array *array(const json::Value &parent,
                                  std::string_view key,
                                  const std::string &path, bool required) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      if (required) set(path + "." + std::string(key), "required value is missing");
      return nullptr;
    }
    if (!value->is_array()) {
      set(path + "." + std::string(key), "expected array");
      return nullptr;
    }
    return &value->as_array();
  }

  bool string(const json::Value &parent, std::string_view key,
              const std::string &path, std::string &output) {
    const auto *value = parent.find(key);
    if (value == nullptr || !value->is_string() || value->as_string().empty())
      return set(path + "." + std::string(key),
                 "expected non-empty string");
    output = value->as_string();
    return true;
  }

  bool boolean(const json::Value &parent, std::string_view key,
               const std::string &path, bool &output, bool fallback) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      output = fallback;
      return true;
    }
    if (!value->is_bool())
      return set(path + "." + std::string(key), "expected boolean");
    output = value->as_bool();
    return true;
  }

  std::optional<std::int64_t>
  integer(const json::Value &parent, std::string_view key,
          const std::string &path, std::int64_t minimum,
          std::int64_t maximum) {
    const auto *value = parent.find(key);
    if (value == nullptr || !value->is_number()) {
      set(path + "." + std::string(key), "expected integer");
      return std::nullopt;
    }
    const double number = value->as_number();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum)) {
      set(path + "." + std::string(key),
          "integer is outside the supported range");
      return std::nullopt;
    }
    return static_cast<std::int64_t>(number);
  }

  std::optional<double> number(const json::Value &parent, std::string_view key,
                               const std::string &path, double minimum,
                               double maximum) {
    const auto *value = parent.find(key);
    if (value == nullptr || !value->is_number() ||
        !std::isfinite(value->as_number()) || value->as_number() < minimum ||
        value->as_number() > maximum) {
      set(path + "." + std::string(key),
          "number is outside the supported range");
      return std::nullopt;
    }
    return value->as_number();
  }

  std::optional<std::uint32_t>
  color(const json::Value &parent, std::string_view key,
        const std::string &path) {
    const auto *value = parent.find(key);
    if (value == nullptr || !value->is_string() ||
        value->as_string().size() != 7 || value->as_string().front() != '#') {
      set(path + "." + std::string(key), "expected #RRGGBB color");
      return std::nullopt;
    }
    std::uint32_t result{};
    for (const char character : value->as_string().substr(1)) {
      result <<= 4U;
      if (character >= '0' && character <= '9')
        result |= static_cast<std::uint32_t>(character - '0');
      else if (character >= 'a' && character <= 'f')
        result |= static_cast<std::uint32_t>(character - 'a' + 10);
      else if (character >= 'A' && character <= 'F')
        result |= static_cast<std::uint32_t>(character - 'A' + 10);
      else {
        set(path + "." + std::string(key), "expected #RRGGBB color");
        return std::nullopt;
      }
    }
    return result;
  }

  VisualError error(std::string path, std::string message) {
    return {.path = std::move(path), .message = std::move(message)};
  }
  bool set(std::string path, std::string message) {
    if (error_.message.empty()) error_ = error(std::move(path), std::move(message));
    return false;
  }

  const json::Value &root_;
  const GameProfile &base_;
  VisualError error_;
};

void color_text(std::string &output, std::uint32_t color) {
  constexpr std::string_view hex = "0123456789ABCDEF";
  output += "\"#";
  for (int shift = 20; shift >= 0; shift -= 4)
    output.push_back(hex[(color >> static_cast<unsigned>(shift)) & 0xFU]);
  output.push_back('"');
}

bool valid_region(const NormalizedRegion &region) {
  return std::isfinite(region.x) && std::isfinite(region.y) &&
         std::isfinite(region.width) && std::isfinite(region.height) &&
         region.x >= 0.0 && region.y >= 0.0 && region.width > 0.0 &&
         region.height > 0.0 &&
         region.x + region.width <= 1.0 + 1e-12 &&
         region.y + region.height <= 1.0 + 1e-12;
}

bool valid_sha256(const std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

} // namespace

VisualError validate_visual_profile(const VisualProfile &profile,
                                    const GameProfile &base_profile) {
  const auto invalid = [](std::string path, std::string message) {
    return VisualError{std::move(path), std::move(message)};
  };
  if (profile.schema_version != 2)
    return invalid("$.schemaVersion", "unsupported visual schema version");
  if (profile.profile_id != base_profile.id)
    return invalid("$.profileId", "visual sidecar does not match profile id");
  if (base_profile.game.global && !profile.triggers.empty())
    return invalid("$.triggers", "global profiles cannot use visual capture");
  if (profile.sample_rate_hz != 5 && profile.sample_rate_hz != 10 &&
      profile.sample_rate_hz != 15)
    return invalid("$.sampleRateHz", "sample rate must be 5, 10, or 15 Hz");
  if (profile.rules.size() > max_pixel_rules)
    return invalid("$.rules", "at most four pixel rules are supported");
  if (profile.templates.size() > max_template_states)
    return invalid("$.templates",
                   "at most four template observations are supported");

  std::set<std::string, std::less<>> observation_ids;
  for (std::size_t index = 0; index < profile.rules.size(); ++index) {
    const auto &rule = profile.rules[index];
    const auto path = "$.rules[" + std::to_string(index) + "]";
    if (rule.id.empty() || rule.name.empty())
      return invalid(path, "rule id and name are required");
    if (!observation_ids.insert(rule.id).second)
      return invalid(path + ".id", "duplicate visual observation id");
    if (!valid_region(rule.region))
      return invalid(path + ".region",
                     "normalized region must be positive and remain in frame");
    if (rule.stable_samples < 1 || rule.stable_samples > 15)
      return invalid(path + ".stableSamples",
                     "stable sample count must be between 1 and 15");
    if (const auto *coverage =
            std::get_if<ColorCoveragePredicate>(&rule.predicate);
        coverage != nullptr &&
        (!std::isfinite(coverage->minimum_fraction) ||
         coverage->minimum_fraction < 0.0 ||
         coverage->minimum_fraction > 1.0))
      return invalid(path + ".predicate.minimumPercent",
                     "minimum percentage is outside the supported range");
  }

  for (std::size_t index = 0; index < profile.templates.size(); ++index) {
    const auto &state = profile.templates[index];
    const auto path = "$.templates[" + std::to_string(index) + "]";
    if (state.id.empty() || state.name.empty())
      return invalid(path, "template id and name are required");
    if (!observation_ids.insert(state.id).second)
      return invalid(path + ".id", "duplicate visual observation id");
    if (!valid_region(state.search_region))
      return invalid(path + ".searchRegion",
                     "normalized search region must remain in frame");
    if (!std::isfinite(state.expected_width) ||
        !std::isfinite(state.expected_height) ||
        state.expected_width <= 0.0 || state.expected_width > 1.0 ||
        state.expected_height <= 0.0 || state.expected_height > 1.0)
      return invalid(path + ".expectedSize",
                     "expected template size must be normalized and positive");
    if (state.expected_width > state.search_region.width + 1e-12 ||
        state.expected_height > state.search_region.height + 1e-12)
      return invalid(path + ".expectedSize",
                     "expected template size must fit inside its search region");
    if (state.references.empty() ||
        state.references.size() > max_template_references)
      return invalid(path + ".references",
                     "one to four template references are required");
    std::set<std::string, std::less<>> references;
    for (std::size_t reference_index = 0;
         reference_index < state.references.size(); ++reference_index) {
      const auto &reference = state.references[reference_index];
      const auto reference_path =
          path + ".references[" + std::to_string(reference_index) + "].asset";
      if (!valid_sha256(reference.asset_sha256))
        return invalid(reference_path,
                       "asset must be a lower-case SHA-256 digest");
      if (!references.insert(reference.asset_sha256).second)
        return invalid(reference_path, "duplicate template reference");
    }
    if (!std::isfinite(state.confidence_threshold) ||
        state.confidence_threshold < 0.0 ||
        state.confidence_threshold > 1.0)
      return invalid(path + ".confidence",
                     "confidence must be between zero and one");
    if (!std::isfinite(state.scale_tolerance) ||
        state.scale_tolerance < 0.0 || state.scale_tolerance > 0.20)
      return invalid(path + ".scaleTolerancePercent",
                     "scale tolerance must be between zero and 20 percent");
    if (state.stable_samples < 1 || state.stable_samples > 15)
      return invalid(path + ".stableSamples",
                     "stable sample count must be between 1 and 15");
  }

  std::set<std::string, std::less<>> cooldown_ids;
  for (const auto &cooldown : base_profile.cooldowns)
    cooldown_ids.insert(cooldown.id);
  std::set<std::string, std::less<>> assigned;
  for (std::size_t index = 0; index < profile.triggers.size(); ++index) {
    const auto &trigger = profile.triggers[index];
    const auto path = "$.triggers[" + std::to_string(index) + "]";
    if (!cooldown_ids.contains(trigger.cooldown_id))
      return invalid(path + ".cooldownId", "unknown cooldown id");
    if (!has_observation(profile, trigger.observation_id))
      return invalid(path + ".observationId", "unknown visual observation id");
    if (!assigned.insert(trigger.cooldown_id).second)
      return invalid(path + ".cooldownId",
                     "only one visual trigger per cooldown is supported");
    if (trigger.mode == VisualTriggerMode::confirm_input) {
      const bool has_binding = std::ranges::any_of(
          base_profile.bindings, [&](const InputBinding &binding) {
            return binding.cooldown_id == trigger.cooldown_id;
          });
      if (!has_binding)
        return invalid(path, "input confirmation requires an input binding");
      if (trigger.confirmation_timeout < Milliseconds(50) ||
          trigger.confirmation_timeout > Milliseconds(5'000))
        return invalid(path + ".confirmationTimeoutMs",
                       "confirmation timeout is outside the supported range");
    }
  }
  return {};
}

const PixelRule *find_pixel_rule(const VisualProfile &profile,
                                 const std::string_view id) noexcept {
  const auto found = std::ranges::find(profile.rules, id, &PixelRule::id);
  return found == profile.rules.end() ? nullptr : &*found;
}

const TemplateState *find_template_state(const VisualProfile &profile,
                                         const std::string_view id) noexcept {
  const auto found =
      std::ranges::find(profile.templates, id, &TemplateState::id);
  return found == profile.templates.end() ? nullptr : &*found;
}

bool has_observation(const VisualProfile &profile,
                     const std::string_view id) noexcept {
  return find_pixel_rule(profile, id) != nullptr ||
         find_template_state(profile, id) != nullptr;
}

std::uint8_t observation_stable_samples(const VisualProfile &profile,
                                        const std::string_view id) noexcept {
  if (const auto *rule = find_pixel_rule(profile, id)) return rule->stable_samples;
  if (const auto *state = find_template_state(profile, id))
    return state->stable_samples;
  return 1;
}

PixelRepeatPolicy observation_repeat_policy(
    const VisualProfile &profile, const std::string_view id) noexcept {
  if (const auto *rule = find_pixel_rule(profile, id)) return rule->repeat;
  if (const auto *state = find_template_state(profile, id))
    return state->repeat;
  return PixelRepeatPolicy::edge;
}

VisualResult parse_visual_profile_json(const std::string_view source,
                                       const GameProfile &base_profile) {
  if (source.size() > max_visual_json_bytes)
    return VisualError{"$", "visual sidecar exceeds 256 KiB limit"};
  const auto parsed = json::parse(source);
  if (const auto *error = std::get_if<json::ParseError>(&parsed))
    return VisualError{"$", "invalid JSON at byte " +
                                std::to_string(error->offset) + ": " +
                                error->message};
  return Reader(std::get<json::Value>(parsed), base_profile).read();
}

std::string serialize_visual_profile_json(const VisualProfile &profile) {
  std::string output;
  output.reserve(2'048);
  output += "{\n  \"schemaVersion\": 2,\n  \"profileId\": ";
  json::append_quoted(output, profile.profile_id);
  output += ",\n  \"sampleRateHz\": " +
            std::to_string(profile.sample_rate_hz) + ",\n  \"quality\": ";
  json::append_quoted(output, profile.quality == CaptureQuality::low
                     ? "low"
                     : profile.quality == CaptureQuality::exact ? "exact"
                                                                 : "balanced");
  output += ",\n  \"observations\": [\n";
  const auto observation_count =
      profile.rules.size() + profile.templates.size();
  std::size_t observation_index{};
  for (std::size_t index = 0; index < profile.rules.size(); ++index) {
    const auto &rule = profile.rules[index];
    output += "    {\n      \"type\": \"pixel\",\n      \"id\": ";
    json::append_quoted(output, rule.id);
    output += ",\n      \"name\": "; json::append_quoted(output, rule.name);
    output += ",\n      \"region\": {\"x\": "; json::append_number(output, rule.region.x);
    output += ", \"y\": "; json::append_number(output, rule.region.y);
    output += ", \"width\": "; json::append_number(output, rule.region.width);
    output += ", \"height\": "; json::append_number(output, rule.region.height);
    output += "},\n      \"predicate\": {";
    std::visit(
        [&](const auto &predicate) {
          using T = std::decay_t<decltype(predicate)>;
          if constexpr (std::is_same_v<T, AverageColorPredicate>) {
            output += "\"type\": \"averageColor\", \"target\": ";
            color_text(output, predicate.target_color);
            output += ", \"tolerance\": " +
                      std::to_string(predicate.tolerance);
          } else if constexpr (std::is_same_v<T, BrightnessPredicate>) {
            output += "\"type\": \"brightness\", \"comparison\": \"";
            output += predicate.comparison == BrightnessComparison::above
                          ? "above"
                          : "below";
            output += "\", \"threshold\": " +
                      std::to_string(predicate.threshold);
          } else {
            output += "\"type\": \"colorCoverage\", \"target\": ";
            color_text(output, predicate.target_color);
            output += ", \"tolerance\": " +
                      std::to_string(predicate.tolerance) +
                      ", \"minimumPercent\": ";
            json::append_number(output, predicate.minimum_fraction * 100.0);
          }
        },
        rule.predicate);
    output += "},\n      \"stableSamples\": " +
              std::to_string(rule.stable_samples) + ",\n      \"repeat\": \"" +
              std::string(rule.repeat == PixelRepeatPolicy::edge
                              ? "edge"
                              : "whenFullyReady") +
              "\"\n    }";
    output += ++observation_index == observation_count ? "\n" : ",\n";
  }
  for (const auto &state : profile.templates) {
    output += "    {\n      \"type\": \"template\",\n      \"id\": ";
    json::append_quoted(output, state.id);
    output += ",\n      \"name\": ";
    json::append_quoted(output, state.name);
    output += ",\n      \"searchRegion\": {\"x\": ";
    json::append_number(output, state.search_region.x);
    output += ", \"y\": ";
    json::append_number(output, state.search_region.y);
    output += ", \"width\": ";
    json::append_number(output, state.search_region.width);
    output += ", \"height\": ";
    json::append_number(output, state.search_region.height);
    output += "},\n      \"expectedSize\": {\"width\": ";
    json::append_number(output, state.expected_width);
    output += ", \"height\": ";
    json::append_number(output, state.expected_height);
    output += "},\n      \"references\": [";
    for (std::size_t index = 0; index < state.references.size(); ++index) {
      if (index != 0) output += ", ";
      output += "{\"asset\": ";
      json::append_quoted(output, state.references[index].asset_sha256);
      output += "}";
    }
    output += "],\n      \"confidence\": ";
    json::append_number(output, state.confidence_threshold);
    output += ",\n      \"scaleTolerancePercent\": ";
    json::append_number(output, state.scale_tolerance * 100.0);
    output += ",\n      \"colorTolerance\": " +
              std::to_string(state.color_tolerance);
    output += ",\n      \"stableSamples\": " +
              std::to_string(state.stable_samples);
    output += ",\n      \"repeat\": \"" +
              std::string(state.repeat == PixelRepeatPolicy::edge
                              ? "edge"
                              : "whenFullyReady") +
              "\"\n    }";
    output += ++observation_index == observation_count ? "\n" : ",\n";
  }
  output += "  ],\n  \"triggers\": [\n";
  for (std::size_t index = 0; index < profile.triggers.size(); ++index) {
    const auto &trigger = profile.triggers[index];
    output += "    {\n      \"cooldownId\": "; json::append_quoted(output, trigger.cooldown_id);
    output += ",\n      \"observationId\": ";
    json::append_quoted(output, trigger.observation_id);
    output += ",\n      \"enabled\": ";
    output += trigger.enabled ? "true" : "false";
    output += ",\n      \"mode\": \"" +
              std::string(trigger.mode == VisualTriggerMode::pixel_only
                              ? "pixelOnly"
                              : "confirmInput") +
              "\"";
    if (trigger.mode == VisualTriggerMode::pixel_only) {
      output += ",\n      \"keepInput\": ";
      output += trigger.keep_input ? "true" : "false";
    } else {
      output += ",\n      \"confirmationTimeoutMs\": " +
                std::to_string(trigger.confirmation_timeout.count());
    }
    output += ",\n      \"onUnavailable\": \"" +
              std::string(trigger.unavailable_policy ==
                                  CaptureUnavailablePolicy::trigger_from_input
                              ? "triggerFromInput"
                              : "ignore") +
              "\"";
    output += "\n    }";
    output += index + 1 == profile.triggers.size() ? "\n" : ",\n";
  }
  output += "  ]\n}\n";
  return output;
}

bool has_enabled_visual_assignments(const VisualProfile &profile) noexcept {
  return std::ranges::any_of(profile.triggers,
                             &VisualTrigger::enabled);
}

} // namespace layerc::core

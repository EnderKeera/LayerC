#pragma once

#include "test_support.hpp"

#include "layerc/core/automation.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace layerc::test {

// The runtime accepts only an accepted automation profile, so a fixture the
// canonical validator rejects is a broken fixture: fail the calling case
// loudly instead of handing the evaluator an unchecked AST.
[[nodiscard]] inline layerc::core::ValidatedAutomationProfile
accept_automation(layerc::core::AutomationProfile profile,
                  const layerc::core::GameProfile &base) {
  auto validated =
      layerc::core::ValidatedAutomationProfile::validate(std::move(profile),
                                                         base);
  if (const auto *error =
          std::get_if<layerc::core::AutomationError>(&validated))
    throw std::runtime_error("automation fixture is not a valid profile: " +
                             error->describe());
  return std::get<layerc::core::ValidatedAutomationProfile>(
      std::move(validated));
}

} // namespace layerc::test

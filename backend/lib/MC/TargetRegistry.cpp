// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/TargetRegistry.h"

#include "rcc/Target/TargetMachine.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] std::vector<const Target *> &registeredTargets() {
  static std::vector<const Target *> Targets;
  return Targets;
}

} // namespace

TargetMachineOptions Target::defaultMachineOptions() const {
  return DefaultOptions();
}

Expected<std::unique_ptr<TargetMachine>>
Target::createTargetMachine(TargetMachineOptions Options) const {
  return TargetMachineCtor(*this, std::move(Options));
}

void TargetRegistry::registerTarget(const Target &RegisteredTarget) {
  std::vector<const Target *> &Targets = registeredTargets();
  if (std::ranges::find(Targets, &RegisteredTarget) == Targets.end())
    Targets.push_back(&RegisteredTarget);
}

Expected<const Target *> TargetRegistry::lookupTarget(std::string_view Triple) {
  const Target *Best = nullptr;
  unsigned BestQuality = 0;
  for (const Target *Candidate : registeredTargets()) {
    const unsigned Quality = Candidate->matchQuality(Triple);
    if (Quality > BestQuality) {
      Best = Candidate;
      BestQuality = Quality;
    } else if (Quality != 0 && Quality == BestQuality) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "target triple matches multiple registered targets");
    }
  }
  if (Best == nullptr) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "no registered target supports triple '" + std::string(Triple) + "'");
  }
  return Best;
}

Expected<const Target *> TargetRegistry::defaultTarget() {
  const Target *Default = nullptr;
  for (const Target *Candidate : registeredTargets()) {
    if (!Candidate->isDefault())
      continue;
    if (Default != nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "multiple targets are registered as the default target");
    }
    Default = Candidate;
  }
  if (Default == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "no default target is registered");
  return Default;
}

} // namespace rcc

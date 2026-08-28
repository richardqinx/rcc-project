// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_TARGETREGISTRY_H
#define RCC_MC_TARGETREGISTRY_H

#include "rcc/Support/Error.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace rcc {

class TargetMachine;
struct TargetMachineOptions;

class Target final {
public:
  using MatchQualityFn = unsigned (*)(std::string_view Triple);
  using NormalizeTripleFn = std::string (*)(std::string_view Triple);
  using DefaultOptionsFn = TargetMachineOptions (*)();
  using DefaultABIFn = std::string (*)(std::span<const std::string> Features);
  using TargetMachineCtorFn = Expected<std::unique_ptr<TargetMachine>> (*)(
      const Target &RegisteredTarget, TargetMachineOptions Options);

  constexpr Target(std::string_view Name, std::string_view Description, bool IsDefault,
                   MatchQualityFn MatchQuality, NormalizeTripleFn NormalizeTriple,
                   DefaultOptionsFn DefaultOptions, DefaultABIFn DefaultABI,
                   std::span<const std::string_view> BaselineFeatures,
                   TargetMachineCtorFn TargetMachineCtor) noexcept
      : Name(Name), Description(Description), IsDefault(IsDefault), MatchQuality(MatchQuality),
        NormalizeTriple(NormalizeTriple), DefaultOptions(DefaultOptions), DefaultABI(DefaultABI),
        BaselineFeatures(BaselineFeatures), TargetMachineCtor(TargetMachineCtor) {}

  [[nodiscard]] std::string_view name() const noexcept {
    return Name;
  }

  [[nodiscard]] std::string_view description() const noexcept {
    return Description;
  }

  [[nodiscard]] bool isDefault() const noexcept {
    return IsDefault;
  }

  [[nodiscard]] unsigned matchQuality(std::string_view Triple) const noexcept {
    return MatchQuality(Triple);
  }

  [[nodiscard]] std::string normalizeTriple(std::string_view Triple) const {
    return NormalizeTriple(Triple);
  }

  [[nodiscard]] TargetMachineOptions defaultMachineOptions() const;
  [[nodiscard]] std::string defaultABI(std::span<const std::string> Features) const {
    return DefaultABI(Features);
  }

  [[nodiscard]] std::span<const std::string_view> baselineFeatures() const noexcept {
    return BaselineFeatures;
  }

  [[nodiscard]] Expected<std::unique_ptr<TargetMachine>>
  createTargetMachine(TargetMachineOptions Options) const;

private:
  std::string_view Name;
  std::string_view Description;
  bool IsDefault = false;
  MatchQualityFn MatchQuality = nullptr;
  NormalizeTripleFn NormalizeTriple = nullptr;
  DefaultOptionsFn DefaultOptions = nullptr;
  DefaultABIFn DefaultABI = nullptr;
  std::span<const std::string_view> BaselineFeatures;
  TargetMachineCtorFn TargetMachineCtor = nullptr;
};

class TargetRegistry final {
public:
  static void registerTarget(const Target &RegisteredTarget);
  [[nodiscard]] static Expected<const Target *> lookupTarget(std::string_view Triple);
  [[nodiscard]] static Expected<const Target *> defaultTarget();
};

} // namespace rcc

#endif // RCC_MC_TARGETREGISTRY_H

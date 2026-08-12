// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVSubtarget.h"

#include "GISel/RISCVCallLowering.h"
#include "GISel/RISCVLegalizerInfo.h"
#include "GISel/RISCVRegisterBankInfo.h"
#include "RISCV.h"
#include "rcc/CodeGen/GlobalISel/InstructionSelector.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace rcc::riscv {
namespace {

struct ExtensionName final {
  std::string_view Name;
  RISCVExtension Extension;
};

constexpr std::array ExtensionNames{
    ExtensionName{"i", RISCVExtension::I},
    ExtensionName{"m", RISCVExtension::M},
    ExtensionName{"a", RISCVExtension::A},
    ExtensionName{"f", RISCVExtension::F},
    ExtensionName{"d", RISCVExtension::D},
    ExtensionName{"c", RISCVExtension::C},
    ExtensionName{"zicsr", RISCVExtension::Zicsr},
    ExtensionName{"zifencei", RISCVExtension::Zifencei},
    ExtensionName{"zacas", RISCVExtension::Zacas},
    ExtensionName{"zabha", RISCVExtension::Zabha},
};

[[nodiscard]] constexpr std::uint16_t extensionBit(RISCVExtension Extension) noexcept {
  return static_cast<std::uint16_t>(std::uint16_t{1} << static_cast<std::uint8_t>(Extension));
}

[[nodiscard]] const ExtensionName *findExtension(std::string_view Name) noexcept {
  for (const ExtensionName &Entry : ExtensionNames) {
    if (Entry.Name == Name)
      return &Entry;
  }
  return nullptr;
}

} // namespace

Expected<RISCVSubtargetFeatures>
RISCVSubtargetFeatures::fromFeatureList(std::span<const std::string> Features) {
  std::uint16_t Enabled = extensionBit(RISCVExtension::I);
  for (const std::string &Feature : Features) {
    if (Feature.size() < 2 || (Feature.front() != '+' && Feature.front() != '-')) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "RISC-V target feature must use canonical +/- spelling");
    }
    const ExtensionName *Extension = findExtension(std::string_view(Feature).substr(1));
    if (Extension == nullptr) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "RISC-V backend does not support target feature '" + Feature + "'");
    }
    if (Feature.front() == '+')
      Enabled |= extensionBit(Extension->Extension);
    else
      Enabled &= static_cast<std::uint16_t>(~extensionBit(Extension->Extension));
  }

  RISCVSubtargetFeatures Result(Enabled);
  if (!Result.has(RISCVExtension::I)) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "RV64 target feature configuration cannot disable the base I extension");
  }
  if (Result.has(RISCVExtension::D) && !Result.has(RISCVExtension::F)) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "RISC-V D extension requires the F extension");
  }
  if ((Result.has(RISCVExtension::Zacas) || Result.has(RISCVExtension::Zabha)) &&
      !Result.has(RISCVExtension::A)) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "RISC-V Zacas and Zabha extensions require the A extension");
  }
  return Result;
}

RISCVSubtargetFeatures RISCVSubtargetFeatures::rv64gc() noexcept {
  return RISCVSubtargetFeatures(extensionBit(RISCVExtension::I) | extensionBit(RISCVExtension::M) |
                                extensionBit(RISCVExtension::A) | extensionBit(RISCVExtension::F) |
                                extensionBit(RISCVExtension::D) | extensionBit(RISCVExtension::C) |
                                extensionBit(RISCVExtension::Zicsr) |
                                extensionBit(RISCVExtension::Zifencei));
}

bool RISCVSubtargetFeatures::has(RISCVExtension Extension) const noexcept {
  if (Extension >= RISCVExtension::Count)
    return false;
  return (Enabled & extensionBit(Extension)) != 0;
}

RISCVSubtarget::RISCVSubtarget(RISCVSubtargetFeatures Features) : Features(Features) {}

RISCVSubtarget::~RISCVSubtarget() = default;

const TargetCallLowering *RISCVSubtarget::getCallLowering() const {
  if (!CallLoweringInfo)
    CallLoweringInfo = std::make_unique<RISCVCallLowering>();
  return CallLoweringInfo.get();
}

InstructionSelector *RISCVSubtarget::getInstructionSelector() const {
  if (!InstSelector)
    InstSelector.reset(createRISCVInstructionSelector(*this, *getRegBankInfo()));
  return InstSelector.get();
}

const LegalizerInfo *RISCVSubtarget::getLegalizerInfo() const {
  if (!Legalizer)
    Legalizer = std::make_unique<RISCVLegalizerInfo>(*this);
  return Legalizer.get();
}

const RISCVRegisterBankInfo *RISCVSubtarget::getRegBankInfo() const {
  if (!RegBankInfo)
    RegBankInfo = std::make_unique<RISCVRegisterBankInfo>();
  return RegBankInfo.get();
}

} // namespace rcc::riscv

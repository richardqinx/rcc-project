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

struct OptionName final {
  std::string_view Name;
  RISCVSubtargetOption Option;
};

constexpr std::array ExtensionNames{
    ExtensionName{"i", RISCVExtension::I},
    ExtensionName{"m", RISCVExtension::M},
    ExtensionName{"a", RISCVExtension::A},
    ExtensionName{"f", RISCVExtension::F},
    ExtensionName{"d", RISCVExtension::D},
    ExtensionName{"c", RISCVExtension::C},
    ExtensionName{"zba", RISCVExtension::Zba},
    ExtensionName{"zbb", RISCVExtension::Zbb},
    ExtensionName{"zicsr", RISCVExtension::Zicsr},
    ExtensionName{"zifencei", RISCVExtension::Zifencei},
    ExtensionName{"zacas", RISCVExtension::Zacas},
    ExtensionName{"zabha", RISCVExtension::Zabha},
};

constexpr std::array OptionNames{
    OptionName{"disable-postmisched-load-clustering",
               RISCVSubtargetOption::DisablePostMISchedLoadClustering},
    OptionName{"disable-postmisched-store-clustering",
               RISCVSubtargetOption::DisablePostMISchedStoreClustering},
    OptionName{"fusion-add-load", RISCVSubtargetOption::FusionAddLoad},
    OptionName{"fusion-add-mem", RISCVSubtargetOption::FusionAddMemory},
    OptionName{"fusion-addi-load", RISCVSubtargetOption::FusionAddImmediateLoad},
    OptionName{"fusion-auipc-addi", RISCVSubtargetOption::FusionAUIPCAddImmediate},
    OptionName{"fusion-auipc-load", RISCVSubtargetOption::FusionAUIPCLoad},
    OptionName{"fusion-ld-add", RISCVSubtargetOption::FusionLoadDoubleAdd},
    OptionName{"fusion-logic-imm-reg", RISCVSubtargetOption::FusionLogicImmediateRegister},
    OptionName{"fusion-logic-reg-imm", RISCVSubtargetOption::FusionLogicRegisterImmediate},
    OptionName{"fusion-logic-reg-reg", RISCVSubtargetOption::FusionLogicRegisterRegister},
    OptionName{"fusion-lui-addi", RISCVSubtargetOption::FusionLUIAddImmediate},
    OptionName{"fusion-lui-load", RISCVSubtargetOption::FusionLUILoad},
    OptionName{"fusion-mul-add", RISCVSubtargetOption::FusionMultiplyAdd},
    OptionName{"fusion-shift-bit-extract", RISCVSubtargetOption::FusionShiftBitExtract},
    OptionName{"fusion-shifted-zextw", RISCVSubtargetOption::FusionShiftedZeroExtendWord},
    OptionName{"fusion-zexth", RISCVSubtargetOption::FusionZeroExtendHalf},
    OptionName{"fusion-zextw", RISCVSubtargetOption::FusionZeroExtendWord},
    OptionName{"prefer-w-inst", RISCVSubtargetOption::PreferWordInstructions},
    OptionName{"reserve-x6", RISCVSubtargetOption::ReserveX6},
    OptionName{"reserve-x7", RISCVSubtargetOption::ReserveX7},
    OptionName{"save-restore", RISCVSubtargetOption::SaveRestore},
    OptionName{"tagged-globals", RISCVSubtargetOption::TaggedGlobals},
    OptionName{"unaligned-scalar-mem", RISCVSubtargetOption::UnalignedScalarMemory},
    OptionName{"use-postra-scheduler", RISCVSubtargetOption::UsePostRAScheduler},
};

[[nodiscard]] constexpr std::uint16_t extensionBit(RISCVExtension Extension) noexcept {
  return static_cast<std::uint16_t>(std::uint16_t{1} << static_cast<std::uint8_t>(Extension));
}

[[nodiscard]] constexpr std::uint32_t optionBit(RISCVSubtargetOption Option) noexcept {
  static_assert(static_cast<std::uint8_t>(RISCVSubtargetOption::Count) <= 32);
  return std::uint32_t{1} << static_cast<std::uint8_t>(Option);
}

[[nodiscard]] const ExtensionName *findExtension(std::string_view Name) noexcept {
  for (const ExtensionName &Entry : ExtensionNames) {
    if (Entry.Name == Name)
      return &Entry;
  }
  return nullptr;
}

[[nodiscard]] const OptionName *findOption(std::string_view Name) noexcept {
  for (const OptionName &Entry : OptionNames) {
    if (Entry.Name == Name)
      return &Entry;
  }
  return nullptr;
}

} // namespace

Expected<RISCVSubtargetFeatures>
RISCVSubtargetFeatures::fromFeatureList(std::span<const std::string> Features) {
  std::uint16_t Enabled = extensionBit(RISCVExtension::I);
  std::uint16_t ExplicitlyDisabled = 0;
  std::uint32_t Options = 0;
  for (const std::string &Feature : Features) {
    if (Feature.size() < 2 || (Feature.front() != '+' && Feature.front() != '-')) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "RISC-V target feature must use canonical +/- spelling");
    }
    const std::string_view Name = std::string_view(Feature).substr(1);
    const ExtensionName *Extension = findExtension(Name);
    const OptionName *Option = findOption(Name);
    if (Extension == nullptr && Option == nullptr) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "RISC-V backend does not support target feature '" + Feature + "'");
    }
    if (Extension != nullptr) {
      if (Feature.front() == '+') {
        Enabled |= extensionBit(Extension->Extension);
        ExplicitlyDisabled &= static_cast<std::uint16_t>(~extensionBit(Extension->Extension));
      } else {
        Enabled &= static_cast<std::uint16_t>(~extensionBit(Extension->Extension));
        ExplicitlyDisabled |= extensionBit(Extension->Extension);
      }
    } else if (Feature.front() == '+') {
      Options |= optionBit(Option->Option);
    } else {
      Options &= ~optionBit(Option->Option);
    }
  }

  RISCVSubtargetFeatures Result(Enabled, Options);
  if (!Result.has(RISCVExtension::I)) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "RV64 target feature configuration cannot disable the base I extension");
  }
  if (Result.has(RISCVExtension::D) &&
      (ExplicitlyDisabled & extensionBit(RISCVExtension::F)) != 0) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "RISC-V D extension requires the F extension");
  }
  if (Result.has(RISCVExtension::D))
    Enabled |= extensionBit(RISCVExtension::F);
  if ((Result.has(RISCVExtension::Zacas) || Result.has(RISCVExtension::Zabha)) &&
      (ExplicitlyDisabled & extensionBit(RISCVExtension::A)) != 0) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "RISC-V Zacas and Zabha extensions require the A extension");
  }
  if (Result.has(RISCVExtension::Zacas) || Result.has(RISCVExtension::Zabha))
    Enabled |= extensionBit(RISCVExtension::A);
  return RISCVSubtargetFeatures(Enabled, Options);
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

bool RISCVSubtargetFeatures::has(RISCVSubtargetOption Option) const noexcept {
  if (Option >= RISCVSubtargetOption::Count)
    return false;
  return (Options & optionBit(Option)) != 0;
}

RISCVSubtarget::RISCVSubtarget(RISCVSubtargetFeatures Features, RelocationModel Relocation,
                               CodeModel Code)
    : Features(Features), Relocation(Relocation), Code(Code) {}

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

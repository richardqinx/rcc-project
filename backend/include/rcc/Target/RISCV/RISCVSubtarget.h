// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVSUBTARGET_H
#define RCC_TARGET_RISCV_RISCVSUBTARGET_H

#include "rcc/CodeGen/CommandFlags.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace rcc {

class InstructionSelector;
class LegalizerInfo;
class TargetCallLowering;
class TargetFrameLowering;
class TargetSchedulingHooks;
class TargetSchedulingModel;

namespace riscv {

class RISCVRegisterBankInfo;

enum class RISCVExtension : std::uint8_t {
  I,
  M,
  A,
  F,
  D,
  C,
  Zba,
  Zbb,
  Zicsr,
  Zifencei,
  Zacas,
  Zabha,
  Zcmp,
  Count,
};

enum class RISCVSubtargetOption : std::uint8_t {
  DisablePostMISchedLoadClustering,
  DisablePostMISchedStoreClustering,
  FusionAddLoad,
  FusionAddMemory,
  FusionAddImmediateLoad,
  FusionAUIPCAddImmediate,
  FusionAUIPCLoad,
  FusionBitfieldExtract,
  FusionLoadDoubleAdd,
  FusionLogicImmediateRegister,
  FusionLogicRegisterImmediate,
  FusionLogicRegisterRegister,
  FusionLUIAddImmediate,
  FusionLUILoad,
  FusionMultiplyAdd,
  FusionShiftBitExtract,
  FusionShiftedAddLoad,
  FusionShiftedZeroExtendWord,
  FusionZeroExtendHalf,
  FusionZeroExtendWord,
  PreferWordInstructions,
  ReserveX6,
  ReserveX7,
  SaveRestore,
  TaggedGlobals,
  UnalignedScalarMemory,
  UsePostRAScheduler,
  Count,
};

enum class RISCVABI : std::uint8_t {
  LP64,
  LP64D,
};

[[nodiscard]] Expected<RISCVABI> parseRISCVABI(std::string_view Name);

class RISCVSubtargetFeatures final {
public:
  [[nodiscard]] static Expected<RISCVSubtargetFeatures>
  fromFeatureList(std::span<const std::string> Features);
  [[nodiscard]] static RISCVSubtargetFeatures rv64gc() noexcept;

  [[nodiscard]] bool has(RISCVExtension Extension) const noexcept;
  [[nodiscard]] bool has(RISCVSubtargetOption Option) const noexcept;
  [[nodiscard]] bool hasFeature(std::string_view Feature) const noexcept;

  friend bool operator==(const RISCVSubtargetFeatures &Left,
                         const RISCVSubtargetFeatures &Right) = default;

private:
  explicit constexpr RISCVSubtargetFeatures(std::uint16_t Enabled,
                                            std::uint32_t Options = 0) noexcept
      : Enabled(Enabled), Options(Options) {}

  std::uint16_t Enabled = 0;
  std::uint32_t Options = 0;
};

class RISCVSubtarget final {
public:
  explicit RISCVSubtarget(RISCVSubtargetFeatures Features,
                          RelocationModel Relocation = RelocationModel::Static,
                          CodeModel Code = CodeModel::Small, RISCVABI ABI = RISCVABI::LP64D);
  RISCVSubtarget(const RISCVSubtarget &) = delete;
  RISCVSubtarget &operator=(const RISCVSubtarget &) = delete;
  RISCVSubtarget(RISCVSubtarget &&) = delete;
  RISCVSubtarget &operator=(RISCVSubtarget &&) = delete;
  ~RISCVSubtarget();

  [[nodiscard]] const RISCVSubtargetFeatures &features() const noexcept {
    return Features;
  }

  [[nodiscard]] RelocationModel relocationModel() const noexcept {
    return Relocation;
  }

  [[nodiscard]] CodeModel codeModel() const noexcept {
    return Code;
  }

  [[nodiscard]] RISCVABI targetABI() const noexcept {
    return ABI;
  }

  [[nodiscard]] const TargetCallLowering *getCallLowering() const;
  [[nodiscard]] InstructionSelector *getInstructionSelector() const;
  [[nodiscard]] const LegalizerInfo *getLegalizerInfo() const;
  [[nodiscard]] const RISCVRegisterBankInfo *getRegBankInfo() const;
  [[nodiscard]] const TargetFrameLowering &frameLowering() const noexcept;
  [[nodiscard]] const TargetSchedulingModel &schedulingModel() const noexcept;

private:
  RISCVSubtargetFeatures Features;
  RelocationModel Relocation;
  CodeModel Code;
  RISCVABI ABI;
  std::unique_ptr<TargetFrameLowering> FrameLoweringInfo;
  mutable std::unique_ptr<TargetCallLowering> CallLoweringInfo;
  mutable std::unique_ptr<InstructionSelector> InstSelector;
  mutable std::unique_ptr<LegalizerInfo> Legalizer;
  mutable std::unique_ptr<RISCVRegisterBankInfo> RegBankInfo;
  std::unique_ptr<TargetSchedulingHooks> SchedulingHooks;
  std::unique_ptr<TargetSchedulingModel> SchedulingModel;
};

} // namespace riscv
} // namespace rcc

#endif // RCC_TARGET_RISCV_RISCVSUBTARGET_H

// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/RegBankSelect.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &bankSelectContract() {
  static const MachinePassContract Contract{
      .Name = "regbankselect",
      .Requires = {MachineFunctionProperty::Legalized},
      .Sets = {MachineFunctionProperty::RegBankSelected},
      .Clears = {},
      .RequiredAnalyses = {},
      .VerificationStage = PipelineStage::RegBankSelected,
  };
  return Contract;
}

void combineFingerprint(std::uint64_t &State, std::uint64_t Value) noexcept {
  State ^= Value + 0x9e3779b97f4a7c15ULL + (State << 6U) + (State >> 2U);
}

[[nodiscard]] std::uint64_t
registerConfigurationFingerprint(const MachineRegisterInfo &RegInfo) noexcept {
  std::uint64_t Result = 0xcbf29ce484222325ULL;
  for (const VirtualRegisterInfo &Info : RegInfo.virtualRegisters()) {
    combineFingerprint(Result, Info.IsTombstone ? 1U : 0U);
    combineFingerprint(Result, Info.Bank ? Info.Bank->value() : 0U);
    combineFingerprint(Result, Info.RegClass ? Info.RegClass->value() : 0U);
  }
  return Result;
}

[[nodiscard]] std::vector<MachineInstr *> layoutInstructions(MachineFunction &Function) {
  std::vector<MachineInstr *> Result;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next())
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next())
      Result.push_back(Instruction);
  return Result;
}

[[nodiscard]] Expected<void> requireProperties(const MachineFunction &Function,
                                               const MachinePassContract &Contract) {
  const MachineFunctionProperties Missing = Contract.Requires.missingFrom(Function.properties());
  if (!Missing.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "pass '" + Contract.Name +
                              "' requires properties: " + Missing.toString());
  }
  return {};
}

[[nodiscard]] Expected<void> restoreRegisterConfiguration(
    MachineRegisterInfo &RegInfo,
    const std::vector<std::pair<Register, std::pair<std::optional<RegisterBankID>,
                                                    std::optional<RegisterClassID>>>> &Snapshot) {
  for (const auto &[Reg, Configuration] : Snapshot) {
    auto ClearBank = RegInfo.clearRegisterBank(Reg);
    if (!ClearBank)
      return std::unexpected(ClearBank.error());
    if (Configuration.first) {
      auto SetBank = RegInfo.setRegisterBank(Reg, *Configuration.first);
      if (!SetBank)
        return std::unexpected(SetBank.error());
    }
    auto ClearClass = RegInfo.clearRegisterClass(Reg);
    if (!ClearClass)
      return std::unexpected(ClearClass.error());
    if (Configuration.second) {
      auto SetClass = RegInfo.setRegisterClass(Reg, *Configuration.second);
      if (!SetClass)
        return std::unexpected(SetClass.error());
    }
  }
  return {};
}

[[nodiscard]] Error passFailure(std::string Message,
                                ErrorCode Code = ErrorCode::VerificationError) {
  return Error(Code, std::move(Message));
}

[[nodiscard]] Error prefixedError(const Error &Source, std::string Prefix) {
  Prefix.append(Source.message());
  return Error(Source.code(), std::move(Prefix));
}

[[nodiscard]] Expected<void> repairTrackedLiveness(MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   MachinePassResult &Result) {
  if (!Result.Changed || !Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    return {};
  }
  auto Rebuilt = rebuildMachineLiveness(Function, Target);
  if (!Rebuilt)
    return std::unexpected(Rebuilt.error());
  if (*Rebuilt)
    Result.Changes.add(MachinePassChange::Operands);
  return {};
}

} // namespace

const MachinePassContract &RegBankSelect::contract() const noexcept {
  return bankSelectContract();
}

Expected<MachinePassResult> RegBankSelect::run(MachineFunction &Function,
                                               const TargetDescription &Target,
                                               MachineAnalysisManager &) {
  if (Info == nullptr)
    return std::unexpected(passFailure("register-bank select has no target behavior interface",
                                       ErrorCode::InvalidArgument));
  auto Required = requireProperties(Function, contract());
  if (!Required)
    return std::unexpected(Required.error());

  MachinePassResult Result;
  MachineRegisterInfo &RegInfo = Function.registerInfo();
  const std::uint64_t RegisterConfigurationBefore = registerConfigurationFingerprint(RegInfo);
  using RegisterConfiguration =
      std::pair<std::optional<RegisterBankID>, std::optional<RegisterClassID>>;
  std::vector<std::pair<Register, RegisterConfiguration>> Snapshot;
  std::vector<std::pair<Register, RegisterBankID>> PlannedBanks;
  for (std::size_t Index = 0; Index < RegInfo.virtualRegisters().size(); ++Index) {
    auto Reg = Register::makeVirtual(Index);
    if (!Reg)
      return std::unexpected(Reg.error());
    const VirtualRegisterInfo &StoredInfo = RegInfo.virtualRegisters()[Index];
    if (StoredInfo.IsTombstone)
      continue;
    Snapshot.emplace_back(*Reg, RegisterConfiguration{StoredInfo.Bank, StoredInfo.RegClass});
    auto InfoResult = RegInfo.virtualRegisterInfo(*Reg);
    if (!InfoResult)
      return std::unexpected(InfoResult.error());
    const VirtualRegisterInfo &InfoValue = InfoResult->get();
    const MachineInstr *Context = nullptr;
    for (const MachineOperand &Operand : RegInfo.regOperands(*Reg)) {
      Context = Operand.parent();
      if (Context != nullptr)
        break;
    }
    auto Bank = Info->selectBank(Function, *Reg, InfoValue, Context, Target);
    if (!Bank)
      return std::unexpected(Bank.error());
    if (Target.registers().registerBank(*Bank) == nullptr)
      return std::unexpected(passFailure("target register-bank selector returned an unknown bank"));
    if (InfoValue.Bank && *InfoValue.Bank != *Bank)
      return std::unexpected(
          passFailure("target register-bank selector conflicts with an existing bank"));
    if (InfoValue.RegClass && !Target.registers().bankCoversClass(*Bank, *InfoValue.RegClass))
      return std::unexpected(
          passFailure("existing register class is not covered by selected bank"));
    PlannedBanks.emplace_back(*Reg, *Bank);
  }

  for (MachineInstr *Instruction : layoutInstructions(Function)) {
    auto Validation = Info->validateInstruction(*Instruction, Function, Target);
    if (!Validation)
      return std::unexpected(
          prefixedError(Validation.error(), "register-bank mapping precondition failed: "));
  }

  for (const auto &[Reg, Bank] : PlannedBanks) {
    const auto InfoResult = RegInfo.virtualRegisterInfo(Reg);
    if (!InfoResult)
      return std::unexpected(InfoResult.error());
    if (!InfoResult->get().Bank) {
      auto SetBank = RegInfo.setRegisterBank(Reg, Bank);
      if (!SetBank)
        return std::unexpected(SetBank.error());
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Registers);
    }
  }

  const auto Restore = [&]() -> Expected<void> {
    return restoreRegisterConfiguration(RegInfo, Snapshot);
  };
  for (MachineInstr *Instruction : layoutInstructions(Function)) {
    auto Constraint = Info->constrainInstruction(*Instruction, Function, Target);
    if (!Constraint) {
      auto Restored = Restore();
      if (!Restored)
        return std::unexpected(prefixedError(Restored.error(), "register-bank rollback failed: "));
      return std::unexpected(Constraint.error());
    }
    auto Validation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!Validation) {
      auto Restored = Restore();
      if (!Restored)
        return std::unexpected(prefixedError(Restored.error(), "register-bank rollback failed: "));
      return std::unexpected(prefixedError(Validation.error(),
                                           "register-bank selection produced invalid MachineIR: "));
    }
  }
  auto RegisterValidation = Target.validateRegisterInfo(RegInfo);
  if (!RegisterValidation) {
    auto Restored = Restore();
    if (!Restored)
      return std::unexpected(prefixedError(Restored.error(), "register-bank rollback failed: "));
    return std::unexpected(prefixedError(RegisterValidation.error(),
                                         "register-bank selection produced invalid registers: "));
  }

  if (registerConfigurationFingerprint(RegInfo) != RegisterConfigurationBefore) {
    Result.Changed = true;
    Result.Changes.add(MachinePassChange::Registers);
  }

  auto Liveness = repairTrackedLiveness(Function, Target, Result);
  if (!Liveness) {
    auto Restored = Restore();
    if (!Restored)
      return std::unexpected(prefixedError(Restored.error(), "register-bank rollback failed: "));
    return std::unexpected(
        prefixedError(Liveness.error(), "register-bank liveness repair failed: "));
  }
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

} // namespace rcc

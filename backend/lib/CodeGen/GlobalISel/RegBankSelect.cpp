// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/RegBankSelect.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
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

struct PlannedRegisterBank final {
  Register Reg;
  RegisterBankID Bank;
  bool IsConstrained = false;
};

struct MappingMutation final {
  MachineOperand *Operand = nullptr;
  Register Original;
  Register Fresh;
  MachineInstr *Copy = nullptr;
};

[[nodiscard]] Expected<MachineInstr *> buildBankCopy(MachineFunction &Function,
                                                     const TargetDescription &Target,
                                                     MachineInstr &Instruction, bool InsertBefore,
                                                     Register Destination, Register Source) {
  MachineIRBuilder Builder(Function, Target);
  auto Point = InsertBefore ? Builder.setInsertPointBefore(Instruction)
                            : Builder.setInsertPointAfter(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::COPY));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto DestinationOperand = Pending->addDef(Destination);
  if (!DestinationOperand)
    return std::unexpected(DestinationOperand.error());
  auto SourceOperand = Pending->addUse(Source);
  if (!SourceOperand)
    return std::unexpected(SourceOperand.error());
  if (Instruction.debugLocation()) {
    auto Debug = Pending->setDebugLocation(*Instruction.debugLocation());
    if (!Debug)
      return std::unexpected(Debug.error());
  }
  return Pending->build();
}

[[nodiscard]] Expected<void> repairMappedOperand(MachineFunction &Function,
                                                 const TargetDescription &Target,
                                                 MachineInstr &Instruction, MachineOperand &Operand,
                                                 RegisterBankID RequiredBank,
                                                 std::vector<MappingMutation> &Mutations) {
  const Register *Original = Operand.getIf<Register>();
  if (Original == nullptr || !Original->isVirtual() || Operand.isDef() == Operand.isUse()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "register-bank repair requires a virtual def or use operand");
  }
  const Register OriginalReg = *Original;
  auto OriginalInfo = Function.registerInfo().virtualRegisterInfo(OriginalReg);
  if (!OriginalInfo)
    return std::unexpected(OriginalInfo.error());
  if (!OriginalInfo->get().Type) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "register-bank repair requires a virtual-register type");
  }
  if (Operand.subReg().value() != 0) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "register-bank repair does not support subregister operands");
  }

  auto Fresh = Function.registerInfo().createVirtualRegister(*OriginalInfo->get().Type);
  if (!Fresh)
    return std::unexpected(Fresh.error());
  auto SetBank = Function.registerInfo().setRegisterBank(*Fresh, RequiredBank);
  if (!SetBank) {
    auto Erased = Function.registerInfo().eraseVirtualRegister(*Fresh);
    (void)Erased;
    return std::unexpected(SetBank.error());
  }

  auto Copy = Operand.isDef()
                  ? buildBankCopy(Function, Target, Instruction, false, OriginalReg, *Fresh)
                  : buildBankCopy(Function, Target, Instruction, true, *Fresh, OriginalReg);
  if (!Copy) {
    auto Erased = Function.registerInfo().eraseVirtualRegister(*Fresh);
    (void)Erased;
    return std::unexpected(Copy.error());
  }
  auto Replaced = Operand.setRegister(*Fresh);
  if (!Replaced) {
    if ((*Copy)->parent() != nullptr) {
      auto Removed = (*Copy)->parent()->removeInstruction(**Copy);
      (void)Removed;
    }
    auto Erased = Function.registerInfo().eraseVirtualRegister(*Fresh);
    (void)Erased;
    return std::unexpected(Replaced.error());
  }
  Mutations.push_back(MappingMutation{
      .Operand = &Operand, .Original = OriginalReg, .Fresh = *Fresh, .Copy = *Copy});
  return {};
}

[[nodiscard]] Expected<void> rollbackMappingMutations(MachineRegisterInfo &RegInfo,
                                                      std::vector<MappingMutation> &Mutations) {
  for (auto Iterator = Mutations.rbegin(); Iterator != Mutations.rend(); ++Iterator) {
    auto Restored = Iterator->Operand->setRegister(Iterator->Original);
    if (!Restored)
      return std::unexpected(Restored.error());
    if (Iterator->Copy != nullptr && Iterator->Copy->parent() != nullptr) {
      auto Removed = Iterator->Copy->parent()->removeInstruction(*Iterator->Copy);
      if (!Removed)
        return std::unexpected(Removed.error());
    }
    auto Erased = RegInfo.eraseVirtualRegister(Iterator->Fresh);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  Mutations.clear();
  return {};
}

[[nodiscard]] Expected<void>
applyInstructionMappings(MachineFunction &Function, const TargetDescription &Target,
                         const RegisterBankInfo &Info,
                         std::unordered_set<Register> &ConstrainedRegisters,
                         std::vector<MappingMutation> &Mutations, MachinePassResult &Result) {
  MachineRegisterInfo &RegInfo = Function.registerInfo();
  const std::vector<MachineInstr *> Instructions = layoutInstructions(Function);
  for (MachineInstr *Instruction : Instructions) {
    auto Mapping = Info.getInstructionMapping(*Instruction, Function, Target);
    if (!Mapping)
      return std::unexpected(Mapping.error());
    if (Mapping->empty())
      continue;
    if (Mapping->size() != Instruction->operandCount()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "target register-bank mapping has the wrong operand count");
    }
    for (std::size_t Index = 0; Index < Mapping->size(); ++Index) {
      if (!(*Mapping)[Index])
        continue;
      const RegisterBankID RequiredBank = *Mapping->at(Index);
      if (Target.registers().registerBank(RequiredBank) == nullptr) {
        return makeUnexpected(ErrorCode::InternalError,
                              "target instruction mapping names an unknown register bank");
      }
      MachineOperand *Operand = Instruction->operandAt(Index);
      const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
      if (Reg == nullptr || !Reg->isVirtual()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "target register-bank mapping requires a virtual-register operand");
      }
      auto RegisterInfo = RegInfo.virtualRegisterInfo(*Reg);
      if (!RegisterInfo)
        return std::unexpected(RegisterInfo.error());
      if (!RegisterInfo->get().Bank) {
        auto SetBank = RegInfo.setRegisterBank(*Reg, RequiredBank);
        if (!SetBank)
          return std::unexpected(SetBank.error());
        ConstrainedRegisters.insert(*Reg);
        Result.Changed = true;
        Result.Changes.add(MachinePassChange::Registers);
        continue;
      }
      if (*RegisterInfo->get().Bank == RequiredBank) {
        ConstrainedRegisters.insert(*Reg);
        continue;
      }
      if (!ConstrainedRegisters.contains(*Reg) && !RegisterInfo->get().RegClass) {
        auto SetBank = RegInfo.setRegisterBank(*Reg, RequiredBank);
        if (!SetBank)
          return std::unexpected(SetBank.error());
        ConstrainedRegisters.insert(*Reg);
        Result.Changed = true;
        Result.Changes.add(MachinePassChange::Registers);
        continue;
      }
      auto Repaired =
          repairMappedOperand(Function, Target, *Instruction, *Operand, RequiredBank, Mutations);
      if (!Repaired)
        return std::unexpected(Repaired.error());
      ConstrainedRegisters.insert(Mutations.back().Fresh);
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Instructions);
      Result.Changes.add(MachinePassChange::Operands);
      Result.Changes.add(MachinePassChange::Registers);
    }
  }
  return {};
}

} // namespace

const MachinePassContract &RegBankSelect::contract() const noexcept {
  return bankSelectContract();
}

Expected<MachinePassResult> RegBankSelect::run(MachineFunction &Function,
                                               const TargetDescription &Target,
                                               MachineFunctionAnalysisManager &) {
  if (Function.properties().has(MachineFunctionProperty::FailedISel))
    return MachinePassResult{
        .Changed = false,
        .Changes = {},
        .Preserved = PreservedAnalyses::all(),
    };
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
  std::vector<PlannedRegisterBank> PlannedBanks;
  std::unordered_set<Register> ConstrainedRegisters;
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
    if (Target.registers().registerBank(Bank->Bank) == nullptr)
      return std::unexpected(passFailure("target register-bank selector returned an unknown bank"));
    if (InfoValue.Bank && *InfoValue.Bank != Bank->Bank)
      return std::unexpected(
          passFailure("target register-bank selector conflicts with an existing bank"));
    if (InfoValue.RegClass && !Target.registers().bankCoversClass(Bank->Bank, *InfoValue.RegClass))
      return std::unexpected(
          passFailure("existing register class is not covered by selected bank"));
    PlannedBanks.push_back(PlannedRegisterBank{.Reg = *Reg,
                                               .Bank = Bank->Bank,
                                               .IsConstrained = Bank->IsConstrained ||
                                                                InfoValue.RegClass.has_value()});
  }

  for (MachineInstr *Instruction : layoutInstructions(Function)) {
    auto Validation = Info->validateInstruction(*Instruction, Function, Target);
    if (!Validation)
      return std::unexpected(
          withErrorContext(Validation.error(), "register-bank mapping precondition failed: "));
  }

  for (const PlannedRegisterBank &Plan : PlannedBanks) {
    const auto InfoResult = RegInfo.virtualRegisterInfo(Plan.Reg);
    if (!InfoResult)
      return std::unexpected(InfoResult.error());
    if (!InfoResult->get().Bank) {
      auto SetBank = RegInfo.setRegisterBank(Plan.Reg, Plan.Bank);
      if (!SetBank)
        return std::unexpected(SetBank.error());
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Registers);
    }
    if (Plan.IsConstrained)
      ConstrainedRegisters.insert(Plan.Reg);
  }

  std::vector<MappingMutation> MappingMutations;
  const auto Restore = [&]() -> Expected<void> {
    auto MappingRestore = rollbackMappingMutations(RegInfo, MappingMutations);
    if (!MappingRestore)
      return std::unexpected(MappingRestore.error());
    return restoreRegisterConfiguration(RegInfo, Snapshot);
  };
  auto Mapped = applyInstructionMappings(Function, Target, *Info, ConstrainedRegisters,
                                         MappingMutations, Result);
  if (!Mapped) {
    auto Restored = Restore();
    if (!Restored)
      return std::unexpected(withErrorContext(Restored.error(), "register-bank rollback failed: "));
    return std::unexpected(withErrorContext(Mapped.error(), "register-bank mapping failed: "));
  }
  for (MachineInstr *Instruction : layoutInstructions(Function)) {
    auto Validation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!Validation) {
      auto Restored = Restore();
      if (!Restored)
        return std::unexpected(
            withErrorContext(Restored.error(), "register-bank rollback failed: "));
      return std::unexpected(withErrorContext(
          Validation.error(), "register-bank selection produced invalid MachineIR: "));
    }
  }
  auto RegisterValidation = Target.validateRegisterInfo(RegInfo);
  if (!RegisterValidation) {
    auto Restored = Restore();
    if (!Restored)
      return std::unexpected(withErrorContext(Restored.error(), "register-bank rollback failed: "));
    return std::unexpected(withErrorContext(
        RegisterValidation.error(), "register-bank selection produced invalid registers: "));
  }

  if (registerConfigurationFingerprint(RegInfo) != RegisterConfigurationBefore) {
    Result.Changed = true;
    Result.Changes.add(MachinePassChange::Registers);
  }

  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

} // namespace rcc

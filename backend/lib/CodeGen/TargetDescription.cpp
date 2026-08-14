// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetDescription.h"

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const OperandDescriptor &descriptorForOperand(const InstructionDescriptor &Descriptor,
                                                            std::size_t Index) {
  if (Index < Descriptor.Operands.size())
    return Descriptor.Operands[Index];
  return Descriptor.Operands.back();
}

[[nodiscard]] std::uint64_t minimumTypeSizeInBits(LowLevelType Type) noexcept {
  if (Type.isToken())
    return 0;
  const std::uint64_t ScalarSize = Type.scalarBitWidth();
  if (!Type.isVector())
    return ScalarSize;
  return ScalarSize * Type.minimumElementCount();
}

[[nodiscard]] bool isPureDef(const MachineOperand &Operand) noexcept {
  return Operand.isRegister() && Operand.isDef() && !Operand.isUse();
}

[[nodiscard]] bool isPureUse(const MachineOperand &Operand) noexcept {
  return Operand.isRegister() && Operand.isUse() && !Operand.isDef();
}

} // namespace

Expected<TargetDescription> TargetDescription::create(InstructionSet Instructions,
                                                      TargetRegisterInfo Registers,
                                                      TargetMIRTextInfoDesc MIRTextDescription,
                                                      TargetBooleanContentInfo BooleanInfo) {
  auto MIRText = TargetMIRTextInfo::create(std::move(MIRTextDescription), Registers);
  if (!MIRText)
    return std::unexpected(MIRText.error());

  for (const InstructionDescriptor &Instruction : Instructions.descriptors()) {
    for (const OperandDescriptor &Operand : Instruction.Operands) {
      if (Operand.AllowAnyTargetFlags && Operand.AllowedTargetFlags != 0) {
        return makeUnexpected(
            ErrorCode::InvalidArgument,
            "instruction operand cannot combine unrestricted and explicit target flags");
      }
      if (!Operand.AllowAnyTargetFlags &&
          (Operand.AllowedTargetFlags & ~MIRText->serializableOperandFlagMask()) != 0) {
        return makeUnexpected(
            ErrorCode::InvalidArgument,
            "instruction operand allows a target flag outside the MIR text schema");
      }
      if (Operand.RegClass && Registers.registerClass(*Operand.RegClass) == nullptr) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "instruction operand references an unknown target register class");
      }
      if (Operand.RegBank && Registers.registerBank(*Operand.RegBank) == nullptr) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "instruction operand references an unknown target register bank");
      }
      if (Operand.RegClass && Operand.RegBank &&
          !Registers.bankCoversClass(*Operand.RegBank, *Operand.RegClass)) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "instruction operand register class is not covered by its bank");
      }
    }

    auto ValidateImplicit =
        [&Registers](const ImplicitRegisterDescriptor &Implicit) -> Expected<void> {
      const PhysicalRegisterDesc *Register = Registers.physicalRegister(Implicit.Reg);
      if (Register == nullptr) {
        return makeUnexpected(
            ErrorCode::InvalidArgument,
            "instruction implicit operand references an unknown physical register");
      }
      if (Implicit.Renamable && (Register->IsConstant || Register->IsArtificial)) {
        return makeUnexpected(
            ErrorCode::InvalidArgument,
            "renamable implicit operand references a constant or artificial register");
      }
      return {};
    };
    for (const ImplicitRegisterDescriptor &Implicit : Instruction.ImplicitDefs) {
      auto Validation = ValidateImplicit(Implicit);
      if (!Validation)
        return std::unexpected(Validation.error());
    }
    for (const ImplicitRegisterDescriptor &Implicit : Instruction.ImplicitUses) {
      auto Validation = ValidateImplicit(Implicit);
      if (!Validation)
        return std::unexpected(Validation.error());
    }
    for (const ImplicitRegisterDescriptor &Def : Instruction.ImplicitDefs) {
      if (!Def.EarlyClobber)
        continue;
      if (std::ranges::any_of(Instruction.ImplicitUses,
                              [&Registers, Def](const ImplicitRegisterDescriptor &Use) {
                                return Registers.regsOverlap(Def.Reg, Use.Reg);
                              })) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "early-clobber implicit definition overlaps an implicit use");
      }
    }
  }

  return TargetDescription(std::move(Instructions), std::move(Registers), std::move(*MIRText),
                           BooleanInfo);
}

Expected<void> TargetDescription::validateVirtualRegister(Register Reg,
                                                          const VirtualRegisterInfo &Info) const {
  if (!Reg.isVirtual()) {
    return makeUnexpected(ErrorCode::InternalError,
                          "target virtual-register validation received a non-virtual register");
  }

  auto TargetFlagValidation = MIRText.validateVirtualRegisterFlags(Info.TargetFlags);
  if (!TargetFlagValidation)
    return std::unexpected(TargetFlagValidation.error());

  const RegisterBankDesc *Bank = nullptr;
  if (Info.Bank) {
    Bank = Registers.registerBank(*Info.Bank);
    if (Bank == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "virtual register references an unknown target register bank");
    }
  }

  const RegisterClassDesc *Class = nullptr;
  if (Info.RegClass) {
    Class = Registers.registerClass(*Info.RegClass);
    if (Class == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "virtual register references an unknown target register class");
    }
  }

  if (Info.Bank && Info.RegClass && !Registers.bankCoversClass(*Info.Bank, *Info.RegClass)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "virtual register class is not covered by its selected register bank");
  }

  if (Info.Type) {
    const std::uint64_t MinimumSize = minimumTypeSizeInBits(*Info.Type);
    if (MinimumSize == 0 && (Bank != nullptr || Class != nullptr)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "token virtual register cannot have a bank or register class");
    }
    if (Bank != nullptr && MinimumSize > Bank->MaximumSizeInBits) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "virtual register type exceeds its register bank maximum size");
    }
    if (Class != nullptr && MinimumSize > Class->SizeInBits) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "virtual register type exceeds its register class size");
    }
  }

  for (const PhysicalRegister Hint : Info.AllocationHints) {
    const PhysicalRegisterDesc *HintDesc = Registers.physicalRegister(Hint);
    if (HintDesc == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "virtual register allocation hint names an unknown physical register");
    }
    if (HintDesc->IsConstant || HintDesc->IsArtificial || HintDesc->IsReserved) {
      return makeUnexpected(
          ErrorCode::VerificationError,
          "virtual register allocation hint names a constant, artificial, or reserved register");
    }
    if (Info.RegClass && !Registers.registerClassContains(*Info.RegClass, Hint)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "virtual register allocation hint is outside its register class");
    }
    if (!Info.RegClass && Info.Bank && !Registers.bankContainsRegister(*Info.Bank, Hint)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "virtual register allocation hint is outside its register bank");
    }
  }
  return {};
}

std::optional<RegisterClassID>
TargetDescription::effectiveRegisterClass(const MachineOperand &Operand,
                                          const MachineRegisterInfo &RegInfo) const {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || !Reg->isVirtual())
    return std::nullopt;

  auto Info = RegInfo.virtualRegisterInfo(*Reg);
  if (!Info)
    return std::nullopt;
  const std::optional<RegisterClassID> RegClass = Info->get().RegClass;
  if (!RegClass)
    return std::nullopt;
  if (Operand.subReg().value() == 0)
    return RegClass;
  return Registers.subRegClass(*RegClass, Operand.subReg());
}

Expected<void> TargetDescription::validateTargetOperand(const OperandDescriptor *Descriptor,
                                                        const MachineOperand &Operand,
                                                        const MachineRegisterInfo &RegInfo) const {
  auto TargetFlagValidation = MIRText.validateOperandFlags(Operand.targetFlags());
  if (!TargetFlagValidation)
    return std::unexpected(TargetFlagValidation.error());

  if (const auto *Index = Operand.getIf<TargetIndexOperand>();
      Index != nullptr && !MIRText.targetIndexName(Index->Index)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target-index operand is not registered for MIR serialization");
  }
  if (const auto *Intrinsic = Operand.getIf<IntrinsicOperand>();
      Intrinsic != nullptr && !MIRText.intrinsicName(Intrinsic->ID)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "intrinsic operand is not registered for MIR serialization");
  }

  if (const auto *Mask = Operand.getIf<RegisterMaskOperand>(); Mask != nullptr) {
    if (!Mask->Mask) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "register-mask operand contains a null mask");
    }
    return Registers.validateRegisterMask(*Mask->Mask);
  }
  if (const auto *Mask = Operand.getIf<LiveOutRegisterMaskOperand>(); Mask != nullptr) {
    if (!Mask->Mask) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "live-out register-mask operand contains a null mask");
    }
    return Registers.validateRegisterMask(*Mask->Mask);
  }

  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr)
    return {};
  if (Reg->isNoRegister()) {
    if (Operand.subReg().value() != 0) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "noreg operand cannot carry a subregister index");
    }
    if (Operand.registerFlags().has(RegisterOperandFlag::Renamable)) {
      return makeUnexpected(ErrorCode::VerificationError, "noreg operand cannot be renamable");
    }
    return {};
  }

  if (const auto PhysReg = Reg->physicalRegister()) {
    const PhysicalRegister PhysRegValue = *PhysReg;
    const PhysicalRegisterDesc *PhysRegDesc = Registers.physicalRegister(PhysRegValue);
    if (PhysRegDesc == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "operand references an unknown physical register");
    }
    if (Operand.subReg().value() != 0) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical register operand cannot carry a subregister index");
    }
    if (Descriptor != nullptr && Descriptor->RegClass &&
        !Registers.registerClassContains(*Descriptor->RegClass, PhysRegValue)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical register operand is outside its required register class");
    }
    if (Descriptor != nullptr && Descriptor->RegBank &&
        !Registers.bankContainsRegister(*Descriptor->RegBank, PhysRegValue)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical register operand is outside its required register bank");
    }
    const bool IsReservedOrAlias = std::ranges::any_of(
        RegInfo.reservedRegisters(), [this, PhysRegValue](PhysicalRegister Reserved) {
          return Registers.regsOverlap(Reserved, PhysRegValue);
        });
    if (Operand.registerFlags().has(RegisterOperandFlag::Renamable) &&
        (IsReservedOrAlias || PhysRegDesc->IsConstant || PhysRegDesc->IsArtificial ||
         PhysRegDesc->IsReserved)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "fixed physical register operand cannot be renamable");
    }
    return {};
  }

  if (Operand.registerFlags().has(RegisterOperandFlag::Renamable)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "virtual register operand cannot be renamable");
  }

  auto Info = RegInfo.virtualRegisterInfo(*Reg);
  if (!Info)
    return std::unexpected(Info.error());
  auto RegisterValidation = validateVirtualRegister(*Reg, Info->get());
  if (!RegisterValidation)
    return std::unexpected(RegisterValidation.error());

  if (Operand.subReg().value() != 0) {
    if (Registers.subRegIndex(Operand.subReg()) == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "operand references an unknown subregister index");
    }
    const std::optional<RegisterClassID> RegClass = Info->get().RegClass;
    if (!RegClass) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic virtual register cannot carry a subregister index");
    }
    if (!Registers.subRegClass(*RegClass, Operand.subReg())) {
      return makeUnexpected(
          ErrorCode::VerificationError,
          "virtual register class does not support the operand subregister index");
    }
  }

  if (Descriptor != nullptr && Descriptor->RegBank && Info->get().Bank != Descriptor->RegBank) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "virtual register bank does not match the operand descriptor");
  }
  if (Descriptor != nullptr && Descriptor->RegClass) {
    const auto EffectiveClass = effectiveRegisterClass(Operand, RegInfo);
    if (!EffectiveClass || !Registers.isSubclass(*EffectiveClass, *Descriptor->RegClass)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "virtual register class does not satisfy the operand descriptor");
    }
  }
  return {};
}

Expected<void> TargetDescription::validateMemOperands(
    Opcode InstructionOpcode, std::span<const MachineMemOperand *const> MemOperands) const {
  auto DescriptorValidation = Instructions.validateMemOperands(InstructionOpcode, MemOperands);
  if (!DescriptorValidation)
    return std::unexpected(DescriptorValidation.error());
  for (const MachineMemOperand *MemOperand : MemOperands) {
    if (MemOperand == nullptr)
      continue;
    auto TargetFlagValidation =
        MIRText.validateMachineMemOperandFlags(MemOperand->description().TargetFlags);
    if (!TargetFlagValidation)
      return std::unexpected(TargetFlagValidation.error());
  }
  return {};
}

Expected<void> TargetDescription::validateCFIInfo(const MachineCFIInfo &Info) const {
  auto ValidateRegister = [this](DwarfRegisterNumber Register) -> Expected<void> {
    if (!MIRText.physicalRegister(Register.value())) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "CFI instruction references an unmapped DWARF register");
    }
    return {};
  };

  for (const MachineCFIInstruction &Instruction : Info.instructions()) {
    if (const auto *Rule = Instruction.getIf<MachineCFIRegisterRule>()) {
      auto Result = ValidateRegister(Rule->Register);
      if (!Result)
        return Result;
    }
    if (const auto *Rule = Instruction.getIf<MachineCFIRegisterOffsetRule>()) {
      auto Result = ValidateRegister(Rule->Register);
      if (!Result)
        return Result;
    }
    if (const auto *Rule = Instruction.getIf<MachineCFIRegisterRegisterRule>()) {
      auto RegisterResult = ValidateRegister(Rule->Register);
      if (!RegisterResult)
        return RegisterResult;
      auto SourceResult = ValidateRegister(Rule->SourceRegister);
      if (!SourceResult)
        return SourceResult;
    }
    if (const auto *Rule = Instruction.getIf<MachineCFIAddressSpaceCFARule>()) {
      auto Result = ValidateRegister(Rule->Register);
      if (!Result)
        return Result;
    }
    if (const auto *Rule = Instruction.getIf<MachineCFIRegisterPairRule>()) {
      for (const DwarfRegisterNumber Register :
           {Rule->Register, Rule->FirstRegister, Rule->SecondRegister}) {
        auto Result = ValidateRegister(Register);
        if (!Result)
          return Result;
      }
    }
    if (const auto *Rule = Instruction.getIf<MachineCFIVectorRegistersRule>()) {
      auto Result = ValidateRegister(Rule->Register);
      if (!Result)
        return Result;
      for (const MachineCFIVectorRegisterLane &Lane : Rule->VectorRegisters) {
        Result = ValidateRegister(Lane.Register);
        if (!Result)
          return Result;
      }
    }
    if (const auto *Rule = Instruction.getIf<MachineCFIVectorOffsetRule>()) {
      auto RegisterResult = ValidateRegister(Rule->Register);
      if (!RegisterResult)
        return RegisterResult;
      auto MaskResult = ValidateRegister(Rule->MaskRegister);
      if (!MaskResult)
        return MaskResult;
    }
    if (const auto *Rule = Instruction.getIf<MachineCFIVectorRegisterMaskRule>()) {
      for (const DwarfRegisterNumber Register :
           {Rule->Register, Rule->SpillRegister, Rule->MaskRegister}) {
        auto Result = ValidateRegister(Register);
        if (!Result)
          return Result;
      }
    }
  }
  return {};
}

Expected<void> TargetDescription::validateFrameInfo(const MachineFrameInfo &Info) const {
  for (const StackObject &Object : Info.objects()) {
    const StackObjectDesc *Description = Object.description();
    if (Description != nullptr && !MIRText.supportsStackID(Description->Stack)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "stack object uses a stack ID unsupported by the target");
    }
  }
  return {};
}

Expected<void>
TargetDescription::validateExplicitOperands(Opcode InstructionOpcode,
                                            std::span<const MachineOperand> Operands,
                                            const MachineRegisterInfo &RegInfo) const {
  auto StructuralValidation =
      Instructions.validateExplicitOperands(InstructionOpcode, Operands, RegInfo);
  if (!StructuralValidation)
    return std::unexpected(StructuralValidation.error());

  const InstructionDescriptor *Descriptor = Instructions.descriptor(InstructionOpcode);
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError, "instruction opcode is not registered");
  }
  for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
    auto TargetValidation =
        validateTargetOperand(&descriptorForOperand(*Descriptor, Index), Operands[Index], RegInfo);
    if (!TargetValidation)
      return std::unexpected(TargetValidation.error());
  }
  return {};
}

Expected<void>
TargetDescription::validateTieCompatibility(const MachineOperand &Def, const MachineOperand &Use,
                                            const MachineRegisterInfo &RegInfo) const {
  const Register *DefReg = Def.getIf<Register>();
  const Register *UseReg = Use.getIf<Register>();
  if (DefReg == nullptr || UseReg == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "target tie validation received a non-register operand");
  }
  if (DefReg->isNoRegister() || UseReg->isNoRegister())
    return {};

  const auto DefPhys = DefReg->physicalRegister();
  const auto UsePhys = UseReg->physicalRegister();
  if (DefPhys && UsePhys) {
    if (*DefPhys != *UsePhys) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "tied physical register operands must name the same register");
    }
    return {};
  }

  const auto DefClass = effectiveRegisterClass(Def, RegInfo);
  const auto UseClass = effectiveRegisterClass(Use, RegInfo);
  if (DefClass && UseClass && !Registers.registerClassesOverlap(*DefClass, *UseClass)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tied virtual register classes have no compatible physical register");
  }

  auto BankFor = [&RegInfo](Register Reg) -> std::optional<RegisterBankID> {
    if (!Reg.isVirtual())
      return std::nullopt;
    auto Info = RegInfo.virtualRegisterInfo(Reg);
    return Info ? Info->get().Bank : std::nullopt;
  };
  const auto DefBank = BankFor(*DefReg);
  const auto UseBank = BankFor(*UseReg);

  auto ClassIntersectsBank = [this](RegisterClassID ClassID, RegisterBankID BankID) {
    const RegisterClassDesc *Class = Registers.registerClass(ClassID);
    return Class != nullptr &&
           std::ranges::any_of(Class->Registers, [this, BankID](PhysicalRegister Reg) {
             return Registers.bankContainsRegister(BankID, Reg);
           });
  };

  if (DefPhys && UseClass && !Registers.registerClassContains(*UseClass, *DefPhys)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tied physical register is outside the virtual use register class");
  }
  if (UsePhys && DefClass && !Registers.registerClassContains(*DefClass, *UsePhys)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tied physical register is outside the virtual def register class");
  }
  if (DefPhys && UseBank && !Registers.bankContainsRegister(*UseBank, *DefPhys)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tied physical register is outside the virtual use register bank");
  }
  if (UsePhys && DefBank && !Registers.bankContainsRegister(*DefBank, *UsePhys)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tied physical register is outside the virtual def register bank");
  }
  if (DefClass && UseBank && !ClassIntersectsBank(*DefClass, *UseBank)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tied def register class and use register bank do not intersect");
  }
  if (UseClass && DefBank && !ClassIntersectsBank(*UseClass, *DefBank)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tied use register class and def register bank do not intersect");
  }
  if (!DefClass && !UseClass && DefBank && UseBank &&
      !Registers.registerBanksOverlap(*DefBank, *UseBank)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tied virtual register banks have no compatible physical register");
  }
  return {};
}

Expected<void> TargetDescription::validateOperandTies(
    Opcode InstructionOpcode, std::span<const MachineOperand> ExplicitOperands,
    std::span<const MachineOperandTie> Ties, const MachineRegisterInfo &RegInfo) const {
  auto StructuralValidation =
      Instructions.validateOperandTies(InstructionOpcode, ExplicitOperands, Ties);
  if (!StructuralValidation)
    return std::unexpected(StructuralValidation.error());

  for (const MachineOperandTie Tie : Ties) {
    auto Compatibility = validateTieCompatibility(ExplicitOperands[Tie.DefOperand],
                                                  ExplicitOperands[Tie.UseOperand], RegInfo);
    if (!Compatibility)
      return std::unexpected(Compatibility.error());
  }
  return {};
}

Expected<void>
TargetDescription::validateImplicitOperands(Opcode InstructionOpcode,
                                            std::span<const MachineOperand *const> ImplicitOperands,
                                            const MachineRegisterInfo &RegInfo) const {
  auto StructuralValidation =
      Instructions.validateImplicitOperands(InstructionOpcode, ImplicitOperands);
  if (!StructuralValidation)
    return std::unexpected(StructuralValidation.error());

  for (const MachineOperand *Operand : ImplicitOperands) {
    if (Operand == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "implicit operand sequence contains a null operand");
    }
    auto TargetValidation = validateTargetOperand(nullptr, *Operand, RegInfo);
    if (!TargetValidation)
      return std::unexpected(TargetValidation.error());
  }
  return {};
}

Expected<void>
TargetDescription::validateEarlyClobber(std::span<const MachineOperand> ExplicitOperands,
                                        std::span<const MachineOperand *const> ImplicitOperands,
                                        std::span<const MachineOperandTie> Ties) const {
  struct IndexedOperand final {
    const MachineOperand *Operand = nullptr;
    std::optional<std::size_t> ExplicitIndex;
  };

  std::vector<IndexedOperand> Operands;
  Operands.reserve(ExplicitOperands.size() + ImplicitOperands.size());
  for (std::size_t Index = 0; Index < ExplicitOperands.size(); ++Index)
    Operands.push_back(IndexedOperand{&ExplicitOperands[Index], Index});
  for (const MachineOperand *Operand : ImplicitOperands)
    Operands.push_back(IndexedOperand{Operand, std::nullopt});

  auto AreTied = [Ties](const IndexedOperand &Def, const IndexedOperand &Use) {
    if (!Def.ExplicitIndex || !Use.ExplicitIndex)
      return false;
    return std::ranges::any_of(Ties, [&Def, &Use](MachineOperandTie Tie) {
      return Tie.DefOperand == *Def.ExplicitIndex && Tie.UseOperand == *Use.ExplicitIndex;
    });
  };

  for (const IndexedOperand &DefEntry : Operands) {
    if (DefEntry.Operand == nullptr)
      continue;
    const MachineOperand &Def = *DefEntry.Operand;
    if (!Def.isDef() || !Def.registerFlags().has(RegisterOperandFlag::EarlyClobber))
      continue;

    const Register *DefReg = Def.getIf<Register>();
    if (DefReg == nullptr || DefReg->isNoRegister())
      continue;
    for (const IndexedOperand &UseEntry : Operands) {
      if (UseEntry.Operand == nullptr || UseEntry.Operand == DefEntry.Operand)
        continue;
      const MachineOperand &Use = *UseEntry.Operand;
      if (!Use.isUse() || AreTied(DefEntry, UseEntry))
        continue;

      const Register *UseReg = Use.getIf<Register>();
      if (UseReg == nullptr || UseReg->isNoRegister())
        continue;
      if (DefReg->isVirtual() && UseReg->isVirtual() && *DefReg == *UseReg) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "early-clobber virtual definition overlaps an untied use");
      }

      const auto DefPhys = DefReg->physicalRegister();
      const auto UsePhys = UseReg->physicalRegister();
      if (DefPhys && UsePhys && Registers.regsOverlap(*DefPhys, *UsePhys)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "early-clobber physical definition overlaps an untied use");
      }
    }
  }
  return {};
}

Expected<void> TargetDescription::validateInstruction(const MachineInstr &Instruction,
                                                      const MachineRegisterInfo &RegInfo) const {
  auto StructuralValidation = Instructions.validateInstruction(Instruction, RegInfo);
  if (!StructuralValidation)
    return std::unexpected(StructuralValidation.error());

  const auto Operands = Instruction.operands();
  std::size_t ExplicitCount = 0;
  while (ExplicitCount < Operands.size() && !Operands[ExplicitCount].isImplicit()) {
    ++ExplicitCount;
  }

  auto ExplicitValidation =
      validateExplicitOperands(Instruction.opcode(), Operands.first(ExplicitCount), RegInfo);
  if (!ExplicitValidation)
    return std::unexpected(ExplicitValidation.error());

  std::vector<const MachineOperand *> ImplicitOperands;
  for (std::size_t Index = ExplicitCount; Index < Operands.size(); ++Index)
    ImplicitOperands.push_back(&Operands[Index]);
  auto ImplicitValidation =
      validateImplicitOperands(Instruction.opcode(), ImplicitOperands, RegInfo);
  if (!ImplicitValidation)
    return std::unexpected(ImplicitValidation.error());

  std::vector<MachineOperandTie> Ties;
  for (std::size_t Index = 0; Index < ExplicitCount; ++Index) {
    const MachineOperand &Operand = Operands[Index];
    if (!Operand.tiedOperandIndex() || Index > *Operand.tiedOperandIndex()) {
      continue;
    }
    const std::size_t OtherIndex = *Operand.tiedOperandIndex();
    if (isPureDef(Operand) && isPureUse(Operands[OtherIndex])) {
      Ties.push_back(MachineOperandTie{Index, OtherIndex});
    } else {
      Ties.push_back(MachineOperandTie{OtherIndex, Index});
    }
  }
  auto TieValidation =
      validateOperandTies(Instruction.opcode(), Operands.first(ExplicitCount), Ties, RegInfo);
  if (!TieValidation)
    return std::unexpected(TieValidation.error());

  return validateEarlyClobber(Operands.first(ExplicitCount), ImplicitOperands, Ties);
}

Expected<void> TargetDescription::validateRegisterInfo(const MachineRegisterInfo &RegInfo) const {
  for (const PhysicalRegisterLiveIn &LiveIn : RegInfo.liveIns()) {
    if (!Registers.isKnownPhysicalRegister(LiveIn.PhysReg)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "function live-in names an unknown physical register");
    }
    if (!LiveIn.InitialVirtualRegister)
      continue;
    auto Info = RegInfo.virtualRegisterInfo(*LiveIn.InitialVirtualRegister);
    if (!Info)
      return std::unexpected(Info.error());
    const std::optional<RegisterClassID> RegClass = Info->get().RegClass;
    if (RegClass && !Registers.registerClassContains(*RegClass, LiveIn.PhysReg)) {
      return makeUnexpected(
          ErrorCode::VerificationError,
          "function live-in physical register is outside its initial virtual register class");
    }
    const std::optional<RegisterBankID> Bank = Info->get().Bank;
    if (Bank && !Registers.bankContainsRegister(*Bank, LiveIn.PhysReg)) {
      return makeUnexpected(
          ErrorCode::VerificationError,
          "function live-in physical register is outside its initial virtual register bank");
    }
  }
  for (const PhysicalRegister Reg : RegInfo.reservedRegisters()) {
    if (!Registers.isKnownPhysicalRegister(Reg)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "reserved-register list names an unknown physical register");
    }
  }

  const auto VirtualRegisters = RegInfo.virtualRegisters();
  for (std::size_t Index = 0; Index < VirtualRegisters.size(); ++Index) {
    const VirtualRegisterInfo &Info = VirtualRegisters[Index];
    if (Info.IsTombstone)
      continue;
    auto Reg = Register::makeVirtual(Index);
    if (!Reg)
      return std::unexpected(Reg.error());
    auto Validation = validateVirtualRegister(*Reg, Info);
    if (!Validation)
      return std::unexpected(Validation.error());
    for (const PhysicalRegister Hint : Info.AllocationHints) {
      const bool IsStaticReservedOrAlias = std::ranges::any_of(
          Registers.physicalRegisters(), [this, Hint](const PhysicalRegisterDesc &Register) {
            return Register.IsReserved && Registers.regsOverlap(Register.Reg, Hint);
          });
      const bool IsFunctionReservedOrAlias =
          std::ranges::any_of(RegInfo.reservedRegisters(), [this, Hint](PhysicalRegister Reserved) {
            return Registers.regsOverlap(Reserved, Hint);
          });
      if (IsStaticReservedOrAlias || IsFunctionReservedOrAlias) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "virtual register allocation hint overlaps a reserved physical register");
      }
    }
  }
  return {};
}

Expected<void> TargetDescription::validateBlockLiveIns(const MachineBasicBlock &Block) const {
  for (const MachineBasicBlockLiveIn &LiveIn : Block.liveIns()) {
    if (!Registers.isKnownPhysicalRegister(LiveIn.PhysReg)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "block live-in names an unknown physical register");
    }
    if (!Registers.isValidLaneMask(LiveIn.PhysReg, LiveIn.Lanes)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "block live-in lane mask is not valid for its physical register");
    }
  }
  return {};
}

} // namespace rcc

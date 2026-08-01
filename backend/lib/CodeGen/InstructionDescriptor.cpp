// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/InstructionDescriptor.h"

#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] bool isRegisterOnly(const OperandKindSet &Kinds) noexcept {
  OperandKindSet RegisterKind{MachineOperand::Kind::Register};
  return Kinds == RegisterKind;
}

[[nodiscard]] bool isDefRole(RegisterOperandRole Role) noexcept {
  return Role == RegisterOperandRole::Def || Role == RegisterOperandRole::DefUse;
}

[[nodiscard]] bool isUseRole(RegisterOperandRole Role) noexcept {
  return Role == RegisterOperandRole::Use || Role == RegisterOperandRole::DefUse;
}

[[nodiscard]] bool formsDefUsePair(RegisterOperandRole Left, RegisterOperandRole Right) noexcept {
  return (Left == RegisterOperandRole::Def && Right == RegisterOperandRole::Use) ||
         (Left == RegisterOperandRole::Use && Right == RegisterOperandRole::Def);
}

[[nodiscard]] Expected<void> validateDescriptorStructure(const InstructionDescriptor &Descriptor) {
  if (!Descriptor.InstructionOpcode.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "instruction descriptor opcode is invalid");
  if (Descriptor.Name.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "instruction descriptor name is empty");
  if (Descriptor.NumExplicitDefs > Descriptor.Operands.size()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "instruction descriptor explicit def count is out of range");
  }

  bool SawOptional = false;
  for (std::size_t Index = 0; Index < Descriptor.Operands.size(); ++Index) {
    const OperandDescriptor &Operand = Descriptor.Operands[Index];
    if (Operand.Kinds.empty()) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "operand descriptor must accept at least one operand kind");
    }

    const bool IsRegister = Operand.Kinds.contains(MachineOperand::Kind::Register);
    if (IsRegister != (Operand.RegisterRole != RegisterOperandRole::None)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "register-capable operand descriptor must declare a register role");
    }
    if (isDefRole(Operand.RegisterRole) && !isRegisterOnly(Operand.Kinds)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "register definition descriptor must accept only registers");
    }
    if (!IsRegister &&
        (Operand.RegClass || Operand.RegBank || Operand.EarlyClobber || Operand.AllowSubReg)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "non-register operand descriptor has register constraints");
    }
    if (Operand.EarlyClobber && !isDefRole(Operand.RegisterRole)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "early-clobber operand descriptor must define a register");
    }
    if (Index < Descriptor.NumExplicitDefs && !isDefRole(Operand.RegisterRole)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "canonical explicit defs must precede instruction uses");
    }
    if (Index >= Descriptor.NumExplicitDefs && isDefRole(Operand.RegisterRole)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "register definition appears after the explicit def prefix");
    }

    if (Descriptor.Variadic && Operand.Optional) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "variadic instruction descriptor cannot contain optional operands");
    }
    if (Operand.Optional)
      SawOptional = true;
    else if (SawOptional && !Operand.VariadicGroup) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "optional operand descriptors must form a trailing suffix");
    }

    if (Operand.VariadicGroup &&
        (!Descriptor.Variadic || Index + 1 != Descriptor.Operands.size())) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "variadic operand group must be the final descriptor");
    }
    if (Operand.VariadicGroup && (Operand.Optional || Operand.DefaultTiedOperand)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "variadic operand descriptor cannot be optional or have a default tie");
    }
  }

  if (Descriptor.Variadic &&
      (Descriptor.Operands.empty() || !Descriptor.Operands.back().VariadicGroup)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "variadic instruction descriptor requires a final variadic group");
  }
  if (!Descriptor.Variadic &&
      std::ranges::any_of(Descriptor.Operands,
                          [](const OperandDescriptor &Operand) { return Operand.VariadicGroup; })) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "non-variadic instruction descriptor has a variadic group");
  }

  for (std::size_t Index = 0; Index < Descriptor.Operands.size(); ++Index) {
    const OperandDescriptor &Operand = Descriptor.Operands[Index];
    if (!Operand.DefaultTiedOperand)
      continue;
    const std::size_t OtherIndex = *Operand.DefaultTiedOperand;
    if (OtherIndex >= Descriptor.Operands.size() || OtherIndex == Index ||
        Descriptor.Operands[OtherIndex].DefaultTiedOperand != Index) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "default operand tie must be reciprocal and in range");
    }
    if (Operand.Optional || Descriptor.Operands[OtherIndex].Optional ||
        !isRegisterOnly(Operand.Kinds) || !isRegisterOnly(Descriptor.Operands[OtherIndex].Kinds) ||
        !formsDefUsePair(Operand.RegisterRole, Descriptor.Operands[OtherIndex].RegisterRole)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "default operand tie must connect required register def and use");
    }
  }

  std::unordered_set<NamedOperandID> OperandNames;
  for (const OperandDescriptor &Operand : Descriptor.Operands) {
    if (Operand.Name && !OperandNames.insert(*Operand.Name).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "instruction descriptor has duplicate named operands");
    }
  }

  std::unordered_set<PhysicalRegister> ImplicitDefs;
  for (const ImplicitRegisterDescriptor &Implicit : Descriptor.ImplicitDefs) {
    if (!Implicit.Reg.isValid() || !ImplicitDefs.insert(Implicit.Reg).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "implicit definitions must name unique physical registers");
    }
  }
  std::unordered_set<PhysicalRegister> ImplicitUses;
  for (const ImplicitRegisterDescriptor &Implicit : Descriptor.ImplicitUses) {
    if (!Implicit.Reg.isValid() || Implicit.EarlyClobber ||
        !ImplicitUses.insert(Implicit.Reg).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "implicit uses must name unique physical registers and cannot "
                            "early-clobber");
    }
  }

  const bool IsBranch = Descriptor.Flags.has(InstructionFlag::Branch);
  const bool IsTerminator = Descriptor.Flags.has(InstructionFlag::Terminator);
  if ((IsBranch || Descriptor.Flags.has(InstructionFlag::Return) ||
       Descriptor.Flags.has(InstructionFlag::Barrier) ||
       Descriptor.Flags.has(InstructionFlag::IndirectBranch)) &&
      !IsTerminator) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "branch, return, and barrier descriptors must be terminators");
  }
  if (Descriptor.Flags.has(InstructionFlag::IndirectBranch) && !IsBranch) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "indirect branch descriptor must also be a branch");
  }

  const bool IsPseudo = Descriptor.Flags.has(InstructionFlag::Pseudo);
  const bool IsMeta = Descriptor.Flags.has(InstructionFlag::Meta);
  if (IsMeta && (!IsPseudo || Descriptor.Flags.has(InstructionFlag::ProducesCode))) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "meta instruction must be a non-emitting pseudo");
  }
  if (!IsPseudo && Descriptor.Owner != PseudoOwner::None) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "non-pseudo instruction cannot have a pseudo owner");
  }
  if (IsPseudo && !IsMeta && Descriptor.Owner == PseudoOwner::None) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "executable pseudo must declare its expansion owner");
  }
  if (Descriptor.Flags.has(InstructionFlag::TerminalEmission) &&
      (!IsPseudo || Descriptor.Owner != PseudoOwner::AsmPrinter ||
       !Descriptor.Flags.has(InstructionFlag::ProducesCode))) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "terminal emission pseudo must produce code and be owned by AsmPrinter");
  }
  if (Descriptor.RequiresMemoryOperands && !Descriptor.Flags.has(InstructionFlag::MayLoad) &&
      !Descriptor.Flags.has(InstructionFlag::MayStore)) {
    return makeUnexpected(ErrorCode::InvalidArgument, "MMO requirement needs mayLoad or mayStore");
  }

  return {};
}

[[nodiscard]] const OperandDescriptor &descriptorForOperand(const InstructionDescriptor &Descriptor,
                                                            std::size_t Index) {
  if (Index < Descriptor.Operands.size())
    return Descriptor.Operands[Index];
  return Descriptor.Operands.back();
}

} // namespace

OperandKindSet::OperandKindSet(std::initializer_list<MachineOperand::Kind> Kinds) noexcept {
  for (const MachineOperand::Kind Kind : Kinds)
    add(Kind);
}

InstructionFlags::InstructionFlags(std::initializer_list<InstructionFlag> Flags) noexcept {
  for (const InstructionFlag Flag : Flags)
    set(Flag);
}

Expected<InstructionSet>
InstructionSet::create(std::vector<InstructionDescriptor> Descriptors,
                       std::vector<ImmediatePredicate> ImmediatePredicates) {
  if (Descriptors.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "instruction set requires at least one descriptor");
  }

  InstructionSet Result(std::move(Descriptors), std::move(ImmediatePredicates));

  std::unordered_set<std::string> PredicateNames;
  for (std::size_t Index = 0; Index < Result.ImmediatePredicates.size(); ++Index) {
    const ImmediatePredicate &Predicate = Result.ImmediatePredicates[Index];
    if (Predicate.Name.empty() || !Predicate.Predicate) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "immediate predicate requires a name and callable");
    }
    if (!Result.PredicateToIndex.emplace(Predicate.ID, Index).second) {
      return makeUnexpected(ErrorCode::InvalidArgument, "duplicate immediate predicate ID");
    }
    if (!PredicateNames.insert(Predicate.Name).second) {
      return makeUnexpected(ErrorCode::InvalidArgument, "duplicate immediate predicate name");
    }
  }

  for (std::size_t Index = 0; Index < Result.Descriptors.size(); ++Index) {
    const InstructionDescriptor &Descriptor = Result.Descriptors[Index];
    const auto Validation = validateDescriptorStructure(Descriptor);
    if (!Validation)
      return std::unexpected(Validation.error());
    if (!Result.OpcodeToIndex.emplace(Descriptor.InstructionOpcode, Index).second)
      return makeUnexpected(ErrorCode::InvalidArgument, "duplicate instruction opcode");
    if (!Result.NameToIndex.emplace(Descriptor.Name, Index).second)
      return makeUnexpected(ErrorCode::InvalidArgument, "duplicate instruction name");

    for (const OperandDescriptor &Operand : Descriptor.Operands) {
      if (Operand.ImmediatePredicate &&
          Result.immediatePredicate(*Operand.ImmediatePredicate) == nullptr) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "operand descriptor references an unknown immediate predicate");
      }
    }
  }

  return Result;
}

const InstructionDescriptor *InstructionSet::descriptor(Opcode InstructionOpcode) const noexcept {
  const auto Position = OpcodeToIndex.find(InstructionOpcode);
  if (Position == OpcodeToIndex.end())
    return nullptr;
  return &Descriptors[Position->second];
}

const InstructionDescriptor *InstructionSet::descriptor(std::string_view Name) const noexcept {
  const auto Position = NameToIndex.find(std::string(Name));
  if (Position == NameToIndex.end())
    return nullptr;
  return &Descriptors[Position->second];
}

Expected<void> InstructionSet::validateExplicitOperands(const InstructionDescriptor &Descriptor,
                                                        std::span<const MachineOperand> Operands,
                                                        const MachineRegisterInfo &RegInfo) const {
  std::size_t RequiredCount = Descriptor.Operands.size();
  if (Descriptor.Variadic)
    --RequiredCount;
  else {
    while (RequiredCount != 0 && Descriptor.Operands[RequiredCount - 1].Optional)
      --RequiredCount;
  }

  if (Operands.size() < RequiredCount ||
      (!Descriptor.Variadic && Operands.size() > Descriptor.Operands.size())) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "instruction has an invalid explicit operand count");
  }

  for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
    const MachineOperand &Operand = Operands[Index];
    if (Operand.isImplicit()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "explicit operand sequence contains an implicit register");
    }
    const auto Validation =
        validateOperand(descriptorForOperand(Descriptor, Index), Operand, RegInfo);
    if (!Validation)
      return std::unexpected(Validation.error());
  }
  return {};
}

Expected<void> InstructionSet::validateExplicitOperands(Opcode InstructionOpcode,
                                                        std::span<const MachineOperand> Operands,
                                                        const MachineRegisterInfo &RegInfo) const {
  const InstructionDescriptor *Descriptor = descriptor(InstructionOpcode);
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError, "instruction opcode is not registered");
  }
  return validateExplicitOperands(*Descriptor, Operands, RegInfo);
}

Expected<void> InstructionSet::validateOperand(const OperandDescriptor &Descriptor,
                                               const MachineOperand &Operand,
                                               const MachineRegisterInfo &RegInfo) const {
  if (!Descriptor.Kinds.contains(Operand.kind())) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "instruction operand kind does not match its descriptor");
  }
  if ((Operand.targetFlags() & ~Descriptor.AllowedTargetFlags) != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "instruction operand uses unregistered target flags");
  }

  if (Operand.isRegister()) {
    const bool IsDef = Operand.isDef();
    const bool IsUse = Operand.isUse();
    const bool ExpectedDef = isDefRole(Descriptor.RegisterRole);
    const bool ExpectedUse = isUseRole(Descriptor.RegisterRole);
    if (IsDef != ExpectedDef || IsUse != ExpectedUse) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "register operand def/use role does not match its descriptor");
    }
    if (Operand.registerFlags().has(RegisterOperandFlag::EarlyClobber) != Descriptor.EarlyClobber) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "register operand early-clobber does not match its descriptor");
    }
    if (!Descriptor.AllowSubReg && Operand.subReg().value() != 0) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "register operand has a disallowed subregister index");
    }

    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr) {
      return makeUnexpected(ErrorCode::InternalError, "register operand has inconsistent storage");
    }
    if (Reg->isVirtual()) {
      auto Info = RegInfo.virtualRegisterInfo(*Reg);
      if (!Info)
        return std::unexpected(Info.error());
      if (Descriptor.RegBank && Info->get().Bank != Descriptor.RegBank) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "virtual register bank does not match operand descriptor");
      }
      if (Descriptor.RegClass && Info->get().RegClass != Descriptor.RegClass) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "virtual register class does not match operand descriptor");
      }
    }
  }

  if (Descriptor.ImmediatePredicate) {
    const ImmediatePredicate *Predicate = immediatePredicate(*Descriptor.ImmediatePredicate);
    if (Predicate == nullptr || !Predicate->Predicate(Operand)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "instruction operand fails its immediate predicate");
    }
  }

  return {};
}

Expected<void> InstructionSet::validateInstruction(const MachineInstr &Instruction,
                                                   const MachineRegisterInfo &RegInfo) const {
  const InstructionDescriptor *Descriptor = descriptor(Instruction.opcode());
  if (Descriptor == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "instruction opcode is not registered");

  const auto AllOperands = Instruction.operands();
  std::size_t ExplicitCount = 0;
  while (ExplicitCount < AllOperands.size() && !AllOperands[ExplicitCount].isImplicit())
    ++ExplicitCount;
  for (std::size_t Index = ExplicitCount; Index < AllOperands.size(); ++Index) {
    if (!AllOperands[Index].isImplicit()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "explicit operand appears after implicit operands");
    }
  }

  const auto ExplicitValidation =
      validateExplicitOperands(*Descriptor, AllOperands.first(ExplicitCount), RegInfo);
  if (!ExplicitValidation)
    return std::unexpected(ExplicitValidation.error());

  for (std::size_t Index = 0; Index < ExplicitCount; ++Index) {
    const MachineOperand &Operand = AllOperands[Index];
    const OperandDescriptor &OperandDesc = descriptorForOperand(*Descriptor, Index);
    if (OperandDesc.DefaultTiedOperand &&
        Operand.tiedOperandIndex() != OperandDesc.DefaultTiedOperand) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "required descriptor tie is not materialized");
    }
    if (!Operand.tiedOperandIndex())
      continue;
    const std::size_t OtherIndex = *Operand.tiedOperandIndex();
    if (OtherIndex >= ExplicitCount || AllOperands[OtherIndex].tiedOperandIndex() != Index ||
        !Operand.isRegister() || !AllOperands[OtherIndex].isRegister() ||
        Operand.isDef() == AllOperands[OtherIndex].isDef() ||
        Operand.isUse() == AllOperands[OtherIndex].isUse()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "instruction operand tie is invalid or not reciprocal");
    }
  }

  const std::size_t DefaultImplicitCount =
      Descriptor->ImplicitDefs.size() + Descriptor->ImplicitUses.size();
  const std::size_t ActualImplicitCount = AllOperands.size() - ExplicitCount;
  if (ActualImplicitCount < DefaultImplicitCount ||
      (!Descriptor->AllowExtraImplicitOperands && ActualImplicitCount != DefaultImplicitCount)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "instruction implicit operand count does not match its descriptor");
  }

  std::size_t ImplicitIndex = ExplicitCount;
  auto ValidateImplicit = [&](const ImplicitRegisterDescriptor &Implicit,
                              bool IsDef) -> Expected<void> {
    const MachineOperand &Operand = AllOperands[ImplicitIndex++];
    const auto Reg = Operand.registerValue();
    if (!Reg || Reg->physicalRegister() != Implicit.Reg || Operand.isDef() != IsDef ||
        Operand.isUse() == IsDef || !Operand.isImplicit() ||
        Operand.registerFlags().has(RegisterOperandFlag::EarlyClobber) != Implicit.EarlyClobber ||
        Operand.registerFlags().has(RegisterOperandFlag::Renamable) != Implicit.Renamable) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "default implicit register operand does not match its descriptor");
    }
    return {};
  };

  for (const ImplicitRegisterDescriptor &Implicit : Descriptor->ImplicitDefs) {
    auto Validation = ValidateImplicit(Implicit, true);
    if (!Validation)
      return std::unexpected(Validation.error());
  }
  for (const ImplicitRegisterDescriptor &Implicit : Descriptor->ImplicitUses) {
    auto Validation = ValidateImplicit(Implicit, false);
    if (!Validation)
      return std::unexpected(Validation.error());
  }
  for (; ImplicitIndex < AllOperands.size(); ++ImplicitIndex) {
    if (!AllOperands[ImplicitIndex].isRegister() || !AllOperands[ImplicitIndex].isImplicit()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "dynamic implicit operand must be an implicit register");
    }
  }

  const bool MayLoad = Descriptor->Flags.has(InstructionFlag::MayLoad);
  const bool MayStore = Descriptor->Flags.has(InstructionFlag::MayStore);
  if (Descriptor->RequiresMemoryOperands && Instruction.memOperands().empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "memory instruction is missing MachineMemOperand");
  }
  for (const MachineMemOperand *MemOperand : Instruction.memOperands()) {
    if (MemOperand == nullptr || (MemOperand->isLoad() && !MayLoad) ||
        (MemOperand->isStore() && !MayStore)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "MachineMemOperand access conflicts with instruction descriptor");
    }
  }
  if (!MayLoad && !MayStore && !Instruction.memOperands().empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "non-memory instruction carries MachineMemOperand");
  }

  return {};
}

Expected<std::vector<MachineOperand>>
InstructionSet::createDefaultImplicitOperands(const InstructionDescriptor &Descriptor) const {
  std::vector<MachineOperand> Result;
  Result.reserve(Descriptor.ImplicitDefs.size() + Descriptor.ImplicitUses.size());

  auto AppendImplicit = [&Result](const ImplicitRegisterDescriptor &Implicit,
                                  bool IsDef) -> Expected<void> {
    RegisterOperandFlags Flags{
        IsDef ? RegisterOperandFlag::Define : RegisterOperandFlag::Use,
        RegisterOperandFlag::Implicit,
    };
    if (Implicit.EarlyClobber)
      Flags.set(RegisterOperandFlag::EarlyClobber);
    if (Implicit.Renamable)
      Flags.set(RegisterOperandFlag::Renamable);

    auto Operand = MachineOperand::registerOperand(Register::fromPhysical(Implicit.Reg), Flags);
    if (!Operand)
      return std::unexpected(Operand.error());
    Result.push_back(std::move(*Operand));
    return {};
  };

  for (const ImplicitRegisterDescriptor &Implicit : Descriptor.ImplicitDefs) {
    auto AppendResult = AppendImplicit(Implicit, true);
    if (!AppendResult)
      return std::unexpected(AppendResult.error());
  }
  for (const ImplicitRegisterDescriptor &Implicit : Descriptor.ImplicitUses) {
    auto AppendResult = AppendImplicit(Implicit, false);
    if (!AppendResult)
      return std::unexpected(AppendResult.error());
  }
  return Result;
}

const ImmediatePredicate *
InstructionSet::immediatePredicate(ImmediatePredicateID ID) const noexcept {
  const auto Position = PredicateToIndex.find(ID);
  if (Position == PredicateToIndex.end())
    return nullptr;
  return &ImmediatePredicates[Position->second];
}

} // namespace rcc

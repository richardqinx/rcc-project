// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/InstructionDescriptor.h"

#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
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

[[nodiscard]] bool isPureDef(const MachineOperand &Operand) noexcept {
  return Operand.isRegister() && Operand.isDef() && !Operand.isUse();
}

[[nodiscard]] bool isPureUse(const MachineOperand &Operand) noexcept {
  return Operand.isRegister() && Operand.isUse() && !Operand.isDef();
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
  std::optional<std::size_t> VariadicBegin;
  std::optional<std::size_t> VariadicEnd;
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
    if (Operand.TypeConstraint) {
      if (!isRegisterOnly(Operand.Kinds) || Operand.TypeConstraint->Kinds.empty() ||
          Operand.TypeConstraint->Kinds.contains(LLT::Kind::Invalid)) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "typed operand descriptor requires a nonempty register type set");
      }
    }
    if (Operand.EarlyClobber && !isDefRole(Operand.RegisterRole)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "early-clobber operand descriptor must define a register");
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

    if (Operand.VariadicGroup && (Operand.Optional || Operand.DefaultTiedOperand)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "variadic operand descriptor cannot be optional or have a default tie");
    }
    if (Operand.VariadicGroup) {
      if (!Descriptor.Variadic) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "non-variadic instruction descriptor has a variadic group");
      }
      if (!VariadicBegin)
        VariadicBegin = Index;
      VariadicEnd = Index + 1;
    } else if (VariadicEnd) {
      const bool LaterGroup =
          std::ranges::any_of(Descriptor.Operands.begin() + static_cast<std::ptrdiff_t>(Index),
                              Descriptor.Operands.end(), [](const OperandDescriptor &Candidate) {
                                return Candidate.VariadicGroup;
                              });
      if (LaterGroup) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "variadic operand group descriptors must be contiguous");
      }
    }
  }

  if (Descriptor.Variadic && !VariadicBegin) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "variadic instruction descriptor requires a variadic group");
  }
  if (!Descriptor.Variadic && Descriptor.MinimumVariadicRepetitions != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "non-variadic instruction has a minimum variadic repetition count");
  }

  bool SawUse = false;
  std::size_t MinimumDefs = 0;
  for (std::size_t Index = 0; Index < Descriptor.Operands.size(); ++Index) {
    const OperandDescriptor &Operand = Descriptor.Operands[Index];
    const std::size_t Repetitions =
        Operand.VariadicGroup ? Descriptor.MinimumVariadicRepetitions : 1;
    for (std::size_t Repeat = 0; Repeat < Repetitions; ++Repeat) {
      if (isDefRole(Operand.RegisterRole)) {
        if (SawUse) {
          return makeUnexpected(ErrorCode::InvalidArgument,
                                "register definition appears after an instruction use");
        }
        ++MinimumDefs;
      } else {
        SawUse = true;
      }
    }
  }
  if (Descriptor.NumExplicitDefs != MinimumDefs) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "instruction descriptor explicit def count does not match its shape");
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

struct OperandShape final {
  std::size_t VariadicBegin = 0;
  std::size_t VariadicEnd = 0;
  std::size_t Repetitions = 0;
};

[[nodiscard]] Expected<OperandShape> resolveOperandShape(const InstructionDescriptor &Descriptor,
                                                         std::size_t OperandCount) {
  if (!Descriptor.Variadic) {
    std::size_t RequiredCount = Descriptor.Operands.size();
    while (RequiredCount != 0 && Descriptor.Operands[RequiredCount - 1].Optional)
      --RequiredCount;
    if (OperandCount < RequiredCount || OperandCount > Descriptor.Operands.size()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "instruction has an invalid explicit operand count");
    }
    return OperandShape{};
  }

  const auto Begin = std::ranges::find_if(
      Descriptor.Operands, [](const OperandDescriptor &Operand) { return Operand.VariadicGroup; });
  const auto End = std::ranges::find_if_not(
      Begin, Descriptor.Operands.end(),
      [](const OperandDescriptor &Operand) { return Operand.VariadicGroup; });
  const std::size_t VariadicBegin =
      static_cast<std::size_t>(std::distance(Descriptor.Operands.begin(), Begin));
  const std::size_t VariadicEnd =
      static_cast<std::size_t>(std::distance(Descriptor.Operands.begin(), End));
  const std::size_t GroupSize = VariadicEnd - VariadicBegin;
  const std::size_t FixedCount = Descriptor.Operands.size() - GroupSize;
  if (GroupSize == 0 || OperandCount < FixedCount) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "instruction has an invalid variadic operand count");
  }
  const std::size_t VariableCount = OperandCount - FixedCount;
  if (VariableCount % GroupSize != 0 ||
      VariableCount / GroupSize < Descriptor.MinimumVariadicRepetitions) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "instruction has an incomplete or undersized variadic operand group");
  }
  return OperandShape{VariadicBegin, VariadicEnd, VariableCount / GroupSize};
}

[[nodiscard]] const OperandDescriptor &descriptorForOperand(const InstructionDescriptor &Descriptor,
                                                            const OperandShape &Shape,
                                                            std::size_t Index) {
  if (!Descriptor.Variadic)
    return Descriptor.Operands[Index];
  if (Index < Shape.VariadicBegin)
    return Descriptor.Operands[Index];
  const std::size_t GroupSize = Shape.VariadicEnd - Shape.VariadicBegin;
  const std::size_t VariableCount = Shape.Repetitions * GroupSize;
  if (Index < Shape.VariadicBegin + VariableCount) {
    return Descriptor.Operands[Shape.VariadicBegin + (Index - Shape.VariadicBegin) % GroupSize];
  }
  return Descriptor.Operands[Shape.VariadicEnd + (Index - Shape.VariadicBegin - VariableCount)];
}

} // namespace

OperandKindSet::OperandKindSet(std::initializer_list<MachineOperand::Kind> Kinds) noexcept {
  for (const MachineOperand::Kind Kind : Kinds)
    add(Kind);
}

LLTKindSet::LLTKindSet(std::initializer_list<LLT::Kind> Kinds) noexcept {
  for (const LLT::Kind Kind : Kinds)
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
  auto Shape = resolveOperandShape(Descriptor, Operands.size());
  if (!Shape)
    return std::unexpected(Shape.error());

  bool SawUse = false;
  std::unordered_map<std::uint16_t, LLT> EqualityGroups;
  for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
    const MachineOperand &Operand = Operands[Index];
    if (Operand.isImplicit()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "explicit operand sequence contains an implicit register");
    }
    const OperandDescriptor &OperandDesc = descriptorForOperand(Descriptor, *Shape, Index);
    const auto Validation = validateOperand(OperandDesc, Operand, RegInfo);
    if (!Validation)
      return std::unexpected(Validation.error());
    if (Operand.isRegister()) {
      if (isDefRole(OperandDesc.RegisterRole)) {
        if (SawUse) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "instruction register definition appears after a use");
        }
      } else {
        SawUse = true;
      }
    } else {
      SawUse = true;
    }

    if (!OperandDesc.TypeConstraint)
      continue;
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "typed instruction operand must use a virtual register");
    }
    auto Info = RegInfo.virtualRegisterInfo(*Reg);
    if (!Info)
      return std::unexpected(Info.error());
    if (!Info->get().Type) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "typed instruction operand virtual register has no LLT");
    }
    const LLT Type = *Info->get().Type;
    if (!OperandDesc.TypeConstraint->Kinds.contains(Type.kind())) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "instruction operand LLT kind is not permitted by its descriptor");
    }
    if (OperandDesc.TypeConstraint->EqualityGroup) {
      const auto [Position, Inserted] =
          EqualityGroups.emplace(*OperandDesc.TypeConstraint->EqualityGroup, Type);
      if (!Inserted && Position->second != Type) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "instruction operands in one LLT group have different types");
      }
    }
  }
  if (Descriptor.VerifyExplicitOperands != nullptr)
    return Descriptor.VerifyExplicitOperands(Descriptor.InstructionOpcode, Operands, RegInfo);
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

Expected<void> InstructionSet::validateOperandTies(const InstructionDescriptor &Descriptor,
                                                   std::span<const MachineOperand> ExplicitOperands,
                                                   std::span<const MachineOperandTie> Ties) const {
  std::vector<std::optional<std::size_t>> Partners(ExplicitOperands.size());
  for (const MachineOperandTie Tie : Ties) {
    if (Tie.DefOperand >= ExplicitOperands.size() || Tie.UseOperand >= ExplicitOperands.size() ||
        Tie.DefOperand == Tie.UseOperand || !isPureDef(ExplicitOperands[Tie.DefOperand]) ||
        !isPureUse(ExplicitOperands[Tie.UseOperand])) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "instruction tie must connect an in-range pure register def and use");
    }
    if (Partners[Tie.DefOperand] || Partners[Tie.UseOperand]) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "instruction operand participates in duplicate or conflicting ties");
    }
    Partners[Tie.DefOperand] = Tie.UseOperand;
    Partners[Tie.UseOperand] = Tie.DefOperand;
  }

  for (std::size_t Index = 0; Index < Descriptor.Operands.size(); ++Index) {
    const OperandDescriptor &Operand = Descriptor.Operands[Index];
    if (Operand.DefaultTiedOperand &&
        (Index >= Partners.size() || Partners[Index] != Operand.DefaultTiedOperand)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "required descriptor tie is not materialized");
    }
  }
  return {};
}

Expected<void> InstructionSet::validateOperandTies(Opcode InstructionOpcode,
                                                   std::span<const MachineOperand> ExplicitOperands,
                                                   std::span<const MachineOperandTie> Ties) const {
  const InstructionDescriptor *Descriptor = descriptor(InstructionOpcode);
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError, "instruction opcode is not registered");
  }
  return validateOperandTies(*Descriptor, ExplicitOperands, Ties);
}

Expected<void> InstructionSet::validateImplicitOperands(
    const InstructionDescriptor &Descriptor,
    std::span<const MachineOperand *const> ImplicitOperands) const {
  const std::size_t DefaultCount = Descriptor.ImplicitDefs.size() + Descriptor.ImplicitUses.size();
  if (ImplicitOperands.size() < DefaultCount ||
      (!Descriptor.AllowExtraImplicitOperands && ImplicitOperands.size() != DefaultCount)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "instruction implicit operand count does not match its descriptor");
  }

  std::size_t Index = 0;
  auto ValidateDefault = [&](const ImplicitRegisterDescriptor &Implicit,
                             bool IsDef) -> Expected<void> {
    const MachineOperand *Operand = ImplicitOperands[Index++];
    const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
    if (Reg == nullptr || Reg->physicalRegister() != Implicit.Reg || Operand->isDef() != IsDef ||
        Operand->isUse() == IsDef || !Operand->isImplicit() ||
        Operand->registerFlags().has(RegisterOperandFlag::EarlyClobber) != Implicit.EarlyClobber ||
        Operand->registerFlags().has(RegisterOperandFlag::Renamable) != Implicit.Renamable) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "default implicit register operand does not match its descriptor");
    }
    return {};
  };

  for (const ImplicitRegisterDescriptor &Implicit : Descriptor.ImplicitDefs) {
    auto Validation = ValidateDefault(Implicit, true);
    if (!Validation)
      return std::unexpected(Validation.error());
  }
  for (const ImplicitRegisterDescriptor &Implicit : Descriptor.ImplicitUses) {
    auto Validation = ValidateDefault(Implicit, false);
    if (!Validation)
      return std::unexpected(Validation.error());
  }
  for (; Index < ImplicitOperands.size(); ++Index) {
    const MachineOperand *Operand = ImplicitOperands[Index];
    if (Operand == nullptr || !Operand->isRegister() || !Operand->isImplicit()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "dynamic implicit operand must be an implicit register");
    }
  }
  return {};
}

Expected<void> InstructionSet::validateImplicitOperands(
    Opcode InstructionOpcode, std::span<const MachineOperand *const> ImplicitOperands) const {
  const InstructionDescriptor *Descriptor = descriptor(InstructionOpcode);
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError, "instruction opcode is not registered");
  }
  return validateImplicitOperands(*Descriptor, ImplicitOperands);
}

Expected<void>
InstructionSet::validateMemOperands(const InstructionDescriptor &Descriptor,
                                    std::span<const MachineMemOperand *const> MemOperands) const {
  const bool MayLoad = Descriptor.Flags.has(InstructionFlag::MayLoad);
  const bool MayStore = Descriptor.Flags.has(InstructionFlag::MayStore);
  if (Descriptor.RequiresMemoryOperands && MemOperands.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "memory instruction is missing MachineMemOperand");
  }
  if (!MayLoad && !MayStore && !MemOperands.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "non-memory instruction carries MachineMemOperand");
  }
  for (const MachineMemOperand *MemOperand : MemOperands) {
    if (MemOperand == nullptr || (MemOperand->isLoad() && !MayLoad) ||
        (MemOperand->isStore() && !MayStore)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "MachineMemOperand access conflicts with instruction descriptor");
    }
  }
  return {};
}

Expected<void>
InstructionSet::validateMemOperands(Opcode InstructionOpcode,
                                    std::span<const MachineMemOperand *const> MemOperands) const {
  const InstructionDescriptor *Descriptor = descriptor(InstructionOpcode);
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError, "instruction opcode is not registered");
  }
  return validateMemOperands(*Descriptor, MemOperands);
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
    if (Descriptor.RegisterRole == RegisterOperandRole::Any) {
      if (!IsDef && !IsUse) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "unclassified register operand must define or use its register");
      }
    } else if (IsDef != ExpectedDef || IsUse != ExpectedUse) {
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

  std::vector<MachineOperandTie> Ties;
  for (std::size_t Index = 0; Index < ExplicitCount; ++Index) {
    const MachineOperand &Operand = AllOperands[Index];
    if (!Operand.tiedOperandIndex())
      continue;
    const std::size_t OtherIndex = *Operand.tiedOperandIndex();
    if (OtherIndex >= ExplicitCount || AllOperands[OtherIndex].tiedOperandIndex() != Index) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "instruction operand tie is out of range or not reciprocal");
    }
    if (Index > OtherIndex)
      continue;
    if (isPureUse(Operand) && isPureDef(AllOperands[OtherIndex]))
      Ties.push_back(MachineOperandTie{OtherIndex, Index});
    else
      Ties.push_back(MachineOperandTie{Index, OtherIndex});
  }

  auto TieValidation = validateOperandTies(*Descriptor, AllOperands.first(ExplicitCount), Ties);
  if (!TieValidation)
    return std::unexpected(TieValidation.error());

  std::vector<const MachineOperand *> ImplicitOperands;
  ImplicitOperands.reserve(AllOperands.size() - ExplicitCount);
  for (std::size_t Index = ExplicitCount; Index < AllOperands.size(); ++Index) {
    if (AllOperands[Index].tiedOperandIndex()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "implicit register operand cannot participate in a tie");
    }
    ImplicitOperands.push_back(&AllOperands[Index]);
  }

  auto ImplicitValidation = validateImplicitOperands(*Descriptor, ImplicitOperands);
  if (!ImplicitValidation)
    return std::unexpected(ImplicitValidation.error());
  auto MemoryValidation = validateMemOperands(*Descriptor, Instruction.memOperands());
  if (!MemoryValidation)
    return std::unexpected(MemoryValidation.error());
  if (Descriptor->VerifyInstructionSemantics != nullptr)
    return Descriptor->VerifyInstructionSemantics(Instruction, RegInfo);
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

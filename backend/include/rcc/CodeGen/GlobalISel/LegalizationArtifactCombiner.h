// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_LEGALIZATIONARTIFACTCOMBINER_H
#define RCC_CODEGEN_GLOBALISEL_LEGALIZATIONARTIFACTCOMBINER_H

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "rcc/CodeGen/GlobalISel/GISelValueTracking.h"
#include "rcc/CodeGen/GlobalISel/GenericMachineInstrs.h"
#include "rcc/CodeGen/GlobalISel/LegalizerInfo.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/Support/Error.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {

class LegalizationArtifactCombiner final {
  MachineIRBuilder &Builder;
  MachineRegisterInfo &MRI;
  const LegalizerInfo &LI;
  GISelValueTracking *VT;

  [[nodiscard]] static bool isArtifactCast(Opcode InstructionOpcode) noexcept {
    switch (static_cast<GenericOpcode>(InstructionOpcode.value())) {
    case GenericOpcode::G_TRUNC:
    case GenericOpcode::G_SEXT:
    case GenericOpcode::G_ZEXT:
    case GenericOpcode::G_ANYEXT:
      return true;
    default:
      return false;
    }
  }

public:
  LegalizationArtifactCombiner(MachineIRBuilder &Builder, MachineRegisterInfo &MRI,
                               const LegalizerInfo &Info,
                               GISelValueTracking *ValueTracking = nullptr)
      : Builder(Builder), MRI(MRI), LI(Info), VT(ValueTracking) {}

  [[nodiscard]] Expected<bool> tryCombineInstruction(MachineInstr &Instruction,
                                                     std::vector<MachineInstr *> &DeadInstructions,
                                                     GISelObserverWrapper &Observer) {
    if (!DeadInstructions.empty()) {
      auto Deleted = deleteMarkedDeadInsts(DeadInstructions, Observer);
      if (!Deleted)
        return std::unexpected(Deleted.error());
    }

    std::vector<Register> UpdatedDefs;
    Expected<bool> Changed = false;
    switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
    default:
      return false;
    case GenericOpcode::G_ANYEXT:
      Changed = tryCombineAnyExt(Instruction, DeadInstructions, UpdatedDefs, Observer);
      break;
    case GenericOpcode::G_ZEXT:
      Changed = tryCombineZExt(Instruction, DeadInstructions, UpdatedDefs, Observer);
      break;
    case GenericOpcode::G_SEXT:
      Changed = tryCombineSExt(Instruction, DeadInstructions, UpdatedDefs, Observer);
      break;
    case GenericOpcode::G_UNMERGE_VALUES:
      Changed = tryCombineUnmergeValues(Instruction, DeadInstructions, UpdatedDefs, Observer);
      break;
    case GenericOpcode::G_MERGE_VALUES:
    case GenericOpcode::G_BUILD_VECTOR:
    case GenericOpcode::G_CONCAT_VECTORS: {
      const auto Destination = Instruction.operandAt(0)->registerValue();
      if (Destination) {
        for (MachineOperand &Use : MRI.useOperands(*Destination)) {
          MachineInstr *User = Use.parent();
          if (User == nullptr)
            continue;
          if (User->opcode() == opcode(GenericOpcode::G_UNMERGE_VALUES) ||
              User->opcode() == opcode(GenericOpcode::G_TRUNC)) {
            UpdatedDefs.push_back(*Destination);
            break;
          }
        }
      }
      Changed = tryCombineMergeLike(Instruction, DeadInstructions, UpdatedDefs, Observer);
      break;
    }
    case GenericOpcode::G_EXTRACT:
      Changed = tryCombineExtract(Instruction, DeadInstructions, UpdatedDefs);
      break;
    case GenericOpcode::G_TRUNC:
      Changed = tryCombineTrunc(Instruction, DeadInstructions, UpdatedDefs, Observer);
      if (Changed && !*Changed)
        UpdatedDefs.push_back(*Instruction.operandAt(0)->registerValue());
      break;
    }
    if (!Changed)
      return std::unexpected(Changed.error());

    while (!UpdatedDefs.empty()) {
      const Register NewDef = UpdatedDefs.back();
      UpdatedDefs.pop_back();
      std::vector<MachineInstr *> Uses;
      for (MachineOperand &Use : MRI.useOperands(NewDef)) {
        if (Use.parent() != nullptr)
          Uses.push_back(Use.parent());
      }
      for (MachineInstr *Use : Uses) {
        switch (static_cast<GenericOpcode>(Use->opcode().value())) {
        case GenericOpcode::G_ANYEXT:
        case GenericOpcode::G_ZEXT:
        case GenericOpcode::G_SEXT:
        case GenericOpcode::G_UNMERGE_VALUES:
        case GenericOpcode::G_EXTRACT:
        case GenericOpcode::G_TRUNC:
        case GenericOpcode::G_BUILD_VECTOR:
          Observer.changedInstr(*Use);
          break;
        case GenericOpcode::G_ASSERT_SEXT:
        case GenericOpcode::G_ASSERT_ZEXT:
        case GenericOpcode::G_ASSERT_ALIGN:
          if (const auto Copy = Use->operandAt(0)->registerValue(); Copy && Copy->isVirtual())
            UpdatedDefs.push_back(*Copy);
          break;
        default:
          if (Use->opcode() == opcode(GenericOpcode::COPY)) {
            if (const auto Copy = Use->operandAt(0)->registerValue(); Copy && Copy->isVirtual())
              UpdatedDefs.push_back(*Copy);
          }
          break;
        }
      }
    }
    return *Changed;
  }

private:
  class ArtifactValueFinder final {
  public:
    explicit ArtifactValueFinder(LegalizationArtifactCombiner &Combiner) : Combiner(Combiner) {}

    [[nodiscard]] Expected<Register> findValueFromDef(Register DefReg, std::uint64_t StartBit,
                                                      std::uint64_t Size,
                                                      LowLevelType DestinationType);
    [[nodiscard]] Expected<bool> tryCombineUnmergeDefs(GUnmerge &Instruction,
                                                       GISelChangeObserver &Observer,
                                                       std::vector<Register> &UpdatedDefs);
    [[nodiscard]] Expected<GUnmerge *> findUnmergeThatDefinesReg(Register Reg, std::uint64_t Size,
                                                                 std::size_t &DefOperandIndex);
    [[nodiscard]] Expected<bool>
    isSequenceFromUnmerge(GMergeLikeInstr &Instruction, std::size_t MergeStartIndex,
                          GUnmerge *Unmerge, std::size_t UnmergeStartIndex,
                          std::size_t ElementCount, std::uint64_t ElementSize, bool AllowUndef);
    [[nodiscard]] Expected<bool> tryCombineMergeLike(GMergeLikeInstr &Instruction,
                                                     std::vector<MachineInstr *> &DeadInstructions,
                                                     std::vector<Register> &UpdatedDefs,
                                                     GISelChangeObserver &Observer);

  private:
    [[nodiscard]] Expected<Register> findValueFromDefImpl(Register DefReg, std::uint64_t StartBit,
                                                          std::uint64_t Size,
                                                          LowLevelType DestinationType);
    [[nodiscard]] Expected<Register> findValueFromConcat(GConcatVectors &Instruction,
                                                         std::uint64_t StartBit, std::uint64_t Size,
                                                         LowLevelType DestinationType);
    [[nodiscard]] Expected<Register>
    findValueFromBuildVector(GBuildVector &Instruction, std::uint64_t StartBit, std::uint64_t Size);
    [[nodiscard]] Expected<Register> findValueFromInsert(MachineInstr &Instruction,
                                                         std::uint64_t StartBit, std::uint64_t Size,
                                                         LowLevelType DestinationType);
    [[nodiscard]] Expected<Register> findValueFromExtension(MachineInstr &Instruction,
                                                            std::uint64_t StartBit,
                                                            std::uint64_t Size,
                                                            LowLevelType DestinationType);
    [[nodiscard]] Expected<Register> findValueFromTrunc(MachineInstr &Instruction,
                                                        std::uint64_t StartBit, std::uint64_t Size,
                                                        LowLevelType DestinationType);

    LegalizationArtifactCombiner &Combiner;
    Register CurrentBest;
    std::unordered_set<Register> Visited;
  };

  [[nodiscard]] Expected<bool> tryCombineAnyExt(MachineInstr &Instruction,
                                                std::vector<MachineInstr *> &DeadInstructions,
                                                std::vector<Register> &UpdatedDefs,
                                                GISelObserverWrapper &Observer);
  [[nodiscard]] Expected<bool> tryCombineZExt(MachineInstr &Instruction,
                                              std::vector<MachineInstr *> &DeadInstructions,
                                              std::vector<Register> &UpdatedDefs,
                                              GISelObserverWrapper &Observer);
  [[nodiscard]] Expected<bool> tryCombineSExt(MachineInstr &Instruction,
                                              std::vector<MachineInstr *> &DeadInstructions,
                                              std::vector<Register> &UpdatedDefs,
                                              GISelObserverWrapper &Observer);
  [[nodiscard]] Expected<bool> tryCombineTrunc(MachineInstr &Instruction,
                                               std::vector<MachineInstr *> &DeadInstructions,
                                               std::vector<Register> &UpdatedDefs,
                                               GISelObserverWrapper &Observer);
  [[nodiscard]] Expected<bool>
  tryCombineUnmergeValues(MachineInstr &Instruction, std::vector<MachineInstr *> &DeadInstructions,
                          std::vector<Register> &UpdatedDefs, GISelObserverWrapper &Observer);
  [[nodiscard]] Expected<bool> tryCombineExtract(MachineInstr &Instruction,
                                                 std::vector<MachineInstr *> &DeadInstructions,
                                                 std::vector<Register> &UpdatedDefs);
  [[nodiscard]] Expected<bool> tryCombineMergeLike(MachineInstr &Instruction,
                                                   std::vector<MachineInstr *> &DeadInstructions,
                                                   std::vector<Register> &UpdatedDefs,
                                                   GISelObserverWrapper &Observer);
  [[nodiscard]] Expected<bool> tryFoldImplicitDef(MachineInstr &Instruction,
                                                  std::vector<MachineInstr *> &DeadInstructions,
                                                  std::vector<Register> &UpdatedDefs,
                                                  GISelObserverWrapper &Observer);

  [[nodiscard]] Expected<MachineInstr *> buildUnary(GenericOpcode InstructionOpcode,
                                                    Register Destination, Register Source);
  [[nodiscard]] Expected<MachineInstr *> buildMergeLike(Register Destination,
                                                        const std::vector<Register> &Sources);
  [[nodiscard]] Expected<void> buildConstant(Register Destination, APInt Value);
  [[nodiscard]] Expected<Register> buildConstant(LowLevelType Type, APInt Value);
  [[nodiscard]] Expected<bool> isConstantUnsupported(LowLevelType Type) const;
  [[nodiscard]] static std::optional<std::int64_t> immediateOperand(const MachineInstr &Instruction,
                                                                    std::size_t Index);
  [[nodiscard]] static std::size_t getDefIndex(const MachineInstr &Instruction, Register SearchDef);
  [[nodiscard]] Expected<std::pair<MachineInstr *, Register>>
  definitionIgnoringCopies(Register Reg) const;
  [[nodiscard]] Expected<std::vector<Register>> buildOrFindUnmerge(LowLevelType DestinationType,
                                                                   Register Source);
  [[nodiscard]] Expected<LowLevelType> divideType(LowLevelType Type, std::size_t Parts) const;
  [[nodiscard]] Expected<bool> tryFoldUnmergeCast(MachineInstr &Instruction, MachineInstr &Cast,
                                                  std::vector<MachineInstr *> &DeadInstructions,
                                                  std::vector<Register> &UpdatedDefs);
  [[nodiscard]] static bool canFoldMergeOpcode(Opcode MergeOpcode,
                                               std::optional<Opcode> ConvertOpcode,
                                               LowLevelType OperandType,
                                               LowLevelType DestinationType);

  [[nodiscard]] Expected<void> replaceRegOrBuildCopy(Register Destination, Register Source,
                                                     std::vector<Register> &UpdatedDefs,
                                                     GISelChangeObserver &Observer) {
    auto DestinationType = MRI.getType(Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());
    auto SourceType = MRI.getType(Source);
    if (!SourceType)
      return std::unexpected(SourceType.error());
    if (*DestinationType != *SourceType) {
      auto Copy = Builder.buildCopy(Destination, Source);
      if (!Copy)
        return std::unexpected(Copy.error());
      UpdatedDefs.push_back(Destination);
      return {};
    }

    std::vector<MachineInstr *> Uses;
    for (MachineOperand &Use : MRI.useOperands(Destination)) {
      if (Use.parent() == nullptr)
        continue;
      Uses.push_back(Use.parent());
      Observer.changingInstr(*Use.parent());
    }
    auto Replaced = MRI.replaceRegWith(Destination, Source);
    if (!Replaced)
      return std::unexpected(Replaced.error());
    UpdatedDefs.push_back(Source);
    for (MachineInstr *Use : Uses)
      Observer.changedInstr(*Use);
    return {};
  }

  [[nodiscard]] Register lookThroughCopyInstrs(Register Reg) const {
    while (MachineInstr *Definition = MRI.getVRegDef(Reg)) {
      if (Definition->opcode() != opcode(GenericOpcode::COPY) || Definition->operandCount() < 2)
        break;
      const auto Source = Definition->operandAt(1)->registerValue();
      if (!Source || !Source->isVirtual())
        break;
      Reg = *Source;
    }
    return Reg;
  }

  [[nodiscard]] static std::optional<Register> registerOperand(const MachineInstr &Instruction,
                                                               std::size_t Index) {
    const MachineOperand *Operand = Instruction.operandAt(Index);
    return Operand == nullptr ? std::nullopt : Operand->registerValue();
  }

  [[nodiscard]] static bool hasOpcode(const MachineInstr *Instruction,
                                      GenericOpcode ExpectedOpcode) noexcept {
    return Instruction != nullptr && Instruction->opcode() == opcode(ExpectedOpcode);
  }

  void markDefDead(MachineInstr &Instruction, MachineInstr &Definition,
                   std::vector<MachineInstr *> &DeadInstructions, std::size_t DefIndex = 0) {
    MachineInstr *Previous = &Instruction;
    while (Previous != &Definition) {
      const auto Source = artifactSourceRegister(*Previous);
      if (!Source)
        break;
      MachineInstr *SourceDefinition = MRI.getVRegDef(*Source);
      if (SourceDefinition == nullptr || !MRI.hasOneUse(*Source))
        break;
      if (SourceDefinition != &Definition)
        DeadInstructions.push_back(SourceDefinition);
      Previous = SourceDefinition;
    }
    if (Previous != &Definition)
      return;

    std::size_t CurrentDef = 0;
    for (const MachineOperand &Operand : Definition.operands()) {
      if (!Operand.isDef())
        continue;
      const auto Reg = Operand.registerValue();
      if (!Reg)
        return;
      if (CurrentDef == DefIndex) {
        if (!MRI.hasOneUse(*Reg))
          return;
      } else if (!MRI.useEmpty(*Reg)) {
        return;
      }
      ++CurrentDef;
    }
    DeadInstructions.push_back(&Definition);
  }

  void markInstAndDefDead(MachineInstr &Instruction, MachineInstr &Definition,
                          std::vector<MachineInstr *> &DeadInstructions, std::size_t DefIndex = 0) {
    DeadInstructions.push_back(&Instruction);
    markDefDead(Instruction, Definition, DeadInstructions, DefIndex);
  }

  [[nodiscard]] Expected<void> deleteMarkedDeadInsts(std::vector<MachineInstr *> &DeadInstructions,
                                                     GISelObserverWrapper &) {
    for (MachineInstr *Dead : DeadInstructions) {
      if (Dead == nullptr || Dead->parent() == nullptr)
        continue;
      auto Removed = Dead->parent()->removeInstruction(*Dead);
      if (!Removed)
        return std::unexpected(Removed.error());
    }
    DeadInstructions.clear();
    return {};
  }

  [[nodiscard]] static std::optional<Register>
  artifactSourceRegister(const MachineInstr &Instruction) {
    switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
    case GenericOpcode::G_TRUNC:
    case GenericOpcode::G_ZEXT:
    case GenericOpcode::G_ANYEXT:
    case GenericOpcode::G_SEXT:
    case GenericOpcode::G_EXTRACT:
    case GenericOpcode::G_ASSERT_SEXT:
    case GenericOpcode::G_ASSERT_ZEXT:
    case GenericOpcode::G_ASSERT_ALIGN:
      return registerOperand(Instruction, 1);
    case GenericOpcode::G_UNMERGE_VALUES:
      return registerOperand(Instruction, Instruction.operandCount() - 1);
    default:
      if (Instruction.opcode() == opcode(GenericOpcode::COPY))
        return registerOperand(Instruction, 1);
      return std::nullopt;
    }
  }

  [[nodiscard]] Expected<bool> isInstUnsupported(LegalizationQuery Query) const {
    return LI.isUnsupported(Query);
  }

  [[nodiscard]] Expected<bool> isInstLegal(LegalizationQuery Query) const {
    return LI.isLegal(Query);
  }
};

inline Expected<MachineInstr *>
LegalizationArtifactCombiner::buildUnary(GenericOpcode InstructionOpcode, Register Destination,
                                         Register Source) {
  auto Pending = Builder.buildInstr(opcode(InstructionOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Source); !Added)
    return std::unexpected(Added.error());
  return Pending->build();
}

inline Expected<MachineInstr *>
LegalizationArtifactCombiner::buildMergeLike(Register Destination,
                                             const std::vector<Register> &Sources) {
  if (Sources.empty())
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "merge-like instruction requires source registers");
  auto DestinationType = MRI.getType(Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  auto SourceType = MRI.getType(Sources.front());
  if (!SourceType)
    return std::unexpected(SourceType.error());

  GenericOpcode InstructionOpcode = GenericOpcode::G_MERGE_VALUES;
  if (DestinationType->isVector())
    InstructionOpcode =
        SourceType->isVector() ? GenericOpcode::G_CONCAT_VECTORS : GenericOpcode::G_BUILD_VECTOR;
  auto Pending = Builder.buildInstr(opcode(InstructionOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(Destination); !Added)
    return std::unexpected(Added.error());
  for (Register Source : Sources) {
    if (auto Added = Pending->addUse(Source); !Added)
      return std::unexpected(Added.error());
  }
  return Pending->build();
}

inline Expected<void> LegalizationArtifactCombiner::buildConstant(Register Destination,
                                                                  APInt Value) {
  auto Built = Builder.buildConstant(Destination, std::move(Value));
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

inline Expected<Register> LegalizationArtifactCombiner::buildConstant(LowLevelType Type,
                                                                      APInt Value) {
  auto Destination = MRI.createVirtualRegister(Type);
  if (!Destination)
    return std::unexpected(Destination.error());
  auto Built = buildConstant(*Destination, std::move(Value));
  if (!Built)
    return std::unexpected(Built.error());
  return *Destination;
}

inline Expected<bool> LegalizationArtifactCombiner::isConstantUnsupported(LowLevelType Type) const {
  if (!Type.isVector())
    return isInstUnsupported(LegalizationQuery(opcode(GenericOpcode::G_CONSTANT), {Type}));
  const auto ElementType = Type.scalarElementType();
  if (!ElementType)
    return true;
  auto ConstantUnsupported =
      isInstUnsupported(LegalizationQuery(opcode(GenericOpcode::G_CONSTANT), {*ElementType}));
  if (!ConstantUnsupported || *ConstantUnsupported)
    return ConstantUnsupported;
  return isInstUnsupported(
      LegalizationQuery(opcode(GenericOpcode::G_BUILD_VECTOR), {Type, *ElementType}));
}

inline std::optional<std::int64_t>
LegalizationArtifactCombiner::immediateOperand(const MachineInstr &Instruction, std::size_t Index) {
  const MachineOperand *Operand = Instruction.operandAt(Index);
  if (Operand == nullptr)
    return std::nullopt;
  if (const auto *Value = Operand->getIf<std::int64_t>())
    return *Value;
  return std::nullopt;
}

inline std::size_t LegalizationArtifactCombiner::getDefIndex(const MachineInstr &Instruction,
                                                             Register SearchDef) {
  std::size_t Index = 0;
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (!Operand.isDef())
      continue;
    if (Operand.registerValue() == SearchDef)
      return Index;
    ++Index;
  }
  return Index;
}

inline Expected<std::pair<MachineInstr *, Register>>
LegalizationArtifactCombiner::definitionIgnoringCopies(Register Reg) const {
  std::unordered_set<Register> Visited;
  while (Reg.isVirtual() && Visited.insert(Reg).second) {
    MachineInstr *Definition = MRI.getVRegDef(Reg);
    if (Definition == nullptr)
      return std::pair<MachineInstr *, Register>{nullptr, Reg};
    if (Definition->opcode() != opcode(GenericOpcode::COPY))
      return std::pair{Definition, Reg};
    const auto Source = registerOperand(*Definition, 1);
    if (!Source)
      return std::pair{Definition, Reg};
    Reg = *Source;
  }
  return std::pair<MachineInstr *, Register>{nullptr, Reg};
}

inline Expected<LowLevelType> LegalizationArtifactCombiner::divideType(LowLevelType Type,
                                                                       std::size_t Parts) const {
  if (Parts == 0 || Type.minimumBitWidth() % Parts != 0)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LowLevelType cannot be divided into equal parts");
  if (!Type.isVector())
    return LowLevelType::makeCategoryNeutralScalar(Type.scalarBitWidth() / Parts);
  if (Type.minimumElementCount() % Parts != 0)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "vector element count cannot be divided into equal parts");
  const auto ElementType = Type.scalarElementType();
  if (!ElementType)
    return makeUnexpected(ErrorCode::InvalidArgument, "vector has no scalar element type");
  const std::uint32_t Elements = Type.minimumElementCount() / static_cast<std::uint32_t>(Parts);
  if (Elements == 1)
    return *ElementType;
  return Type.isScalableVector() ? LowLevelType::makeScalableVector(Elements, *ElementType)
                                 : LowLevelType::makeFixedVector(Elements, *ElementType);
}

inline Expected<std::vector<Register>>
LegalizationArtifactCombiner::buildOrFindUnmerge(LowLevelType DestinationType, Register Source) {
  auto SourceType = MRI.getType(Source);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  if (DestinationType.minimumBitWidth() == 0 ||
      SourceType->minimumBitWidth() % DestinationType.minimumBitWidth() != 0)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "unmerge destination does not tile the source type");
  const std::size_t Count =
      static_cast<std::size_t>(SourceType->minimumBitWidth() / DestinationType.minimumBitWidth());

  for (MachineOperand &Use : MRI.useOperands(Source)) {
    MachineInstr *Candidate = Use.parent();
    if (!GUnmerge::classof(Candidate) || Candidate->operandCount() != Count + 1)
      continue;
    std::vector<Register> Existing;
    Existing.reserve(Count);
    bool Matches = true;
    for (std::size_t Index = 0; Index < Count; ++Index) {
      const auto Reg = registerOperand(*Candidate, Index);
      if (!Reg) {
        Matches = false;
        break;
      }
      auto Type = MRI.getType(*Reg);
      if (!Type || *Type != DestinationType) {
        Matches = false;
        break;
      }
      Existing.push_back(*Reg);
    }
    if (Matches)
      return Existing;
  }

  std::vector<Register> Destinations;
  Destinations.reserve(Count);
  for (std::size_t Index = 0; Index < Count; ++Index) {
    auto Destination = MRI.createVirtualRegister(DestinationType);
    if (!Destination)
      return std::unexpected(Destination.error());
    Destinations.push_back(*Destination);
  }
  auto Built = Builder.buildUnmergeValues(Destinations, Source);
  if (!Built)
    return std::unexpected(Built.error());
  return Destinations;
}

inline bool LegalizationArtifactCombiner::canFoldMergeOpcode(Opcode MergeOpcode,
                                                             std::optional<Opcode> ConvertOpcode,
                                                             LowLevelType OperandType,
                                                             LowLevelType DestinationType) {
  const auto Generic = static_cast<GenericOpcode>(MergeOpcode.value());
  if (Generic == GenericOpcode::G_BUILD_VECTOR || Generic == GenericOpcode::G_MERGE_VALUES) {
    if (!ConvertOpcode)
      return true;
    const auto ElementType = OperandType.scalarElementType();
    return !DestinationType.isVector() && OperandType.isVector() && ElementType &&
           DestinationType == *ElementType;
  }
  if (Generic != GenericOpcode::G_CONCAT_VECTORS)
    return false;
  if (!ConvertOpcode)
    return true;
  if (!DestinationType.isVector())
    return false;
  const auto ElementType = OperandType.scalarElementType();
  if (!ElementType)
    return false;
  if (*ConvertOpcode == opcode(GenericOpcode::G_TRUNC))
    return DestinationType.minimumBitWidth() <= ElementType->minimumBitWidth();
  return DestinationType.minimumBitWidth() >= ElementType->minimumBitWidth();
}

inline Expected<bool> LegalizationArtifactCombiner::tryCombineAnyExt(
    MachineInstr &Instruction, std::vector<MachineInstr *> &DeadInstructions,
    std::vector<Register> &UpdatedDefs, GISelObserverWrapper &Observer) {
  const auto Destination = registerOperand(Instruction, 0);
  const auto OriginalSource = registerOperand(Instruction, 1);
  if (!Destination || !OriginalSource)
    return makeUnexpected(ErrorCode::VerificationError, "G_ANYEXT has invalid register operands");
  const Register Source = lookThroughCopyInstrs(*OriginalSource);
  MachineInstr *SourceDefinition = MRI.getVRegDef(Source);
  if (SourceDefinition == nullptr)
    return false;
  if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  if (hasOpcode(SourceDefinition, GenericOpcode::G_TRUNC)) {
    const auto TruncSource = registerOperand(*SourceDefinition, 1);
    if (!TruncSource)
      return false;
    auto DestinationType = MRI.getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());
    auto TruncSourceType = MRI.getType(*TruncSource);
    if (!TruncSourceType)
      return std::unexpected(TruncSourceType.error());
    if (*DestinationType == *TruncSourceType) {
      auto Replaced = replaceRegOrBuildCopy(*Destination, *TruncSource, UpdatedDefs, Observer);
      if (!Replaced)
        return std::unexpected(Replaced.error());
    } else {
      auto Built = Builder.buildAnyExtOrTrunc(*Destination, *TruncSource);
      if (!Built)
        return std::unexpected(Built.error());
      UpdatedDefs.push_back(*Destination);
    }
    markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
    return true;
  }

  if (hasOpcode(SourceDefinition, GenericOpcode::G_ANYEXT) ||
      hasOpcode(SourceDefinition, GenericOpcode::G_SEXT) ||
      hasOpcode(SourceDefinition, GenericOpcode::G_ZEXT)) {
    const auto ExtensionSource = registerOperand(*SourceDefinition, 1);
    if (!ExtensionSource)
      return false;
    auto Built = buildUnary(static_cast<GenericOpcode>(SourceDefinition->opcode().value()),
                            *Destination, *ExtensionSource);
    if (!Built)
      return std::unexpected(Built.error());
    UpdatedDefs.push_back(*Destination);
    markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
    return true;
  }

  if (hasOpcode(SourceDefinition, GenericOpcode::G_CONSTANT)) {
    auto DestinationType = MRI.getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());
    auto Legal =
        isInstLegal(LegalizationQuery(opcode(GenericOpcode::G_CONSTANT), {*DestinationType}));
    if (!Legal)
      return std::unexpected(Legal.error());
    const auto *Immediate = SourceDefinition->operandAt(1)->getIf<IntegerImmediateOperand>();
    if (*Legal && Immediate != nullptr) {
      auto Extended = Immediate->Value.sext(DestinationType->scalarBitWidth());
      if (!Extended)
        return std::unexpected(Extended.error());
      auto Built = buildConstant(*Destination, std::move(*Extended));
      if (!Built)
        return std::unexpected(Built.error());
      UpdatedDefs.push_back(*Destination);
      markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
      return true;
    }
  }
  return tryFoldImplicitDef(Instruction, DeadInstructions, UpdatedDefs, Observer);
}

inline Expected<bool> LegalizationArtifactCombiner::tryCombineZExt(
    MachineInstr &Instruction, std::vector<MachineInstr *> &DeadInstructions,
    std::vector<Register> &UpdatedDefs, GISelObserverWrapper &Observer) {
  const auto Destination = registerOperand(Instruction, 0);
  const auto OriginalSource = registerOperand(Instruction, 1);
  if (!Destination || !OriginalSource)
    return makeUnexpected(ErrorCode::VerificationError, "G_ZEXT has invalid register operands");
  const Register Source = lookThroughCopyInstrs(*OriginalSource);
  MachineInstr *SourceDefinition = MRI.getVRegDef(Source);
  if (SourceDefinition == nullptr)
    return false;
  if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  if (hasOpcode(SourceDefinition, GenericOpcode::G_TRUNC) ||
      hasOpcode(SourceDefinition, GenericOpcode::G_SEXT)) {
    const auto NarrowSource = registerOperand(*SourceDefinition, 1);
    if (!NarrowSource)
      return false;
    auto DestinationType = MRI.getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());
    auto SourceType = MRI.getType(Source);
    if (!SourceType)
      return std::unexpected(SourceType.error());
    auto AndUnsupported =
        isInstUnsupported(LegalizationQuery(opcode(GenericOpcode::G_AND), {*DestinationType}));
    if (!AndUnsupported)
      return std::unexpected(AndUnsupported.error());
    auto ConstantUnsupported = isConstantUnsupported(*DestinationType);
    if (!ConstantUnsupported)
      return std::unexpected(ConstantUnsupported.error());
    if (*AndUnsupported || *ConstantUnsupported)
      return false;

    Register WideSource = *NarrowSource;
    auto WideSourceType = MRI.getType(WideSource);
    if (!WideSourceType)
      return std::unexpected(WideSourceType.error());
    if (*WideSourceType != *DestinationType) {
      auto Wide = MRI.createVirtualRegister(*DestinationType);
      if (!Wide)
        return std::unexpected(Wide.error());
      auto Built = hasOpcode(SourceDefinition, GenericOpcode::G_SEXT)
                       ? Builder.buildSExtOrTrunc(*Wide, WideSource)
                       : Builder.buildAnyExtOrTrunc(*Wide, WideSource);
      if (!Built)
        return std::unexpected(Built.error());
      WideSource = *Wide;
    }
    auto Mask =
        APInt::getLowBitsSet(DestinationType->scalarBitWidth(), SourceType->scalarBitWidth());
    if (!Mask)
      return std::unexpected(Mask.error());
    bool ElideAnd = false;
    if (VT != nullptr) {
      auto KnownZeroes = VT->getKnownZeroes(WideSource);
      if (!KnownZeroes)
        return std::unexpected(KnownZeroes.error());
      auto CoveredBits = KnownZeroes->bitwiseOr(*Mask);
      if (!CoveredBits)
        return std::unexpected(CoveredBits.error());
      ElideAnd = CoveredBits->isAllOnes();
    }
    if (ElideAnd) {
      auto Replaced = replaceRegOrBuildCopy(*Destination, WideSource, UpdatedDefs, Observer);
      if (!Replaced)
        return std::unexpected(Replaced.error());
    } else {
      auto MaskRegister = buildConstant(*DestinationType, std::move(*Mask));
      if (!MaskRegister)
        return std::unexpected(MaskRegister.error());
      auto Built = Builder.buildAnd(*Destination, WideSource, *MaskRegister);
      if (!Built)
        return std::unexpected(Built.error());
    }
    UpdatedDefs.push_back(*Destination);
    markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
    return true;
  }

  if (hasOpcode(SourceDefinition, GenericOpcode::G_ZEXT)) {
    const auto ExtensionSource = registerOperand(*SourceDefinition, 1);
    if (!ExtensionSource)
      return false;
    Observer.changingInstr(Instruction);
    auto Updated = Instruction.operandAt(1)->setRegister(*ExtensionSource);
    if (!Updated)
      return std::unexpected(Updated.error());
    Observer.changedInstr(Instruction);
    UpdatedDefs.push_back(*Destination);
    markDefDead(Instruction, *SourceDefinition, DeadInstructions);
    return true;
  }

  if (hasOpcode(SourceDefinition, GenericOpcode::G_CONSTANT)) {
    auto DestinationType = MRI.getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());
    auto Legal =
        isInstLegal(LegalizationQuery(opcode(GenericOpcode::G_CONSTANT), {*DestinationType}));
    if (!Legal)
      return std::unexpected(Legal.error());
    const auto *Immediate = SourceDefinition->operandAt(1)->getIf<IntegerImmediateOperand>();
    if (*Legal && Immediate != nullptr) {
      auto Extended = Immediate->Value.zext(DestinationType->scalarBitWidth());
      if (!Extended)
        return std::unexpected(Extended.error());
      auto Built = buildConstant(*Destination, std::move(*Extended));
      if (!Built)
        return std::unexpected(Built.error());
      UpdatedDefs.push_back(*Destination);
      markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
      return true;
    }
  }
  return tryFoldImplicitDef(Instruction, DeadInstructions, UpdatedDefs, Observer);
}

inline Expected<bool> LegalizationArtifactCombiner::tryCombineSExt(
    MachineInstr &Instruction, std::vector<MachineInstr *> &DeadInstructions,
    std::vector<Register> &UpdatedDefs, GISelObserverWrapper &Observer) {
  const auto Destination = registerOperand(Instruction, 0);
  const auto OriginalSource = registerOperand(Instruction, 1);
  if (!Destination || !OriginalSource)
    return makeUnexpected(ErrorCode::VerificationError, "G_SEXT has invalid register operands");
  const Register Source = lookThroughCopyInstrs(*OriginalSource);
  MachineInstr *SourceDefinition = MRI.getVRegDef(Source);
  if (SourceDefinition == nullptr)
    return false;
  if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  if (hasOpcode(SourceDefinition, GenericOpcode::G_TRUNC)) {
    const auto TruncSource = registerOperand(*SourceDefinition, 1);
    if (!TruncSource)
      return false;
    auto DestinationType = MRI.getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());
    auto SourceType = MRI.getType(Source);
    if (!SourceType)
      return std::unexpected(SourceType.error());
    auto Unsupported = isInstUnsupported(
        LegalizationQuery(opcode(GenericOpcode::G_SEXT_INREG), {*DestinationType}));
    if (!Unsupported)
      return std::unexpected(Unsupported.error());
    if (*Unsupported)
      return false;
    Register WideSource = *TruncSource;
    auto WideSourceType = MRI.getType(WideSource);
    if (!WideSourceType)
      return std::unexpected(WideSourceType.error());
    if (*WideSourceType != *DestinationType) {
      auto Wide = MRI.createVirtualRegister(*DestinationType);
      if (!Wide)
        return std::unexpected(Wide.error());
      auto Built = Builder.buildAnyExtOrTrunc(*Wide, WideSource);
      if (!Built)
        return std::unexpected(Built.error());
      WideSource = *Wide;
    }
    bool ElideSExtInReg = false;
    if (VT != nullptr) {
      auto SignBits = VT->computeNumSignBits(WideSource);
      if (!SignBits)
        return std::unexpected(SignBits.error());
      ElideSExtInReg = *SignBits > DestinationType->scalarBitWidth() - SourceType->scalarBitWidth();
    }
    if (ElideSExtInReg) {
      auto Replaced = replaceRegOrBuildCopy(*Destination, WideSource, UpdatedDefs, Observer);
      if (!Replaced)
        return std::unexpected(Replaced.error());
    } else {
      auto Built = Builder.buildSExtInReg(*Destination, WideSource, SourceType->scalarBitWidth());
      if (!Built)
        return std::unexpected(Built.error());
    }
    UpdatedDefs.push_back(*Destination);
    markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
    return true;
  }

  if (hasOpcode(SourceDefinition, GenericOpcode::G_ZEXT) ||
      hasOpcode(SourceDefinition, GenericOpcode::G_SEXT)) {
    const auto ExtensionSource = registerOperand(*SourceDefinition, 1);
    if (!ExtensionSource)
      return false;
    auto Built = buildUnary(static_cast<GenericOpcode>(SourceDefinition->opcode().value()),
                            *Destination, *ExtensionSource);
    if (!Built)
      return std::unexpected(Built.error());
    UpdatedDefs.push_back(*Destination);
    markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
    return true;
  }

  if (hasOpcode(SourceDefinition, GenericOpcode::G_CONSTANT)) {
    auto DestinationType = MRI.getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());
    auto Legal =
        isInstLegal(LegalizationQuery(opcode(GenericOpcode::G_CONSTANT), {*DestinationType}));
    if (!Legal)
      return std::unexpected(Legal.error());
    const auto *Immediate = SourceDefinition->operandAt(1)->getIf<IntegerImmediateOperand>();
    if (*Legal && Immediate != nullptr) {
      auto Extended = Immediate->Value.sext(DestinationType->scalarBitWidth());
      if (!Extended)
        return std::unexpected(Extended.error());
      auto Built = buildConstant(*Destination, std::move(*Extended));
      if (!Built)
        return std::unexpected(Built.error());
      UpdatedDefs.push_back(*Destination);
      markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
      return true;
    }
  }
  return tryFoldImplicitDef(Instruction, DeadInstructions, UpdatedDefs, Observer);
}

inline Expected<bool> LegalizationArtifactCombiner::tryFoldImplicitDef(
    MachineInstr &Instruction, std::vector<MachineInstr *> &DeadInstructions,
    std::vector<Register> &UpdatedDefs, GISelObserverWrapper &) {
  const auto Destination = registerOperand(Instruction, 0);
  const auto Source = registerOperand(Instruction, 1);
  if (!Destination || !Source)
    return false;
  MachineInstr *Definition = MRI.getVRegDef(lookThroughCopyInstrs(*Source));
  if (!hasOpcode(Definition, GenericOpcode::G_IMPLICIT_DEF))
    return false;
  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (Instruction.opcode() == opcode(GenericOpcode::G_ANYEXT)) {
    auto Legal =
        isInstLegal(LegalizationQuery(opcode(GenericOpcode::G_IMPLICIT_DEF), {*DestinationType}));
    if (!Legal)
      return std::unexpected(Legal.error());
    if (!*Legal)
      return false;
    auto Built = Builder.buildImplicitDef(*Destination);
    if (!Built)
      return std::unexpected(Built.error());
  } else {
    auto Unsupported = isConstantUnsupported(*DestinationType);
    if (!Unsupported)
      return std::unexpected(Unsupported.error());
    if (*Unsupported)
      return false;
    auto Built = Builder.buildConstant(*Destination, 0);
    if (!Built)
      return std::unexpected(Built.error());
  }
  UpdatedDefs.push_back(*Destination);
  markInstAndDefDead(Instruction, *Definition, DeadInstructions);
  return true;
}

inline Expected<bool> LegalizationArtifactCombiner::tryCombineTrunc(
    MachineInstr &Instruction, std::vector<MachineInstr *> &DeadInstructions,
    std::vector<Register> &UpdatedDefs, GISelObserverWrapper &Observer) {
  const auto Destination = registerOperand(Instruction, 0);
  const auto OriginalSource = registerOperand(Instruction, 1);
  if (!Destination || !OriginalSource)
    return makeUnexpected(ErrorCode::VerificationError, "G_TRUNC has invalid register operands");
  const Register Source = lookThroughCopyInstrs(*OriginalSource);
  MachineInstr *SourceDefinition = MRI.getVRegDef(Source);
  if (SourceDefinition == nullptr)
    return false;
  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  if (hasOpcode(SourceDefinition, GenericOpcode::G_CONSTANT)) {
    auto Legal =
        isInstLegal(LegalizationQuery(opcode(GenericOpcode::G_CONSTANT), {*DestinationType}));
    if (!Legal)
      return std::unexpected(Legal.error());
    const auto *Immediate = SourceDefinition->operandAt(1)->getIf<IntegerImmediateOperand>();
    if (*Legal && Immediate != nullptr) {
      auto Truncated = Immediate->Value.trunc(DestinationType->scalarBitWidth());
      if (!Truncated)
        return std::unexpected(Truncated.error());
      auto Built = buildConstant(*Destination, std::move(*Truncated));
      if (!Built)
        return std::unexpected(Built.error());
      UpdatedDefs.push_back(*Destination);
      markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
      return true;
    }
  }

  if (GMerge::classof(SourceDefinition)) {
    auto &Merge = castGenericMachineInstr<GMerge>(*SourceDefinition);
    if (Merge.getNumSources() == 0)
      return false;
    const Register FirstSource = Merge.getSourceReg(0);
    auto FirstSourceType = MRI.getType(FirstSource);
    if (!FirstSourceType)
      return std::unexpected(FirstSourceType.error());
    if (!DestinationType->isCategoryNeutralScalar() || !FirstSourceType->isCategoryNeutralScalar())
      return false;
    const std::uint64_t DestinationSize = DestinationType->minimumBitWidth();
    const std::uint64_t SourceSize = FirstSourceType->minimumBitWidth();
    if (DestinationSize < SourceSize) {
      auto Unsupported = isInstUnsupported(
          LegalizationQuery(opcode(GenericOpcode::G_TRUNC), {*DestinationType, *FirstSourceType}));
      if (!Unsupported)
        return std::unexpected(Unsupported.error());
      if (*Unsupported)
        return false;
      auto Built = Builder.buildTrunc(*Destination, FirstSource);
      if (!Built)
        return std::unexpected(Built.error());
      UpdatedDefs.push_back(*Destination);
    } else if (DestinationSize == SourceSize) {
      auto Replaced = replaceRegOrBuildCopy(*Destination, FirstSource, UpdatedDefs, Observer);
      if (!Replaced)
        return std::unexpected(Replaced.error());
    } else if (DestinationSize % SourceSize == 0) {
      const std::size_t SourceCount = static_cast<std::size_t>(DestinationSize / SourceSize);
      if (SourceCount >= Merge.getNumSources())
        return false;
      auto Unsupported = isInstUnsupported(LegalizationQuery(opcode(GenericOpcode::G_MERGE_VALUES),
                                                             {*DestinationType, *FirstSourceType}));
      if (!Unsupported)
        return std::unexpected(Unsupported.error());
      if (*Unsupported)
        return false;
      std::vector<Register> Sources;
      Sources.reserve(SourceCount);
      for (std::size_t Index = 0; Index < SourceCount; ++Index)
        Sources.push_back(Merge.getSourceReg(Index));
      auto Built = Builder.buildMergeValues(*Destination, Sources);
      if (!Built)
        return std::unexpected(Built.error());
      UpdatedDefs.push_back(*Destination);
    } else {
      return false;
    }
    markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
    return true;
  }

  if (hasOpcode(SourceDefinition, GenericOpcode::G_TRUNC)) {
    const auto TruncSource = registerOperand(*SourceDefinition, 1);
    if (!TruncSource)
      return false;
    auto Built = Builder.buildTrunc(*Destination, *TruncSource);
    if (!Built)
      return std::unexpected(Built.error());
    UpdatedDefs.push_back(*Destination);
    markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
    return true;
  }

  ArtifactValueFinder Finder(*this);
  auto Found = Finder.findValueFromDef(*Destination, 0, DestinationType->minimumBitWidth(),
                                       *DestinationType);
  if (!Found)
    return std::unexpected(Found.error());
  if (!Found->isNoRegister()) {
    auto FoundType = MRI.getType(*Found);
    if (!FoundType)
      return std::unexpected(FoundType.error());
    if (*FoundType == *DestinationType) {
      auto Replaced = replaceRegOrBuildCopy(*Destination, *Found, UpdatedDefs, Observer);
      if (!Replaced)
        return std::unexpected(Replaced.error());
      markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions);
      return true;
    }
  }
  return false;
}

inline Expected<bool> LegalizationArtifactCombiner::ArtifactValueFinder::tryCombineMergeLike(
    GMergeLikeInstr &Instruction, std::vector<MachineInstr *> &DeadInstructions,
    std::vector<Register> &UpdatedDefs, GISelChangeObserver &Observer) {
  if (Instruction.getNumSources() == 0)
    return false;
  const Register FirstElement = Instruction.getSourceReg(0);
  auto ElementType = Combiner.MRI.getType(FirstElement);
  if (!ElementType)
    return std::unexpected(ElementType.error());
  const std::uint64_t ElementSize = ElementType->minimumBitWidth();
  std::size_t FirstUnmergeIndex = 0;
  auto UnmergeResult = findUnmergeThatDefinesReg(FirstElement, ElementSize, FirstUnmergeIndex);
  if (!UnmergeResult)
    return std::unexpected(UnmergeResult.error());
  GUnmerge *Unmerge = *UnmergeResult;
  if (Unmerge == nullptr)
    return false;

  const Register Destination = Instruction.getReg(0);
  const Register UnmergeSource = Unmerge->getSourceReg();
  auto DestinationType = Combiner.MRI.getType(Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  auto UnmergeSourceType = Combiner.MRI.getType(UnmergeSource);
  if (!UnmergeSourceType)
    return std::unexpected(UnmergeSourceType.error());
  const std::size_t SourceCount = Instruction.getNumSources();

  if (*DestinationType == *UnmergeSourceType && FirstUnmergeIndex == 0) {
    auto Sequence = isSequenceFromUnmerge(Instruction, 0, Unmerge, 0, SourceCount, ElementSize,
                                          DestinationType->isVector());
    if (!Sequence)
      return std::unexpected(Sequence.error());
    if (!*Sequence)
      return false;
    auto Replaced =
        Combiner.replaceRegOrBuildCopy(Destination, UnmergeSource, UpdatedDefs, Observer);
    if (!Replaced)
      return std::unexpected(Replaced.error());
    DeadInstructions.push_back(&Instruction);
    return true;
  }

  const bool MatchingDomains =
      (!DestinationType->isVector() && !UnmergeSourceType->isVector()) ||
      (DestinationType->isVector() && UnmergeSourceType->isVector() &&
       DestinationType->scalarElementType() == UnmergeSourceType->scalarElementType());
  if (MatchingDomains && FirstUnmergeIndex % SourceCount == 0 &&
      UnmergeSourceType->minimumBitWidth() % DestinationType->minimumBitWidth() == 0) {
    auto Sequence = isSequenceFromUnmerge(Instruction, 0, Unmerge, FirstUnmergeIndex, SourceCount,
                                          ElementSize, false);
    if (!Sequence)
      return std::unexpected(Sequence.error());
    if (*Sequence) {
      auto NewUnmerge = Combiner.buildOrFindUnmerge(*DestinationType, UnmergeSource);
      if (!NewUnmerge)
        return std::unexpected(NewUnmerge.error());
      const std::size_t DestinationIndex = static_cast<std::size_t>(
          (FirstUnmergeIndex * ElementSize) / DestinationType->minimumBitWidth());
      if (DestinationIndex >= NewUnmerge->size())
        return false;
      auto Replaced = Combiner.replaceRegOrBuildCopy(Destination, (*NewUnmerge)[DestinationIndex],
                                                     UpdatedDefs, Observer);
      if (!Replaced)
        return std::unexpected(Replaced.error());
      DeadInstructions.push_back(&Instruction);
      return true;
    }
  }

  if (DestinationType->isVector() == UnmergeSourceType->isVector() &&
      DestinationType->minimumBitWidth() % UnmergeSourceType->minimumBitWidth() == 0) {
    const std::size_t UnmergeElementCount = Unmerge->getNumDefs();
    if (UnmergeElementCount == 0 || SourceCount % UnmergeElementCount != 0)
      return false;
    std::vector<Register> MergeSources;
    for (std::size_t Start = 0; Start < SourceCount; Start += UnmergeElementCount) {
      std::size_t SourceDefIndex = 0;
      auto SourceUnmerge =
          findUnmergeThatDefinesReg(Instruction.getSourceReg(Start), ElementSize, SourceDefIndex);
      if (!SourceUnmerge)
        return std::unexpected(SourceUnmerge.error());
      if (*SourceUnmerge == nullptr || (*SourceUnmerge)->getNumDefs() != UnmergeElementCount ||
          SourceDefIndex != 0)
        return false;
      auto Sequence = isSequenceFromUnmerge(Instruction, Start, *SourceUnmerge, 0,
                                            UnmergeElementCount, ElementSize, false);
      if (!Sequence)
        return std::unexpected(Sequence.error());
      if (!*Sequence)
        return false;
      MergeSources.push_back((*SourceUnmerge)->getSourceReg());
    }
    auto Built = Combiner.buildMergeLike(Destination, MergeSources);
    if (!Built)
      return std::unexpected(Built.error());
    UpdatedDefs.push_back(Destination);
    DeadInstructions.push_back(&Instruction);
    return true;
  }
  return false;
}

inline Expected<bool> LegalizationArtifactCombiner::tryCombineMergeLike(
    MachineInstr &Instruction, std::vector<MachineInstr *> &DeadInstructions,
    std::vector<Register> &UpdatedDefs, GISelObserverWrapper &Observer) {
  if (!GMergeLikeInstr::classof(&Instruction))
    return false;
  ArtifactValueFinder Finder(*this);
  return Finder.tryCombineMergeLike(castGenericMachineInstr<GMergeLikeInstr>(Instruction),
                                    DeadInstructions, UpdatedDefs, Observer);
}

inline Expected<bool>
LegalizationArtifactCombiner::tryFoldUnmergeCast(MachineInstr &Instruction, MachineInstr &Cast,
                                                 std::vector<MachineInstr *> &DeadInstructions,
                                                 std::vector<Register> &UpdatedDefs) {
  if (!isArtifactCast(Cast.opcode()) || Cast.opcode() != opcode(GenericOpcode::G_TRUNC))
    return false;
  const auto CastSource = registerOperand(Cast, 1);
  if (!CastSource)
    return false;
  auto CastSourceType = MRI.getType(*CastSource);
  if (!CastSourceType)
    return std::unexpected(CastSourceType.error());
  const std::size_t OriginalDefCount = Instruction.operandCount() - 1;
  if (OriginalDefCount == 0)
    return false;
  const auto FirstDestination = registerOperand(Instruction, 0);
  if (!FirstDestination)
    return false;
  auto DestinationType = MRI.getType(*FirstDestination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (DestinationType->isVector() || !CastSourceType->isCategoryNeutralScalar() ||
      CastSourceType->minimumBitWidth() % DestinationType->minimumBitWidth() != 0)
    return false;
  auto Unsupported = isInstUnsupported(LegalizationQuery(opcode(GenericOpcode::G_UNMERGE_VALUES),
                                                         {*DestinationType, *CastSourceType}));
  if (!Unsupported)
    return std::unexpected(Unsupported.error());
  if (*Unsupported)
    return false;
  const std::size_t NewDefCount = static_cast<std::size_t>(CastSourceType->minimumBitWidth() /
                                                           DestinationType->minimumBitWidth());
  std::vector<Register> Destinations;
  Destinations.reserve(NewDefCount);
  for (std::size_t Index = 0; Index < NewDefCount; ++Index) {
    if (Index < OriginalDefCount) {
      const auto Destination = registerOperand(Instruction, Index);
      if (!Destination)
        return false;
      Destinations.push_back(*Destination);
    } else {
      auto Destination = MRI.createVirtualRegister(*DestinationType);
      if (!Destination)
        return std::unexpected(Destination.error());
      Destinations.push_back(*Destination);
    }
  }
  if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto Built = Builder.buildUnmergeValues(Destinations, *CastSource);
  if (!Built)
    return std::unexpected(Built.error());
  UpdatedDefs.insert(UpdatedDefs.end(), Destinations.begin(),
                     Destinations.begin() + static_cast<std::ptrdiff_t>(OriginalDefCount));
  markInstAndDefDead(Instruction, Cast, DeadInstructions);
  return true;
}

inline Expected<bool> LegalizationArtifactCombiner::tryCombineUnmergeValues(
    MachineInstr &Instruction, std::vector<MachineInstr *> &DeadInstructions,
    std::vector<Register> &UpdatedDefs, GISelObserverWrapper &Observer) {
  if (!GUnmerge::classof(&Instruction))
    return false;
  auto &Unmerge = castGenericMachineInstr<GUnmerge>(Instruction);
  if (Unmerge.getNumDefs() == 0)
    return false;
  const Register Source = Unmerge.getSourceReg();
  auto Definition = definitionIgnoringCopies(Source);
  if (!Definition)
    return std::unexpected(Definition.error());
  MachineInstr *SourceDefinition = Definition->first;
  if (SourceDefinition == nullptr)
    return false;
  const std::size_t SourceDefIndex = getDefIndex(*SourceDefinition, Definition->second);
  auto OperandType = MRI.getType(Source);
  if (!OperandType)
    return std::unexpected(OperandType.error());
  auto DestinationType = MRI.getType(Unmerge.getReg(0));
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  ArtifactValueFinder Finder(*this);
  auto AllCombined = Finder.tryCombineUnmergeDefs(Unmerge, Observer, UpdatedDefs);
  if (!AllCombined)
    return std::unexpected(AllCombined.error());
  if (*AllCombined) {
    markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions, SourceDefIndex);
    return true;
  }

  if (GUnmerge::classof(SourceDefinition)) {
    auto &SourceUnmerge = castGenericMachineInstr<GUnmerge>(*SourceDefinition);
    const Register OriginalSource = SourceUnmerge.getSourceReg();
    auto OriginalSourceType = MRI.getType(OriginalSource);
    if (!OriginalSourceType)
      return std::unexpected(OriginalSourceType.error());
    auto NewUnmerge = buildOrFindUnmerge(*DestinationType, OriginalSource);
    if (!NewUnmerge)
      return std::unexpected(NewUnmerge.error());
    const std::size_t BaseIndex = SourceDefIndex * Unmerge.getNumDefs();
    if (BaseIndex + Unmerge.getNumDefs() > NewUnmerge->size())
      return false;
    for (std::size_t Index = 0; Index < Unmerge.getNumDefs(); ++Index) {
      auto Replaced = replaceRegOrBuildCopy(Unmerge.getReg(Index), (*NewUnmerge)[BaseIndex + Index],
                                            UpdatedDefs, Observer);
      if (!Replaced)
        return std::unexpected(Replaced.error());
    }
    markInstAndDefDead(Instruction, *SourceDefinition, DeadInstructions, SourceDefIndex);
    return true;
  }

  MachineInstr *Merge = SourceDefinition;
  std::optional<Opcode> ConvertOpcode;
  if (isArtifactCast(SourceDefinition->opcode())) {
    ConvertOpcode = SourceDefinition->opcode();
    const auto CastSource = registerOperand(*SourceDefinition, 1);
    if (!CastSource)
      return false;
    auto CastDefinition = definitionIgnoringCopies(*CastSource);
    if (!CastDefinition)
      return std::unexpected(CastDefinition.error());
    Merge = CastDefinition->first;
  }
  if (Merge == nullptr ||
      !canFoldMergeOpcode(Merge->opcode(), ConvertOpcode, *OperandType, *DestinationType))
    return tryFoldUnmergeCast(Instruction, *SourceDefinition, DeadInstructions, UpdatedDefs);

  const std::size_t MergeSourceCount = Merge->operandCount() - 1;
  const std::size_t DefCount = Unmerge.getNumDefs();
  if (MergeSourceCount < DefCount) {
    if (DefCount % MergeSourceCount != 0)
      return false;
    const std::size_t NewDefCount = DefCount / MergeSourceCount;
    for (std::size_t SourceIndex = 0; SourceIndex < MergeSourceCount; ++SourceIndex) {
      std::vector<Register> Destinations;
      Destinations.reserve(NewDefCount);
      for (std::size_t Index = 0; Index < NewDefCount; ++Index)
        Destinations.push_back(Unmerge.getReg(SourceIndex * NewDefCount + Index));
      Register SplitSource = *registerOperand(*Merge, SourceIndex + 1);
      if (ConvertOpcode) {
        auto ConvertedType = divideType(*OperandType, MergeSourceCount);
        if (!ConvertedType)
          return std::unexpected(ConvertedType.error());
        auto Converted = MRI.createVirtualRegister(*ConvertedType);
        if (!Converted)
          return std::unexpected(Converted.error());
        auto Built =
            buildUnary(static_cast<GenericOpcode>(ConvertOpcode->value()), *Converted, SplitSource);
        if (!Built)
          return std::unexpected(Built.error());
        SplitSource = *Converted;
      }
      auto Built = Builder.buildUnmergeValues(Destinations, SplitSource);
      if (!Built)
        return std::unexpected(Built.error());
      UpdatedDefs.insert(UpdatedDefs.end(), Destinations.begin(), Destinations.end());
    }
  } else if (MergeSourceCount > DefCount) {
    if (ConvertOpcode || MergeSourceCount % DefCount != 0)
      return false;
    const std::size_t SourcesPerDef = MergeSourceCount / DefCount;
    for (std::size_t DefIndex = 0; DefIndex < DefCount; ++DefIndex) {
      std::vector<Register> Sources;
      Sources.reserve(SourcesPerDef);
      for (std::size_t Index = 0; Index < SourcesPerDef; ++Index) {
        const auto SourceReg = registerOperand(*Merge, 1 + DefIndex * SourcesPerDef + Index);
        if (!SourceReg)
          return false;
        Sources.push_back(*SourceReg);
      }
      auto Built = buildMergeLike(Unmerge.getReg(DefIndex), Sources);
      if (!Built)
        return std::unexpected(Built.error());
      UpdatedDefs.push_back(Unmerge.getReg(DefIndex));
    }
  } else {
    const auto FirstMergeSource = registerOperand(*Merge, 1);
    if (!FirstMergeSource)
      return false;
    auto MergeSourceType = MRI.getType(*FirstMergeSource);
    if (!MergeSourceType)
      return std::unexpected(MergeSourceType.error());
    if (!ConvertOpcode && *DestinationType != *MergeSourceType) {
      if (DestinationType->isPointer())
        ConvertOpcode = opcode(GenericOpcode::G_INTTOPTR);
      else if (MergeSourceType->isPointer())
        ConvertOpcode = opcode(GenericOpcode::G_PTRTOINT);
      else
        ConvertOpcode = opcode(GenericOpcode::G_BITCAST);
    }
    for (std::size_t Index = 0; Index < DefCount; ++Index) {
      const auto MergeSource = registerOperand(*Merge, Index + 1);
      if (!MergeSource)
        return false;
      if (ConvertOpcode) {
        if (!MRI.useEmpty(Unmerge.getReg(Index))) {
          auto Built = buildUnary(static_cast<GenericOpcode>(ConvertOpcode->value()),
                                  Unmerge.getReg(Index), *MergeSource);
          if (!Built)
            return std::unexpected(Built.error());
          UpdatedDefs.push_back(Unmerge.getReg(Index));
        }
      } else {
        auto Replaced =
            replaceRegOrBuildCopy(Unmerge.getReg(Index), *MergeSource, UpdatedDefs, Observer);
        if (!Replaced)
          return std::unexpected(Replaced.error());
      }
    }
  }
  markInstAndDefDead(Instruction, *Merge, DeadInstructions);
  return true;
}

inline Expected<bool>
LegalizationArtifactCombiner::tryCombineExtract(MachineInstr &Instruction,
                                                std::vector<MachineInstr *> &DeadInstructions,
                                                std::vector<Register> &UpdatedDefs) {
  const auto Destination = registerOperand(Instruction, 0);
  const auto OriginalSource = registerOperand(Instruction, 1);
  const auto OffsetValue = immediateOperand(Instruction, 2);
  if (!Destination || !OriginalSource || !OffsetValue || *OffsetValue < 0)
    return false;
  const Register Source = lookThroughCopyInstrs(*OriginalSource);
  MachineInstr *Merge = MRI.getVRegDef(Source);
  if (Merge == nullptr)
    return false;
  if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (hasOpcode(Merge, GenericOpcode::G_IMPLICIT_DEF)) {
    auto Built = Builder.buildImplicitDef(*Destination);
    if (!Built)
      return std::unexpected(Built.error());
    UpdatedDefs.push_back(*Destination);
    markInstAndDefDead(Instruction, *Merge, DeadInstructions);
    return true;
  }
  if (!GMergeLikeInstr::classof(Merge))
    return false;
  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  auto SourceType = MRI.getType(Source);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  const std::size_t SourceCount = Merge->operandCount() - 1;
  if (SourceCount == 0)
    return false;
  const std::uint64_t MergeSourceSize = SourceType->minimumBitWidth() / SourceCount;
  const std::uint64_t Offset = static_cast<std::uint64_t>(*OffsetValue);
  const std::size_t FirstSource = static_cast<std::size_t>(Offset / MergeSourceSize);
  const std::size_t LastSource =
      static_cast<std::size_t>((Offset + DestinationType->minimumBitWidth() - 1) / MergeSourceSize);
  if (FirstSource != LastSource || FirstSource >= SourceCount)
    return false;
  const auto MergeSource = registerOperand(*Merge, FirstSource + 1);
  if (!MergeSource)
    return false;
  auto Built =
      Builder.buildExtract(*Destination, *MergeSource, Offset - FirstSource * MergeSourceSize);
  if (!Built)
    return std::unexpected(Built.error());
  UpdatedDefs.push_back(*Destination);
  markInstAndDefDead(Instruction, *Merge, DeadInstructions);
  return true;
}

inline Expected<Register> LegalizationArtifactCombiner::ArtifactValueFinder::findValueFromDef(
    Register DefReg, std::uint64_t StartBit, std::uint64_t Size, LowLevelType DestinationType) {
  CurrentBest = {};
  Visited.clear();
  auto Found = findValueFromDefImpl(DefReg, StartBit, Size, DestinationType);
  if (!Found)
    return std::unexpected(Found.error());
  return *Found == DefReg ? Register{} : *Found;
}

inline Expected<Register> LegalizationArtifactCombiner::ArtifactValueFinder::findValueFromConcat(
    GConcatVectors &Instruction, std::uint64_t StartBit, std::uint64_t Size,
    LowLevelType DestinationType) {
  if (Instruction.getNumSources() == 0)
    return CurrentBest;
  const Register FirstSource = Instruction.getSourceReg(0);
  auto FirstType = Combiner.MRI.getType(FirstSource);
  if (!FirstType)
    return std::unexpected(FirstType.error());
  const std::uint64_t SourceSize = FirstType->minimumBitWidth();
  if (SourceSize == 0)
    return CurrentBest;
  const std::size_t SourceIndex = static_cast<std::size_t>(StartBit / SourceSize);
  const std::uint64_t SourceOffset = StartBit % SourceSize;
  if (SourceIndex >= Instruction.getNumSources() || SourceOffset + Size > SourceSize)
    return CurrentBest;
  const Register Source = Instruction.getSourceReg(SourceIndex);
  if (SourceOffset == 0 && Size == SourceSize)
    CurrentBest = Source;
  return findValueFromDefImpl(Source, SourceOffset, Size, DestinationType);
}

inline Expected<Register>
LegalizationArtifactCombiner::ArtifactValueFinder::findValueFromBuildVector(
    GBuildVector &Instruction, std::uint64_t StartBit, std::uint64_t Size) {
  if (Instruction.getNumSources() == 0)
    return CurrentBest;
  const Register FirstSource = Instruction.getSourceReg(0);
  auto FirstType = Combiner.MRI.getType(FirstSource);
  if (!FirstType)
    return std::unexpected(FirstType.error());
  const std::uint64_t SourceSize = FirstType->minimumBitWidth();
  if (SourceSize == 0 || StartBit % SourceSize != 0 || Size < SourceSize || Size % SourceSize != 0)
    return CurrentBest;
  const std::size_t FirstIndex = static_cast<std::size_t>(StartBit / SourceSize);
  const std::size_t SourceCount = static_cast<std::size_t>(Size / SourceSize);
  if (FirstIndex + SourceCount > Instruction.getNumSources())
    return CurrentBest;
  if (SourceCount == 1)
    return Instruction.getSourceReg(FirstIndex);
  if (FirstIndex == 0 && SourceCount == Instruction.getNumSources())
    return Instruction.getReg(0);

  auto VectorType = LowLevelType::makeFixedVector(SourceCount, *FirstType);
  if (!VectorType)
    return std::unexpected(VectorType.error());
  auto Legal = Combiner.isInstLegal(
      LegalizationQuery(opcode(GenericOpcode::G_BUILD_VECTOR), {*VectorType, *FirstType}));
  if (!Legal)
    return std::unexpected(Legal.error());
  if (!*Legal)
    return CurrentBest;

  std::vector<Register> Sources;
  Sources.reserve(SourceCount);
  for (std::size_t Index = FirstIndex; Index < FirstIndex + SourceCount; ++Index)
    Sources.push_back(Instruction.getSourceReg(Index));
  auto Destination = Combiner.MRI.createVirtualRegister(*VectorType);
  if (!Destination)
    return std::unexpected(Destination.error());
  auto Built = Combiner.buildMergeLike(*Destination, Sources);
  if (!Built)
    return std::unexpected(Built.error());
  return *Destination;
}

inline Expected<Register> LegalizationArtifactCombiner::ArtifactValueFinder::findValueFromInsert(
    MachineInstr &Instruction, std::uint64_t StartBit, std::uint64_t Size,
    LowLevelType DestinationType) {
  const auto Container = registerOperand(Instruction, 1);
  const auto Inserted = registerOperand(Instruction, 2);
  const auto InsertOffset = immediateOperand(Instruction, 3);
  if (!Container || !Inserted || !InsertOffset || *InsertOffset < 0)
    return CurrentBest;
  auto InsertedType = Combiner.MRI.getType(*Inserted);
  if (!InsertedType)
    return std::unexpected(InsertedType.error());
  const std::uint64_t Offset = static_cast<std::uint64_t>(*InsertOffset);
  const std::uint64_t InsertedEnd = Offset + InsertedType->minimumBitWidth();
  const std::uint64_t End = StartBit + Size;
  if (End <= Offset || InsertedEnd <= StartBit)
    return findValueFromDefImpl(*Container, StartBit, Size, DestinationType);
  if (Offset <= StartBit && End <= InsertedEnd) {
    const std::uint64_t NewStart = StartBit - Offset;
    if (NewStart == 0 && Size == InsertedType->minimumBitWidth())
      CurrentBest = *Inserted;
    return findValueFromDefImpl(*Inserted, NewStart, Size, DestinationType);
  }
  return Register{};
}

inline Expected<Register> LegalizationArtifactCombiner::ArtifactValueFinder::findValueFromExtension(
    MachineInstr &Instruction, std::uint64_t StartBit, std::uint64_t Size,
    LowLevelType DestinationType) {
  const auto Source = registerOperand(Instruction, 1);
  if (!Source)
    return CurrentBest;
  auto SourceType = Combiner.MRI.getType(*Source);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  if (!SourceType->isCategoryNeutralScalar() || StartBit + Size > SourceType->minimumBitWidth())
    return CurrentBest;
  if (StartBit == 0 && SourceType->minimumBitWidth() == Size)
    CurrentBest = *Source;
  return findValueFromDefImpl(*Source, StartBit, Size, DestinationType);
}

inline Expected<Register> LegalizationArtifactCombiner::ArtifactValueFinder::findValueFromTrunc(
    MachineInstr &Instruction, std::uint64_t StartBit, std::uint64_t Size,
    LowLevelType DestinationType) {
  const auto Source = registerOperand(Instruction, 1);
  if (!Source)
    return CurrentBest;
  auto SourceType = Combiner.MRI.getType(*Source);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  if (!SourceType->isCategoryNeutralScalar())
    return CurrentBest;
  return findValueFromDefImpl(*Source, StartBit, Size, DestinationType);
}

inline Expected<Register> LegalizationArtifactCombiner::ArtifactValueFinder::findValueFromDefImpl(
    Register DefReg, std::uint64_t StartBit, std::uint64_t Size, LowLevelType DestinationType) {
  if (!DefReg.isVirtual() || !Visited.insert(DefReg).second)
    return CurrentBest;
  auto Definition = Combiner.definitionIgnoringCopies(DefReg);
  if (!Definition)
    return std::unexpected(Definition.error());
  MachineInstr *Instruction = Definition->first;
  DefReg = Definition->second;
  if (Instruction == nullptr)
    return CurrentBest;

  switch (static_cast<GenericOpcode>(Instruction->opcode().value())) {
  case GenericOpcode::G_CONCAT_VECTORS:
    return findValueFromConcat(castGenericMachineInstr<GConcatVectors>(*Instruction), StartBit,
                               Size, DestinationType);
  case GenericOpcode::G_UNMERGE_VALUES: {
    auto DefType = Combiner.MRI.getType(DefReg);
    if (!DefType)
      return std::unexpected(DefType.error());
    const std::size_t DefIndex = getDefIndex(*Instruction, DefReg);
    const auto Source = registerOperand(*Instruction, Instruction->operandCount() - 1);
    if (!Source)
      return CurrentBest;
    auto Found = findValueFromDefImpl(*Source, StartBit + DefIndex * DefType->minimumBitWidth(),
                                      Size, DestinationType);
    if (!Found)
      return std::unexpected(Found.error());
    if (!Found->isNoRegister())
      return *Found;
    if (StartBit == 0 && Size == DefType->minimumBitWidth())
      return DefReg;
    return CurrentBest;
  }
  case GenericOpcode::G_BUILD_VECTOR:
    return findValueFromBuildVector(castGenericMachineInstr<GBuildVector>(*Instruction), StartBit,
                                    Size);
  case GenericOpcode::G_INSERT:
    return findValueFromInsert(*Instruction, StartBit, Size, DestinationType);
  case GenericOpcode::G_TRUNC:
    return findValueFromTrunc(*Instruction, StartBit, Size, DestinationType);
  case GenericOpcode::G_SEXT:
  case GenericOpcode::G_ZEXT:
  case GenericOpcode::G_ANYEXT:
    return findValueFromExtension(*Instruction, StartBit, Size, DestinationType);
  case GenericOpcode::G_IMPLICIT_DEF: {
    auto Type = Combiner.MRI.getType(DefReg);
    if (!Type)
      return std::unexpected(Type.error());
    if (*Type == DestinationType)
      return DefReg;
    auto Undef = Combiner.MRI.createVirtualRegister(DestinationType);
    if (!Undef)
      return std::unexpected(Undef.error());
    auto Built = Combiner.Builder.buildImplicitDef(*Undef);
    if (!Built)
      return std::unexpected(Built.error());
    return *Undef;
  }
  default:
    return CurrentBest;
  }
}

inline Expected<bool> LegalizationArtifactCombiner::ArtifactValueFinder::tryCombineUnmergeDefs(
    GUnmerge &Instruction, GISelChangeObserver &Observer, std::vector<Register> &UpdatedDefs) {
  if (Instruction.getNumDefs() == 0)
    return false;
  auto DestinationType = Combiner.MRI.getType(Instruction.getReg(0));
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  bool AllDead = true;
  for (std::size_t Index = 0; Index < Instruction.getNumDefs(); ++Index) {
    const Register Def = Instruction.getReg(Index);
    if (Combiner.MRI.useEmpty(Def))
      continue;
    auto Found = findValueFromDef(Def, 0, DestinationType->minimumBitWidth(), *DestinationType);
    if (!Found)
      return std::unexpected(Found.error());
    if (Found->isNoRegister()) {
      AllDead = false;
      continue;
    }
    auto FoundType = Combiner.MRI.getType(*Found);
    if (!FoundType)
      return std::unexpected(FoundType.error());
    if (*FoundType != *DestinationType) {
      AllDead = false;
      continue;
    }
    auto Replaced = Combiner.replaceRegOrBuildCopy(Def, *Found, UpdatedDefs, Observer);
    if (!Replaced)
      return std::unexpected(Replaced.error());
    Observer.changingInstr(Instruction);
    auto Restored = Instruction.operandAt(Index)->setRegister(Def);
    if (!Restored)
      return std::unexpected(Restored.error());
    Observer.changedInstr(Instruction);
  }
  return AllDead;
}

inline Expected<GUnmerge *>
LegalizationArtifactCombiner::ArtifactValueFinder::findUnmergeThatDefinesReg(
    Register Reg, std::uint64_t Size, std::size_t &DefOperandIndex) {
  auto Type = Combiner.MRI.getType(Reg);
  if (!Type)
    return std::unexpected(Type.error());
  CurrentBest = {};
  Visited.clear();
  auto Found = findValueFromDefImpl(Reg, 0, Size, *Type);
  if (!Found)
    return std::unexpected(Found.error());
  if (Found->isNoRegister())
    return nullptr;
  auto Definition = Combiner.definitionIgnoringCopies(*Found);
  if (!Definition)
    return std::unexpected(Definition.error());
  if (!GUnmerge::classof(Definition->first))
    return nullptr;
  DefOperandIndex = getDefIndex(*Definition->first, Definition->second);
  return &castGenericMachineInstr<GUnmerge>(*Definition->first);
}

inline Expected<bool> LegalizationArtifactCombiner::ArtifactValueFinder::isSequenceFromUnmerge(
    GMergeLikeInstr &Instruction, std::size_t MergeStartIndex, GUnmerge *Unmerge,
    std::size_t UnmergeStartIndex, std::size_t ElementCount, std::uint64_t ElementSize,
    bool AllowUndef) {
  for (std::size_t Index = 0; Index < ElementCount; ++Index) {
    const Register Source = Instruction.getSourceReg(MergeStartIndex + Index);
    std::size_t SourceDefIndex = 0;
    auto SourceUnmerge = findUnmergeThatDefinesReg(Source, ElementSize, SourceDefIndex);
    if (!SourceUnmerge)
      return std::unexpected(SourceUnmerge.error());
    if (*SourceUnmerge == Unmerge) {
      if (SourceDefIndex != UnmergeStartIndex + Index)
        return false;
      continue;
    }
    MachineInstr *Definition = Combiner.MRI.getVRegDef(Source);
    if (!AllowUndef || !hasOpcode(Definition, GenericOpcode::G_IMPLICIT_DEF))
      return false;
  }
  return true;
}

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_LEGALIZATIONARTIFACTCOMBINER_H

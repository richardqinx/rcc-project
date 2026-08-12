// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineSSAExit.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

struct EdgeKey final {
  const MachineBasicBlock *From = nullptr;
  const MachineBasicBlock *To = nullptr;

  friend bool operator==(const EdgeKey &, const EdgeKey &) noexcept = default;
};

struct EdgeKeyHash final {
  [[nodiscard]] std::size_t operator()(const EdgeKey &Key) const noexcept {
    const std::size_t FromHash = std::hash<const MachineBasicBlock *>{}(Key.From);
    const std::size_t ToHash = std::hash<const MachineBasicBlock *>{}(Key.To);
    return FromHash ^ (ToHash << 1U);
  }
};

struct RegisterValue final {
  Register Reg;
  SubRegIndex SubReg;
  RegisterOperandFlags Flags;
  std::uint32_t TargetFlags = 0;
};

struct PhiIncoming final {
  RegisterValue Value;
  MachineBasicBlock *Predecessor = nullptr;
  bool IsUndef = false;
};

struct PhiPlan final {
  MachineInstr *Instruction = nullptr;
  RegisterValue Destination;
  Register FreshIncoming;
  std::vector<PhiIncoming> IncomingValues;
};

[[nodiscard]] Error prefixedError(const Error &Source, std::string Prefix) {
  Prefix.append(Source.message());
  return Error(Source.code(), std::move(Prefix));
}

[[nodiscard]] Expected<void> requireProperties(const MachineFunction &Function,
                                               const MachinePassContract &Contract) {
  const MachineFunctionProperties Missing = Contract.Requires.missingFrom(Function.properties());
  if (Missing.empty())
    return {};
  return makeUnexpected(ErrorCode::VerificationError,
                        Contract.Name + " requires properties: " + Missing.toString());
}

[[nodiscard]] bool isPhi(const MachineInstr &Instruction, const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  return Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Phi);
}

[[nodiscard]] bool isMeta(const MachineInstr &Instruction, const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  return Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Meta);
}

[[nodiscard]] MachineInstr *firstTerminator(MachineBasicBlock &Block,
                                            const TargetDescription &Target) {
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Terminator))
      return Instruction;
  }
  return nullptr;
}

[[nodiscard]] MachineInstr *firstCodeAfterPhiRegion(MachineBasicBlock &Block,
                                                    const TargetDescription &Target) {
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (!isPhi(*Instruction, Target) && !isMeta(*Instruction, Target))
      return Instruction;
  }
  return nullptr;
}

[[nodiscard]] RegisterOperandFlags generatedDefFlags(RegisterOperandFlags Original) noexcept {
  RegisterOperandFlags Result;
  if (Original.has(RegisterOperandFlag::Undef))
    Result.set(RegisterOperandFlag::Undef);
  return Result;
}

[[nodiscard]] Expected<void> copyDebugLocation(MachineInstrBuilder &Builder,
                                               const MachineInstr *Source) {
  if (Source == nullptr || !Source->debugLocation())
    return {};
  return Builder.setDebugLocation(*Source->debugLocation());
}

[[nodiscard]] Expected<Register> cloneRegisterConstraints(MachineFunction &Function,
                                                          Register Source) {
  if (!Source.isVirtual()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "SSA deconstruction requires a virtual PHI destination");
  }
  auto SourceInfo = Function.registerInfo().virtualRegisterInfo(Source);
  if (!SourceInfo)
    return std::unexpected(SourceInfo.error());
  const VirtualRegisterInfo Info = SourceInfo->get();

  auto Fresh = Function.registerInfo().createVirtualRegister();
  if (!Fresh)
    return std::unexpected(Fresh.error());
  if (Info.Type) {
    auto SetType = Function.registerInfo().setType(*Fresh, *Info.Type);
    if (!SetType)
      return std::unexpected(SetType.error());
  }
  if (Info.Bank) {
    auto SetBank = Function.registerInfo().setRegisterBank(*Fresh, *Info.Bank);
    if (!SetBank)
      return std::unexpected(SetBank.error());
  }
  if (Info.RegClass) {
    auto SetClass = Function.registerInfo().setRegisterClass(*Fresh, *Info.RegClass);
    if (!SetClass)
      return std::unexpected(SetClass.error());
  }
  for (const PhysicalRegister Hint : Info.AllocationHints) {
    auto AddedHint = Function.registerInfo().addAllocationHint(*Fresh, Hint);
    if (!AddedHint)
      return std::unexpected(AddedHint.error());
  }
  if (Info.TargetFlags != 0) {
    auto SetFlags = Function.registerInfo().setTargetRegisterFlags(*Fresh, Info.TargetFlags);
    if (!SetFlags)
      return std::unexpected(SetFlags.error());
  }
  return *Fresh;
}

[[nodiscard]] Expected<MachineInstr *>
buildCopy(MachineFunction &Function, const TargetDescription &Target,
          const TargetSSAInfo &TargetInfo, MachineBasicBlock &Block, MachineInstr *Before,
          RegisterValue Destination, RegisterValue Source, const MachineInstr *DebugSource) {
  auto Legality = TargetInfo.validateCopy(Destination.Reg, Destination.SubReg, Source.Reg,
                                          Source.SubReg, Function, Target);
  if (!Legality)
    return std::unexpected(prefixedError(Legality.error(), "illegal COPY: "));

  MachineIRBuilder Builder(Function, Target);
  auto Point =
      Before == nullptr ? Builder.setInsertPoint(Block) : Builder.setInsertPointBefore(*Before);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::COPY));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Destination.Reg, generatedDefFlags(Destination.Flags),
                             Destination.SubReg, Destination.TargetFlags);
  if (!Def)
    return std::unexpected(Def.error());
  auto Use = Pending->addUse(Source.Reg, {}, Source.SubReg, Source.TargetFlags);
  if (!Use)
    return std::unexpected(Use.error());
  auto Debug = copyDebugLocation(*Pending, DebugSource);
  if (!Debug)
    return std::unexpected(Debug.error());
  return Pending->build();
}

[[nodiscard]] Expected<MachineInstr *>
buildImplicitDef(MachineFunction &Function, const TargetDescription &Target,
                 MachineBasicBlock &Block, MachineInstr *Before, RegisterValue Destination,
                 const MachineInstr *DebugSource) {
  MachineIRBuilder Builder(Function, Target);
  auto Point =
      Before == nullptr ? Builder.setInsertPoint(Block) : Builder.setInsertPointBefore(*Before);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::IMPLICIT_DEF));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Destination.Reg, generatedDefFlags(Destination.Flags),
                             Destination.SubReg, Destination.TargetFlags);
  if (!Def)
    return std::unexpected(Def.error());
  auto Debug = copyDebugLocation(*Pending, DebugSource);
  if (!Debug)
    return std::unexpected(Debug.error());
  return Pending->build();
}

[[nodiscard]] bool isImplicitlyUndef(const MachineFunction &Function,
                                     const MachineOperand &Operand) {
  if (Operand.registerFlags().has(RegisterOperandFlag::Undef))
    return true;
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || Reg->isNoRegister())
    return true;
  if (!Reg->isVirtual())
    return false;
  const MachineOperand *Definition = Function.registerInfo().getUniqueVRegDef(*Reg);
  if (Definition == nullptr || Definition->parent() == nullptr)
    return false;
  if (Definition->subReg().value() != 0 && Definition->subReg() != Operand.subReg())
    return false;
  const Opcode DefinitionOpcode = Definition->parent()->opcode();
  return DefinitionOpcode == opcode(GenericOpcode::IMPLICIT_DEF) ||
         DefinitionOpcode == opcode(GenericOpcode::G_IMPLICIT_DEF) ||
         Definition->registerFlags().has(RegisterOperandFlag::Undef);
}

[[nodiscard]] Expected<void> validatePhiValue(const MachineFunction &Function,
                                              const TargetDescription &Target,
                                              const MachineOperand &Destination,
                                              const MachineOperand &Source) {
  const Register *DestinationReg = Destination.getIf<Register>();
  const Register *SourceReg = Source.getIf<Register>();
  if (DestinationReg == nullptr || SourceReg == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "PHI operands must name registers");
  if (SourceReg->isNoRegister())
    return {};

  if (DestinationReg->isVirtual() && SourceReg->isVirtual()) {
    auto DestinationInfo = Function.registerInfo().virtualRegisterInfo(*DestinationReg);
    if (!DestinationInfo)
      return std::unexpected(DestinationInfo.error());
    auto SourceInfo = Function.registerInfo().virtualRegisterInfo(*SourceReg);
    if (!SourceInfo)
      return std::unexpected(SourceInfo.error());
    if (DestinationInfo->get().Type && SourceInfo->get().Type &&
        DestinationInfo->get().Type != SourceInfo->get().Type) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI destination and incoming value types do not match");
    }
  }
  return Target.validateTieCompatibility(Destination, Source, Function.registerInfo());
}

[[nodiscard]] Expected<std::vector<PhiPlan>> collectPhiPlans(MachineBasicBlock &Block,
                                                             MachineFunction &Function,
                                                             const TargetDescription &Target) {
  std::vector<PhiPlan> Plans;
  bool SawCode = false;
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI elimination encountered an unknown opcode");
    }
    if (Instruction->opcode() == opcode(GenericOpcode::G_PHI)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI elimination requires G_PHI to be selected to PHI");
    }
    if (!Descriptor->Flags.has(InstructionFlag::Phi)) {
      SawCode |= !Descriptor->Flags.has(InstructionFlag::Meta);
      continue;
    }
    if (SawCode) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI instruction appears after the leading PHI/meta region");
    }
    if (Instruction->operandCount() < 3 || (Instruction->operandCount() - 1U) % 2U != 0) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI must contain a destination and incoming value/block pairs");
    }

    MachineOperand *DestinationOperand = Instruction->operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr || !Destination->isVirtual() || !DestinationOperand->isDef() ||
        DestinationOperand->isUse() || DestinationOperand->isImplicit()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI destination must be an explicit virtual-register definition");
    }

    PhiPlan Plan;
    Plan.Instruction = Instruction;
    Plan.Destination =
        RegisterValue{*Destination, DestinationOperand->subReg(),
                      DestinationOperand->registerFlags(), DestinationOperand->targetFlags()};
    std::unordered_set<MachineBasicBlock *> SeenPredecessors;
    for (std::size_t Index = 1; Index < Instruction->operandCount(); Index += 2U) {
      MachineOperand *SourceOperand = Instruction->operandAt(Index);
      MachineOperand *BlockOperand = Instruction->operandAt(Index + 1U);
      const Register *Source =
          SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
      const BasicBlockOperand *IncomingBlock =
          BlockOperand == nullptr ? nullptr : BlockOperand->getIf<BasicBlockOperand>();
      if (Source == nullptr || SourceOperand == nullptr || !SourceOperand->isUse() ||
          SourceOperand->isDef() || SourceOperand->isImplicit() || IncomingBlock == nullptr ||
          IncomingBlock->Block == nullptr || BlockOperand->isImplicit()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "PHI incoming operands must be explicit register/block pairs");
      }
      if (std::ranges::find(Block.predecessors(), IncomingBlock->Block) ==
          Block.predecessors().end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "PHI incoming block is not a CFG predecessor");
      }
      if (!SeenPredecessors.insert(IncomingBlock->Block).second) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "PHI contains duplicate incoming predecessors");
      }
      auto Compatibility = validatePhiValue(Function, Target, *DestinationOperand, *SourceOperand);
      if (!Compatibility)
        return std::unexpected(Compatibility.error());
      Plan.IncomingValues.push_back(
          PhiIncoming{RegisterValue{*Source, SourceOperand->subReg(),
                                    SourceOperand->registerFlags(), SourceOperand->targetFlags()},
                      IncomingBlock->Block, isImplicitlyUndef(Function, *SourceOperand)});
    }
    if (SeenPredecessors.size() != Block.predecessors().size()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI incoming blocks do not exactly cover CFG predecessors");
    }
    Plans.push_back(std::move(Plan));
  }
  return Plans;
}

[[nodiscard]] Expected<void> copyLiveIns(const MachineBasicBlock &Source,
                                         MachineBasicBlock &Destination) {
  for (const MachineBasicBlockLiveIn &LiveIn : Source.liveIns()) {
    auto Added = Destination.addLiveIn(LiveIn.PhysReg, LiveIn.Lanes);
    if (!Added)
      return std::unexpected(Added.error());
  }
  return {};
}

[[nodiscard]] Expected<void> ensurePhysicalLiveIn(MachineBasicBlock &Block, PhysicalRegister Reg,
                                                  const TargetDescription &Target) {
  const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(Reg);
  if (Description == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "PHI incoming value references an unknown physical register");
  }
  LaneMask Required;
  for (const RegisterUnitLane &Unit : Description->Units)
    Required = Required | Unit.Lanes;
  if (Required.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "PHI incoming physical register has no addressable lanes");
  }

  for (const MachineBasicBlockLiveIn &LiveIn : Block.liveIns()) {
    if (LiveIn.PhysReg != Reg)
      continue;
    if (LiveIn.Lanes.contains(Required))
      return {};
    const LaneMask Combined = LiveIn.Lanes | Required;
    auto Removed = Block.removeLiveIn(Reg);
    if (!Removed)
      return std::unexpected(Removed.error());
    return Block.addLiveIn(Reg, Combined);
  }
  return Block.addLiveIn(Reg, Required);
}

[[nodiscard]] Expected<MachineBasicBlock *>
placePhiCopiesOnEdge(MachineFunction &Function, const TargetDescription &Target,
                     const TargetSSAInfo &TargetInfo, MachineBasicBlock &From,
                     MachineBasicBlock &To,
                     std::unordered_map<EdgeKey, MachineBasicBlock *, EdgeKeyHash> &Cache,
                     MachinePassResult &PassResult) {
  if (From.successors().size() <= 1)
    return &From;

  const EdgeKey Key{&From, &To};
  if (const auto Existing = Cache.find(Key); Existing != Cache.end())
    return Existing->second;
  if (std::ranges::none_of(From.successors(), [&To](const MachineSuccessor &Successor) {
        return Successor.Block == &To;
      })) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "PHI incoming predecessor has no edge to its successor");
  }

  auto SourceFallthrough = TargetInfo.isFallthroughEdge(From, To, Function, Target);
  if (!SourceFallthrough)
    return std::unexpected(SourceFallthrough.error());

  MachineBasicBlock *LayoutPredecessor = To.previous();
  if (LayoutPredecessor != nullptr && LayoutPredecessor != &From) {
    auto OtherFallthrough = TargetInfo.isFallthroughEdge(*LayoutPredecessor, To, Function, Target);
    if (!OtherFallthrough)
      return std::unexpected(OtherFallthrough.error());
    if (*OtherFallthrough) {
      auto Branch = TargetInfo.insertUnconditionalBranch(*LayoutPredecessor, To, Function, Target);
      if (!Branch)
        return std::unexpected(Branch.error());
      PassResult.Changed = true;
      PassResult.Changes.add(MachinePassChange::Instructions);
    }
  }

  auto EdgeBlockResult = Function.createBasicBlock();
  if (!EdgeBlockResult)
    return std::unexpected(EdgeBlockResult.error());
  MachineBasicBlock *EdgeBlock = *EdgeBlockResult;
  auto Inserted = Function.insertBlockBefore(To, *EdgeBlock);
  if (!Inserted)
    return std::unexpected(Inserted.error());

  auto Rewritten = TargetInfo.rewriteBranchTarget(From, To, *EdgeBlock, Function, Target);
  if (!Rewritten)
    return std::unexpected(Rewritten.error());
  if (!*Rewritten && !*SourceFallthrough) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "PHI elimination cannot split an indirect or unmodeled CFG edge");
  }

  auto Split = Function.cfgUpdater().splitEdge(From, To, *EdgeBlock);
  if (!Split)
    return std::unexpected(Split.error());
  auto LiveIns = copyLiveIns(To, *EdgeBlock);
  if (!LiveIns)
    return std::unexpected(LiveIns.error());
  auto Branch = TargetInfo.insertUnconditionalBranch(*EdgeBlock, To, Function, Target);
  if (!Branch)
    return std::unexpected(Branch.error());

  Cache.emplace(Key, EdgeBlock);
  PassResult.Changed = true;
  PassResult.Changes.add(MachinePassChange::CFG);
  PassResult.Changes.add(MachinePassChange::BlockLayout);
  PassResult.Changes.add(MachinePassChange::Instructions);
  if (*Rewritten)
    PassResult.Changes.add(MachinePassChange::Operands);
  return EdgeBlock;
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

[[nodiscard]] Expected<SubRegIndex> subRegIndex(const MachineOperand &Operand,
                                                const TargetDescription &Target) {
  std::uint64_t Value = 0;
  if (const auto *Immediate = Operand.getIf<std::int64_t>(); Immediate != nullptr) {
    if (*Immediate <= 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "subregister pseudo requires a positive subregister index");
    Value = static_cast<std::uint64_t>(*Immediate);
  } else if (const auto *WideImmediate = Operand.getIf<IntegerImmediateOperand>();
             WideImmediate != nullptr) {
    if (WideImmediate->Value.isNegative() || WideImmediate->Value.bitWidth() > 64)
      return makeUnexpected(ErrorCode::VerificationError,
                            "subregister pseudo index does not fit an unsigned integer");
    Value = WideImmediate->Value.words().front();
  } else {
    return makeUnexpected(ErrorCode::VerificationError,
                          "subregister pseudo is missing its immediate index");
  }
  if (Value == 0 || Value > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "subregister pseudo index is outside the supported range");
  }
  const SubRegIndex Result = SubRegIndex::fromValue(static_cast<std::uint32_t>(Value));
  if (Target.registers().subRegIndex(Result) == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "subregister pseudo references an unknown target index");
  }
  return Result;
}

[[nodiscard]] Expected<SubRegIndex> composeSubReg(SubRegIndex Outer, SubRegIndex Inner,
                                                  const TargetDescription &Target) {
  if (Outer.value() == 0)
    return Inner;
  if (Inner.value() == 0)
    return Outer;
  const auto Composed = Target.registers().composeSubRegIndices(Outer, Inner);
  if (!Composed) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "target cannot compose the requested subregister indices");
  }
  return *Composed;
}

[[nodiscard]] Expected<void> removeInstruction(MachineInstr &Instruction) {
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "rewritten instruction is not linked");
  return Block->removeInstruction(Instruction);
}

[[nodiscard]] Expected<void> expandRegSequence(MachineInstr &Instruction, MachineFunction &Function,
                                               const TargetDescription &Target,
                                               const TargetSSAInfo &TargetInfo) {
  if (Instruction.operandCount() < 3 || (Instruction.operandCount() - 1U) % 2U != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "REG_SEQUENCE requires a destination and source/subregister pairs");
  }
  MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Destination == nullptr || Destination->isNoRegister() || !DestinationOperand->isDef()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "REG_SEQUENCE requires a register destination definition");
  }
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "REG_SEQUENCE is not linked to a block");

  RegisterValue DestinationValue{*Destination, DestinationOperand->subReg(),
                                 DestinationOperand->registerFlags(),
                                 DestinationOperand->targetFlags()};
  auto Initial =
      buildImplicitDef(Function, Target, *Block, &Instruction, DestinationValue, &Instruction);
  if (!Initial)
    return std::unexpected(Initial.error());

  for (std::size_t Index = 1; Index < Instruction.operandCount(); Index += 2U) {
    MachineOperand *SourceOperand = Instruction.operandAt(Index);
    MachineOperand *IndexOperand = Instruction.operandAt(Index + 1U);
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    if (Source == nullptr || SourceOperand == nullptr || !SourceOperand->isUse() ||
        IndexOperand == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "REG_SEQUENCE contains a malformed source/subregister pair");
    }
    auto InnerSubReg = subRegIndex(*IndexOperand, Target);
    if (!InnerSubReg)
      return std::unexpected(InnerSubReg.error());
    auto DestinationSubReg = composeSubReg(DestinationOperand->subReg(), *InnerSubReg, Target);
    if (!DestinationSubReg)
      return std::unexpected(DestinationSubReg.error());

    RegisterValue LaneDestination{
        *Destination, *DestinationSubReg, {}, DestinationOperand->targetFlags()};
    if (isImplicitlyUndef(Function, *SourceOperand)) {
      auto Undef =
          buildImplicitDef(Function, Target, *Block, &Instruction, LaneDestination, &Instruction);
      if (!Undef)
        return std::unexpected(Undef.error());
      continue;
    }
    RegisterValue SourceValue{*Source, SourceOperand->subReg(), SourceOperand->registerFlags(),
                              SourceOperand->targetFlags()};
    auto Copy = buildCopy(Function, Target, TargetInfo, *Block, &Instruction, LaneDestination,
                          SourceValue, &Instruction);
    if (!Copy)
      return std::unexpected(Copy.error());
  }
  return removeInstruction(Instruction);
}

[[nodiscard]] Expected<void> expandInsertSubreg(MachineInstr &Instruction,
                                                MachineFunction &Function,
                                                const TargetDescription &Target,
                                                const TargetSSAInfo &TargetInfo) {
  if (Instruction.operandCount() != 4) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "INSERT_SUBREG requires destination, base, inserted value, and index");
  }
  MachineOperand *DestinationOperand = Instruction.operandAt(0);
  MachineOperand *BaseOperand = Instruction.operandAt(1);
  MachineOperand *InsertedOperand = Instruction.operandAt(2);
  MachineOperand *IndexOperand = Instruction.operandAt(3);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Base = BaseOperand == nullptr ? nullptr : BaseOperand->getIf<Register>();
  const Register *Inserted =
      InsertedOperand == nullptr ? nullptr : InsertedOperand->getIf<Register>();
  if (Destination == nullptr || Base == nullptr || Inserted == nullptr ||
      Destination->isNoRegister() || DestinationOperand == nullptr ||
      !DestinationOperand->isDef() || BaseOperand == nullptr || !BaseOperand->isUse() ||
      InsertedOperand == nullptr || !InsertedOperand->isUse() || IndexOperand == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "INSERT_SUBREG contains malformed register operands");
  }
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "INSERT_SUBREG is not linked to a block");

  RegisterValue DestinationValue{*Destination, DestinationOperand->subReg(),
                                 DestinationOperand->registerFlags(),
                                 DestinationOperand->targetFlags()};
  if (isImplicitlyUndef(Function, *BaseOperand)) {
    auto Undef =
        buildImplicitDef(Function, Target, *Block, &Instruction, DestinationValue, &Instruction);
    if (!Undef)
      return std::unexpected(Undef.error());
  } else if (*Destination != *Base || DestinationOperand->subReg() != BaseOperand->subReg()) {
    RegisterValue BaseValue{*Base, BaseOperand->subReg(), BaseOperand->registerFlags(),
                            BaseOperand->targetFlags()};
    auto Copy = buildCopy(Function, Target, TargetInfo, *Block, &Instruction, DestinationValue,
                          BaseValue, &Instruction);
    if (!Copy)
      return std::unexpected(Copy.error());
  }

  auto InnerSubReg = subRegIndex(*IndexOperand, Target);
  if (!InnerSubReg)
    return std::unexpected(InnerSubReg.error());
  auto DestinationSubReg = composeSubReg(DestinationOperand->subReg(), *InnerSubReg, Target);
  if (!DestinationSubReg)
    return std::unexpected(DestinationSubReg.error());
  RegisterValue LaneDestination{
      *Destination, *DestinationSubReg, {}, DestinationOperand->targetFlags()};
  if (isImplicitlyUndef(Function, *InsertedOperand)) {
    auto Undef =
        buildImplicitDef(Function, Target, *Block, &Instruction, LaneDestination, &Instruction);
    if (!Undef)
      return std::unexpected(Undef.error());
  } else {
    RegisterValue InsertedValue{*Inserted, InsertedOperand->subReg(),
                                InsertedOperand->registerFlags(), InsertedOperand->targetFlags()};
    auto Copy = buildCopy(Function, Target, TargetInfo, *Block, &Instruction, LaneDestination,
                          InsertedValue, &Instruction);
    if (!Copy)
      return std::unexpected(Copy.error());
  }
  return removeInstruction(Instruction);
}

[[nodiscard]] Expected<void> swapRegisterUses(MachineOperand &Left, MachineOperand &Right) {
  const std::optional<Register> LeftReg = Left.registerValue();
  const std::optional<Register> RightReg = Right.registerValue();
  if (!LeftReg || !RightReg || !Left.isUse() || Left.isDef() || !Right.isUse() || Right.isDef()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "commutation requires two pure register uses");
  }
  const SubRegIndex LeftSubReg = Left.subReg();
  const SubRegIndex RightSubReg = Right.subReg();
  const RegisterOperandFlags LeftFlags = Left.registerFlags();
  const RegisterOperandFlags RightFlags = Right.registerFlags();
  const std::uint32_t LeftTargetFlags = Left.targetFlags();
  const std::uint32_t RightTargetFlags = Right.targetFlags();

  auto SetLeftReg = Left.setRegister(*RightReg);
  if (!SetLeftReg)
    return std::unexpected(SetLeftReg.error());
  auto SetRightReg = Right.setRegister(*LeftReg);
  if (!SetRightReg)
    return std::unexpected(SetRightReg.error());
  auto SetLeftSubReg = Left.setSubReg(RightSubReg);
  if (!SetLeftSubReg)
    return std::unexpected(SetLeftSubReg.error());
  auto SetRightSubReg = Right.setSubReg(LeftSubReg);
  if (!SetRightSubReg)
    return std::unexpected(SetRightSubReg.error());
  auto SetLeftFlags = Left.setRegisterFlags(RightFlags);
  if (!SetLeftFlags)
    return std::unexpected(SetLeftFlags.error());
  auto SetRightFlags = Right.setRegisterFlags(LeftFlags);
  if (!SetRightFlags)
    return std::unexpected(SetRightFlags.error());
  Left.setTargetFlags(RightTargetFlags);
  Right.setTargetFlags(LeftTargetFlags);
  return {};
}

[[nodiscard]] Expected<bool> tryCommute(MachineInstr &Instruction, std::size_t TiedUseIndex,
                                        Register Destination, const TargetDescription &Target,
                                        MachineFunction &Function) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr || !Descriptor->Flags.has(InstructionFlag::Commutable))
    return false;
  MachineOperand *TiedUse = Instruction.operandAt(TiedUseIndex);
  if (TiedUse == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "tied use disappeared during commutation");

  for (std::size_t Index = 0; Index < Instruction.operandCount(); ++Index) {
    if (Index == TiedUseIndex)
      continue;
    MachineOperand *Candidate = Instruction.operandAt(Index);
    if (Candidate == nullptr || Candidate->isImplicit() || !Candidate->isUse() ||
        Candidate->isDef() || Candidate->tiedOperandIndex() ||
        Candidate->registerValue() != Destination || isImplicitlyUndef(Function, *Candidate)) {
      continue;
    }
    auto Swapped = swapRegisterUses(*TiedUse, *Candidate);
    if (!Swapped)
      return std::unexpected(Swapped.error());
    auto Validation = Target.validateInstruction(Instruction, Function.registerInfo());
    if (Validation)
      return true;
    auto Restored = swapRegisterUses(*TiedUse, *Candidate);
    if (!Restored)
      return std::unexpected(Restored.error());
  }
  return false;
}

[[nodiscard]] Expected<void> rewriteUndefTie(MachineOperand &Use, const MachineOperand &Def) {
  const std::optional<Register> Destination = Def.registerValue();
  if (!Destination)
    return makeUnexpected(ErrorCode::VerificationError, "tied definition is not a register");
  auto SetRegister = Use.setRegister(*Destination);
  if (!SetRegister)
    return std::unexpected(SetRegister.error());
  auto SetSubReg = Use.setSubReg(Def.subReg());
  if (!SetSubReg)
    return std::unexpected(SetSubReg.error());
  RegisterOperandFlags Flags = Use.registerFlags();
  Flags.set(RegisterOperandFlag::Undef);
  Flags.clear(RegisterOperandFlag::Kill);
  return Use.setRegisterFlags(Flags);
}

[[nodiscard]] Expected<void> verifyNoStageTenPseudos(const MachineFunction &Function,
                                                     const TargetDescription &Target) {
  for (const MachineInstr *Instruction : Function.allInstructions()) {
    if (Instruction->parent() == nullptr)
      continue;
    if (Instruction->opcode() == opcode(GenericOpcode::G_PHI) ||
        Instruction->opcode() == opcode(GenericOpcode::PHI) ||
        Instruction->opcode() == opcode(GenericOpcode::REG_SEQUENCE) ||
        Instruction->opcode() == opcode(GenericOpcode::INSERT_SUBREG)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "SSA exit left a Stage 10 pseudo instruction in MachineIR");
    }
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "SSA exit produced an instruction with no descriptor");
    for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
      const MachineOperand *Operand = Instruction->operandAt(Index);
      if (Operand == nullptr || !Operand->isUse())
        continue;
      const std::optional<std::size_t> Tie = Operand->tiedOperandIndex();
      if (!Tie)
        continue;
      const MachineOperand *Def = Instruction->operandAt(Tie.value());
      if (Def == nullptr || Def->registerValue() != Operand->registerValue()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "two-address rewrite left an unequal tied register");
      }
    }
  }
  return {};
}

} // namespace

const MachinePassContract &PHIEliminationPass::contract() const noexcept {
  static const MachinePassContract Contract{
      .Name = "phi-node-elimination",
      .Requires = {MachineFunctionProperty::IsSSA, MachineFunctionProperty::Selected,
                   MachineFunctionProperty::RegBankSelected},
      .Sets = {MachineFunctionProperty::NoPHIs},
      .Clears = {MachineFunctionProperty::IsSSA},
      .RequiredAnalyses = {},
      .VerificationStage = PipelineStage::SSAEliminated,
  };
  return Contract;
}

Expected<MachinePassResult> PHIEliminationPass::run(MachineFunction &Function,
                                                    const TargetDescription &Target,
                                                    MachineAnalysisManager &Analyses) {
  if (Info == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "PHI elimination has no target SSA behavior interface");
  }
  auto Required = requireProperties(Function, contract());
  if (!Required)
    return std::unexpected(Required.error());

  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        prefixedError(Trial.error(), "PHI-elimination transaction setup failed: "));
  }
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());

  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(prefixedError(
        Result.error(), "PHI elimination diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  return Result;
}

Expected<MachinePassResult> PHIEliminationPass::runImplementation(MachineFunction &Function,
                                                                  const TargetDescription &Target,
                                                                  MachineActionReplay &Actions) {
  MachinePassResult Result;
  std::unordered_map<EdgeKey, MachineBasicBlock *, EdgeKeyHash> SplitEdges;
  const std::vector<MachineBasicBlock *> Blocks(Function.allBlocks().begin(),
                                                Function.allBlocks().end());

  for (MachineBasicBlock *Block : Blocks) {
    auto Plans = collectPhiPlans(*Block, Function, Target);
    if (!Plans)
      return std::unexpected(Plans.error());
    if (Plans->empty())
      continue;

    for (PhiPlan &Plan : *Plans) {
      auto Accepted = Actions.request("eliminate-phi", Function, Plan.Instruction->stableID());
      if (!Accepted)
        return std::unexpected(Accepted.error());
      if (!*Accepted) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "required PHI elimination action was rejected");
      }
      auto Fresh = cloneRegisterConstraints(Function, Plan.Destination.Reg);
      if (!Fresh)
        return std::unexpected(Fresh.error());
      Plan.FreshIncoming = *Fresh;
    }

    for (PhiPlan &Plan : *Plans) {
      for (const PhiIncoming &Incoming : Plan.IncomingValues) {
        auto EdgeBlock = placePhiCopiesOnEdge(Function, Target, *Info, *Incoming.Predecessor,
                                              *Block, SplitEdges, Result);
        if (!EdgeBlock)
          return std::unexpected(EdgeBlock.error());
        if (*EdgeBlock != Incoming.Predecessor) {
          const std::optional<PhysicalRegister> Physical = Incoming.Value.Reg.physicalRegister();
          if (Physical) {
            auto LiveIn = ensurePhysicalLiveIn(**EdgeBlock, *Physical, Target);
            if (!LiveIn)
              return std::unexpected(LiveIn.error());
          }
        }
        MachineInstr *Terminator = firstTerminator(**EdgeBlock, Target);
        RegisterValue Fresh{Plan.FreshIncoming, Plan.Destination.SubReg, Plan.Destination.Flags,
                            Plan.Destination.TargetFlags};
        Expected<MachineInstr *> Transfer =
            Incoming.IsUndef ? buildImplicitDef(Function, Target, **EdgeBlock, Terminator, Fresh,
                                                Plan.Instruction)
                             : buildCopy(Function, Target, *Info, **EdgeBlock, Terminator, Fresh,
                                         Incoming.Value, Plan.Instruction);
        if (!Transfer)
          return std::unexpected(Transfer.error());
        Result.Changed = true;
        Result.Changes.add(MachinePassChange::Instructions);
      }
    }

    MachineInstr *InsertionPoint = firstCodeAfterPhiRegion(*Block, Target);
    for (PhiPlan &Plan : *Plans) {
      RegisterValue Fresh{
          Plan.FreshIncoming, Plan.Destination.SubReg, {}, Plan.Destination.TargetFlags};
      auto Copy = buildCopy(Function, Target, *Info, *Block, InsertionPoint, Plan.Destination,
                            Fresh, Plan.Instruction);
      if (!Copy)
        return std::unexpected(Copy.error());
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Instructions);
    }

    for (PhiPlan &Plan : *Plans) {
      auto Removed = Block->removeInstruction(*Plan.Instruction);
      if (!Removed)
        return std::unexpected(Removed.error());
    }
    Result.Changes.add(MachinePassChange::Registers);
  }

  if (Result.Changed && Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    auto Repaired = repairTrackedLiveness(Function, Target, Result);
    if (!Repaired)
      return std::unexpected(
          prefixedError(Repaired.error(), "PHI-elimination liveness repair failed: "));
  }
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

const MachinePassContract &TwoAddressRewritePass::contract() const noexcept {
  static const MachinePassContract Contract{
      .Name = "twoaddressinstruction",
      .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs},
      .Sets = {MachineFunctionProperty::TiedOpsRewritten},
      .Clears = {MachineFunctionProperty::IsSSA},
      .RequiredAnalyses = {},
      .VerificationStage = PipelineStage::TiedOpsRewritten,
  };
  return Contract;
}

Expected<MachinePassResult> TwoAddressRewritePass::run(MachineFunction &Function,
                                                       const TargetDescription &Target,
                                                       MachineAnalysisManager &Analyses) {
  if (Info == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "two-address rewrite has no target SSA behavior interface");
  }
  auto Required = requireProperties(Function, contract());
  if (!Required)
    return std::unexpected(Required.error());

  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(prefixedError(Trial.error(), "two-address transaction setup failed: "));
  }
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());

  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(prefixedError(
        Result.error(), "two-address rewrite diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  return Result;
}

Expected<MachinePassResult>
TwoAddressRewritePass::runImplementation(MachineFunction &Function, const TargetDescription &Target,
                                         MachineActionReplay &Actions) {
  MachinePassResult Result;
  const std::vector<MachineInstr *> Instructions(Function.allInstructions().begin(),
                                                 Function.allInstructions().end());
  for (MachineInstr *Instruction : Instructions) {
    if (Instruction->parent() == nullptr)
      continue;
    if (Instruction->opcode() == opcode(GenericOpcode::REG_SEQUENCE)) {
      auto Accepted = Actions.request("expand-reg-sequence", Function, Instruction->stableID());
      if (!Accepted)
        return std::unexpected(Accepted.error());
      if (!*Accepted)
        return makeUnexpected(ErrorCode::VerificationError,
                              "required REG_SEQUENCE expansion action was rejected");
      auto Expanded = expandRegSequence(*Instruction, Function, Target, *Info);
      if (!Expanded)
        return std::unexpected(Expanded.error());
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Instructions);
      continue;
    }
    if (Instruction->opcode() == opcode(GenericOpcode::INSERT_SUBREG)) {
      auto Accepted = Actions.request("expand-insert-subreg", Function, Instruction->stableID());
      if (!Accepted)
        return std::unexpected(Accepted.error());
      if (!*Accepted)
        return makeUnexpected(ErrorCode::VerificationError,
                              "required INSERT_SUBREG expansion action was rejected");
      auto Expanded = expandInsertSubreg(*Instruction, Function, Target, *Info);
      if (!Expanded)
        return std::unexpected(Expanded.error());
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Instructions);
      continue;
    }

    std::size_t RewriteAttempts = 0;
    bool Restart = true;
    while (Restart && Instruction->parent() != nullptr) {
      Restart = false;
      if (++RewriteAttempts > Instruction->operandCount() * 2U + 8U) {
        return makeUnexpected(ErrorCode::InternalError,
                              "two-address target conversion did not make progress");
      }
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "two-address rewrite encountered an unknown opcode");
      }

      for (std::size_t UseIndex = 0; UseIndex < Instruction->operandCount(); ++UseIndex) {
        MachineOperand *Use = Instruction->operandAt(UseIndex);
        if (Use == nullptr || !Use->isUse())
          continue;
        const std::optional<std::size_t> TiedDefIndex = Use->tiedOperandIndex();
        if (!TiedDefIndex)
          continue;
        const std::size_t DefIndex = TiedDefIndex.value();
        MachineOperand *Def = Instruction->operandAt(DefIndex);
        if (Def == nullptr || !Def->isDef()) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "tied use does not reference a definition");
        }
        const std::optional<Register> DefReg = Def->registerValue();
        const std::optional<Register> UseReg = Use->registerValue();
        if (!DefReg || !UseReg || DefReg->isNoRegister()) {
          return makeUnexpected(ErrorCode::Unsupported,
                                "two-address rewrite requires register ties");
        }
        if (*DefReg == *UseReg)
          continue;

        if (isImplicitlyUndef(Function, *Use)) {
          auto Accepted = Actions.request("rewrite-undef-tie", Function, Instruction->stableID());
          if (!Accepted)
            return std::unexpected(Accepted.error());
          if (!*Accepted)
            return makeUnexpected(ErrorCode::VerificationError,
                                  "required undef tie rewrite action was rejected");
          auto Rewritten = rewriteUndefTie(*Use, *Def);
          if (!Rewritten)
            return std::unexpected(Rewritten.error());
          Result.Changed = true;
          Result.Changes.add(MachinePassChange::Operands);
          continue;
        }

        auto AllowCommute =
            Actions.request("commute-tied-operands", Function, Instruction->stableID());
        if (!AllowCommute)
          return std::unexpected(AllowCommute.error());
        if (*AllowCommute) {
          auto Commuted = tryCommute(*Instruction, UseIndex, *DefReg, Target, Function);
          if (!Commuted)
            return std::unexpected(Commuted.error());
          if (*Commuted) {
            Result.Changed = true;
            Result.Changes.add(MachinePassChange::Operands);
            continue;
          }
        }

        auto AllowConversion =
            Actions.request("convert-three-address", Function, Instruction->stableID());
        if (!AllowConversion)
          return std::unexpected(AllowConversion.error());
        if (*AllowConversion) {
          auto Converted =
              Info->tryConvertToThreeAddress(*Instruction, DefIndex, UseIndex, Function, Target);
          if (!Converted)
            return std::unexpected(Converted.error());
          if (*Converted) {
            Result.Changed = true;
            Result.Changes.add(MachinePassChange::Instructions);
            Result.Changes.add(MachinePassChange::Operands);
            Result.Changes.add(MachinePassChange::Registers);
            Restart = true;
            break;
          }
        }

        auto AllowCopy = Actions.request("insert-tied-copy", Function, Instruction->stableID());
        if (!AllowCopy)
          return std::unexpected(AllowCopy.error());
        if (!*AllowCopy)
          return makeUnexpected(ErrorCode::VerificationError,
                                "required tied COPY insertion action was rejected");
        MachineBasicBlock *Block = Instruction->parent();
        if (Block == nullptr)
          return makeUnexpected(ErrorCode::InternalError,
                                "two-address instruction disappeared before COPY insertion");
        RegisterValue Destination{*DefReg, Def->subReg(), Def->registerFlags(), Def->targetFlags()};
        RegisterValue Source{*UseReg, Use->subReg(), Use->registerFlags(), Use->targetFlags()};
        auto Copy = buildCopy(Function, Target, *Info, *Block, Instruction, Destination, Source,
                              Instruction);
        if (!Copy)
          return std::unexpected(Copy.error());
        auto SetRegister = Use->setRegister(*DefReg);
        if (!SetRegister)
          return std::unexpected(SetRegister.error());
        auto SetSubReg = Use->setSubReg(Def->subReg());
        if (!SetSubReg)
          return std::unexpected(SetSubReg.error());
        RegisterOperandFlags Flags = Use->registerFlags();
        Flags.clear(RegisterOperandFlag::Kill);
        auto SetFlags = Use->setRegisterFlags(Flags);
        if (!SetFlags)
          return std::unexpected(SetFlags.error());
        Result.Changed = true;
        Result.Changes.add(MachinePassChange::Instructions);
        Result.Changes.add(MachinePassChange::Operands);
      }
    }
  }

  auto FinalState = verifyNoStageTenPseudos(Function, Target);
  if (!FinalState)
    return std::unexpected(FinalState.error());
  if (Result.Changed && Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    auto Repaired = repairTrackedLiveness(Function, Target, Result);
    if (!Repaired)
      return std::unexpected(
          prefixedError(Repaired.error(), "two-address liveness repair failed: "));
  }
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

} // namespace rcc

// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunctionCloner.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace rcc {
namespace {

struct CloneState final {
  const MachineFunction &Source;
  MachineFunction &Destination;
  MachineFunctionCloneResult &Result;
  std::unordered_map<const MachineBasicBlock *, MachineBasicBlock *> Blocks;
  std::unordered_map<const MachineInstr *, MachineInstr *> Instructions;
  std::unordered_map<const MachineMemOperand *, const MachineMemOperand *> MemOperands;
};

[[nodiscard]] Expected<MachineOperand> cloneOperand(const MachineOperand &SourceOperand,
                                                    CloneState &State) {
  switch (SourceOperand.kind()) {
  case MachineOperand::Kind::Register: {
    const Register *SourceReg = SourceOperand.getIf<Register>();
    if (SourceReg == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source register operand has inconsistent storage");
    }
    Register DestinationReg = *SourceReg;
    if (SourceReg->isVirtual()) {
      const auto Position = State.Result.VirtualRegisters.find(*SourceReg);
      if (Position == State.Result.VirtualRegisters.end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "source operand references an unmapped virtual register");
      }
      DestinationReg = Position->second;
    }

    MachineOperand Result = SourceOperand.cloneDetached();
    auto SetResult = Result.setRegister(DestinationReg);
    if (!SetResult)
      return std::unexpected(SetResult.error());
    return Result;
  }
  case MachineOperand::Kind::BasicBlock: {
    const auto *SourceBlock = SourceOperand.getIf<BasicBlockOperand>();
    if (SourceBlock == nullptr || SourceBlock->Block == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError, "source basic-block operand is invalid");
    }
    const auto Position = State.Blocks.find(SourceBlock->Block);
    if (Position == State.Blocks.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source basic-block operand is not in live layout");
    }
    return MachineOperand::basicBlock(*Position->second, SourceOperand.targetFlags());
  }
  case MachineOperand::Kind::BlockAddress: {
    const auto *Address = SourceOperand.getIf<BlockAddressOperand>();
    if (Address == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source block-address operand is invalid");
    }
    std::string FunctionName = Address->FunctionName;
    if (FunctionName == State.Source.name())
      FunctionName = std::string(State.Destination.name());
    return MachineOperand::blockAddress(std::move(FunctionName), Address->BlockName,
                                        Address->Offset, SourceOperand.targetFlags());
  }
  case MachineOperand::Kind::FrameIndex: {
    const auto *Frame = SourceOperand.getIf<FrameIndexOperand>();
    if (Frame == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError, "source frame-index operand is invalid");
    }
    const auto Position = State.Result.FrameIndices.find(Frame->Index);
    if (Position == State.Result.FrameIndices.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source operand references an unmapped frame index");
    }
    return MachineOperand::frameIndex(Position->second, Frame->Offset, SourceOperand.targetFlags());
  }
  case MachineOperand::Kind::ConstantPoolIndex: {
    const auto *Constant = SourceOperand.getIf<ConstantPoolOperand>();
    if (Constant == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source constant-pool operand is invalid");
    }
    State.Result.ConstantPoolIndices.emplace(Constant->Index, Constant->Index);
    return SourceOperand.cloneDetached();
  }
  case MachineOperand::Kind::JumpTableIndex: {
    const auto *JumpTable = SourceOperand.getIf<JumpTableOperand>();
    if (JumpTable == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError, "source jump-table operand is invalid");
    }
    State.Result.JumpTableIndices.emplace(JumpTable->Index, JumpTable->Index);
    return SourceOperand.cloneDetached();
  }
  case MachineOperand::Kind::Immediate:
  case MachineOperand::Kind::FloatingImmediate:
  case MachineOperand::Kind::GlobalSymbol:
  case MachineOperand::Kind::ExternalSymbol:
  case MachineOperand::Kind::RegisterMask:
  case MachineOperand::Kind::Metadata:
  case MachineOperand::Kind::TargetIndex:
  case MachineOperand::Kind::MCSymbol:
    return SourceOperand.cloneDetached();
  case MachineOperand::Kind::Count:
    return makeUnexpected(ErrorCode::VerificationError, "source operand has an invalid kind");
  }
  return makeUnexpected(ErrorCode::InternalError, "source operand kind is not handled");
}

void recordMemoryIndexMappings(const MachineMemOperandDesc &Description,
                               MachineFunctionCloneResult &Result) {
  if (const auto Constant = Description.Base.constantPoolIndex())
    Result.ConstantPoolIndices.emplace(*Constant, *Constant);
  if (const auto JumpTable = Description.Base.jumpTableIndex())
    Result.JumpTableIndices.emplace(*JumpTable, *JumpTable);
}

[[nodiscard]] Expected<const MachineMemOperand *>
cloneMemOperand(const MachineMemOperand &SourceOperand, CloneState &State) {
  const auto Existing = State.MemOperands.find(&SourceOperand);
  if (Existing != State.MemOperands.end())
    return Existing->second;

  MachineMemOperandDesc Description = SourceOperand.description();
  recordMemoryIndexMappings(Description, State.Result);
  if (const auto Frame = Description.Base.frameIndex()) {
    const auto Position = State.Result.FrameIndices.find(*Frame);
    if (Position == State.Result.FrameIndices.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source MMO references an unmapped frame index");
    }
    Description.Base = MemoryBase::frameIndex(Position->second);
  }
  auto DestinationOperand = State.Destination.createMemOperand(std::move(Description));
  if (!DestinationOperand)
    return std::unexpected(DestinationOperand.error());
  State.MemOperands.emplace(&SourceOperand, *DestinationOperand);
  return *DestinationOperand;
}

[[nodiscard]] Expected<void> cloneVirtualRegisters(CloneState &State) {
  const auto SourceRegisters = State.Source.registerInfo().virtualRegisters();
  for (std::size_t Index = 0; Index < SourceRegisters.size(); ++Index) {
    const VirtualRegisterInfo &SourceInfo = SourceRegisters[Index];
    if (SourceInfo.IsTombstone)
      continue;

    auto SourceReg = Register::makeVirtual(Index);
    if (!SourceReg)
      return std::unexpected(SourceReg.error());
    Expected<Register> DestinationReg =
        SourceInfo.Type ? State.Destination.registerInfo().createVirtualRegister(*SourceInfo.Type)
                        : State.Destination.registerInfo().createVirtualRegister();
    if (!DestinationReg)
      return std::unexpected(DestinationReg.error());

    if (SourceInfo.Bank) {
      auto SetResult =
          State.Destination.registerInfo().setRegisterBank(*DestinationReg, *SourceInfo.Bank);
      if (!SetResult)
        return std::unexpected(SetResult.error());
    }
    if (SourceInfo.RegClass) {
      auto SetResult =
          State.Destination.registerInfo().setRegisterClass(*DestinationReg, *SourceInfo.RegClass);
      if (!SetResult)
        return std::unexpected(SetResult.error());
    }
    for (const PhysicalRegister Hint : SourceInfo.AllocationHints) {
      auto AddResult = State.Destination.registerInfo().addAllocationHint(*DestinationReg, Hint);
      if (!AddResult)
        return std::unexpected(AddResult.error());
    }
    State.Result.VirtualRegisters.emplace(*SourceReg, *DestinationReg);
  }
  return {};
}

[[nodiscard]] Expected<void> cloneRegisterConfiguration(CloneState &State) {
  for (const PhysicalRegisterLiveIn &LiveIn : State.Source.registerInfo().liveIns()) {
    std::optional<Register> Initial;
    if (LiveIn.InitialVirtualRegister) {
      const auto Position = State.Result.VirtualRegisters.find(*LiveIn.InitialVirtualRegister);
      if (Position == State.Result.VirtualRegisters.end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "source live-in references an unmapped virtual register");
      }
      Initial = Position->second;
    }
    auto AddResult = State.Destination.registerInfo().addLiveIn(LiveIn.PhysReg, Initial);
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }
  for (const PhysicalRegister Reg : State.Source.registerInfo().reservedRegisters()) {
    auto ReserveResult = State.Destination.registerInfo().reservePhysicalRegister(Reg);
    if (!ReserveResult)
      return std::unexpected(ReserveResult.error());
  }
  return {};
}

[[nodiscard]] Expected<void> cloneFrameObjects(CloneState &State) {
  for (const StackObject &SourceObject : State.Source.frameInfo().objects()) {
    const StackObjectDesc *Description = SourceObject.description();
    if (Description == nullptr)
      continue;
    auto DestinationIndex = State.Destination.frameInfo().createObject(*Description);
    if (!DestinationIndex)
      return std::unexpected(DestinationIndex.error());
    State.Result.FrameIndices.emplace(SourceObject.index(), *DestinationIndex);
  }
  auto CallFrameResult = State.Destination.frameInfo().setMaxCallFrameSize(
      State.Source.frameInfo().maxCallFrameSize());
  if (!CallFrameResult)
    return std::unexpected(CallFrameResult.error());
  return {};
}

[[nodiscard]] Expected<void> cloneBlocks(CloneState &State) {
  for (const MachineBasicBlock *SourceBlock = State.Source.firstBlock(); SourceBlock != nullptr;
       SourceBlock = SourceBlock->next()) {
    auto DestinationBlock = State.Destination.createBasicBlock();
    if (!DestinationBlock)
      return std::unexpected(DestinationBlock.error());
    auto AppendResult = State.Destination.appendBlock(**DestinationBlock);
    if (!AppendResult)
      return std::unexpected(AppendResult.error());
    State.Blocks.emplace(SourceBlock, *DestinationBlock);
    State.Result.BlockStableIDs.emplace(SourceBlock->stableID(), (*DestinationBlock)->stableID());

    for (const MachineBasicBlockLiveIn &LiveIn : SourceBlock->liveIns()) {
      auto AddResult = (*DestinationBlock)->addLiveIn(LiveIn.PhysReg, LiveIn.Lanes);
      if (!AddResult)
        return std::unexpected(AddResult.error());
    }
  }
  return {};
}

[[nodiscard]] Expected<void> cloneInstructions(CloneState &State) {
  for (const MachineBasicBlock *SourceBlock = State.Source.firstBlock(); SourceBlock != nullptr;
       SourceBlock = SourceBlock->next()) {
    const auto DestinationBlockPosition = State.Blocks.find(SourceBlock);
    if (DestinationBlockPosition == State.Blocks.end()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "source block was not mapped before instruction cloning");
    }
    MachineBasicBlock *DestinationBlock = DestinationBlockPosition->second;
    for (const MachineInstr *SourceInstruction = SourceBlock->firstInstruction();
         SourceInstruction != nullptr; SourceInstruction = SourceInstruction->next()) {
      auto DestinationInstruction =
          State.Destination.createInstruction(SourceInstruction->opcode());
      if (!DestinationInstruction)
        return std::unexpected(DestinationInstruction.error());
      MachineInstr *NewInstruction = *DestinationInstruction;
      State.Instructions.emplace(SourceInstruction, NewInstruction);
      State.Result.InstructionStableIDs.emplace(SourceInstruction->stableID(),
                                                NewInstruction->stableID());
      NewInstruction->reserveOperands(SourceInstruction->operandCount());

      for (const MachineOperand &SourceOperand : SourceInstruction->operands()) {
        auto DestinationOperand = cloneOperand(SourceOperand, State);
        if (!DestinationOperand)
          return std::unexpected(DestinationOperand.error());
        auto AddResult = NewInstruction->addOperand(std::move(*DestinationOperand));
        if (!AddResult)
          return std::unexpected(AddResult.error());
      }

      const auto SourceOperands = SourceInstruction->operands();
      for (std::size_t Index = 0; Index < SourceOperands.size(); ++Index) {
        const auto Tied = SourceOperands[Index].tiedOperandIndex();
        if (!Tied || Index > *Tied)
          continue;
        const std::size_t DefIndex = SourceOperands[Index].isDef() ? Index : *Tied;
        const std::size_t UseIndex = SourceOperands[Index].isDef() ? *Tied : Index;
        auto TieResult = NewInstruction->tieOperands(DefIndex, UseIndex);
        if (!TieResult)
          return std::unexpected(TieResult.error());
      }

      for (const MachineMemOperand *SourceMemOperand : SourceInstruction->memOperands()) {
        auto DestinationMemOperand = cloneMemOperand(*SourceMemOperand, State);
        if (!DestinationMemOperand)
          return std::unexpected(DestinationMemOperand.error());
        auto AddResult = NewInstruction->addMemOperand(**DestinationMemOperand);
        if (!AddResult)
          return std::unexpected(AddResult.error());
      }

      NewInstruction->setFlags(SourceInstruction->flags());
      if (SourceInstruction->sourceLocation()) {
        NewInstruction->setSourceLocation(*SourceInstruction->sourceLocation());
      }
      if (SourceInstruction->origin())
        NewInstruction->setOrigin(*SourceInstruction->origin());

      auto AppendResult = DestinationBlock->appendInstruction(*NewInstruction);
      if (!AppendResult)
        return std::unexpected(AppendResult.error());
    }
  }
  return {};
}

[[nodiscard]] Expected<void> finalizeFrameClone(CloneState &State) {
  for (const StackObject &SourceObject : State.Source.frameInfo().objects()) {
    const StackObjectDesc *SourceDescription = SourceObject.description();
    if (SourceDescription == nullptr)
      continue;
    const auto FramePosition = State.Result.FrameIndices.find(SourceObject.index());
    if (FramePosition == State.Result.FrameIndices.end()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "source stack object was not mapped before frame finalization");
    }

    StackObjectDesc DestinationDescription = *SourceDescription;
    for (StackObjectLifetimeRange &Range : DestinationDescription.Lifetime) {
      const auto Begin = State.Result.InstructionStableIDs.find(Range.Begin);
      const auto End = State.Result.InstructionStableIDs.find(Range.End);
      if (Begin == State.Result.InstructionStableIDs.end() ||
          End == State.Result.InstructionStableIDs.end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "source stack lifetime references an unmapped instruction");
      }
      Range.Begin = Begin->second;
      Range.End = End->second;
    }
    auto UpdateResult = State.Destination.frameInfo().updateObject(
        FramePosition->second, std::move(DestinationDescription));
    if (!UpdateResult)
      return std::unexpected(UpdateResult.error());
  }

  if (State.Source.frameInfo().isFrozen()) {
    const auto StackSize = State.Source.frameInfo().staticStackSize();
    if (!StackSize) {
      return makeUnexpected(ErrorCode::InternalError,
                            "verified frozen source frame has no static stack size");
    }
    auto FreezeResult =
        State.Destination.frameInfo().freeze(*StackSize, State.Source.frameInfo().stackAlignment());
    if (!FreezeResult)
      return std::unexpected(FreezeResult.error());
  }
  return {};
}

[[nodiscard]] Expected<void> cloneCFG(CloneState &State) {
  MachineCFGUpdater Updater = State.Destination.cfgUpdater();
  for (const MachineBasicBlock *SourceBlock = State.Source.firstBlock(); SourceBlock != nullptr;
       SourceBlock = SourceBlock->next()) {
    const auto DestinationBlockPosition = State.Blocks.find(SourceBlock);
    if (DestinationBlockPosition == State.Blocks.end()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "source block was not mapped before CFG cloning");
    }
    MachineBasicBlock *DestinationBlock = DestinationBlockPosition->second;
    for (const MachineSuccessor &Successor : SourceBlock->successors()) {
      const auto Target = State.Blocks.find(Successor.Block);
      if (Target == State.Blocks.end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "source CFG references a detached block");
      }
      auto AddResult =
          Updater.addSuccessor(*DestinationBlock, *Target->second, Successor.Probability);
      if (!AddResult)
        return std::unexpected(AddResult.error());
    }
  }
  return {};
}

} // namespace

MachineFunctionCloneResult::MachineFunctionCloneResult() = default;
MachineFunctionCloneResult::MachineFunctionCloneResult(MachineFunctionCloneResult &&) noexcept =
    default;
MachineFunctionCloneResult &
MachineFunctionCloneResult::operator=(MachineFunctionCloneResult &&) noexcept = default;
MachineFunctionCloneResult::~MachineFunctionCloneResult() = default;

Expected<MachineFunctionCloneResult> cloneMachineFunction(const MachineFunction &Source,
                                                          const InstructionSet &Instructions,
                                                          MachineFunctionCloneOptions Options) {
  auto SourceVerification = MachineVerifier::verify(Source, Instructions);
  if (!SourceVerification)
    return std::unexpected(SourceVerification.error());
  if (!Options.NewStableID.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "clone function StableID must be valid");
  }

  std::string Name = Options.NewName ? std::move(*Options.NewName) : std::string(Source.name());
  auto Destination = MachineFunction::create(std::move(Name), Options.NewStableID);
  if (!Destination)
    return std::unexpected(Destination.error());

  MachineFunctionCloneResult Result;
  Result.ClonedFunction = std::move(*Destination);
  CloneState State{Source, *Result.ClonedFunction, Result, {}, {}, {}};

  auto VRegResult = cloneVirtualRegisters(State);
  if (!VRegResult)
    return std::unexpected(VRegResult.error());
  auto RegisterResult = cloneRegisterConfiguration(State);
  if (!RegisterResult)
    return std::unexpected(RegisterResult.error());
  auto FrameResult = cloneFrameObjects(State);
  if (!FrameResult)
    return std::unexpected(FrameResult.error());
  auto BlockResult = cloneBlocks(State);
  if (!BlockResult)
    return std::unexpected(BlockResult.error());
  auto InstructionResult = cloneInstructions(State);
  if (!InstructionResult)
    return std::unexpected(InstructionResult.error());
  auto FrameFinalizationResult = finalizeFrameClone(State);
  if (!FrameFinalizationResult)
    return std::unexpected(FrameFinalizationResult.error());
  auto CFGResult = cloneCFG(State);
  if (!CFGResult)
    return std::unexpected(CFGResult.error());

  Result.ClonedFunction->properties() = Source.properties();
  auto DestinationVerification = MachineVerifier::verify(*Result.ClonedFunction, Instructions);
  if (!DestinationVerification)
    return std::unexpected(DestinationVerification.error());
  return Result;
}

} // namespace rcc

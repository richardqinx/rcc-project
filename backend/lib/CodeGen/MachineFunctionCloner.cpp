// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunctionCloner.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineCompilationStatus.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetFunctionState.h"

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
    const auto Position = State.Result.ConstantPoolIndices.find(Constant->Index);
    if (Position == State.Result.ConstantPoolIndices.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source operand references an unmapped constant pool entry");
    }
    return MachineOperand::constantPool(Position->second, Constant->Offset,
                                        SourceOperand.targetFlags());
  }
  case MachineOperand::Kind::JumpTableIndex: {
    const auto *JumpTable = SourceOperand.getIf<JumpTableOperand>();
    if (JumpTable == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError, "source jump-table operand is invalid");
    }
    const auto Position = State.Result.JumpTableIndices.find(JumpTable->Index);
    if (Position == State.Result.JumpTableIndices.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source operand references an unmapped jump table");
    }
    return MachineOperand::jumpTable(Position->second, SourceOperand.targetFlags());
  }
  case MachineOperand::Kind::Immediate:
  case MachineOperand::Kind::IntegerImmediate:
  case MachineOperand::Kind::FloatingImmediate:
  case MachineOperand::Kind::GlobalSymbol:
  case MachineOperand::Kind::ExternalSymbol:
  case MachineOperand::Kind::RegisterMask:
  case MachineOperand::Kind::LiveOutRegisterMask:
  case MachineOperand::Kind::Metadata:
  case MachineOperand::Kind::TargetIndex:
  case MachineOperand::Kind::MCSymbol:
  case MachineOperand::Kind::CFIIndex:
  case MachineOperand::Kind::Intrinsic:
  case MachineOperand::Kind::Predicate:
  case MachineOperand::Kind::ShuffleMask:
  case MachineOperand::Kind::DebugInstrRef:
  case MachineOperand::Kind::LaneMask:
    return SourceOperand.cloneDetached();
  case MachineOperand::Kind::Count:
    return makeUnexpected(ErrorCode::VerificationError, "source operand has an invalid kind");
  }
  return makeUnexpected(ErrorCode::InternalError, "source operand kind is not handled");
}

[[nodiscard]] Expected<const MachineMemOperand *>
cloneMemOperand(const MachineMemOperand &SourceOperand, CloneState &State) {
  const auto Existing = State.MemOperands.find(&SourceOperand);
  if (Existing != State.MemOperands.end())
    return Existing->second;

  MachineMemOperandDesc Description = SourceOperand.description();
  if (const auto Frame = Description.Base.frameIndex()) {
    const auto Position = State.Result.FrameIndices.find(*Frame);
    if (Position == State.Result.FrameIndices.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source MMO references an unmapped frame index");
    }
    Description.Base = MemoryBase::frameIndex(Position->second);
  }
  if (const auto Constant = Description.Base.constantPoolIndex()) {
    const auto Position = State.Result.ConstantPoolIndices.find(*Constant);
    if (Position == State.Result.ConstantPoolIndices.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source MMO references an unmapped constant pool entry");
    }
    Description.Base = MemoryBase::constantPool(Position->second);
  }
  if (const auto JumpTable = Description.Base.jumpTableIndex()) {
    const auto Position = State.Result.JumpTableIndices.find(*JumpTable);
    if (Position == State.Result.JumpTableIndices.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source MMO references an unmapped jump table");
    }
    Description.Base = MemoryBase::jumpTable(Position->second);
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

    if (SourceInfo.Name) {
      auto DestinationInfo =
          State.Destination.registerInfo().virtualRegisterInfo(*DestinationReg);
      if (!DestinationInfo)
        return std::unexpected(DestinationInfo.error());
      DestinationInfo->get().Name = SourceInfo.Name;
    }

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
    if (SourceInfo.TargetFlags != 0) {
      auto SetResult = State.Destination.registerInfo().setTargetRegisterFlags(
          *DestinationReg, SourceInfo.TargetFlags);
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
  MachineFrameState FrameState = State.Source.frameInfo().state();
  FrameState.SavePoints.clear();
  FrameState.RestorePoints.clear();
  auto MapFrameReference = [&State](std::optional<FrameIndex> &Reference) -> Expected<void> {
    if (!Reference)
      return {};
    const auto Position = State.Result.FrameIndices.find(*Reference);
    if (Position == State.Result.FrameIndices.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "source frame state references an unmapped stack object");
    }
    Reference = Position->second;
    return {};
  };
  auto StackProtectorResult = MapFrameReference(FrameState.StackProtector);
  if (!StackProtectorResult)
    return StackProtectorResult;
  auto FunctionContextResult = MapFrameReference(FrameState.FunctionContext);
  if (!FunctionContextResult)
    return FunctionContextResult;
  auto StateResult = State.Destination.frameInfo().setState(std::move(FrameState));
  if (!StateResult)
    return std::unexpected(StateResult.error());
  return {};
}

[[nodiscard]] Expected<void> cloneConstantPool(CloneState &State) {
  for (const MachineConstantPoolEntry &SourceEntry : State.Source.constantPool().entries()) {
    if (SourceEntry.isTombstone())
      continue;
    auto Description = SourceEntry.cloneDescription();
    if (!Description)
      return std::unexpected(Description.error());
    if (auto *Address = std::get_if<BlockAddressConstant>(&Description->Value);
        Address != nullptr && Address->FunctionName == State.Source.name()) {
      Address->FunctionName = std::string(State.Destination.name());
    }

    auto DestinationIndex = State.Destination.constantPool().addEntry(std::move(*Description));
    if (!DestinationIndex)
      return std::unexpected(DestinationIndex.error());
    State.Result.ConstantPoolIndices.emplace(SourceEntry.index(), *DestinationIndex);
  }
  if (State.Source.constantPool().isFrozen()) {
    auto FreezeResult = State.Destination.constantPool().freeze();
    if (!FreezeResult)
      return std::unexpected(FreezeResult.error());
  }
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
    (*DestinationBlock)->attributes() = SourceBlock->attributes();
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

[[nodiscard]] Expected<void> cloneJumpTables(CloneState &State) {
  for (const MachineJumpTableEntry &SourceEntry : State.Source.jumpTableInfo().tables()) {
    const MachineJumpTableDesc *SourceDescription = SourceEntry.description();
    if (SourceDescription == nullptr)
      continue;
    MachineJumpTableDesc DestinationDescription = *SourceDescription;
    for (MachineBasicBlock *&Target : DestinationDescription.Targets) {
      const auto Position = State.Blocks.find(Target);
      if (Position == State.Blocks.end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "source jump table references an unmapped block");
      }
      Target = Position->second;
    }
    auto DestinationIndex =
        State.Destination.jumpTableInfo().createTable(std::move(DestinationDescription));
    if (!DestinationIndex)
      return std::unexpected(DestinationIndex.error());
    State.Result.JumpTableIndices.emplace(SourceEntry.index(), *DestinationIndex);
  }
  if (State.Source.jumpTableInfo().isFrozen()) {
    auto FreezeResult = State.Destination.jumpTableInfo().freeze();
    if (!FreezeResult)
      return std::unexpected(FreezeResult.error());
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
      if (SourceInstruction->debugLocation()) {
        auto DebugLocationResult =
            NewInstruction->setDebugLocation(*SourceInstruction->debugLocation());
        if (!DebugLocationResult)
          return std::unexpected(DebugLocationResult.error());
      }
      if (SourceInstruction->origin())
        NewInstruction->setOrigin(*SourceInstruction->origin());
      if (SourceInstruction->debugInstrNumber() != 0) {
        auto DebugNumberResult =
            NewInstruction->setDebugInstrNumber(SourceInstruction->debugInstrNumber());
        if (!DebugNumberResult)
          return std::unexpected(DebugNumberResult.error());
      }
      if (SourceInstruction->preInstrSymbol()) {
        auto SymbolResult =
            NewInstruction->setPreInstrSymbol(std::string(*SourceInstruction->preInstrSymbol()));
        if (!SymbolResult)
          return std::unexpected(SymbolResult.error());
      }
      if (SourceInstruction->postInstrSymbol()) {
        auto SymbolResult =
            NewInstruction->setPostInstrSymbol(std::string(*SourceInstruction->postInstrSymbol()));
        if (!SymbolResult)
          return std::unexpected(SymbolResult.error());
      }
      if (SourceInstruction->heapAllocMarker()) {
        auto MarkerResult =
            NewInstruction->setHeapAllocMarker(*SourceInstruction->heapAllocMarker());
        if (!MarkerResult)
          return std::unexpected(MarkerResult.error());
      }
      if (SourceInstruction->pcSections()) {
        auto SectionsResult = NewInstruction->setPCSections(*SourceInstruction->pcSections());
        if (!SectionsResult)
          return std::unexpected(SectionsResult.error());
      }
      if (SourceInstruction->mmra()) {
        auto MMRAResult = NewInstruction->setMMRA(*SourceInstruction->mmra());
        if (!MMRAResult)
          return std::unexpected(MMRAResult.error());
      }
      if (SourceInstruction->cfiType()) {
        auto TypeResult = NewInstruction->setCFIType(*SourceInstruction->cfiType());
        if (!TypeResult)
          return std::unexpected(TypeResult.error());
      }
      if (SourceInstruction->deactivationSymbol()) {
        auto SymbolResult = NewInstruction->setDeactivationSymbol(
            std::string(*SourceInstruction->deactivationSymbol()));
        if (!SymbolResult)
          return std::unexpected(SymbolResult.error());
      }

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

  MachineFrameState FrameState = State.Destination.frameInfo().state();
  auto ClonePoints =
      [&State](std::span<const MachineFrameSaveRestorePoint> SourcePoints,
               std::vector<MachineFrameSaveRestorePoint> &DestinationPoints) -> Expected<void> {
    DestinationPoints.clear();
    DestinationPoints.reserve(SourcePoints.size());
    for (const MachineFrameSaveRestorePoint &Point : SourcePoints) {
      const auto Position = State.Result.BlockStableIDs.find(Point.Block);
      if (Position == State.Result.BlockStableIDs.end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "source frame save/restore point references an unmapped block");
      }
      DestinationPoints.push_back({Position->second, Point.Registers});
    }
    return {};
  };
  auto SaveResult = ClonePoints(State.Source.frameInfo().state().SavePoints, FrameState.SavePoints);
  if (!SaveResult)
    return SaveResult;
  auto RestoreResult =
      ClonePoints(State.Source.frameInfo().state().RestorePoints, FrameState.RestorePoints);
  if (!RestoreResult)
    return RestoreResult;
  auto StateResult = State.Destination.frameInfo().setState(std::move(FrameState));
  if (!StateResult)
    return std::unexpected(StateResult.error());

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

Expected<MachineBasicBlock *>
MachineFunctionCloneContext::mapBlock(const MachineBasicBlock &SourceBlock) const {
  const auto Position = Blocks.find(&SourceBlock);
  if (Position == Blocks.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target function state references an unmapped basic block");
  }
  return Position->second;
}

Expected<MachineInstr *>
MachineFunctionCloneContext::mapInstruction(const MachineInstr &SourceInstruction) const {
  const auto Position = Instructions.find(&SourceInstruction);
  if (Position == Instructions.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target function state references an unmapped instruction");
  }
  return Position->second;
}

Expected<MachineBlockStableID>
MachineFunctionCloneContext::mapBlockStableID(MachineBlockStableID SourceID) const {
  const auto Position = Result.BlockStableIDs.find(SourceID);
  if (Position == Result.BlockStableIDs.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target function state references an unmapped block StableID");
  }
  return Position->second;
}

Expected<MachineInstrStableID>
MachineFunctionCloneContext::mapInstructionStableID(MachineInstrStableID SourceID) const {
  const auto Position = Result.InstructionStableIDs.find(SourceID);
  if (Position == Result.InstructionStableIDs.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "function state references an unmapped instruction StableID");
  }
  return Position->second;
}

Expected<Register> MachineFunctionCloneContext::mapRegister(Register SourceRegister) const {
  if (!SourceRegister.isVirtual())
    return SourceRegister;
  const auto Position = Result.VirtualRegisters.find(SourceRegister);
  if (Position == Result.VirtualRegisters.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target function state references an unmapped virtual register");
  }
  return Position->second;
}

Expected<FrameIndex> MachineFunctionCloneContext::mapFrameIndex(FrameIndex SourceIndex) const {
  const auto Position = Result.FrameIndices.find(SourceIndex);
  if (Position == Result.FrameIndices.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target function state references an unmapped frame index");
  }
  return Position->second;
}

Expected<ConstantPoolIndex>
MachineFunctionCloneContext::mapConstantPoolIndex(ConstantPoolIndex SourceIndex) const {
  const auto Position = Result.ConstantPoolIndices.find(SourceIndex);
  if (Position == Result.ConstantPoolIndices.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target function state references an unmapped constant pool entry");
  }
  return Position->second;
}

Expected<JumpTableIndex>
MachineFunctionCloneContext::mapJumpTableIndex(JumpTableIndex SourceIndex) const {
  const auto Position = Result.JumpTableIndices.find(SourceIndex);
  if (Position == Result.JumpTableIndices.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target function state references an unmapped jump table");
  }
  return Position->second;
}

MachineFunctionCloneResult::MachineFunctionCloneResult() = default;
MachineFunctionCloneResult::MachineFunctionCloneResult(MachineFunctionCloneResult &&) noexcept =
    default;
MachineFunctionCloneResult &
MachineFunctionCloneResult::operator=(MachineFunctionCloneResult &&) noexcept = default;
MachineFunctionCloneResult::~MachineFunctionCloneResult() = default;

Expected<MachineFunctionCloneResult> cloneMachineFunction(const MachineFunction &Source,
                                                          const TargetDescription &Target,
                                                          MachineFunctionCloneOptions Options) {
  auto SourceVerification = MachineVerifier::verify(Source, Target);
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
  auto ConstantPoolResult = cloneConstantPool(State);
  if (!ConstantPoolResult)
    return std::unexpected(ConstantPoolResult.error());
  auto BlockResult = cloneBlocks(State);
  if (!BlockResult)
    return std::unexpected(BlockResult.error());
  auto JumpTableResult = cloneJumpTables(State);
  if (!JumpTableResult)
    return std::unexpected(JumpTableResult.error());
  auto InstructionResult = cloneInstructions(State);
  if (!InstructionResult)
    return std::unexpected(InstructionResult.error());
  auto FrameFinalizationResult = finalizeFrameClone(State);
  if (!FrameFinalizationResult)
    return std::unexpected(FrameFinalizationResult.error());
  auto CFGResult = cloneCFG(State);
  if (!CFGResult)
    return std::unexpected(CFGResult.error());

  const MachineFunctionCloneContext CloneContext(Source, *Result.ClonedFunction, Result,
                                                 State.Blocks, State.Instructions);
  if (Source.targetState().hasValue()) {
    auto TargetState = Source.targetState().clone(CloneContext);
    if (!TargetState)
      return std::unexpected(TargetState.error());
    auto InstallResult = Result.ClonedFunction->setTargetState(std::move(*TargetState));
    if (!InstallResult)
      return std::unexpected(InstallResult.error());
  }

  auto CompilationStatus = Source.compilationStatus().clone(CloneContext);
  if (!CompilationStatus)
    return std::unexpected(CompilationStatus.error());
  Result.ClonedFunction->compilationStatus() = std::move(*CompilationStatus);
  auto Records = Source.records().clone(CloneContext);
  if (!Records)
    return std::unexpected(Records.error());
  Result.ClonedFunction->records() = std::move(*Records);
  auto Metadata = Source.metadata().clone();
  if (!Metadata)
    return std::unexpected(Metadata.error());
  Result.ClonedFunction->metadata() = std::move(*Metadata);
  auto CFIInfo = Source.cfiInfo().clone();
  if (!CFIInfo)
    return std::unexpected(CFIInfo.error());
  Result.ClonedFunction->cfiInfo() = std::move(*CFIInfo);
  Result.ClonedFunction->properties() = Source.properties();
  Result.ClonedFunction->attributes() = Source.attributes();
  auto DestinationVerification = MachineVerifier::verify(*Result.ClonedFunction, Target);
  if (!DestinationVerification)
    return std::unexpected(DestinationVerification.error());
  return Result;
}

} // namespace rcc

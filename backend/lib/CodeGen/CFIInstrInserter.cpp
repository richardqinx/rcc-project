// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/CFIInstrInserter.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetFrameLowering.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc {
namespace {

struct SavedLocation final {
  enum class Kind : std::uint8_t {
    Register,
    CFAOffset,
  };

  Kind LocationKind = Kind::Register;
  std::int64_t Value = 0;

  friend bool operator==(SavedLocation Left, SavedLocation Right) noexcept = default;
};

struct BlockCFIInfo final {
  MachineBasicBlock *Block = nullptr;
  std::int64_t IncomingCFAOffset = 0;
  std::int64_t OutgoingCFAOffset = 0;
  DwarfRegisterNumber IncomingCFARegister;
  DwarfRegisterNumber OutgoingCFARegister;
  std::set<std::uint32_t> IncomingSavedRegisters;
  std::set<std::uint32_t> OutgoingSavedRegisters;
  bool Processed = false;
};

using BlockInfoMap = std::unordered_map<MachineBasicBlock *, BlockCFIInfo>;
using SavedLocationMap = std::unordered_map<std::uint32_t, SavedLocation>;

[[noreturn]] void reportFatalCFIError(std::string_view Message) {
  std::fputs("LLVM ERROR: ", stderr);
  std::fwrite(Message.data(), sizeof(char), Message.size(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

[[nodiscard]] Expected<const MachineCFIInstruction *>
referencedCFI(const MachineInstr &Instruction, const MachineFunction &Function) {
  const MachineOperand *Operand = Instruction.operandAt(0);
  const auto *Index = Operand == nullptr ? nullptr : Operand->getIf<CFIIndexOperand>();
  if (Index == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError, "CFI instruction has no CFI index operand");
  }
  const MachineCFIInstruction *CFI = Function.cfiInfo().instruction(Index->Index);
  if (CFI == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "CFI instruction index is out of range");
  return CFI;
}

[[nodiscard]] Expected<void> recordSavedLocation(SavedLocationMap &Locations,
                                                 std::set<std::uint32_t> &SavedRegisters,
                                                 DwarfRegisterNumber Register,
                                                 SavedLocation Location) {
  const std::uint32_t Number = Register.value();
  const auto [Position, Inserted] = Locations.emplace(Number, Location);
  if (!Inserted && Position->second != Location)
    reportFatalCFIError("Different saved locations for the same CSR");
  SavedRegisters.insert(Number);
  return {};
}

[[nodiscard]] Expected<void> calculateOutgoingCFI(BlockCFIInfo &Info, MachineFunction &Function,
                                                  SavedLocationMap &Locations) {
  std::int64_t CFAOffset = Info.IncomingCFAOffset;
  DwarfRegisterNumber CFARegister = Info.IncomingCFARegister;
  std::set<std::uint32_t> SavedRegisters;
  std::set<std::uint32_t> RestoredRegisters;
  std::int64_t RememberStateDepth = 0;

  for (MachineInstr *Instruction = Info.Block->firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (Instruction->opcode() != opcode(GenericOpcode::CFI_INSTRUCTION))
      continue;
    auto CFIResult = referencedCFI(*Instruction, Function);
    if (!CFIResult)
      return std::unexpected(CFIResult.error());
    const MachineCFIInstruction &CFI = **CFIResult;
    switch (CFI.operation()) {
    case MachineCFIOperation::DefCFARegister: {
      const auto *Rule = CFI.getIf<MachineCFIRegisterRule>();
      if (Rule == nullptr)
        return makeUnexpected(ErrorCode::VerificationError, "invalid def_cfa_register rule");
      CFARegister = Rule->Register;
      break;
    }
    case MachineCFIOperation::DefCFAOffset: {
      const auto *Rule = CFI.getIf<MachineCFIOffsetRule>();
      if (Rule == nullptr)
        return makeUnexpected(ErrorCode::VerificationError, "invalid def_cfa_offset rule");
      CFAOffset = Rule->Offset;
      break;
    }
    case MachineCFIOperation::AdjustCFAOffset: {
      const auto *Rule = CFI.getIf<MachineCFIOffsetRule>();
      if (Rule == nullptr)
        return makeUnexpected(ErrorCode::VerificationError, "invalid adjust_cfa_offset rule");
      CFAOffset += Rule->Offset;
      break;
    }
    case MachineCFIOperation::DefCFA: {
      const auto *Rule = CFI.getIf<MachineCFIRegisterOffsetRule>();
      if (Rule == nullptr)
        return makeUnexpected(ErrorCode::VerificationError, "invalid def_cfa rule");
      CFARegister = Rule->Register;
      CFAOffset = Rule->Offset;
      break;
    }
    case MachineCFIOperation::Offset: {
      const auto *Rule = CFI.getIf<MachineCFIRegisterOffsetRule>();
      if (Rule == nullptr)
        return makeUnexpected(ErrorCode::VerificationError, "invalid CFI offset rule");
      auto Recorded =
          recordSavedLocation(Locations, SavedRegisters, Rule->Register,
                              SavedLocation{SavedLocation::Kind::CFAOffset, Rule->Offset});
      if (!Recorded)
        return std::unexpected(Recorded.error());
      break;
    }
    case MachineCFIOperation::Register: {
      const auto *Rule = CFI.getIf<MachineCFIRegisterRegisterRule>();
      if (Rule == nullptr)
        return makeUnexpected(ErrorCode::VerificationError, "invalid CFI register rule");
      auto Recorded = recordSavedLocation(
          Locations, SavedRegisters, Rule->Register,
          SavedLocation{SavedLocation::Kind::Register,
                        static_cast<std::int64_t>(Rule->SourceRegister.value())});
      if (!Recorded)
        return std::unexpected(Recorded.error());
      break;
    }
    case MachineCFIOperation::RelativeOffset: {
      const auto *Rule = CFI.getIf<MachineCFIRegisterOffsetRule>();
      if (Rule == nullptr)
        return makeUnexpected(ErrorCode::VerificationError, "invalid CFI rel_offset rule");
      auto Recorded = recordSavedLocation(
          Locations, SavedRegisters, Rule->Register,
          SavedLocation{SavedLocation::Kind::CFAOffset, Rule->Offset - CFAOffset});
      if (!Recorded)
        return std::unexpected(Recorded.error());
      break;
    }
    case MachineCFIOperation::Restore: {
      const auto *Rule = CFI.getIf<MachineCFIRegisterRule>();
      if (Rule == nullptr)
        return makeUnexpected(ErrorCode::VerificationError, "invalid CFI restore rule");
      RestoredRegisters.insert(Rule->Register.value());
      break;
    }
    case MachineCFIOperation::RememberState:
      ++RememberStateDepth;
      break;
    case MachineCFIOperation::RestoreState:
      --RememberStateDepth;
      break;
    case MachineCFIOperation::SameValue:
    case MachineCFIOperation::LLVMDefAddressSpaceCFA:
    case MachineCFIOperation::Escape:
    case MachineCFIOperation::Undefined:
    case MachineCFIOperation::WindowSave:
    case MachineCFIOperation::NegateReturnAddressState:
    case MachineCFIOperation::NegateReturnAddressStateWithPC:
    case MachineCFIOperation::GNUArgsSize:
    case MachineCFIOperation::Label:
    case MachineCFIOperation::ValueOffset:
    case MachineCFIOperation::LLVMRegisterPair:
    case MachineCFIOperation::LLVMVectorRegisters:
    case MachineCFIOperation::LLVMVectorOffset:
    case MachineCFIOperation::LLVMVectorRegisterMask:
      break;
    case MachineCFIOperation::Count:
      return makeUnexpected(ErrorCode::VerificationError, "invalid CFI operation");
    }
  }

  if (RememberStateDepth != 0) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "cfi_remember_state and cfi_restore_state must be paired in one block");
  }
  Info.Processed = true;
  Info.OutgoingCFAOffset = CFAOffset;
  Info.OutgoingCFARegister = CFARegister;
  Info.OutgoingSavedRegisters = Info.IncomingSavedRegisters;
  Info.OutgoingSavedRegisters.insert(SavedRegisters.begin(), SavedRegisters.end());
  for (const std::uint32_t Register : RestoredRegisters)
    Info.OutgoingSavedRegisters.erase(Register);
  return {};
}

[[nodiscard]] Expected<void> calculateCFI(BlockInfoMap &BlockInfos, SavedLocationMap &Locations,
                                          MachineFunction &Function) {
  MachineBasicBlock *Entry = Function.firstBlock();
  if (Entry == nullptr)
    return {};
  std::vector<MachineBasicBlock *> Stack{Entry};
  while (!Stack.empty()) {
    MachineBasicBlock *Block = Stack.back();
    Stack.pop_back();
    BlockCFIInfo &Info = BlockInfos.at(Block);
    auto Calculated = calculateOutgoingCFI(Info, Function, Locations);
    if (!Calculated)
      return std::unexpected(Calculated.error());
    for (const MachineSuccessor &Successor : Block->successors()) {
      BlockCFIInfo &SuccessorInfo = BlockInfos.at(Successor.Block);
      if (SuccessorInfo.Processed)
        continue;
      SuccessorInfo.IncomingCFAOffset = Info.OutgoingCFAOffset;
      SuccessorInfo.IncomingCFARegister = Info.OutgoingCFARegister;
      SuccessorInfo.IncomingSavedRegisters = Info.OutgoingSavedRegisters;
      Stack.push_back(Successor.Block);
    }
  }
  return {};
}

[[nodiscard]] Expected<void> emitCFI(MachineFunction &Function, MachineBasicBlock &Block,
                                     MachineInstr *Anchor, const TargetDescription &Target,
                                     MachineCFIInstruction CFI) {
  auto Index = Function.cfiInfo().addInstruction(std::move(CFI));
  if (!Index)
    return std::unexpected(Index.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point =
      Anchor == nullptr ? Builder.setInsertPoint(Block) : Builder.setInsertPointBefore(*Anchor);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::CFI_INSTRUCTION));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addCFIInstruction(*Index); !Added)
    return std::unexpected(Added.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] std::vector<std::uint32_t> setDifference(const std::set<std::uint32_t> &Left,
                                                       const std::set<std::uint32_t> &Right) {
  std::vector<std::uint32_t> Difference;
  std::set_difference(Left.begin(), Left.end(), Right.begin(), Right.end(),
                      std::back_inserter(Difference));
  return Difference;
}

[[nodiscard]] Expected<void> emitSavedLocation(MachineFunction &Function, MachineBasicBlock &Block,
                                               MachineInstr *Anchor,
                                               const TargetDescription &Target,
                                               std::uint32_t Register,
                                               const SavedLocationMap &Locations) {
  const auto Position = Locations.find(Register);
  if (Position == Locations.end()) {
    return makeUnexpected(ErrorCode::InternalError, "saved CFI register has no recorded location");
  }
  const DwarfRegisterNumber DwarfRegister = DwarfRegisterNumber::fromValue(Register);
  const SavedLocation &Location = Position->second;
  if (Location.LocationKind == SavedLocation::Kind::CFAOffset) {
    return emitCFI(Function, Block, Anchor, Target,
                   MachineCFIInstruction::offset(DwarfRegister, Location.Value));
  }
  return emitCFI(Function, Block, Anchor, Target,
                 MachineCFIInstruction::registerRule(
                     DwarfRegister,
                     DwarfRegisterNumber::fromValue(static_cast<std::uint32_t>(Location.Value))));
}

[[nodiscard]] Expected<bool> insertCFIInstructions(BlockInfoMap &BlockInfos,
                                                   const SavedLocationMap &Locations,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target) {
  MachineBasicBlock *Entry = Function.firstBlock();
  if (Entry == nullptr)
    return false;
  const BlockCFIInfo *PreviousInfo = &BlockInfos.at(Entry);
  bool Changed = false;
  for (MachineBasicBlock *Block = Entry->next(); Block != nullptr; Block = Block->next()) {
    const BlockCFIInfo &Info = BlockInfos.at(Block);
    MachineInstr *Anchor = Block->firstInstruction();
    const MachineBasicBlock *PreviousBlock = Block->previous();
    const bool ForceFullCFA = PreviousBlock != nullptr && PreviousBlock->attributes().sectionID() !=
                                                              Block->attributes().sectionID();

    if ((PreviousInfo->OutgoingCFAOffset != Info.IncomingCFAOffset &&
         PreviousInfo->OutgoingCFARegister != Info.IncomingCFARegister) ||
        ForceFullCFA) {
      auto Emitted =
          emitCFI(Function, *Block, Anchor, Target,
                  MachineCFIInstruction::defCFA(Info.IncomingCFARegister, Info.IncomingCFAOffset));
      if (!Emitted)
        return std::unexpected(Emitted.error());
      Changed = true;
    } else if (PreviousInfo->OutgoingCFAOffset != Info.IncomingCFAOffset) {
      auto Emitted = emitCFI(Function, *Block, Anchor, Target,
                             MachineCFIInstruction::defCFAOffset(Info.IncomingCFAOffset));
      if (!Emitted)
        return std::unexpected(Emitted.error());
      Changed = true;
    } else if (PreviousInfo->OutgoingCFARegister != Info.IncomingCFARegister) {
      auto Emitted = emitCFI(Function, *Block, Anchor, Target,
                             MachineCFIInstruction::defCFARegister(Info.IncomingCFARegister));
      if (!Emitted)
        return std::unexpected(Emitted.error());
      Changed = true;
    }

    if (ForceFullCFA) {
      for (const std::uint32_t Register : Info.IncomingSavedRegisters) {
        auto Emitted = emitSavedLocation(Function, *Block, Anchor, Target, Register, Locations);
        if (!Emitted)
          return std::unexpected(Emitted.error());
      }
      Changed = true;
      PreviousInfo = &Info;
      continue;
    }

    for (const std::uint32_t Register :
         setDifference(PreviousInfo->OutgoingSavedRegisters, Info.IncomingSavedRegisters)) {
      auto Emitted =
          emitCFI(Function, *Block, Anchor, Target,
                  MachineCFIInstruction::restore(DwarfRegisterNumber::fromValue(Register)));
      if (!Emitted)
        return std::unexpected(Emitted.error());
      Changed = true;
    }
    for (const std::uint32_t Register :
         setDifference(Info.IncomingSavedRegisters, PreviousInfo->OutgoingSavedRegisters)) {
      auto Emitted = emitSavedLocation(Function, *Block, Anchor, Target, Register, Locations);
      if (!Emitted)
        return std::unexpected(Emitted.error());
      Changed = true;
    }
    PreviousInfo = &Info;
  }
  return Changed;
}

} // namespace

const MachinePassContract &CFIInstrInserterPass::contract() const noexcept {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "cfi-instr-inserter",
        .Requires = {},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = std::nullopt,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::Frame};
    return Value;
  }();
  return Contract;
}

Expected<MachinePassResult> CFIInstrInserterPass::run(MachineFunction &Function,
                                                      const TargetDescription &Target,
                                                      MachineFunctionAnalysisManager &) {
  if (FrameLowering == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "CFIInstrInserter requires target frame lowering");
  }
  MachinePassResult Result;
  Result.Preserved = PreservedAnalyses::all();
  if (Function.firstBlock() == nullptr || Function.cfiInfo().instructions().empty())
    return Result;

  const PhysicalRegister InitialRegister = FrameLowering->initialCFARegister(Function, Target);
  const auto InitialDwarfRegister = Target.mirText().dwarfRegister(InitialRegister);
  if (!InitialDwarfRegister) {
    return makeUnexpected(ErrorCode::InternalError,
                          "initial CFA register has no DWARF register number");
  }
  const std::int64_t InitialOffset = FrameLowering->initialCFAOffset(Function);
  BlockInfoMap BlockInfos;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    BlockInfos.emplace(
        Block, BlockCFIInfo{
                   .Block = Block,
                   .IncomingCFAOffset = InitialOffset,
                   .OutgoingCFAOffset = InitialOffset,
                   .IncomingCFARegister = DwarfRegisterNumber::fromValue(*InitialDwarfRegister),
                   .OutgoingCFARegister = DwarfRegisterNumber::fromValue(*InitialDwarfRegister),
                   .IncomingSavedRegisters = {},
                   .OutgoingSavedRegisters = {},
                   .Processed = false,
               });
  }
  SavedLocationMap Locations;
  auto Calculated = calculateCFI(BlockInfos, Locations, Function);
  if (!Calculated)
    return std::unexpected(Calculated.error());
  auto Inserted = insertCFIInstructions(BlockInfos, Locations, Function, Target);
  if (!Inserted)
    return std::unexpected(Inserted.error());
  Result.Changed = *Inserted;
  if (Result.Changed) {
    Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                      MachinePassChange::Frame};
  }
  return Result;
}

} // namespace rcc

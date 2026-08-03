// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineVerifier.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rcc {
namespace {

class FunctionVerifier final {
public:
  FunctionVerifier(const MachineFunction &Function, const TargetDescription &Target,
                   MachineVerifierOptions Options) noexcept
      : Function(Function), Target(Target), Options(Options) {}

  [[nodiscard]] Expected<void> run() {
    if (Function.name().empty() || !Function.stableID().isValid())
      return fail("function identity is invalid");

    auto AttributeResult = Function.attributes().verify();
    if (!AttributeResult)
      return failWith("function attributes", AttributeResult.error());
    auto MetadataResult = Function.metadata().verify();
    if (!MetadataResult)
      return failWith("machine metadata", MetadataResult.error());
    auto CFIResult = Function.cfiInfo().verify();
    if (!CFIResult)
      return failWith("CFI information", CFIResult.error());
    if (Function.targetState().hasValue()) {
      auto TargetStateResult = Function.targetState().verify();
      if (!TargetStateResult)
        return failWith("target function state", TargetStateResult.error());
    }
    auto CatalogResult = catalogArenaObjects();
    if (!CatalogResult)
      return CatalogResult;
    auto FrameResult = Function.frameInfo().verify();
    if (!FrameResult)
      return failWith("frame info", FrameResult.error());
    auto ConstantPoolResult = Function.constantPool().verify();
    if (!ConstantPoolResult)
      return failWith("constant pool", ConstantPoolResult.error());
    auto LayoutResult = verifyBlockLayout();
    if (!LayoutResult)
      return LayoutResult;
    auto RecordResult = verifyFunctionRecords();
    if (!RecordResult)
      return RecordResult;
    auto CompilationStatusResult = verifyCompilationStatus();
    if (!CompilationStatusResult)
      return CompilationStatusResult;
    auto JumpTableResult = Function.jumpTableInfo().verify();
    if (!JumpTableResult)
      return failWith("jump table info", JumpTableResult.error());
    auto FrameLifetimeResult = verifyFrameLifetimes();
    if (!FrameLifetimeResult)
      return FrameLifetimeResult;
    auto CFGResult = verifyCFG();
    if (!CFGResult)
      return CFGResult;
    auto RegisterResult = verifyRegisterInfo();
    if (!RegisterResult)
      return RegisterResult;
    auto DefUseResult = Function.registerInfo().verifyDefUseChains();
    if (!DefUseResult)
      return failWith("register info", DefUseResult.error());
    if (Options.VerifyProperties) {
      auto PropertyResult = verifyProperties();
      if (!PropertyResult)
        return PropertyResult;
    }
    return {};
  }

private:
  [[nodiscard]] Expected<void> fail(std::string Message) const {
    return makeUnexpected(ErrorCode::VerificationError, "machine function '" +
                                                            std::string(Function.name()) +
                                                            "': " + std::move(Message));
  }

  [[nodiscard]] Expected<void> failWith(std::string_view Context, const Error &Nested) const {
    return fail(std::string(Context) + ": " + std::string(Nested.message()));
  }

  [[nodiscard]] static bool hasMatchingStorage(const MachineOperand &Operand) noexcept {
    switch (Operand.kind()) {
    case MachineOperand::Kind::Register:
      return Operand.getIf<Register>() != nullptr;
    case MachineOperand::Kind::Immediate:
      return Operand.getIf<std::int64_t>() != nullptr;
    case MachineOperand::Kind::IntegerImmediate:
      return Operand.getIf<IntegerImmediateOperand>() != nullptr;
    case MachineOperand::Kind::FloatingImmediate:
      return Operand.getIf<FloatingImmediateOperand>() != nullptr;
    case MachineOperand::Kind::BasicBlock:
      return Operand.getIf<BasicBlockOperand>() != nullptr;
    case MachineOperand::Kind::FrameIndex:
      return Operand.getIf<FrameIndexOperand>() != nullptr;
    case MachineOperand::Kind::ConstantPoolIndex:
      return Operand.getIf<ConstantPoolOperand>() != nullptr;
    case MachineOperand::Kind::JumpTableIndex:
      return Operand.getIf<JumpTableOperand>() != nullptr;
    case MachineOperand::Kind::GlobalSymbol:
      return Operand.getIf<GlobalSymbolOperand>() != nullptr;
    case MachineOperand::Kind::ExternalSymbol:
      return Operand.getIf<ExternalSymbolOperand>() != nullptr;
    case MachineOperand::Kind::BlockAddress:
      return Operand.getIf<BlockAddressOperand>() != nullptr;
    case MachineOperand::Kind::RegisterMask:
      return Operand.getIf<RegisterMaskOperand>() != nullptr;
    case MachineOperand::Kind::LiveOutRegisterMask:
      return Operand.getIf<LiveOutRegisterMaskOperand>() != nullptr;
    case MachineOperand::Kind::Metadata:
      return Operand.getIf<MetadataOperand>() != nullptr;
    case MachineOperand::Kind::TargetIndex:
      return Operand.getIf<TargetIndexOperand>() != nullptr;
    case MachineOperand::Kind::MCSymbol:
      return Operand.getIf<MCSymbolOperand>() != nullptr;
    case MachineOperand::Kind::CFIIndex:
      return Operand.getIf<CFIIndexOperand>() != nullptr;
    case MachineOperand::Kind::Intrinsic:
      return Operand.getIf<IntrinsicOperand>() != nullptr;
    case MachineOperand::Kind::Predicate:
      return Operand.getIf<PredicateOperand>() != nullptr;
    case MachineOperand::Kind::ShuffleMask:
      return Operand.getIf<ShuffleMaskOperand>() != nullptr;
    case MachineOperand::Kind::DebugInstrRef:
      return Operand.getIf<DebugInstrRefOperand>() != nullptr;
    case MachineOperand::Kind::LaneMask:
      return Operand.getIf<LaneMaskOperand>() != nullptr;
    case MachineOperand::Kind::Count:
      return false;
    }
    return false;
  }

  [[nodiscard]] Expected<void> catalogArenaObjects() {
    std::unordered_set<MachineBlockStableID> BlockIDs;
    for (const MachineBasicBlock *Block : Function.allBlocks()) {
      if (Block == nullptr || !RegisteredBlocks.insert(Block).second)
        return fail("arena block registry contains a null or duplicate pointer");
      if (&Block->function() != &Function)
        return fail("arena block registry contains a foreign block");
      if (!Block->stableID().isValid() || !BlockIDs.insert(Block->stableID()).second) {
        return fail("arena block registry contains an invalid or duplicate StableID");
      }
      auto AttributeResult = Block->attributes().verify();
      if (!AttributeResult)
        return failWith("basic block attributes", AttributeResult.error());
    }

    std::unordered_set<MachineInstrStableID> InstructionIDs;
    for (const MachineInstr *Instruction : Function.allInstructions()) {
      if (Instruction == nullptr || !RegisteredInstructions.insert(Instruction).second) {
        return fail("arena instruction registry contains a null or duplicate pointer");
      }
      if (&Instruction->function() != &Function)
        return fail("arena instruction registry contains a foreign instruction");
      if (!Instruction->stableID().isValid() ||
          !InstructionIDs.insert(Instruction->stableID()).second) {
        return fail("arena instruction registry contains an invalid or duplicate StableID");
      }
    }

    for (const MachineMemOperand *MemOperand : Function.allMemOperands()) {
      if (MemOperand == nullptr || !RegisteredMemOperands.insert(MemOperand).second) {
        return fail("arena MMO registry contains a null or duplicate pointer");
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> verifyBlockLayout() {
    const MachineBasicBlock *Previous = nullptr;
    const MachineBasicBlock *Block = Function.firstBlock();
    std::uint32_t LayoutIndex = 0;
    while (Block != nullptr) {
      if (!RegisteredBlocks.contains(Block))
        return fail("block layout references an unregistered block");
      if (!LinkedBlocks.insert(Block).second)
        return fail("block layout contains a cycle or duplicate block");
      if (!Block->isLinked() || Block->previous() != Previous)
        return fail("block layout ownership or previous link is inconsistent");
      if (Block->denseNumber() != LayoutIndex || Block->layoutIndex() != LayoutIndex) {
        return fail("block dense number or layout index is stale");
      }
      if (const auto &ProfileID = Block->attributes().profileID()) {
        const std::uint64_t Key = (static_cast<std::uint64_t>(ProfileID->BaseID) << 32U) |
                                  static_cast<std::uint64_t>(ProfileID->CloneID);
        if (!LinkedBlockProfileIDs.insert(Key).second)
          return fail("multiple linked blocks have the same bb_id");
      }

      auto InstructionResult = verifyInstructionLayout(*Block);
      if (!InstructionResult)
        return InstructionResult;
      Previous = Block;
      Block = Block->next();
      ++LayoutIndex;
    }

    if (Previous != Function.lastBlock())
      return fail("function last-block pointer is inconsistent");
    if ((Function.firstBlock() == nullptr) != (Function.lastBlock() == nullptr)) {
      return fail("function has only one of its first/last block pointers");
    }

    for (const MachineBasicBlock *Registered : RegisteredBlocks) {
      if (LinkedBlocks.contains(Registered)) {
        if (!Registered->isLinked())
          return fail("reachable block is not marked linked");
        continue;
      }
      if (Registered->isLinked())
        return fail("unreachable arena block is marked linked");
      if (Registered->previous() != nullptr || Registered->next() != nullptr ||
          Registered->firstInstruction() != nullptr || Registered->lastInstruction() != nullptr ||
          !Registered->predecessors().empty() || !Registered->successors().empty() ||
          Registered->denseNumber() || Registered->layoutIndex()) {
        return fail("detached block retains layout, instruction, or CFG state");
      }
    }

    for (const MachineInstr *Instruction : RegisteredInstructions) {
      auto LocalResult = verifyInstructionLocalState(*Instruction);
      if (!LocalResult)
        return LocalResult;
      if (Instruction->parent() != nullptr) {
        if (!LinkedInstructions.contains(Instruction))
          return fail("instruction has a parent but is absent from block layout");
      } else {
        if (Instruction->previous() != nullptr || Instruction->next() != nullptr) {
          return fail("detached instruction retains layout links");
        }
        for (const MachineOperand &Operand : Instruction->operands()) {
          if (Function.registerInfo().isLinkedOperand(Operand))
            return fail("detached instruction operand remains in a def-use chain");
        }
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> verifyInstructionLayout(const MachineBasicBlock &Block) {
    const MachineInstr *Previous = nullptr;
    const MachineInstr *Instruction = Block.firstInstruction();
    while (Instruction != nullptr) {
      if (!RegisteredInstructions.contains(Instruction))
        return fail("block layout references an unregistered instruction");
      if (!LinkedInstructions.insert(Instruction).second)
        return fail("instruction layout contains a cycle or duplicate instruction");
      LinkedInstructionIDs.insert(Instruction->stableID());
      LinkedInstructionsByID.emplace(Instruction->stableID(), Instruction);
      if (Instruction->parent() != &Block || Instruction->previous() != Previous) {
        return fail("instruction parent or previous link is inconsistent");
      }

      auto LiveResult = verifyLiveInstruction(*Instruction);
      if (!LiveResult)
        return LiveResult;
      Previous = Instruction;
      Instruction = Instruction->next();
    }
    if (Previous != Block.lastInstruction())
      return fail("block last-instruction pointer is inconsistent");
    if ((Block.firstInstruction() == nullptr) != (Block.lastInstruction() == nullptr)) {
      return fail("block has only one of its first/last instruction pointers");
    }
    return {};
  }

  [[nodiscard]] Expected<void> verifyInstructionLocalState(const MachineInstr &Instruction) {
    if (Instruction.sourceLocation() && !Instruction.sourceLocation()->isValid()) {
      return fail("instruction has an invalid source location");
    }
    if (Instruction.debugLocation()) {
      auto DebugLocationResult = Instruction.debugLocation()->verify(Function.metadata());
      if (!DebugLocationResult)
        return failWith("instruction debug location", DebugLocationResult.error());
    }
    if (Instruction.origin() && Instruction.origin()->value() == 0)
      return fail("instruction has an invalid origin ID");

    for (const MachineOperand &Operand : Instruction.operands()) {
      if (Operand.parent() != &Instruction || !hasMatchingStorage(Operand))
        return fail("instruction operand parent or tagged storage is inconsistent");
      if (const auto *Mask = Operand.getIf<RegisterMaskOperand>(); Mask != nullptr && !Mask->Mask) {
        return fail("register-mask operand contains a null mask");
      }
      if (const auto *Mask = Operand.getIf<LiveOutRegisterMaskOperand>();
          Mask != nullptr && !Mask->Mask) {
        return fail("live-out register-mask operand contains a null mask");
      }
      if (const auto *Integer = Operand.getIf<IntegerImmediateOperand>(); Integer != nullptr) {
        auto IntegerResult = Integer->Value.verify();
        if (!IntegerResult)
          return failWith("integer immediate operand", IntegerResult.error());
      }
      if (const auto *Global = Operand.getIf<GlobalSymbolOperand>();
          Global != nullptr && Global->Name.empty()) {
        return fail("global symbol operand has an empty name");
      }
      if (const auto *External = Operand.getIf<ExternalSymbolOperand>();
          External != nullptr && External->Name.empty()) {
        return fail("external symbol operand has an empty name");
      }
      if (const auto *Address = Operand.getIf<BlockAddressOperand>();
          Address != nullptr && (Address->FunctionName.empty() || Address->BlockName.empty())) {
        return fail("block-address operand has an incomplete name");
      }
      if (const auto *Metadata = Operand.getIf<MetadataOperand>(); Metadata != nullptr) {
        auto MetadataResult = Metadata->Value.verify(Function.metadata());
        if (!MetadataResult)
          return failWith("metadata operand", MetadataResult.error());
      }
      if (const auto *Symbol = Operand.getIf<MCSymbolOperand>();
          Symbol != nullptr && Symbol->Name.empty()) {
        return fail("MC symbol operand has an empty name");
      }
      if (const auto *Intrinsic = Operand.getIf<IntrinsicOperand>();
          Intrinsic != nullptr && Intrinsic->ID.value() == 0) {
        return fail("intrinsic operand has an invalid ID");
      }
      if (const auto *Predicate = Operand.getIf<PredicateOperand>(); Predicate != nullptr) {
        auto PredicateResult = Predicate->verify();
        if (!PredicateResult)
          return failWith("predicate operand", PredicateResult.error());
      }
      if (const auto *Shuffle = Operand.getIf<ShuffleMaskOperand>();
          Shuffle != nullptr && Shuffle->Elements.size() < 2) {
        return fail("shuffle-mask operand has fewer than two elements");
      }
    }

    for (const MachineMemOperand *MemOperand : Instruction.memOperands()) {
      if (MemOperand == nullptr || !RegisteredMemOperands.contains(MemOperand)) {
        return fail("instruction references a foreign or null MMO");
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> verifyFrameIndex(FrameIndex Index) const {
    const StackObject *Object = Function.frameInfo().object(Index);
    const StackObjectDesc *Description = Object == nullptr ? nullptr : Object->description();
    if (Description == nullptr || Description->IsDead)
      return fail("live instruction references an unknown or dead stack object");
    return {};
  }

  [[nodiscard]] Expected<void> verifyConstantPoolIndex(ConstantPoolIndex Index) const {
    if (Function.constantPool().entry(Index) == nullptr)
      return fail("live instruction references an unknown constant pool entry");
    return {};
  }

  [[nodiscard]] Expected<void> verifyJumpTableIndex(JumpTableIndex Index) const {
    if (Function.jumpTableInfo().table(Index) == nullptr)
      return fail("live instruction references an unknown jump table");
    return {};
  }

  [[nodiscard]] Expected<void> verifyFrameLifetimes() const {
    for (const StackObject &Object : Function.frameInfo().objects()) {
      const StackObjectDesc *Description = Object.description();
      if (Description == nullptr)
        continue;
      for (const StackObjectLifetimeRange &Range : Description->Lifetime) {
        if (!LinkedInstructionIDs.contains(Range.Begin) ||
            !LinkedInstructionIDs.contains(Range.End)) {
          return fail("stack object lifetime references a detached or unknown instruction");
        }
      }
      if (Description->DebugInfo) {
        auto DebugInfoResult = Description->DebugInfo->verify(Function.metadata());
        if (!DebugInfoResult)
          return failWith("stack object debug information", DebugInfoResult.error());
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> verifyFunctionRecords() const {
    const MachineFunctionRecords &Records = Function.records();
    auto Verification = Records.verify();
    if (!Verification)
      return failWith("function records", Verification.error());

    for (const PhysicalRegister Reg : Records.calleeSavedRegisters()) {
      if (!Target.registers().isKnownPhysicalRegister(Reg))
        return fail("callee-saved register list references an unknown physical register");
    }
    for (const MachineEntryValueInfo &EntryValue : Records.entryValues()) {
      if (!Target.registers().isKnownPhysicalRegister(EntryValue.Reg)) {
        return fail("entry-value record references an unknown physical register");
      }
      auto DebugInfoResult = EntryValue.DebugInfo.verify(Function.metadata());
      if (!DebugInfoResult)
        return failWith("entry-value debug information", DebugInfoResult.error());
    }

    auto VerifyCall = [this](MachineInstrStableID ID,
                             std::string_view Kind) -> Expected<const MachineInstr *> {
      const auto Position = LinkedInstructionsByID.find(ID);
      if (Position == LinkedInstructionsByID.end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "machine function '" + std::string(Function.name()) +
                                  "': " + std::string(Kind) +
                                  " references a detached or unknown instruction");
      }
      const InstructionDescriptor *Descriptor =
          Target.instructionDescriptor(Position->second->opcode());
      if (Descriptor == nullptr || !Descriptor->Flags.has(InstructionFlag::Call)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "machine function '" + std::string(Function.name()) +
                                  "': " + std::string(Kind) + " references a non-call instruction");
      }
      return Position->second;
    };

    for (const MachineCallSiteInfo &CallSite : Records.callSites()) {
      auto Call = VerifyCall(CallSite.CallInstruction, "call-site record");
      if (!Call)
        return std::unexpected(Call.error());
      for (const MachineCallSiteArgument &Argument : CallSite.ForwardedArguments) {
        if (!Target.registers().isKnownPhysicalRegister(Argument.Reg)) {
          return fail("call-site record references an unknown forwarded physical register");
        }
      }
    }

    for (const MachineCalledGlobal &CalledGlobal : Records.calledGlobals()) {
      auto Call = VerifyCall(CalledGlobal.CallInstruction, "called-global record");
      if (!Call)
        return std::unexpected(Call.error());
    }

    for (const MachinePrefetchTarget &Prefetch : Records.prefetchTargets()) {
      const std::uint64_t Key = (static_cast<std::uint64_t>(Prefetch.Block.BaseID) << 32U) |
                                static_cast<std::uint64_t>(Prefetch.Block.CloneID);
      if (!LinkedBlockProfileIDs.contains(Key))
        return fail("prefetch target references an unknown bb_id");
    }

    if (!Records.debugValueSubstitutions().empty() &&
        !Function.attributes().has(MachineFunctionAttribute::DebugInstrRef)) {
      return fail("debug value substitutions require DebugInstrRef");
    }
    for (const MachineDebugValueSubstitution &Substitution : Records.debugValueSubstitutions()) {
      if (Substitution.Subregister.value() != 0 &&
          Target.registers().subRegIndex(Substitution.Subregister) == nullptr) {
        return fail("debug value substitution references an unknown subregister index");
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> verifyCompilationStatus() const {
    const MachineCompilationStatus &Status = Function.compilationStatus();
    auto Verification = Status.verify();
    if (!Verification)
      return failWith("compilation status", Verification.error());

    const MachineCompilationFailure *Failure = Status.failure();
    if (Failure != nullptr && Failure->FailureInstruction &&
        !LinkedInstructionIDs.contains(*Failure->FailureInstruction)) {
      return fail("compilation failure references a detached or unknown instruction");
    }
    return {};
  }

  [[nodiscard]] Expected<void> verifyLiveInstruction(const MachineInstr &Instruction) {
    auto LocalResult = verifyInstructionLocalState(Instruction);
    if (!LocalResult)
      return LocalResult;

    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
    if (Descriptor == nullptr)
      return fail("live instruction uses an unregistered opcode");
    if (Instruction.debugInstrNumber() != 0) {
      if (Descriptor->Flags.has(InstructionFlag::Meta)) {
        return fail("metadata instruction has a debug value tracking number");
      }
      if (!DebugInstructionNumbers.insert(Instruction.debugInstrNumber()).second) {
        return fail("multiple instructions have the same debug value tracking number");
      }
    }
    auto DescriptorResult = Target.validateInstruction(Instruction, Function.registerInfo());
    if (!DescriptorResult)
      return failWith("instruction descriptor", DescriptorResult.error());

    if (Options.Stage && Descriptor->Flags.has(InstructionFlag::Pseudo) &&
        static_cast<std::uint8_t>(*Options.Stage) >
            static_cast<std::uint8_t>(Descriptor->Deadline)) {
      return fail("pseudo instruction survives beyond its expansion deadline");
    }

    HasPhi |= Descriptor->Flags.has(InstructionFlag::Phi);
    HasGenericInstruction |= Descriptor->Flags.has(InstructionFlag::Generic);
    HasExecutablePseudo |= Descriptor->Flags.has(InstructionFlag::Pseudo) &&
                           !Descriptor->Flags.has(InstructionFlag::Meta) &&
                           !Descriptor->Flags.has(InstructionFlag::TerminalEmission);

    const auto Operands = Instruction.operands();
    for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
      const MachineOperand &Operand = Operands[Index];
      if (const auto *Frame = Operand.getIf<FrameIndexOperand>(); Frame != nullptr) {
        HasFrameIndex = true;
        auto FrameResult = verifyFrameIndex(Frame->Index);
        if (!FrameResult)
          return FrameResult;
      }
      if (const auto *Constant = Operand.getIf<ConstantPoolOperand>(); Constant != nullptr) {
        auto ConstantResult = verifyConstantPoolIndex(Constant->Index);
        if (!ConstantResult)
          return ConstantResult;
      }
      if (const auto *JumpTable = Operand.getIf<JumpTableOperand>(); JumpTable != nullptr) {
        auto JumpTableResult = verifyJumpTableIndex(JumpTable->Index);
        if (!JumpTableResult)
          return JumpTableResult;
      }
      if (const auto *CFI = Operand.getIf<CFIIndexOperand>();
          CFI != nullptr && Function.cfiInfo().instruction(CFI->Index) == nullptr) {
        return fail("live instruction references an unknown CFI instruction");
      }

      if (const auto *Block = Operand.getIf<BasicBlockOperand>();
          Block != nullptr &&
          (Block->Block == nullptr || &Block->Block->function() != &Function ||
           !RegisteredBlocks.contains(Block->Block) || !Block->Block->isLinked())) {
        return fail("basic-block operand references a foreign or detached block");
      }

      const Register *Reg = Operand.getIf<Register>();
      if (Reg == nullptr)
        continue;
      const bool ShouldBeLinked = !Reg->isNoRegister();
      if (Function.registerInfo().isLinkedOperand(Operand) != ShouldBeLinked) {
        return fail("live register operand has incorrect def-use linkage");
      }
      if (Reg->isVirtual()) {
        if (!Function.registerInfo().isKnownVirtualRegister(*Reg))
          return fail("instruction references an unknown virtual register");
        if (Operand.isDef())
          ++VirtualDefCounts[*Reg];
      }

      if (Operand.tiedOperandIndex() && Index < *Operand.tiedOperandIndex()) {
        const MachineOperand *Other = Instruction.operandAt(*Operand.tiedOperandIndex());
        const Register *OtherReg = Other == nullptr ? nullptr : Other->getIf<Register>();
        if (OtherReg == nullptr || *OtherReg != *Reg)
          HasUnrewrittenTie = true;
      }
    }
    for (const MachineMemOperand *MemOperand : Instruction.memOperands()) {
      const MemoryBase &Base = MemOperand->description().Base;
      if (const auto Frame = Base.frameIndex()) {
        auto FrameResult = verifyFrameIndex(*Frame);
        if (!FrameResult)
          return FrameResult;
      }
      if (const auto Constant = Base.constantPoolIndex()) {
        auto ConstantResult = verifyConstantPoolIndex(*Constant);
        if (!ConstantResult)
          return ConstantResult;
      }
      if (const auto JumpTable = Base.jumpTableIndex()) {
        auto JumpTableResult = verifyJumpTableIndex(*JumpTable);
        if (!JumpTableResult)
          return JumpTableResult;
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> verifyCFG() {
    for (const MachineBasicBlock *Block : LinkedBlocks) {
      std::unordered_set<const MachineBasicBlock *> Successors;
      for (const MachineSuccessor &Successor : Block->successors()) {
        if (Successor.Block == nullptr || !LinkedBlocks.contains(Successor.Block) ||
            !Successors.insert(Successor.Block).second) {
          return fail("CFG successor list contains a null, detached, or duplicate block");
        }
        if (std::ranges::count(Successor.Block->predecessors(), Block) != 1)
          return fail("CFG successor lacks one reciprocal predecessor");
      }

      std::unordered_set<const MachineBasicBlock *> Predecessors;
      for (const MachineBasicBlock *Predecessor : Block->predecessors()) {
        if (Predecessor == nullptr || !LinkedBlocks.contains(Predecessor) ||
            !Predecessors.insert(Predecessor).second) {
          return fail("CFG predecessor list contains a null, detached, or duplicate block");
        }
        const auto Matches = std::ranges::count_if(
            Predecessor->successors(),
            [Block](const MachineSuccessor &Successor) { return Successor.Block == Block; });
        if (Matches != 1)
          return fail("CFG predecessor lacks one reciprocal successor");
      }

      std::unordered_set<PhysicalRegister> LiveIns;
      for (const MachineBasicBlockLiveIn &LiveIn : Block->liveIns()) {
        if (!LiveIn.PhysReg.isValid() || LiveIn.Lanes.empty() ||
            !LiveIns.insert(LiveIn.PhysReg).second) {
          return fail("block live-in list contains invalid or duplicate entries");
        }
      }
      auto TargetLiveInResult = Target.validateBlockLiveIns(*Block);
      if (!TargetLiveInResult)
        return failWith("block live-ins", TargetLiveInResult.error());
    }
    return {};
  }

  [[nodiscard]] Expected<void> verifyRegisterInfo() {
    const MachineRegisterInfo &RegInfo = Function.registerInfo();
    std::unordered_set<PhysicalRegister> FunctionLiveIns;
    for (const PhysicalRegisterLiveIn &LiveIn : RegInfo.liveIns()) {
      if (!LiveIn.PhysReg.isValid() || !FunctionLiveIns.insert(LiveIn.PhysReg).second) {
        return fail("function live-in list contains invalid or duplicate entries");
      }
      if (LiveIn.InitialVirtualRegister &&
          !RegInfo.isKnownVirtualRegister(*LiveIn.InitialVirtualRegister)) {
        return fail("function live-in references an unknown virtual register");
      }
    }

    std::unordered_set<PhysicalRegister> Reserved;
    for (const PhysicalRegister Reg : RegInfo.reservedRegisters()) {
      if (!Reg.isValid() || !Reserved.insert(Reg).second)
        return fail("reserved-register list contains invalid or duplicate entries");
    }

    for (const VirtualRegisterInfo &Info : RegInfo.virtualRegisters()) {
      if (Info.IsTombstone) {
        if (Info.Type || Info.Bank || Info.RegClass || !Info.AllocationHints.empty()) {
          return fail("virtual-register tombstone retains semantic state");
        }
        continue;
      }
      HasLiveVirtualRegister = true;
      MissingVirtualType |= !Info.Type;
      MissingVirtualBank |= !Info.Bank;
      MissingVirtualClass |= !Info.RegClass;
      if (Info.Type && !Info.Type->isValid())
        return fail("virtual register has an invalid LLT");

      std::unordered_set<PhysicalRegister> Hints;
      for (const PhysicalRegister Hint : Info.AllocationHints) {
        if (!Hint.isValid() || !Hints.insert(Hint).second)
          return fail("virtual register has an invalid or duplicate allocation hint");
      }
    }
    auto TargetResult = Target.validateRegisterInfo(RegInfo);
    if (!TargetResult)
      return failWith("target register info", TargetResult.error());
    return {};
  }

  [[nodiscard]] Expected<void> verifyProperties() {
    const MachineFunctionProperties &Properties = Function.properties();
    auto Has = [&Properties](MachineFunctionProperty Property) { return Properties.has(Property); };

    if (Has(MachineFunctionProperty::RegBankSelected) && !Has(MachineFunctionProperty::Legalized)) {
      return fail("RegBankSelected requires Legalized");
    }
    if (Has(MachineFunctionProperty::Selected) && !Has(MachineFunctionProperty::RegBankSelected)) {
      return fail("Selected requires RegBankSelected");
    }
    if (Has(MachineFunctionProperty::TiedOpsRewritten) && !Has(MachineFunctionProperty::Selected)) {
      return fail("TiedOpsRewritten requires Selected");
    }
    if (Has(MachineFunctionProperty::FrameFinalized) &&
        (!Has(MachineFunctionProperty::NoVRegs) || !Has(MachineFunctionProperty::NoFrameIndices))) {
      return fail("FrameFinalized requires NoVRegs and NoFrameIndices");
    }

    if (Has(MachineFunctionProperty::IsSSA)) {
      for (const auto &[Reg, DefinitionCount] : VirtualDefCounts) {
        if (DefinitionCount > 1)
          return fail("IsSSA function has a multiply-defined virtual register");
      }
    }
    if (Has(MachineFunctionProperty::NoPHIs) && HasPhi)
      return fail("NoPHIs function contains a PHI instruction");
    if (Has(MachineFunctionProperty::NoVRegs) && HasLiveVirtualRegister) {
      return fail("NoVRegs function retains a live virtual register");
    }
    if (Has(MachineFunctionProperty::NoFrameIndices) && HasFrameIndex)
      return fail("NoFrameIndices function contains a frame-index operand");
    if (Has(MachineFunctionProperty::NoExecutablePseudos) && HasExecutablePseudo) {
      return fail("NoExecutablePseudos function contains an executable pseudo");
    }
    if (Has(MachineFunctionProperty::RegBankSelected) && MissingVirtualBank) {
      return fail("RegBankSelected function has a virtual register without a bank");
    }
    if (Has(MachineFunctionProperty::Selected) && (HasGenericInstruction || MissingVirtualClass)) {
      return fail("Selected function contains generic code or an unclassified virtual register");
    }
    if (Has(MachineFunctionProperty::TiedOpsRewritten) && HasUnrewrittenTie) {
      return fail("TiedOpsRewritten function has unequal tied registers");
    }

    const bool NeedsTypes = Options.Mode == MachineVerifierMode::PreLegalizer ||
                            Has(MachineFunctionProperty::Legalized) ||
                            Has(MachineFunctionProperty::RegBankSelected);
    if (NeedsTypes && MissingVirtualType)
      return fail("typed pipeline stage has a virtual register without LLT");

    if (Options.Mode == MachineVerifierMode::PreLegalizer) {
      if (!Has(MachineFunctionProperty::IsSSA) || !Has(MachineFunctionProperty::TracksLiveness)) {
        return fail("pre-legalizer input requires IsSSA and TracksLiveness");
      }
      if (Has(MachineFunctionProperty::Legalized) ||
          Has(MachineFunctionProperty::RegBankSelected) || Has(MachineFunctionProperty::Selected) ||
          Has(MachineFunctionProperty::TiedOpsRewritten) || Has(MachineFunctionProperty::NoVRegs) ||
          Has(MachineFunctionProperty::FrameFinalized)) {
        return fail("pre-legalizer input carries a later-stage property");
      }
    }
    return {};
  }

  const MachineFunction &Function;
  const TargetDescription &Target;
  MachineVerifierOptions Options;
  std::unordered_set<const MachineBasicBlock *> RegisteredBlocks;
  std::unordered_set<const MachineBasicBlock *> LinkedBlocks;
  std::unordered_set<const MachineInstr *> RegisteredInstructions;
  std::unordered_set<const MachineInstr *> LinkedInstructions;
  std::unordered_set<MachineInstrStableID> LinkedInstructionIDs;
  std::unordered_map<MachineInstrStableID, const MachineInstr *> LinkedInstructionsByID;
  std::unordered_set<std::uint64_t> LinkedBlockProfileIDs;
  std::unordered_set<std::uint32_t> DebugInstructionNumbers;
  std::unordered_set<const MachineMemOperand *> RegisteredMemOperands;
  std::unordered_map<Register, std::size_t> VirtualDefCounts;
  bool HasPhi = false;
  bool HasGenericInstruction = false;
  bool HasExecutablePseudo = false;
  bool HasFrameIndex = false;
  bool HasUnrewrittenTie = false;
  bool HasLiveVirtualRegister = false;
  bool MissingVirtualType = false;
  bool MissingVirtualBank = false;
  bool MissingVirtualClass = false;
};

} // namespace

Expected<void> MachineVerifier::verify(const MachineFunction &Function,
                                       const TargetDescription &Target,
                                       MachineVerifierOptions Options) {
  return FunctionVerifier(Function, Target, Options).run();
}

} // namespace rcc

// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineVerifier.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
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
  FunctionVerifier(const MachineFunction &Function, const InstructionSet &Instructions,
                   MachineVerifierOptions Options) noexcept
      : Function(Function), Instructions(Instructions), Options(Options) {}

  [[nodiscard]] Expected<void> run() {
    if (Function.name().empty() || !Function.stableID().isValid())
      return fail("function identity is invalid");

    auto CatalogResult = catalogArenaObjects();
    if (!CatalogResult)
      return CatalogResult;
    auto LayoutResult = verifyBlockLayout();
    if (!LayoutResult)
      return LayoutResult;
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
    case MachineOperand::Kind::Metadata:
      return Operand.getIf<MetadataOperand>() != nullptr;
    case MachineOperand::Kind::TargetIndex:
      return Operand.getIf<TargetIndexOperand>() != nullptr;
    case MachineOperand::Kind::MCSymbol:
      return Operand.getIf<MCSymbolOperand>() != nullptr;
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
    if (Instruction.origin() && Instruction.origin()->value() == 0)
      return fail("instruction has an invalid origin ID");

    for (const MachineOperand &Operand : Instruction.operands()) {
      if (Operand.parent() != &Instruction || !hasMatchingStorage(Operand))
        return fail("instruction operand parent or tagged storage is inconsistent");
      if (const auto *Mask = Operand.getIf<RegisterMaskOperand>(); Mask != nullptr && !Mask->Mask) {
        return fail("register-mask operand contains a null mask");
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
      if (const auto *Metadata = Operand.getIf<MetadataOperand>();
          Metadata != nullptr && Metadata->Marker.empty()) {
        return fail("metadata operand has an empty marker");
      }
      if (const auto *Symbol = Operand.getIf<MCSymbolOperand>();
          Symbol != nullptr && Symbol->Name.empty()) {
        return fail("MC symbol operand has an empty name");
      }
    }

    for (const MachineMemOperand *MemOperand : Instruction.memOperands()) {
      if (MemOperand == nullptr || !RegisteredMemOperands.contains(MemOperand)) {
        return fail("instruction references a foreign or null MMO");
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> verifyLiveInstruction(const MachineInstr &Instruction) {
    auto LocalResult = verifyInstructionLocalState(Instruction);
    if (!LocalResult)
      return LocalResult;

    const InstructionDescriptor *Descriptor = Instructions.descriptor(Instruction.opcode());
    if (Descriptor == nullptr)
      return fail("live instruction uses an unregistered opcode");
    auto DescriptorResult = Instructions.validateInstruction(Instruction, Function.registerInfo());
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
      if (Operand.kind() == MachineOperand::Kind::FrameIndex)
        HasFrameIndex = true;

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
  const InstructionSet &Instructions;
  MachineVerifierOptions Options;
  std::unordered_set<const MachineBasicBlock *> RegisteredBlocks;
  std::unordered_set<const MachineBasicBlock *> LinkedBlocks;
  std::unordered_set<const MachineInstr *> RegisteredInstructions;
  std::unordered_set<const MachineInstr *> LinkedInstructions;
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
                                       const InstructionSet &Instructions,
                                       MachineVerifierOptions Options) {
  return FunctionVerifier(Function, Instructions, Options).run();
}

} // namespace rcc

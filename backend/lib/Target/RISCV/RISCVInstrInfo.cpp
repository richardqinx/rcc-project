// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVInstrInfo.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

constexpr std::uint32_t PCRelativeHighFlag = 2;
constexpr std::uint32_t PCRelativeLowFlag = 4;

[[nodiscard]] bool isConditionalBranch(Opcode Value) noexcept {
  return Value == instructionOpcode(InstructionOpcode::BEQ) ||
         Value == instructionOpcode(InstructionOpcode::BNE) ||
         Value == instructionOpcode(InstructionOpcode::BLT) ||
         Value == instructionOpcode(InstructionOpcode::BGE) ||
         Value == instructionOpcode(InstructionOpcode::BLTU) ||
         Value == instructionOpcode(InstructionOpcode::BGEU) ||
         Value == instructionOpcode(InstructionOpcode::C_BEQZ) ||
         Value == instructionOpcode(InstructionOpcode::C_BNEZ);
}

[[nodiscard]] std::optional<Opcode> reversedOpcode(Opcode Value) noexcept {
  constexpr std::array<std::pair<InstructionOpcode, InstructionOpcode>, 4> Pairs = {{
      {InstructionOpcode::BEQ, InstructionOpcode::BNE},
      {InstructionOpcode::BLT, InstructionOpcode::BGE},
      {InstructionOpcode::BLTU, InstructionOpcode::BGEU},
      {InstructionOpcode::C_BEQZ, InstructionOpcode::C_BNEZ},
  }};
  for (const auto &[Left, Right] : Pairs) {
    if (Value == instructionOpcode(Left))
      return instructionOpcode(Right);
    if (Value == instructionOpcode(Right))
      return instructionOpcode(Left);
  }
  return std::nullopt;
}

[[nodiscard]] bool isZeroRegister(const MachineOperand *Operand) noexcept {
  const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  const std::optional<PhysicalRegister> Physical =
      Reg == nullptr ? std::nullopt : Reg->physicalRegister();
  return Physical && Physical->number() == static_cast<std::uint32_t>(PhysicalRegisterID::X0);
}

[[nodiscard]] bool sameRegister(const MachineOperand *Left, const MachineOperand *Right) noexcept {
  const Register *LeftReg = Left == nullptr ? nullptr : Left->getIf<Register>();
  const Register *RightReg = Right == nullptr ? nullptr : Right->getIf<Register>();
  return LeftReg != nullptr && RightReg != nullptr && *LeftReg == *RightReg &&
         Left->subReg() == Right->subReg();
}

[[nodiscard]] bool isUnconditionalBranch(const MachineInstr &Instruction) noexcept {
  if (Instruction.opcode() == instructionOpcode(InstructionOpcode::C_J) ||
      Instruction.opcode() == instructionOpcode(InstructionOpcode::PseudoBR)) {
    return true;
  }
  return Instruction.opcode() == instructionOpcode(InstructionOpcode::JAL) &&
         isZeroRegister(Instruction.operandAt(0));
}

[[nodiscard]] std::size_t branchTargetIndex(Opcode Value) noexcept {
  if (Value == instructionOpcode(InstructionOpcode::C_J) ||
      Value == instructionOpcode(InstructionOpcode::PseudoBR)) {
    return 0;
  }
  if (Value == instructionOpcode(InstructionOpcode::C_BEQZ) ||
      Value == instructionOpcode(InstructionOpcode::C_BNEZ)) {
    return 1;
  }
  if (Value == instructionOpcode(InstructionOpcode::JAL))
    return 1;
  return 2;
}

[[nodiscard]] Expected<MachineBasicBlock *> directTarget(MachineInstr &Instruction) {
  if (!isConditionalBranch(Instruction.opcode()) &&
      Instruction.opcode() != instructionOpcode(InstructionOpcode::JAL) &&
      Instruction.opcode() != instructionOpcode(InstructionOpcode::C_J) &&
      Instruction.opcode() != instructionOpcode(InstructionOpcode::PseudoBR)) {
    return static_cast<MachineBasicBlock *>(nullptr);
  }
  MachineOperand *Operand = Instruction.operandAt(branchTargetIndex(Instruction.opcode()));
  const BasicBlockOperand *Block =
      Operand == nullptr ? nullptr : Operand->getIf<BasicBlockOperand>();
  if (Block == nullptr || Block->Block == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V direct branch has no basic-block target");
  }
  return Block->Block;
}

[[nodiscard]] Expected<void> setDirectTarget(MachineInstr &Instruction,
                                             MachineBasicBlock &TargetBlock) {
  MachineOperand *Operand = Instruction.operandAt(branchTargetIndex(Instruction.opcode()));
  if (Operand == nullptr || Operand->kind() != MachineOperand::Kind::BasicBlock) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V direct branch has no mutable basic-block target");
  }
  return Operand->setBasicBlock(TargetBlock);
}

[[nodiscard]] Expected<void> removeInstruction(MachineInstr &Instruction) {
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V branch instruction is not linked to a block");
  }
  return Block->removeInstruction(Instruction);
}

[[nodiscard]] Expected<PhysicalRegister> physical(PhysicalRegisterID ID) {
  auto Reg = physicalRegister(ID);
  if (!Reg)
    return std::unexpected(Reg.error());
  return *Reg;
}

[[nodiscard]] Expected<MachineInstr *>
buildJAL(MachineFunction &Function, const TargetDescription &Target, MachineBasicBlock &Block,
         MachineInstr *Before, PhysicalRegister Destination, MachineBasicBlock &TargetBlock) {
  MachineIRBuilder Builder(Function, Target);
  Expected<void> Point =
      Before == nullptr ? Builder.setInsertPoint(Block) : Builder.setInsertPointBefore(*Before);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(instructionOpcode(InstructionOpcode::JAL));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Register::fromPhysical(Destination));
  if (!Def)
    return std::unexpected(Def.error());
  auto TargetOperand = Pending->addBasicBlock(TargetBlock);
  if (!TargetOperand)
    return std::unexpected(TargetOperand.error());
  return Pending->build();
}

[[nodiscard]] Expected<void> copySourceIdentity(const MachineInstr &Source,
                                                MachineInstr &Destination) {
  if (Source.sourceLocation())
    Destination.setSourceLocation(*Source.sourceLocation());
  if (Source.debugLocation()) {
    auto Debug = Destination.setDebugLocation(*Source.debugLocation());
    if (!Debug)
      return std::unexpected(Debug.error());
  }
  if (Source.origin())
    Destination.setOrigin(*Source.origin());
  return {};
}

[[nodiscard]] Expected<void> copyTrailingAnnotations(const MachineInstr &Source,
                                                     MachineInstr &Destination) {
  if (Source.debugInstrNumber() != 0) {
    auto DebugNumber = Destination.setDebugInstrNumber(Source.debugInstrNumber());
    if (!DebugNumber)
      return std::unexpected(DebugNumber.error());
  }
  if (Source.postInstrSymbol()) {
    auto Symbol = Destination.setPostInstrSymbol(std::string(*Source.postInstrSymbol()));
    if (!Symbol)
      return std::unexpected(Symbol.error());
  }
  if (Source.heapAllocMarker()) {
    auto Marker = Destination.setHeapAllocMarker(*Source.heapAllocMarker());
    if (!Marker)
      return std::unexpected(Marker.error());
  }
  if (Source.pcSections()) {
    auto Sections = Destination.setPCSections(*Source.pcSections());
    if (!Sections)
      return std::unexpected(Sections.error());
  }
  if (Source.mmra()) {
    auto Metadata = Destination.setMMRA(*Source.mmra());
    if (!Metadata)
      return std::unexpected(Metadata.error());
  }
  if (Source.cfiType()) {
    auto Type = Destination.setCFIType(*Source.cfiType());
    if (!Type)
      return std::unexpected(Type.error());
  }
  if (Source.deactivationSymbol()) {
    auto Symbol = Destination.setDeactivationSymbol(std::string(*Source.deactivationSymbol()));
    if (!Symbol)
      return std::unexpected(Symbol.error());
  }
  return {};
}

[[nodiscard]] Expected<void> copySingleInstructionAnnotations(const MachineInstr &Source,
                                                              MachineInstr &Destination) {
  Destination.setFlags(Source.flags());
  auto Identity = copySourceIdentity(Source, Destination);
  if (!Identity)
    return std::unexpected(Identity.error());
  if (Source.preInstrSymbol()) {
    auto Symbol = Destination.setPreInstrSymbol(std::string(*Source.preInstrSymbol()));
    if (!Symbol)
      return std::unexpected(Symbol.error());
  }
  return copyTrailingAnnotations(Source, Destination);
}

[[nodiscard]] bool isZeroPhysicalRegister(PhysicalRegister Reg) noexcept {
  return Reg.number() == static_cast<std::uint32_t>(PhysicalRegisterID::X0);
}

[[nodiscard]] Expected<PhysicalRegister>
selectLongBranchScratch(const MachineInstr &Instruction, const MachineBasicBlock &TargetBlock,
                        const MachineFunction &Function, const TargetDescription &Target) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const std::optional<PhysicalRegister> DestinationPhysical =
      Destination == nullptr ? std::nullopt : Destination->physicalRegister();
  if (!DestinationPhysical) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V long JAL has no physical destination");
  }
  if (!isZeroPhysicalRegister(*DestinationPhysical))
    return *DestinationPhysical;

  const auto GPRClass =
      RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::GPR_NX0));
  const RegisterClassDesc *Candidates = Target.registers().registerClass(GPRClass);
  if (Candidates == nullptr) {
    return makeUnexpected(ErrorCode::InternalError, "RISC-V target has no nonzero GPR class");
  }
  for (const PhysicalRegister Candidate : Candidates->AllocationOrder) {
    if (Target.registers().isReservedRegister(Candidate) ||
        Function.registerInfo().isReserved(Candidate)) {
      continue;
    }
    const bool LiveAtTarget =
        std::ranges::any_of(TargetBlock.liveIns(), [&](const MachineBasicBlockLiveIn &LiveIn) {
          return Target.registers().regsOverlap(Candidate, LiveIn.PhysReg);
        });
    if (!LiveAtTarget)
      return Candidate;
  }
  return makeUnexpected(ErrorCode::BranchRelaxationFailed,
                        "RISC-V long branch has no dead physical scratch register");
}

[[nodiscard]] Expected<bool> expandCompressedBranch(MachineInstr &Instruction,
                                                    MachineFunction &Function,
                                                    const TargetDescription &Target) {
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "compressed branch is not linked");
  auto TargetBlock = directTarget(Instruction);
  if (!TargetBlock)
    return std::unexpected(TargetBlock.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());

  if (Instruction.opcode() == instructionOpcode(InstructionOpcode::C_J)) {
    auto Zero = physical(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    auto Replacement = buildJAL(Function, Target, *Block, &Instruction, *Zero, **TargetBlock);
    if (!Replacement)
      return std::unexpected(Replacement.error());
    auto Annotations = copySingleInstructionAnnotations(Instruction, **Replacement);
    if (!Annotations)
      return std::unexpected(Annotations.error());
    auto Removed = removeInstruction(Instruction);
    if (!Removed)
      return std::unexpected(Removed.error());
    return true;
  }

  const MachineOperand *SourceOperand = Instruction.operandAt(0);
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  if (Source == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "compressed conditional branch has no source register");
  auto Zero = physical(PhysicalRegisterID::X0);
  if (!Zero)
    return std::unexpected(Zero.error());
  const InstructionOpcode Expanded =
      Instruction.opcode() == instructionOpcode(InstructionOpcode::C_BEQZ) ? InstructionOpcode::BEQ
                                                                           : InstructionOpcode::BNE;
  auto Pending = Builder.buildInstr(instructionOpcode(Expanded));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto First = Pending->addUse(*Source);
  if (!First)
    return std::unexpected(First.error());
  auto Second = Pending->addUse(Register::fromPhysical(*Zero));
  if (!Second)
    return std::unexpected(Second.error());
  auto Destination = Pending->addBasicBlock(**TargetBlock);
  if (!Destination)
    return std::unexpected(Destination.error());
  auto Replacement = Pending->build();
  if (!Replacement)
    return std::unexpected(Replacement.error());
  auto Annotations = copySingleInstructionAnnotations(Instruction, **Replacement);
  if (!Annotations)
    return std::unexpected(Annotations.error());
  auto Removed = removeInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

[[nodiscard]] Expected<bool> relaxConditionalBranch(MachineInstr &Instruction,
                                                    MachineFunction &Function,
                                                    const TargetDescription &Target) {
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "conditional branch is not linked");
  auto TrueTarget = directTarget(Instruction);
  if (!TrueTarget)
    return std::unexpected(TrueTarget.error());
  MachineBasicBlock *FalseTarget = Block->next();
  if (FalseTarget == nullptr || FalseTarget == *TrueTarget ||
      std::ranges::none_of(Block->successors(), [FalseTarget](const MachineSuccessor &Successor) {
        return Successor.Block == FalseTarget;
      })) {
    return makeUnexpected(ErrorCode::BranchRelaxationFailed,
                          "RISC-V conditional relaxation requires its false edge as fallthrough");
  }

  auto Reversed = reversedOpcode(Instruction.opcode());
  if (!Reversed)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V conditional branch cannot be inverted");
  auto SetOpcode = Instruction.setOpcode(*Reversed);
  if (!SetOpcode)
    return std::unexpected(SetOpcode.error());
  auto SetTarget = setDirectTarget(Instruction, *FalseTarget);
  if (!SetTarget)
    return std::unexpected(SetTarget.error());

  MachineInstr *Following = Instruction.next();
  if (Following != nullptr && isUnconditionalBranch(*Following)) {
    auto Retargeted = setDirectTarget(*Following, **TrueTarget);
    if (!Retargeted)
      return std::unexpected(Retargeted.error());
    return true;
  }
  auto Zero = physical(PhysicalRegisterID::X0);
  if (!Zero)
    return std::unexpected(Zero.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointAfter(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(instructionOpcode(InstructionOpcode::JAL));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Register::fromPhysical(*Zero));
  if (!Def)
    return std::unexpected(Def.error());
  auto Destination = Pending->addBasicBlock(**TrueTarget);
  if (!Destination)
    return std::unexpected(Destination.error());
  auto LongJump = Pending->build();
  if (!LongJump)
    return std::unexpected(LongJump.error());
  auto Identity = copySourceIdentity(Instruction, **LongJump);
  if (!Identity)
    return std::unexpected(Identity.error());
  MachineInstrFlags OriginalFlags = Instruction.flags();
  const bool HadBundledSuccessor = OriginalFlags.has(MachineInstrFlag::BundledWithSucc);
  OriginalFlags.set(MachineInstrFlag::BundledWithSucc);
  Instruction.setFlags(OriginalFlags);
  MachineInstrFlags LongFlags;
  LongFlags.set(MachineInstrFlag::BundledWithPred);
  if (HadBundledSuccessor)
    LongFlags.set(MachineInstrFlag::BundledWithSucc);
  if (OriginalFlags.has(MachineInstrFlag::NoMerge))
    LongFlags.set(MachineInstrFlag::NoMerge);
  (*LongJump)->setFlags(LongFlags);
  return true;
}

[[nodiscard]] Expected<bool> relaxJAL(MachineInstr &Instruction, std::int64_t Offset,
                                      MachineFunction &Function, const TargetDescription &Target) {
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "RISC-V JAL is not linked");
  if (Offset < std::numeric_limits<std::int32_t>::min() ||
      Offset > std::numeric_limits<std::int32_t>::max()) {
    return makeUnexpected(ErrorCode::BranchRelaxationFailed,
                          "RISC-V PC-relative long branch exceeds the signed 32-bit range");
  }
  auto DestinationBlock = directTarget(Instruction);
  if (!DestinationBlock)
    return std::unexpected(DestinationBlock.error());
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Destination == nullptr || !Destination->isPhysical()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V long JAL requires a physical destination register");
  }
  auto Scratch = selectLongBranchScratch(Instruction, **DestinationBlock, Function, Target);
  if (!Scratch)
    return std::unexpected(Scratch.error());
  const std::string Anchor = Instruction.preInstrSymbol()
                                 ? std::string(*Instruction.preInstrSymbol())
                                 : ".Lrcc_long_" + std::to_string(Function.stableID().value()) +
                                       "_" + std::to_string(Instruction.stableID().value());
  const MachineInstrFlags OriginalFlags = Instruction.flags();

  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto HighPending = Builder.buildInstr(instructionOpcode(InstructionOpcode::AUIPC));
  if (!HighPending)
    return std::unexpected(HighPending.error());
  auto HighDef = HighPending->addDef(Register::fromPhysical(*Scratch));
  if (!HighDef)
    return std::unexpected(HighDef.error());
  auto HighTarget = HighPending->addBasicBlock(**DestinationBlock, PCRelativeHighFlag);
  if (!HighTarget)
    return std::unexpected(HighTarget.error());
  auto High = HighPending->build();
  if (!High)
    return std::unexpected(High.error());
  auto HighSymbol = (*High)->setPreInstrSymbol(Anchor);
  if (!HighSymbol)
    return std::unexpected(HighSymbol.error());
  auto HighIdentity = copySourceIdentity(Instruction, **High);
  if (!HighIdentity)
    return std::unexpected(HighIdentity.error());

  auto LowPending = Builder.buildInstr(instructionOpcode(InstructionOpcode::JALR));
  if (!LowPending)
    return std::unexpected(LowPending.error());
  auto LowDef = LowPending->addDef(*Destination);
  if (!LowDef)
    return std::unexpected(LowDef.error());
  auto LowBase = LowPending->addUse(Register::fromPhysical(*Scratch));
  if (!LowBase)
    return std::unexpected(LowBase.error());
  auto LowSymbol = LowPending->addMCSymbol(Anchor, 0, PCRelativeLowFlag);
  if (!LowSymbol)
    return std::unexpected(LowSymbol.error());
  auto Low = LowPending->build();
  if (!Low)
    return std::unexpected(Low.error());
  auto LowIdentity = copySourceIdentity(Instruction, **Low);
  if (!LowIdentity)
    return std::unexpected(LowIdentity.error());
  auto LowAnnotations = copyTrailingAnnotations(Instruction, **Low);
  if (!LowAnnotations)
    return std::unexpected(LowAnnotations.error());

  MachineInstrFlags HighFlags;
  if (OriginalFlags.has(MachineInstrFlag::BundledWithPred))
    HighFlags.set(MachineInstrFlag::BundledWithPred);
  HighFlags.set(MachineInstrFlag::BundledWithSucc);
  if (OriginalFlags.has(MachineInstrFlag::NoMerge))
    HighFlags.set(MachineInstrFlag::NoMerge);
  (*High)->setFlags(HighFlags);
  MachineInstrFlags LowFlags = OriginalFlags;
  LowFlags.clear(MachineInstrFlag::BundledWithPred);
  LowFlags.set(MachineInstrFlag::BundledWithPred);
  (*Low)->setFlags(LowFlags);
  auto Removed = removeInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

class RISCVInstrInfo final : public TargetInstrInfo {
public:
  [[nodiscard]] Expected<MachineBranchAnalysis>
  analyzeBranch(MachineBasicBlock &Block, MachineFunction &Function,
                const TargetDescription &Target) const override {
    if (&Block.function() != &Function)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch analysis references another function");
    std::vector<MachineInstr *> Terminators;
    for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V branch analysis encountered an unknown opcode");
      if (Descriptor->Flags.has(InstructionFlag::Terminator))
        Terminators.push_back(Instruction);
    }

    MachineBranchAnalysis Result;
    if (Terminators.empty())
      return Result;
    if (Terminators.size() > 2) {
      Result.HasUnanalyzableTerminator = true;
      return Result;
    }
    MachineInstr *First = Terminators.front();
    if (isConditionalBranch(First->opcode())) {
      auto TrueTarget = directTarget(*First);
      if (!TrueTarget)
        return std::unexpected(TrueTarget.error());
      Result.Conditional = First;
      Result.TrueTarget = *TrueTarget;
      if (Terminators.size() == 2) {
        MachineInstr *Second = Terminators.back();
        if (!isUnconditionalBranch(*Second)) {
          Result.HasUnanalyzableTerminator = true;
          return Result;
        }
        auto FalseTarget = directTarget(*Second);
        if (!FalseTarget)
          return std::unexpected(FalseTarget.error());
        Result.Unconditional = Second;
        Result.FalseTarget = *FalseTarget;
      } else {
        for (const MachineSuccessor &Successor : Block.successors()) {
          if (Successor.Block == nullptr || Successor.Block == Result.TrueTarget)
            continue;
          if (Result.FalseTarget != nullptr) {
            Result.HasUnanalyzableTerminator = true;
            return Result;
          }
          Result.FalseTarget = Successor.Block;
        }
        if (Result.FalseTarget == nullptr)
          Result.FalseTarget = Block.next();
      }
      return Result;
    }
    if (Terminators.size() == 1 && isUnconditionalBranch(*First)) {
      auto TargetBlock = directTarget(*First);
      if (!TargetBlock)
        return std::unexpected(TargetBlock.error());
      Result.Unconditional = First;
      Result.TrueTarget = *TargetBlock;
      return Result;
    }
    Result.HasUnanalyzableTerminator = true;
    return Result;
  }

  [[nodiscard]] Expected<void> reverseBranchCondition(MachineInstr &Branch,
                                                      MachineBasicBlock &NewTarget,
                                                      MachineFunction &Function,
                                                      const TargetDescription &) const override {
    if (&Branch.function() != &Function || &NewTarget.function() != &Function)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch reversal references another function");
    const std::optional<Opcode> Reversed = reversedOpcode(Branch.opcode());
    if (!Reversed)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch opcode has no inverse condition");
    auto OpcodeResult = Branch.setOpcode(*Reversed);
    if (!OpcodeResult)
      return std::unexpected(OpcodeResult.error());
    return setDirectTarget(Branch, NewTarget);
  }

  [[nodiscard]] Expected<void> removeBranch(MachineInstr &Branch, MachineFunction &Function,
                                            const TargetDescription &) const override {
    if (&Branch.function() != &Function)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch removal references another function");
    return removeInstruction(Branch);
  }

  [[nodiscard]] Expected<bool> rewriteBranchTarget(MachineBasicBlock &Block,
                                                   MachineBasicBlock &OldTarget,
                                                   MachineBasicBlock &NewTarget,
                                                   MachineFunction &Function,
                                                   const TargetDescription &) const override {
    if (&Block.function() != &Function || &OldTarget.function() != &Function ||
        &NewTarget.function() != &Function) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch rewrite references another function");
    }
    bool Changed = false;
    for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      if (!isConditionalBranch(Instruction->opcode()) && !isUnconditionalBranch(*Instruction)) {
        continue;
      }
      auto CurrentTarget = directTarget(*Instruction);
      if (!CurrentTarget)
        return std::unexpected(CurrentTarget.error());
      if (*CurrentTarget != &OldTarget)
        continue;
      auto Rewritten = setDirectTarget(*Instruction, NewTarget);
      if (!Rewritten)
        return std::unexpected(Rewritten.error());
      Changed = true;
    }
    return Changed;
  }

  [[nodiscard]] Expected<MachineInstr *>
  insertBranch(MachineBasicBlock &Block, MachineBasicBlock &TargetBlock, MachineFunction &Function,
               const TargetDescription &Target) const override {
    if (&Block.function() != &Function || &TargetBlock.function() != &Function)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch insertion references another function");
    auto Zero = physical(PhysicalRegisterID::X0);
    if (!Zero)
      return std::unexpected(Zero.error());
    return buildJAL(Function, Target, Block, nullptr, *Zero, TargetBlock);
  }

  [[nodiscard]] Expected<bool> isFallthroughEdge(const MachineBasicBlock &From,
                                                 const MachineBasicBlock &To,
                                                 const MachineFunction &Function,
                                                 const TargetDescription &Target) const override {
    if (&From.function() != &Function || &To.function() != &Function)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V fallthrough query references another machine function");
    if (From.next() != &To ||
        std::ranges::none_of(From.successors(), [&To](const MachineSuccessor &Successor) {
          return Successor.Block == &To;
        })) {
      return false;
    }

    bool CanFallThrough = true;
    for (const MachineInstr *Instruction = From.firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V fallthrough analysis encountered an unknown opcode");
      if (Descriptor->Flags.has(InstructionFlag::Terminator))
        CanFallThrough = isConditionalBranch(Instruction->opcode());
    }
    return CanFallThrough;
  }

  [[nodiscard]] Expected<void> validateCopy(Register Destination, SubRegIndex DestinationSubReg,
                                            Register Source, SubRegIndex SourceSubReg,
                                            const MachineFunction &Function,
                                            const TargetDescription &Target) const override {
    if (Destination.isNoRegister() || Source.isNoRegister())
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V COPY requires two nonzero registers");
    if (DestinationSubReg.value() != 0 || SourceSubReg.value() != 0)
      return makeUnexpected(ErrorCode::Unsupported, "RV64GC does not model register subindices");

    auto DestinationShape = copyShape(Destination, Function, Target);
    if (!DestinationShape)
      return std::unexpected(DestinationShape.error());
    auto SourceShape = copyShape(Source, Function, Target);
    if (!SourceShape)
      return std::unexpected(SourceShape.error());
    if (DestinationShape->Bank == SourceShape->Bank) {
      if (DestinationShape->Width && SourceShape->Width &&
          DestinationShape->Width != SourceShape->Width)
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V same-bank COPY has incompatible register widths");
      return {};
    }

    const RegisterBankID GPR =
        RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::GPR));
    const RegisterBankID FPR =
        RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::FPR));
    const bool IsCrossBankMove = (DestinationShape->Bank == GPR && SourceShape->Bank == FPR) ||
                                 (DestinationShape->Bank == FPR && SourceShape->Bank == GPR);
    if (!IsCrossBankMove)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V COPY crosses unsupported register banks");
    const auto SupportedWidth = [](std::optional<std::uint32_t> Width) {
      return !Width || *Width == 32 || *Width == 64;
    };
    if (!SupportedWidth(DestinationShape->Width) || !SupportedWidth(SourceShape->Width))
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V GPR/FPR COPY requires 32-bit or 64-bit values");
    return {};
  }

  [[nodiscard]] Expected<std::uint32_t>
  instructionSize(const MachineInstr &Instruction, const MachineFunction &,
                  const TargetDescription &Target) const override {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
    if (Descriptor == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V instruction-size query encountered an unknown opcode");
    if (!Descriptor->Flags.has(InstructionFlag::ProducesCode))
      return 0;
    const auto Category =
        std::ranges::find_if(encodingCategories(), [Descriptor](const EncodingCategoryInfo &Info) {
          return Info.ID == Descriptor->EncodingCategory;
        });
    if (Category == encodingCategories().end() || Category->SizeInBytes == 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V producing instruction has no exact encoding size");
    return Category->SizeInBytes;
  }

  [[nodiscard]] Expected<MachineBasicBlock *>
  branchTarget(MachineInstr &Instruction, MachineFunction &Function,
               const TargetDescription &) const override {
    if (&Instruction.function() != &Function)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch target query references another function");
    return directTarget(Instruction);
  }

  [[nodiscard]] Expected<std::optional<bool>>
  evaluateBranchCondition(const MachineInstr &Instruction, const MachineFunction &Function,
                          const TargetDescription &) const override {
    if (&Instruction.function() != &Function) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch evaluation references another function");
    }
    const Opcode Value = Instruction.opcode();
    if (!isConditionalBranch(Value))
      return std::optional<bool>{};
    if (Value == instructionOpcode(InstructionOpcode::C_BEQZ) ||
        Value == instructionOpcode(InstructionOpcode::C_BNEZ)) {
      if (!isZeroRegister(Instruction.operandAt(0)))
        return std::optional<bool>{};
      return std::optional<bool>{Value == instructionOpcode(InstructionOpcode::C_BEQZ)};
    }
    if (!sameRegister(Instruction.operandAt(0), Instruction.operandAt(1)))
      return std::optional<bool>{};
    if (Value == instructionOpcode(InstructionOpcode::BEQ) ||
        Value == instructionOpcode(InstructionOpcode::BGE) ||
        Value == instructionOpcode(InstructionOpcode::BGEU)) {
      return std::optional<bool>{true};
    }
    return std::optional<bool>{false};
  }

  [[nodiscard]] bool isBranchOffsetInRange(const MachineInstr &Instruction,
                                           std::int64_t Offset) const noexcept override {
    if ((Offset & 1) != 0)
      return false;
    const Opcode Value = Instruction.opcode();
    if (Value == instructionOpcode(InstructionOpcode::C_BEQZ) ||
        Value == instructionOpcode(InstructionOpcode::C_BNEZ)) {
      return Offset >= -256 && Offset <= 254;
    }
    if (Value == instructionOpcode(InstructionOpcode::C_J))
      return Offset >= -2048 && Offset <= 2046;
    if (isConditionalBranch(Value))
      return Offset >= -4096 && Offset <= 4094;
    if (Value == instructionOpcode(InstructionOpcode::JAL) ||
        Value == instructionOpcode(InstructionOpcode::PseudoBR)) {
      return Offset >= -1048576 && Offset <= 1048574;
    }
    return true;
  }

  [[nodiscard]] Expected<bool> relaxBranch(MachineInstr &Instruction, std::int64_t Offset,
                                           MachineFunction &Function,
                                           const TargetDescription &Target) const override {
    if (&Instruction.function() != &Function)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V branch relaxation references another function");
    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::C_J) ||
        Instruction.opcode() == instructionOpcode(InstructionOpcode::C_BEQZ) ||
        Instruction.opcode() == instructionOpcode(InstructionOpcode::C_BNEZ)) {
      return expandCompressedBranch(Instruction, Function, Target);
    }
    if (isConditionalBranch(Instruction.opcode()))
      return relaxConditionalBranch(Instruction, Function, Target);
    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::JAL))
      return relaxJAL(Instruction, Offset, Function, Target);
    return false;
  }

private:
  struct CopyShape final {
    RegisterBankID Bank;
    std::optional<std::uint32_t> Width;
  };

  [[nodiscard]] static Expected<CopyShape> copyShape(Register Reg, const MachineFunction &Function,
                                                     const TargetDescription &Target) {
    if (Reg.isVirtual()) {
      auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
      if (!Info)
        return std::unexpected(Info.error());
      const std::optional<RegisterBankID> &Bank = Info->get().Bank;
      const std::optional<RegisterClassID> &RegClass = Info->get().RegClass;
      if (!Bank || !RegClass)
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V COPY virtual register is not fully constrained");
      const RegisterClassDesc *Class = Target.registers().registerClass(*RegClass);
      if (Class == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "RISC-V COPY references an unknown register class");
      return CopyShape{*Bank, Class->SizeInBits};
    }

    const std::optional<PhysicalRegister> Physical = Reg.physicalRegister();
    if (!Physical || !Target.registers().isKnownPhysicalRegister(*Physical))
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V COPY references an unknown physical register");
    for (const RegisterBankDesc &Bank : Target.registers().registerBanks()) {
      if (Target.registers().bankContainsRegister(Bank.ID, *Physical))
        return CopyShape{Bank.ID, std::nullopt};
    }
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V physical register is not assigned to a register bank");
  }
};

} // namespace

const TargetInstrInfo &instrInfo() noexcept {
  static const RISCVInstrInfo Value;
  return Value;
}

} // namespace rcc::riscv

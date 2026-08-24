// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"

#include <memory>
#include <optional>
#include <utility>

namespace rcc::riscv {
namespace {

struct CopyOperands final {
  MachineOperand *Destination = nullptr;
  MachineOperand *Source = nullptr;
  Register DestinationRegister;
  Register SourceRegister;
  PhysicalRegister DestinationPhysical;
  PhysicalRegister SourcePhysical;
};

[[nodiscard]] std::optional<CopyOperands> copyOperands(MachineInstr &Instruction) {
  auto Operands = instrInfo().getCopyOperands(Instruction);
  if (!Operands || Operands->Destination == nullptr || Operands->Source == nullptr)
    return std::nullopt;
  auto Destination = Operands->Destination->registerValue();
  auto Source = Operands->Source->registerValue();
  if (!Destination || !Source)
    return std::nullopt;
  auto DestinationPhysical = Destination->physicalRegister();
  auto SourcePhysical = Source->physicalRegister();
  if (!DestinationPhysical || !SourcePhysical)
    return std::nullopt;
  return CopyOperands{
      .Destination = Operands->Destination,
      .Source = Operands->Source,
      .DestinationRegister = *Destination,
      .SourceRegister = *Source,
      .DestinationPhysical = *DestinationPhysical,
      .SourcePhysical = *SourcePhysical,
  };
}

[[nodiscard]] bool isRegister(PhysicalRegister RegisterValue,
                              PhysicalRegisterID Expected) noexcept {
  return RegisterValue.number() == static_cast<std::uint32_t>(Expected);
}

[[nodiscard]] bool isArgumentRegister(PhysicalRegister RegisterValue) noexcept {
  return isRegister(RegisterValue, PhysicalRegisterID::X10) ||
         isRegister(RegisterValue, PhysicalRegisterID::X11);
}

[[nodiscard]] bool isSavedRegister(PhysicalRegister RegisterValue,
                                   const TargetDescription &Target) noexcept {
  const RegisterClassID Class =
      RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::SR07));
  return Target.registers().registerClassContains(Class, RegisterValue);
}

[[nodiscard]] bool isMoveFromSavedToArgument(const CopyOperands &Copy,
                                             const TargetDescription &Target) noexcept {
  return isArgumentRegister(Copy.DestinationPhysical) &&
         isSavedRegister(Copy.SourcePhysical, Target);
}

[[nodiscard]] bool isMoveFromArgumentToSaved(const CopyOperands &Copy,
                                             const TargetDescription &Target) noexcept {
  return isArgumentRegister(Copy.SourcePhysical) &&
         isSavedRegister(Copy.DestinationPhysical, Target);
}

[[nodiscard]] bool operandOverlaps(const MachineOperand &Operand, PhysicalRegister RegisterValue,
                                   const TargetDescription &Target) noexcept {
  auto OperandRegister = Operand.registerValue();
  if (!OperandRegister)
    return false;
  auto OperandPhysical = OperandRegister->physicalRegister();
  return OperandPhysical && Target.registers().regsOverlap(*OperandPhysical, RegisterValue);
}

[[nodiscard]] bool instructionDefines(const MachineInstr &Instruction,
                                      PhysicalRegister RegisterValue,
                                      const TargetDescription &Target) noexcept {
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (Operand.isDef() && operandOverlaps(Operand, RegisterValue, Target))
      return true;
  }
  return false;
}

[[nodiscard]] bool instructionUses(const MachineInstr &Instruction, PhysicalRegister RegisterValue,
                                   const TargetDescription &Target) noexcept {
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (Operand.isUse() && !Operand.isDebugUse() && operandOverlaps(Operand, RegisterValue, Target))
      return true;
  }
  return false;
}

[[nodiscard]] bool hasHazard(MachineInstr &First, MachineInstr &Second,
                             const CopyOperands &SecondCopy,
                             const TargetDescription &Target) noexcept {
  for (MachineInstr *Instruction = First.next(); Instruction != &Second;
       Instruction = Instruction->next()) {
    if (Instruction == nullptr)
      return true;
    if (Instruction->isDebugInstr())
      continue;
    if (instructionDefines(*Instruction, SecondCopy.DestinationPhysical, Target) ||
        instructionUses(*Instruction, SecondCopy.DestinationPhysical, Target) ||
        instructionDefines(*Instruction, SecondCopy.SourcePhysical, Target)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool isReadBetween(MachineInstr &First, MachineInstr &Second,
                                 PhysicalRegister RegisterValue,
                                 const TargetDescription &Target) noexcept {
  for (MachineInstr *Instruction = First.next(); Instruction != &Second;
       Instruction = Instruction->next()) {
    if (Instruction == nullptr)
      return true;
    if (!Instruction->isDebugInstr() && instructionUses(*Instruction, RegisterValue, Target))
      return true;
  }
  return false;
}

[[nodiscard]] MachineInstr *findMatchingCopy(MachineInstr &First, const CopyOperands &FirstCopy,
                                             bool MoveFromSavedToArgument,
                                             const TargetDescription &Target) noexcept {
  for (MachineInstr *Instruction = First.next(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (Instruction->isDebugInstr())
      continue;
    auto SecondCopy = copyOperands(*Instruction);
    if (!SecondCopy)
      continue;
    const bool Candidate = MoveFromSavedToArgument ? isMoveFromSavedToArgument(*SecondCopy, Target)
                                                   : isMoveFromArgumentToSaved(*SecondCopy, Target);
    if (!Candidate)
      continue;
    if (FirstCopy.DestinationPhysical == SecondCopy->DestinationPhysical)
      return nullptr;
    if (!MoveFromSavedToArgument && FirstCopy.SourcePhysical == SecondCopy->SourcePhysical)
      return nullptr;
    if (hasHazard(First, *Instruction, *SecondCopy, Target))
      return nullptr;
    return Instruction;
  }
  return nullptr;
}

[[nodiscard]] Expected<MachineInstr *> mergeCopies(MachineFunction &Function, MachineInstr &First,
                                                   MachineInstr &Second,
                                                   bool MoveFromSavedToArgument,
                                                   const TargetDescription &Target) {
  auto FirstCopy = copyOperands(First);
  auto SecondCopy = copyOperands(Second);
  if (!FirstCopy || !SecondCopy) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V move merger lost a recognized copy instruction");
  }

  MachineOperand *FirstOperand =
      MoveFromSavedToArgument ? FirstCopy->Source : FirstCopy->Destination;
  MachineOperand *SecondOperand =
      MoveFromSavedToArgument ? SecondCopy->Source : SecondCopy->Destination;
  if (MoveFromSavedToArgument ? !isRegister(FirstCopy->DestinationPhysical, PhysicalRegisterID::X10)
                              : !isRegister(FirstCopy->SourcePhysical, PhysicalRegisterID::X10)) {
    std::swap(FirstOperand, SecondOperand);
  }

  RegisterOperandFlags FirstFlags = FirstOperand->registerFlags();
  RegisterOperandFlags SecondFlags = SecondOperand->registerFlags();
  if (MoveFromSavedToArgument && SecondFlags.has(RegisterOperandFlag::Kill) &&
      isReadBetween(First, Second, *SecondOperand->registerValue()->physicalRegister(), Target)) {
    SecondFlags.clear(RegisterOperandFlag::Kill);
  }

  MachineIRBuilder Builder(Function, Target);
  auto InsertPoint = Builder.setInsertPointBefore(First);
  if (!InsertPoint)
    return std::unexpected(InsertPoint.error());
  const InstructionOpcode Opcode =
      MoveFromSavedToArgument ? InstructionOpcode::CM_MVA01S : InstructionOpcode::CM_MVSA01;
  auto Pending = Builder.buildInstr(instructionOpcode(Opcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  Expected<void> Added = MoveFromSavedToArgument
                             ? Pending->addUse(*FirstOperand->registerValue(), FirstFlags)
                             : Pending->addDef(*FirstOperand->registerValue(), FirstFlags);
  if (!Added)
    return std::unexpected(Added.error());
  Added = MoveFromSavedToArgument ? Pending->addUse(*SecondOperand->registerValue(), SecondFlags)
                                  : Pending->addDef(*SecondOperand->registerValue(), SecondFlags);
  if (!Added)
    return std::unexpected(Added.error());
  return Pending->build();
}

[[nodiscard]] Expected<bool> mergeBlock(MachineFunction &Function, MachineBasicBlock &Block,
                                        const TargetDescription &Target) {
  bool Changed = false;
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;) {
    MachineInstr *Next = Instruction->next();
    if (Instruction->isDebugInstr()) {
      Instruction = Next;
      continue;
    }
    auto FirstCopy = copyOperands(*Instruction);
    if (!FirstCopy) {
      Instruction = Next;
      continue;
    }
    const bool MoveFromSavedToArgument = isMoveFromSavedToArgument(*FirstCopy, Target);
    const bool MoveFromArgumentToSaved = isMoveFromArgumentToSaved(*FirstCopy, Target);
    if (!MoveFromSavedToArgument && !MoveFromArgumentToSaved) {
      Instruction = Next;
      continue;
    }
    MachineInstr *Paired =
        findMatchingCopy(*Instruction, *FirstCopy, MoveFromSavedToArgument, Target);
    if (Paired == nullptr) {
      Instruction = Next;
      continue;
    }
    MachineInstr *AfterPair = Paired->next();
    auto Merged = mergeCopies(Function, *Instruction, *Paired, MoveFromSavedToArgument, Target);
    if (!Merged)
      return std::unexpected(Merged.error());
    auto RemovedFirst = Block.removeInstruction(*Instruction);
    if (!RemovedFirst)
      return std::unexpected(RemovedFirst.error());
    auto RemovedPaired = Block.removeInstruction(*Paired);
    if (!RemovedPaired)
      return std::unexpected(RemovedPaired.error());
    Changed = true;
    Instruction = Next == Paired ? AfterPair : Next;
  }
  return Changed;
}

class RISCVMoveMergePass final : public MachineFunctionPass {
public:
  explicit RISCVMoveMergePass(const RISCVSubtarget &Subtarget) noexcept : Subtarget(&Subtarget) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override {
    static const MachinePassContract Contract = [] {
      MachinePassContract Value{
          .Name = "riscv-move-merge",
          .Requires = {},
          .Sets = {},
          .Clears = {},
          .RequiredAnalyses = {},
          .VerificationStage = std::nullopt,
      };
      Value.ThreadSafety = PassThreadSafety::ThreadSafe;
      Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                               MachinePassChange::Registers};
      return Value;
    }();
    return Contract;
  }

  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &) override {
    MachinePassResult Result;
    Result.Preserved = PreservedAnalyses::all();
    if (Subtarget == nullptr || !Subtarget->features().has(RISCVExtension::Zcmp))
      return Result;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      auto BlockResult = mergeBlock(Function, *Block, Target);
      if (!BlockResult)
        return std::unexpected(BlockResult.error());
      Result.Changed |= *BlockResult;
    }
    if (Result.Changed) {
      Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                        MachinePassChange::Registers};
      Result.Preserved = PreservedAnalyses::none();
    }
    return Result;
  }

private:
  const RISCVSubtarget *Subtarget = nullptr;
};

} // namespace

std::unique_ptr<MachineFunctionPass> createMoveMergePass(const RISCVSubtarget &Subtarget) {
  return std::make_unique<RISCVMoveMergePass>(Subtarget);
}

} // namespace rcc::riscv

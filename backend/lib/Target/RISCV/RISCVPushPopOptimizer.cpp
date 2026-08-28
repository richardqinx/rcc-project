// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LiveRegUnits.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace rcc::riscv {
namespace {

struct PushPopOpcodes final {
  Opcode Pop;
  Opcode PopRet;
  Opcode PopRetZero;
};

[[nodiscard]] const MachinePassContract &pushPopContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "riscv-push-pop-opt",
        .Requires = {MachineFunctionProperty::NoPHIs, MachineFunctionProperty::NoVRegs,
                     MachineFunctionProperty::FrameFinalized,
                     MachineFunctionProperty::NoFrameIndices},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::PostRAFinalized,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] MachineInstr *previousNonDebug(MachineInstr &Instruction) noexcept {
  MachineInstr *Previous = Instruction.previous();
  while (Previous != nullptr && Previous->isDebugInstr())
    Previous = Previous->previous();
  return Previous;
}

[[nodiscard]] MachineInstr *firstTerminator(MachineBasicBlock &Block,
                                            const TargetDescription &Target) noexcept {
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Terminator))
      return Instruction;
  }
  return nullptr;
}

[[nodiscard]] Expected<bool> removeZeroReturnAssignment(MachineInstr &Pop,
                                                        const TargetDescription &Target,
                                                        PhysicalRegister ReturnRegister,
                                                        PhysicalRegister ZeroRegister) {
  LiveRegUnits Modified;
  LiveRegUnits Used;
  for (MachineInstr *Instruction = previousNonDebug(Pop); Instruction != nullptr;
       Instruction = previousNonDebug(*Instruction)) {
    if (const std::optional<MachineCopyOperands> Copy = instrInfo().getCopyOperands(*Instruction)) {
      const std::optional<Register> Destination = Copy->Destination->registerValue();
      const std::optional<Register> Source = Copy->Source->registerValue();
      if (Destination && Source && Destination->physicalRegister() == ReturnRegister &&
          Source->physicalRegister() == ZeroRegister) {
        if (auto Removed = Instruction->eraseFromParent(); !Removed)
          return std::unexpected(Removed.error());
        return true;
      }
    }
    if (auto Accumulated = LiveRegUnits::accumulateUsedDefed(*Instruction, Modified, Used, Target);
        !Accumulated) {
      return std::unexpected(Accumulated.error());
    }
    auto ModifiedAvailable = Modified.available(ReturnRegister, Target);
    if (!ModifiedAvailable)
      return std::unexpected(ModifiedAvailable.error());
    auto UsedAvailable = Used.available(ReturnRegister, Target);
    if (!UsedAvailable)
      return std::unexpected(UsedAvailable.error());
    if (!*ModifiedAvailable || !*UsedAvailable)
      return false;
  }
  return false;
}

[[nodiscard]] Expected<MachineInstr *> buildPopReturn(MachineInstr &Pop, MachineInstr &Return,
                                                      bool ReturnZero,
                                                      const PushPopOpcodes &Opcodes,
                                                      MachineFunction &Function,
                                                      const TargetDescription &Target) {
  const InstructionDescriptor *PopDescriptor = Target.instructionDescriptor(Pop.opcode());
  if (PopDescriptor == nullptr || PopDescriptor->Operands.size() != 2U)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V CM_POP has an invalid instruction descriptor");
  MachineIRBuilder Builder(Function, Target);
  if (auto Point = Builder.setInsertPointBefore(Return); !Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(ReturnZero ? Opcodes.PopRetZero : Opcodes.PopRet);
  if (!Pending)
    return std::unexpected(Pending.error());
  for (std::size_t Index = 0; Index < PopDescriptor->Operands.size(); ++Index) {
    const MachineOperand *Operand = Pop.operandAt(Index);
    if (Operand == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V CM_POP is missing an explicit operand");
    if (auto Added = Pending->addOperand(Operand->cloneDetached()); !Added)
      return std::unexpected(Added.error());
  }
  const std::size_t FirstDynamicImplicit = PopDescriptor->Operands.size() +
                                           PopDescriptor->ImplicitDefs.size() +
                                           PopDescriptor->ImplicitUses.size();
  for (std::size_t Index = FirstDynamicImplicit; Index < Pop.operandCount(); ++Index) {
    const MachineOperand *Operand = Pop.operandAt(Index);
    if (Operand == nullptr || !Operand->isImplicit()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V CM_POP has a malformed dynamic implicit operand");
    }
    if (auto Added = Pending->addImplicitOperand(Operand->cloneDetached()); !Added)
      return std::unexpected(Added.error());
  }
  if (auto Flags = Pending->setFlags({MachineInstrFlag::FrameDestroy}); !Flags)
    return std::unexpected(Flags.error());
  if (Return.debugLocation()) {
    if (auto Location = Pending->setDebugLocation(*Return.debugLocation()); !Location)
      return std::unexpected(Location.error());
  }
  return Pending->build();
}

class RISCVPushPopOptimizationPass final : public MachineFunctionPass {
public:
  explicit RISCVPushPopOptimizationPass(const RISCVSubtarget &Subtarget) noexcept
      : Subtarget(&Subtarget) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override {
    return pushPopContract();
  }

  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &) override {
    MachinePassResult Result;
    if (!Subtarget->features().has(RISCVExtension::Zcmp)) {
      Result.Preserved = PreservedAnalyses::all();
      return Result;
    }

    const InstructionDescriptor *Pop = Target.instructions().descriptor("CM_POP");
    const InstructionDescriptor *PopRet = Target.instructions().descriptor("CM_POPRET");
    const InstructionDescriptor *PopRetZero = Target.instructions().descriptor("CM_POPRETZ");
    if (Pop == nullptr || PopRet == nullptr || PopRetZero == nullptr) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V Zcmp push/pop instructions are not registered");
    }
    auto ReturnRegister = physicalRegister(PhysicalRegisterID::X10);
    if (!ReturnRegister)
      return std::unexpected(ReturnRegister.error());
    auto ZeroRegister = physicalRegister(PhysicalRegisterID::X0);
    if (!ZeroRegister)
      return std::unexpected(ZeroRegister.error());
    const PushPopOpcodes Opcodes{Pop->InstructionOpcode, PopRet->InstructionOpcode,
                                 PopRetZero->InstructionOpcode};

    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      MachineInstr *Return = firstTerminator(*Block, Target);
      if (Return == nullptr ||
          Return->opcode() != instructionOpcode(InstructionOpcode::PseudoRET)) {
        continue;
      }
      MachineInstr *PopInstruction = previousNonDebug(*Return);
      if (PopInstruction == nullptr || PopInstruction->opcode() != Opcodes.Pop ||
          !PopInstruction->flags().has(MachineInstrFlag::FrameDestroy)) {
        continue;
      }
      auto ReturnZero =
          removeZeroReturnAssignment(*PopInstruction, Target, *ReturnRegister, *ZeroRegister);
      if (!ReturnZero)
        return std::unexpected(ReturnZero.error());
      auto NewReturn =
          buildPopReturn(*PopInstruction, *Return, *ReturnZero, Opcodes, Function, Target);
      if (!NewReturn)
        return std::unexpected(NewReturn.error());
      if (auto Removed = PopInstruction->eraseFromParent(); !Removed)
        return std::unexpected(Removed.error());
      if (auto Removed = Return->eraseFromParent(); !Removed)
        return std::unexpected(Removed.error());
      Result.Changes.add(MachinePassChange::Instructions);
      Result.Changes.add(MachinePassChange::Operands);
    }
    Result.Changed = !Result.Changes.empty();
    Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    return Result;
  }

private:
  const RISCVSubtarget *Subtarget;
};

} // namespace

std::unique_ptr<MachineFunctionPass>
createPushPopOptimizationPass(const RISCVSubtarget &Subtarget) {
  return std::make_unique<RISCVPushPopOptimizationPass>(Subtarget);
}

} // namespace rcc::riscv

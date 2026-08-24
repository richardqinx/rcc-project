// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineOutliner.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVPipeline.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

constexpr std::uint32_t CallTargetFlag = 1;
constexpr std::uint32_t PCRelativeLowTargetFlag = 4;

enum class OutlinerConstruction : std::uint32_t {
  TailCall,
  Default,
  RegisterSave,
};

[[nodiscard]] bool isUnsafeOperand(const MachineOperand &Operand) noexcept {
  switch (Operand.kind()) {
  case MachineOperand::Kind::BasicBlock:
  case MachineOperand::Kind::FrameIndex:
  case MachineOperand::Kind::ConstantPoolIndex:
  case MachineOperand::Kind::JumpTableIndex:
  case MachineOperand::Kind::BlockAddress:
  case MachineOperand::Kind::TargetIndex:
    return true;
  case MachineOperand::Kind::Register: {
    const auto Reg = Operand.registerValue();
    return Reg && Reg->isVirtual();
  }
  case MachineOperand::Kind::Immediate:
  case MachineOperand::Kind::IntegerImmediate:
  case MachineOperand::Kind::FloatingImmediate:
  case MachineOperand::Kind::GlobalSymbol:
  case MachineOperand::Kind::ExternalSymbol:
  case MachineOperand::Kind::RegisterMask:
  case MachineOperand::Kind::LiveOutRegisterMask:
  case MachineOperand::Kind::Metadata:
  case MachineOperand::Kind::MCSymbol:
  case MachineOperand::Kind::CFIIndex:
  case MachineOperand::Kind::Intrinsic:
  case MachineOperand::Kind::Predicate:
  case MachineOperand::Kind::ShuffleMask:
  case MachineOperand::Kind::DebugInstrRef:
  case MachineOperand::Kind::LaneMask:
    return false;
  case MachineOperand::Kind::Count:
    return true;
  }
  return true;
}

[[nodiscard]] std::vector<PhysicalRegister>
sortedRegisters(std::span<const PhysicalRegister> Registers) {
  std::vector<PhysicalRegister> Result(Registers.begin(), Registers.end());
  std::ranges::sort(Result, {}, &PhysicalRegister::number);
  Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
  return Result;
}

class RISCVOutlinerInfo final : public TargetMachineOutlinerInfo {
public:
  RISCVOutlinerInfo(bool FunctionSections, bool EnableRegisterSave,
                    std::array<PhysicalRegister, 9> Registers)
      : FunctionSections(FunctionSections), EnableRegisterSave(EnableRegisterSave),
        X0(Registers[0]), X2(Registers[1]), X5(Registers[2]), X6(Registers[3]),
        SaveRegisters{Registers[3], Registers[4], Registers[5],
                      Registers[6], Registers[7], Registers[8]} {}

  [[nodiscard]] bool isFunctionSafeToOutlineFrom(
      const MachineFunction &,
      const MachineOutlinerFunctionProperties &Properties) const noexcept override {
    return !Properties.LinkOnceODR && !Properties.HasSection;
  }

  [[nodiscard]] MachineOutlinerInstrType
  getOutliningType(const MachineInstr &Instruction,
                   const MachineOutlinerFunctionProperties &Properties,
                   const TargetDescription &Target) const noexcept override {
    if (Instruction.isDebugInstr() || Instruction.opcode() == opcode(GenericOpcode::EH_LABEL) ||
        Instruction.opcode() == opcode(GenericOpcode::GC_LABEL) ||
        Instruction.opcode() == opcode(GenericOpcode::ANNOTATION_LABEL)) {
      return MachineOutlinerInstrType::Invisible;
    }
    if (Instruction.opcode() == opcode(GenericOpcode::CFI_INSTRUCTION))
      return MachineOutlinerInstrType::Legal;
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
    if (Descriptor == nullptr)
      return MachineOutlinerInstrType::Illegal;
    for (const MachineOperand &Operand : Instruction.operands()) {
      if (isUnsafeOperand(Operand))
        return MachineOutlinerInstrType::Illegal;
      if ((Operand.targetFlags() & PCRelativeLowTargetFlag) != 0 &&
          (FunctionSections || Properties.HasComdat || Properties.HasSection ||
           Properties.HasSectionPrefix)) {
        return MachineOutlinerInstrType::Illegal;
      }
    }
    if (Descriptor->Flags.has(InstructionFlag::Return))
      return MachineOutlinerInstrType::LegalTerminator;
    if (Descriptor->Flags.has(InstructionFlag::Terminator) ||
        Descriptor->Flags.has(InstructionFlag::Branch) ||
        Descriptor->Flags.has(InstructionFlag::Call) ||
        Descriptor->Flags.has(InstructionFlag::NotDuplicable) ||
        Descriptor->Flags.has(InstructionFlag::Convergent) ||
        Descriptor->Flags.has(InstructionFlag::UnmodeledSideEffects)) {
      return MachineOutlinerInstrType::Illegal;
    }
    return MachineOutlinerInstrType::Legal;
  }

  [[nodiscard]] Expected<bool> analyzeCandidate(MachineOutlinerCandidate &Candidate,
                                                const TargetDescription &Target) const override {
    std::size_t CandidateCFICount = 0;
    for (const MachineInstr *Instruction : Candidate.Instructions) {
      if (Instruction->opcode() == opcode(GenericOpcode::CFI_INSTRUCTION))
        ++CandidateCFICount;
    }

    const InstructionDescriptor *LastDescriptor =
        Target.instructionDescriptor(Candidate.back().opcode());
    const bool IsTail =
        LastDescriptor != nullptr && LastDescriptor->Flags.has(InstructionFlag::Return);
    if (CandidateCFICount != 0 &&
        (!IsTail || CandidateCFICount != Candidate.Function->cfiInfo().instructions().size())) {
      return false;
    }
    if (IsTail) {
      if (!Candidate.isRegisterAvailable(X6, Target))
        return false;
      Candidate.CallConstructionID = static_cast<std::uint32_t>(OutlinerConstruction::TailCall);
      Candidate.CallOverhead = 8;
      return true;
    }

    if (!Candidate.isRegisterAvailable(X5, Target)) {
      if (Candidate.containsRegister(X5, Target) || !EnableRegisterSave)
        return false;
      for (const PhysicalRegister Reg : SaveRegisters) {
        if (Candidate.isRegisterAvailable(Reg, Target)) {
          Candidate.SaveRegister = Reg;
          Candidate.CallConstructionID =
              static_cast<std::uint32_t>(OutlinerConstruction::RegisterSave);
          Candidate.CallOverhead = 16;
          return true;
        }
      }
      return false;
    }
    Candidate.CallConstructionID = static_cast<std::uint32_t>(OutlinerConstruction::Default);
    Candidate.CallOverhead = 8;
    return true;
  }

  [[nodiscard]] Expected<std::uint32_t>
  instructionSize(const MachineInstr &Instruction, const MachineFunction &Function,
                  const TargetDescription &Target) const override {
    return instrInfo().instructionSize(Instruction, Function, Target);
  }

  [[nodiscard]] std::uint32_t
  frameOverhead(const MachineOutlinerCandidate &Candidate) const noexcept override {
    return Candidate.CallConstructionID ==
                   static_cast<std::uint32_t>(OutlinerConstruction::TailCall)
               ? 0
               : 4;
  }

  [[nodiscard]] Expected<void> insertOutlinedCall(MachineOutlinerCandidate &Candidate,
                                                  std::string OutlinedFunctionName,
                                                  const TargetDescription &Target) const override {
    MachineIRBuilder Builder(*Candidate.Function, Target);
    auto InsertPoint = Builder.setInsertPointBefore(Candidate.front());
    if (!InsertPoint)
      return std::unexpected(InsertPoint.error());
    const auto Construction = static_cast<OutlinerConstruction>(Candidate.CallConstructionID);
    const Register StackPointer = Register::fromPhysical(X2);
    const Register Link = Register::fromPhysical(X5);

    if (Construction == OutlinerConstruction::TailCall) {
      auto Pending = Builder.buildInstr(instructionOpcode(InstructionOpcode::PseudoTAIL));
      if (!Pending)
        return std::unexpected(Pending.error());
      if (auto Added = Pending->addGlobalSymbol(std::move(OutlinedFunctionName), 0, CallTargetFlag);
          !Added) {
        return std::unexpected(Added.error());
      }
      if (auto Added = Pending->addImplicitUse(StackPointer); !Added)
        return std::unexpected(Added.error());
      for (const PhysicalRegister Reg : Candidate.DefinedRegisters) {
        if (auto Added = Pending->addImplicitDef(Register::fromPhysical(Reg)); !Added)
          return std::unexpected(Added.error());
      }
      if (auto Added = Pending->addImplicitUse(StackPointer); !Added)
        return std::unexpected(Added.error());
      for (const PhysicalRegister Reg : sortedRegisters(Candidate.LiveIns)) {
        if (auto Added = Pending->addImplicitUse(Register::fromPhysical(Reg)); !Added)
          return std::unexpected(Added.error());
      }
      auto Built = Pending->build();
      return Built ? Expected<void>{} : std::unexpected(Built.error());
    }

    if (Construction == OutlinerConstruction::RegisterSave) {
      auto Save = Builder.buildInstr(instructionOpcode(InstructionOpcode::ADDI));
      if (!Save)
        return std::unexpected(Save.error());
      if (auto Added = Save->addDef(Register::fromPhysical(Candidate.SaveRegister)); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Save->addUse(Link); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Save->addImmediate(0); !Added)
        return std::unexpected(Added.error());
      auto Built = Save->build();
      if (!Built)
        return std::unexpected(Built.error());
    }

    auto Call = Builder.buildInstr(instructionOpcode(InstructionOpcode::PseudoCALLReg));
    if (!Call)
      return std::unexpected(Call.error());
    if (auto Added = Call->addDef(Link); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Call->addGlobalSymbol(std::move(OutlinedFunctionName), 0, CallTargetFlag);
        !Added) {
      return std::unexpected(Added.error());
    }
    if (auto Added = Call->addImplicitDef(Link); !Added)
      return std::unexpected(Added.error());
    for (const PhysicalRegister Reg : Candidate.DefinedRegisters) {
      if (Reg == X5)
        continue;
      if (auto Added = Call->addImplicitDef(Register::fromPhysical(Reg)); !Added)
        return std::unexpected(Added.error());
    }
    std::vector<PhysicalRegister> Uses = Candidate.LiveIns;
    if (Construction == OutlinerConstruction::RegisterSave)
      Uses.push_back(Candidate.SaveRegister);
    for (const PhysicalRegister Reg : sortedRegisters(Uses)) {
      if (auto Added = Call->addImplicitUse(Register::fromPhysical(Reg)); !Added)
        return std::unexpected(Added.error());
    }
    auto BuiltCall = Call->build();
    if (!BuiltCall)
      return std::unexpected(BuiltCall.error());

    if (Construction == OutlinerConstruction::RegisterSave) {
      auto Restore = Builder.buildInstr(instructionOpcode(InstructionOpcode::ADDI));
      if (!Restore)
        return std::unexpected(Restore.error());
      if (auto Added = Restore->addDef(Link); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Restore->addUse(Register::fromPhysical(Candidate.SaveRegister)); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Restore->addImmediate(0); !Added)
        return std::unexpected(Added.error());
      auto Built = Restore->build();
      if (!Built)
        return std::unexpected(Built.error());
    }
    return {};
  }

  [[nodiscard]] Expected<void> buildOutlinedFrame(MachineFunction &Function,
                                                  MachineBasicBlock &Block,
                                                  const MachineOutlinerCandidate &Candidate,
                                                  const TargetDescription &Target) const override {
    if (Candidate.CallConstructionID ==
        static_cast<std::uint32_t>(OutlinerConstruction::TailCall)) {
      return {};
    }
    auto AddedLiveIn = Block.addLiveIn(X5);
    if (!AddedLiveIn)
      return std::unexpected(AddedLiveIn.error());
    MachineIRBuilder Builder(Function, Target);
    auto InsertPoint = Builder.setInsertPoint(Block);
    if (!InsertPoint)
      return std::unexpected(InsertPoint.error());
    auto Return = Builder.buildInstr(instructionOpcode(InstructionOpcode::JALR));
    if (!Return)
      return std::unexpected(Return.error());
    if (auto Added = Return->addDef(Register::fromPhysical(X0)); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Return->addUse(Register::fromPhysical(X5)); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Return->addImmediate(0); !Added)
      return std::unexpected(Added.error());
    auto Built = Return->build();
    return Built ? Expected<void>{} : std::unexpected(Built.error());
  }

private:
  bool FunctionSections = false;
  bool EnableRegisterSave = false;
  PhysicalRegister X0;
  PhysicalRegister X2;
  PhysicalRegister X5;
  PhysicalRegister X6;
  std::array<PhysicalRegister, 6> SaveRegisters;
};

} // namespace

Expected<std::unique_ptr<TargetMachineOutlinerInfo>>
createMachineOutlinerInfo(bool FunctionSections, bool EnableRegisterSave) {
  constexpr std::array IDs{
      PhysicalRegisterID::X0,  PhysicalRegisterID::X2,  PhysicalRegisterID::X5,
      PhysicalRegisterID::X6,  PhysicalRegisterID::X7,  PhysicalRegisterID::X28,
      PhysicalRegisterID::X29, PhysicalRegisterID::X30, PhysicalRegisterID::X31,
  };
  std::array<PhysicalRegister, IDs.size()> Registers;
  for (std::size_t Index = 0; Index < IDs.size(); ++Index) {
    auto Reg = physicalRegister(IDs[Index]);
    if (!Reg)
      return std::unexpected(Reg.error());
    Registers[Index] = *Reg;
  }
  return std::make_unique<RISCVOutlinerInfo>(FunctionSections, EnableRegisterSave, Registers);
}

} // namespace rcc::riscv

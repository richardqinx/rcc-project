// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionAttributes.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/RegisterScavenger.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

struct RegImmPair final {
  Register Reg;
  std::int64_t Imm = 0;
};

[[nodiscard]] constexpr RegisterClassID registerClassID(RegisterClass Value) noexcept {
  return RegisterClassID::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] PhysicalRegister physicalRegister(PhysicalRegisterID Value) noexcept {
  const auto Created = PhysicalRegister::create(static_cast<std::uint32_t>(Value));
  return *Created;
}

[[nodiscard]] bool isOpcode(const MachineInstr &Instruction, InstructionOpcode Value) noexcept {
  return Instruction.opcode() == instructionOpcode(Value);
}

[[nodiscard]] std::optional<PhysicalRegister>
physicalRegisterValue(const MachineOperand *Operand) noexcept {
  const auto Reg = Operand == nullptr ? std::nullopt : Operand->registerValue();
  return Reg ? Reg->physicalRegister() : std::nullopt;
}

[[nodiscard]] bool classContains(RegisterClass Class, Register Reg,
                                 const TargetDescription &Target) noexcept {
  const auto Physical = Reg.physicalRegister();
  return Physical && Target.registers().registerClassContains(registerClassID(Class), *Physical);
}

[[nodiscard]] bool isCompressedRegister(Register Reg, const TargetDescription &Target) noexcept {
  return classContains(RegisterClass::GPRC, Reg, Target) ||
         classContains(RegisterClass::FPR32C, Reg, Target) ||
         classContains(RegisterClass::FPR64C, Reg, Target);
}

[[nodiscard]] bool isCompressibleLoad(const MachineInstr &Instruction,
                                      const RISCVSubtarget &Subtarget) noexcept {
  if (!Subtarget.features().has(RISCVExtension::C))
    return false;
  if (isOpcode(Instruction, InstructionOpcode::LW) ||
      isOpcode(Instruction, InstructionOpcode::LD)) {
    return true;
  }
  return isOpcode(Instruction, InstructionOpcode::FLD) &&
         Subtarget.features().has(RISCVExtension::D);
}

[[nodiscard]] bool isCompressibleStore(const MachineInstr &Instruction,
                                       const RISCVSubtarget &Subtarget) noexcept {
  if (!Subtarget.features().has(RISCVExtension::C))
    return false;
  if (isOpcode(Instruction, InstructionOpcode::SW) ||
      isOpcode(Instruction, InstructionOpcode::SD)) {
    return true;
  }
  return isOpcode(Instruction, InstructionOpcode::FSD) &&
         Subtarget.features().has(RISCVExtension::D);
}

[[nodiscard]] std::optional<unsigned> loadStoreWidth(const MachineInstr &Instruction) noexcept {
  if (isOpcode(Instruction, InstructionOpcode::LW) ||
      isOpcode(Instruction, InstructionOpcode::SW)) {
    return 2U;
  }
  if (isOpcode(Instruction, InstructionOpcode::LD) ||
      isOpcode(Instruction, InstructionOpcode::SD) ||
      isOpcode(Instruction, InstructionOpcode::FLD) ||
      isOpcode(Instruction, InstructionOpcode::FSD)) {
    return 3U;
  }
  return std::nullopt;
}

[[nodiscard]] std::int64_t compressedOffsetMask(const MachineInstr &Instruction) noexcept {
  const std::optional<unsigned> Width = loadStoreWidth(Instruction);
  if (!Width)
    return 0;
  return static_cast<std::int64_t>(std::uint64_t{0x1f} << *Width);
}

[[nodiscard]] bool isCompressibleStackOffset(std::int64_t Offset,
                                             const MachineInstr &Instruction) noexcept {
  const std::optional<unsigned> Width = loadStoreWidth(Instruction);
  if (!Width || Offset < 0)
    return false;
  const std::uint64_t Value = static_cast<std::uint64_t>(Offset);
  const std::uint64_t AlignmentMask = (std::uint64_t{1} << *Width) - 1U;
  return (Value & AlignmentMask) == 0 && (Value >> *Width) < 64U;
}

[[nodiscard]] std::optional<RegImmPair>
regImmPreventingCompression(MachineInstr &Instruction, const RISCVSubtarget &Subtarget,
                            const TargetDescription &Target) noexcept {
  const bool IsLoad = isCompressibleLoad(Instruction, Subtarget);
  const bool IsStore = isCompressibleStore(Instruction, Subtarget);
  if (!IsLoad && !IsStore)
    return std::nullopt;

  MachineOperand *ValueOperand = Instruction.operandAt(0);
  MachineOperand *BaseOperand = Instruction.operandAt(1);
  MachineOperand *OffsetOperand = Instruction.operandAt(2);
  const auto Value = physicalRegisterValue(ValueOperand);
  const auto Base = physicalRegisterValue(BaseOperand);
  const auto *Offset = OffsetOperand == nullptr ? nullptr : OffsetOperand->getIf<std::int64_t>();
  if (!Value || !Base || Offset == nullptr)
    return std::nullopt;

  const std::int64_t Mask = compressedOffsetMask(Instruction);
  const std::int64_t BaseAdjustment = *Offset & ~Mask;
  const Register BaseRegister = Register::fromPhysical(*Base);
  if (*Base == physicalRegister(PhysicalRegisterID::X2)) {
    if (!isCompressibleStackOffset(*Offset, Instruction) && BaseAdjustment != 0)
      return RegImmPair{BaseRegister, BaseAdjustment};
    return std::nullopt;
  }

  const Register ValueRegister = Register::fromPhysical(*Value);
  const bool ValueCompressed = isCompressedRegister(ValueRegister, Target);
  const bool BaseCompressed = isCompressedRegister(BaseRegister, Target);
  if ((!BaseCompressed || BaseAdjustment != 0) && ValueCompressed)
    return RegImmPair{BaseRegister, BaseAdjustment};
  if (IsStore && !ValueCompressed && (BaseCompressed || *Value == *Base) && BaseAdjustment == 0) {
    return RegImmPair{ValueRegister, 0};
  }
  return std::nullopt;
}

[[nodiscard]] bool maskClobbers(const RegisterMask &Mask, PhysicalRegister Reg,
                                const TargetDescription &Target) noexcept {
  for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
    if (Target.registers().regsOverlap(Description.Reg, Reg) && Mask.clobbers(Description.Reg))
      return true;
  }
  return false;
}

[[nodiscard]] bool instructionModifies(const MachineInstr &Instruction, Register Reg,
                                       const TargetDescription &Target) noexcept {
  const auto Physical = Reg.physicalRegister();
  if (!Physical)
    return false;
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (const auto OperandPhysical = physicalRegisterValue(&Operand);
        OperandPhysical && Operand.isDef() &&
        Target.registers().regsOverlap(*OperandPhysical, *Physical)) {
      return true;
    }
    const auto *Mask = Operand.getIf<RegisterMaskOperand>();
    if (Mask != nullptr && Mask->Mask && maskClobbers(*Mask->Mask, *Physical, Target))
      return true;
  }
  return false;
}

[[nodiscard]] bool instructionTouches(const MachineInstr &Instruction, PhysicalRegister Reg,
                                      const TargetDescription &Target) noexcept {
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (const auto OperandPhysical = physicalRegisterValue(&Operand);
        OperandPhysical && Target.registers().regsOverlap(*OperandPhysical, Reg)) {
      return true;
    }
    const auto *Mask = Operand.getIf<RegisterMaskOperand>();
    if (Mask != nullptr && Mask->Mask && maskClobbers(*Mask->Mask, Reg, Target))
      return true;
  }
  return false;
}

[[nodiscard]] std::optional<RegisterClass>
compressedClassFor(Register Reg, const TargetDescription &Target) noexcept {
  if (classContains(RegisterClass::GPR, Reg, Target))
    return RegisterClass::GPRC;
  if (classContains(RegisterClass::FPR32, Reg, Target))
    return RegisterClass::FPR32C;
  if (classContains(RegisterClass::FPR64, Reg, Target))
    return RegisterClass::FPR64C;
  return std::nullopt;
}

[[nodiscard]] Expected<std::optional<PhysicalRegister>>
scavengeCompressedRegister(MachineFunction &Function, MachineInstr &First, MachineInstr &Last,
                           Register OldReg, const TargetDescription &Target) {
  const auto Class = compressedClassFor(OldReg, Target);
  if (!Class)
    return std::optional<PhysicalRegister>{};
  const RegisterClassDesc *Description = Target.registers().registerClass(registerClassID(*Class));
  if (Description == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V compressed register class is unavailable");

  std::vector<PhysicalRegister> Candidates;
  for (PhysicalRegister Candidate : Description->AllocationOrder) {
    bool Touched = false;
    for (MachineInstr *Instruction = &First; Instruction != nullptr;
         Instruction = Instruction->next()) {
      Touched |= instructionTouches(*Instruction, Candidate, Target);
      if (Instruction == &Last)
        break;
    }
    if (!Touched)
      Candidates.push_back(Candidate);
  }
  if (Candidates.empty())
    return std::optional<PhysicalRegister>{};

  RegisterScavenger Scavenger(Function, Target);
  auto Scavenged = Scavenger.scavenge(First, Candidates);
  if (!Scavenged) {
    if (Scavenged.error().code() == ErrorCode::RegisterAllocationFailed)
      return std::optional<PhysicalRegister>{};
    return std::unexpected(Scavenged.error());
  }
  if (Scavenged->RequiresSpill)
    return std::optional<PhysicalRegister>{};
  return std::optional<PhysicalRegister>(Scavenged->Reg);
}

[[nodiscard]] Expected<std::optional<PhysicalRegister>>
analyzeCompressibleUses(MachineFunction &Function, MachineInstr &First, RegImmPair RegImm,
                        std::vector<MachineInstr *> &Instructions, const RISCVSubtarget &Subtarget,
                        const TargetDescription &Target) {
  for (MachineInstr *Instruction = &First; Instruction != nullptr;
       Instruction = Instruction->next()) {
    const auto Candidate = regImmPreventingCompression(*Instruction, Subtarget, Target);
    if (Candidate && Candidate->Reg == RegImm.Reg && Candidate->Imm == RegImm.Imm)
      Instructions.push_back(Instruction);
    if (instructionModifies(*Instruction, RegImm.Reg, Target))
      break;
  }

  if (Instructions.size() <= 1U || (RegImm.Imm != 0 && Instructions.size() < 3U))
    return std::optional<PhysicalRegister>{};
  return scavengeCompressedRegister(Function, First, *Instructions.back(), RegImm.Reg, Target);
}

[[nodiscard]] Expected<MachineInstr *>
insertRegisterCopy(MachineFunction &Function, MachineInstr &Before, PhysicalRegister Destination,
                   RegImmPair Source, const TargetDescription &Target) {
  MachineIRBuilder Builder(Function, Target);
  auto InsertPoint = Builder.setInsertPointBefore(Before);
  if (!InsertPoint)
    return std::unexpected(InsertPoint.error());

  InstructionOpcode Opcode = InstructionOpcode::ADDI;
  const bool IsGPR = classContains(RegisterClass::GPR, Source.Reg, Target);
  if (!IsGPR) {
    if (Source.Imm != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V floating-point compressed copy has an offset");
    if (classContains(RegisterClass::FPR32, Source.Reg, Target))
      Opcode = InstructionOpcode::FSGNJ_S;
    else if (classContains(RegisterClass::FPR64, Source.Reg, Target))
      Opcode = InstructionOpcode::FSGNJ_D;
    else
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V compressed copy has an unsupported register class");
  } else if (Source.Imm < -2048 || Source.Imm > 2047) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V compressed base adjustment is outside ADDI range");
  }

  auto Pending = Builder.buildInstr(instructionOpcode(Opcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Added = Pending->addDef(Register::fromPhysical(Destination));
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pending->addUse(Source.Reg);
  if (!Added)
    return std::unexpected(Added.error());
  if (IsGPR)
    Added = Pending->addImmediate(Source.Imm);
  else
    Added = Pending->addUse(Source.Reg);
  if (!Added)
    return std::unexpected(Added.error());
  if (Before.debugLocation()) {
    Added = Pending->setDebugLocation(*Before.debugLocation());
    if (!Added)
      return std::unexpected(Added.error());
  }
  return Pending->build();
}

[[nodiscard]] Expected<void> updateOperands(MachineInstr &Instruction, RegImmPair OldRegImm,
                                            PhysicalRegister NewReg,
                                            const RISCVSubtarget &Subtarget) {
  const std::size_t FirstOperand =
      isCompressibleStore(Instruction, Subtarget) && OldRegImm.Imm != 0 ? 1U : 0U;
  for (std::size_t Index = FirstOperand; Index < Instruction.operandCount(); ++Index) {
    MachineOperand *Operand = Instruction.operandAt(Index);
    if (Operand == nullptr || Operand->isDef() || Operand->registerValue() != OldRegImm.Reg)
      continue;
    auto Updated = Operand->setRegister(Register::fromPhysical(NewReg));
    if (!Updated)
      return std::unexpected(Updated.error());
  }

  MachineOperand *Offset = Instruction.operandAt(2);
  const auto *OldOffset = Offset == nullptr ? nullptr : Offset->getIf<std::int64_t>();
  if (OldOffset == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V compressible load/store has no immediate offset");
  return Offset->setImmediate(*OldOffset & compressedOffsetMask(Instruction));
}

[[nodiscard]] Expected<bool> makeBlockCompressible(MachineFunction &Function,
                                                   MachineBasicBlock &Block,
                                                   const RISCVSubtarget &Subtarget,
                                                   const TargetDescription &Target) {
  bool Changed = false;
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    const auto RegImm = regImmPreventingCompression(*Instruction, Subtarget, Target);
    if (!RegImm)
      continue;
    std::vector<MachineInstr *> Uses;
    auto NewReg = analyzeCompressibleUses(Function, *Instruction, *RegImm, Uses, Subtarget, Target);
    if (!NewReg)
      return std::unexpected(NewReg.error());
    const PhysicalRegister Scavenged = NewReg->value_or(PhysicalRegister{});
    if (!Scavenged.isValid())
      continue;
    auto Copy = insertRegisterCopy(Function, *Instruction, Scavenged, *RegImm, Target);
    if (!Copy)
      return std::unexpected(Copy.error());
    for (MachineInstr *Use : Uses) {
      auto Updated = updateOperands(*Use, *RegImm, Scavenged, Subtarget);
      if (!Updated)
        return std::unexpected(Updated.error());
    }
    Changed = true;
  }
  return Changed;
}

class RISCVMakeCompressiblePass final : public MachineFunctionPass {
public:
  explicit RISCVMakeCompressiblePass(const RISCVSubtarget &Subtarget) noexcept
      : Subtarget(&Subtarget) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override {
    static const MachinePassContract Contract = [] {
      MachinePassContract Value{
          .Name = "riscv-make-compressible",
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
    if (Subtarget == nullptr || !Subtarget->features().has(RISCVExtension::C) ||
        !Function.attributes().has(MachineFunctionAttribute::MinSize)) {
      return Result;
    }
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      auto BlockResult = makeBlockCompressible(Function, *Block, *Subtarget, Target);
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

std::unique_ptr<MachineFunctionPass> createMakeCompressiblePass(const RISCVSubtarget &Subtarget) {
  return std::make_unique<RISCVMakeCompressiblePass>(Subtarget);
}

} // namespace rcc::riscv

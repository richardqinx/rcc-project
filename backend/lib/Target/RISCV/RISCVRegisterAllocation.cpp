// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace rcc::riscv {
namespace {

struct SpillForm final {
  InstructionOpcode Load;
  InstructionOpcode Store;
  std::uint64_t SizeInBytes;
};

[[nodiscard]] constexpr RegisterClassID registerClassID(RegisterClass Value) noexcept {
  return RegisterClassID::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] constexpr std::optional<SpillForm> spillForm(RegisterClassID Class) noexcept {
  if (Class == registerClassID(RegisterClass::GPR) ||
      Class == registerClassID(RegisterClass::GPRC) ||
      Class == registerClassID(RegisterClass::GPR_NX0) ||
      Class == registerClassID(RegisterClass::GPR_NX0_NX2)) {
    return SpillForm{InstructionOpcode::LD, InstructionOpcode::SD, 8};
  }
  if (Class == registerClassID(RegisterClass::FPR64) ||
      Class == registerClassID(RegisterClass::FPR64C)) {
    return SpillForm{InstructionOpcode::FLD, InstructionOpcode::FSD, 8};
  }
  if (Class == registerClassID(RegisterClass::FPR32) ||
      Class == registerClassID(RegisterClass::FPR32C)) {
    return SpillForm{InstructionOpcode::FLW, InstructionOpcode::FSW, 4};
  }
  return std::nullopt;
}

[[nodiscard]] Expected<const MachineMemOperand *>
createStackMemoryOperand(MachineIRBuilder &Builder, FrameIndex StackSlot, MemoryAccessFlag Access,
                         std::uint64_t SizeInBytes, std::uint64_t AlignmentInBytes) {
  auto Size = MemorySize::fixed(SizeInBytes);
  if (!Size)
    return std::unexpected(Size.error());
  auto Align = Alignment::create(AlignmentInBytes);
  if (!Align)
    return std::unexpected(Align.error());

  MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *Size;
  Description.Align = *Align;
  Description.Base = MemoryBase::frameIndex(StackSlot);
  return Builder.function().createMemOperand(std::move(Description));
}

[[nodiscard]] bool isCalleeSaved(PhysicalRegister Reg) noexcept {
  const std::uint32_t Number = Reg.number();
  const std::array GPRs{
      PhysicalRegisterID::X8,  PhysicalRegisterID::X9,  PhysicalRegisterID::X18,
      PhysicalRegisterID::X19, PhysicalRegisterID::X20, PhysicalRegisterID::X21,
      PhysicalRegisterID::X22, PhysicalRegisterID::X23, PhysicalRegisterID::X24,
      PhysicalRegisterID::X25, PhysicalRegisterID::X26, PhysicalRegisterID::X27,
  };
  const std::array FPRs{
      PhysicalRegisterID::F8_D,  PhysicalRegisterID::F9_D,  PhysicalRegisterID::F18_D,
      PhysicalRegisterID::F19_D, PhysicalRegisterID::F20_D, PhysicalRegisterID::F21_D,
      PhysicalRegisterID::F22_D, PhysicalRegisterID::F23_D, PhysicalRegisterID::F24_D,
      PhysicalRegisterID::F25_D, PhysicalRegisterID::F26_D, PhysicalRegisterID::F27_D,
      PhysicalRegisterID::F8_F,  PhysicalRegisterID::F9_F,  PhysicalRegisterID::F18_F,
      PhysicalRegisterID::F19_F, PhysicalRegisterID::F20_F, PhysicalRegisterID::F21_F,
      PhysicalRegisterID::F22_F, PhysicalRegisterID::F23_F, PhysicalRegisterID::F24_F,
      PhysicalRegisterID::F25_F, PhysicalRegisterID::F26_F, PhysicalRegisterID::F27_F,
  };
  const auto Matches = [Number](PhysicalRegisterID ID) {
    return Number == static_cast<std::uint32_t>(ID);
  };
  return std::ranges::any_of(GPRs, Matches) || std::ranges::any_of(FPRs, Matches);
}

[[nodiscard]] bool isCompressedRegister(PhysicalRegister Reg) noexcept {
  const std::uint32_t Number = Reg.number();
  const std::uint32_t X8 = static_cast<std::uint32_t>(PhysicalRegisterID::X8);
  const std::uint32_t X15 = static_cast<std::uint32_t>(PhysicalRegisterID::X15);
  const std::uint32_t F8D = static_cast<std::uint32_t>(PhysicalRegisterID::F8_D);
  const std::uint32_t F15D = static_cast<std::uint32_t>(PhysicalRegisterID::F15_D);
  const std::uint32_t F8F = static_cast<std::uint32_t>(PhysicalRegisterID::F8_F);
  const std::uint32_t F15F = static_cast<std::uint32_t>(PhysicalRegisterID::F15_F);
  return (Number >= X8 && Number <= X15) || (Number >= F8D && Number <= F15D) ||
         (Number >= F8F && Number <= F15F);
}

class RISCVRegisterAllocationInfo final : public TargetRegisterAllocationInfo {
public:
  [[nodiscard]] bool
  isReservedForFunction(const MachineFunction &Function, PhysicalRegister Reg,
                        const TargetDescription &Target) const noexcept override {
    std::uint64_t RequiredAlignment = 16;
    RequiredAlignment = std::max(
        RequiredAlignment, Function.frameInfo().state().MaxAlignment.value_or(Alignment{}).bytes());
    for (const StackObject &Object : Function.frameInfo().objects()) {
      if (!Object.isTombstone() && !Object.description()->IsDead) {
        RequiredAlignment = std::max(RequiredAlignment, Object.description()->Align.bytes());
      }
    }
    const bool RequiresRealignment = RequiredAlignment > 16;
    if (Reg.number() == static_cast<std::uint32_t>(PhysicalRegisterID::X9))
      return RequiresRealignment;
    if (Reg.number() != static_cast<std::uint32_t>(PhysicalRegisterID::X8))
      return false;
    const MachineFrameState &State = Function.frameInfo().state();
    bool HasCalls = State.HasCalls;
    for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr && !HasCalls;
         Block = Block->next()) {
      for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
           Instruction = Instruction->next()) {
        const InstructionDescriptor *Descriptor =
            Target.instructionDescriptor(Instruction->opcode());
        if (Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Call)) {
          HasCalls = true;
          break;
        }
      }
    }
    const bool HasVariableObject =
        std::ranges::any_of(Function.frameInfo().objects(), [](const StackObject &Object) {
          return !Object.isTombstone() && !Object.description()->IsDead && Object.isVariableSized();
        });
    if (State.IsFrameAddressTaken || State.HasOpaqueSPAdjustment || HasVariableObject ||
        RequiresRealignment)
      return true;
    switch (State.FramePointerPolicy) {
    case MachineFramePointerPolicy::All:
    case MachineFramePointerPolicy::Reserved:
      return true;
    case MachineFramePointerPolicy::NonLeaf:
    case MachineFramePointerPolicy::NonLeafNoReserve:
      return HasCalls;
    case MachineFramePointerPolicy::None:
    case MachineFramePointerPolicy::Count:
      return false;
    }
    return false;
  }

  [[nodiscard]] Expected<void> validateSpillReload(RegisterClassID Class,
                                                   const TargetDescription &Target) const override {
    const RegisterClassDesc *Description = Target.registers().registerClass(Class);
    const auto Form = spillForm(Class);
    if (!Form) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RV64GC register class has no scalar spill/reload form");
    }
    if (Description == nullptr || !Description->Allocatable ||
        Description->SpillSizeInBits != Form->SizeInBytes * 8U ||
        Description->SpillAlignmentInBits == 0 || Description->SpillAlignmentInBits % 8U != 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RV64GC register class has inconsistent spill properties");
    }
    for (const InstructionOpcode OpcodeValue : {Form->Load, Form->Store}) {
      const InstructionDescriptor *Instruction =
          Target.instructionDescriptor(instructionOpcode(OpcodeValue));
      const InstructionFlag MemoryFlag =
          OpcodeValue == Form->Load ? InstructionFlag::MayLoad : InstructionFlag::MayStore;
      if (Instruction == nullptr || Instruction->Operands.size() != 3 ||
          !Instruction->Operands[1].Kinds.contains(MachineOperand::Kind::FrameIndex) ||
          !Instruction->Flags.has(MemoryFlag)) {
        return makeUnexpected(ErrorCode::InternalError,
                              "RV64GC spill instruction descriptor is incomplete");
      }
    }
    return {};
  }

  [[nodiscard]] Expected<MachineInstr *> emitReload(MachineIRBuilder &Builder, Register Destination,
                                                    RegisterClassID Class, FrameIndex StackSlot,
                                                    LaneMask Lanes) const override {
    auto Validated = validateSpillReload(Class, Builder.target());
    if (!Validated)
      return std::unexpected(Validated.error());
    const RegisterClassDesc *Description = Builder.target().registers().registerClass(Class);
    if (Lanes.empty() || !Lanes.intersects(Description->Lanes)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RV64GC reload requires a nonempty class lane mask");
    }
    const auto FormValue = spillForm(Class);
    if (!FormValue) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RV64GC reload lost its validated spill form");
    }
    const SpillForm Form = FormValue.value();
    auto Memory =
        createStackMemoryOperand(Builder, StackSlot, MemoryAccessFlag::Load, Form.SizeInBytes,
                                 Description->SpillAlignmentInBits / 8U);
    if (!Memory)
      return std::unexpected(Memory.error());
    auto Pending = Builder.buildInstr(instructionOpcode(Form.Load));
    if (!Pending)
      return std::unexpected(Pending.error());
    if (auto Result = Pending->addDef(Destination); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addFrameIndex(StackSlot); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addImmediate(0); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addMemOperand(**Memory); !Result)
      return std::unexpected(Result.error());
    return Pending->build();
  }

  [[nodiscard]] Expected<MachineInstr *> emitSpill(MachineIRBuilder &Builder, Register Source,
                                                   RegisterClassID Class, FrameIndex StackSlot,
                                                   LaneMask Lanes) const override {
    auto Validated = validateSpillReload(Class, Builder.target());
    if (!Validated)
      return std::unexpected(Validated.error());
    const RegisterClassDesc *Description = Builder.target().registers().registerClass(Class);
    if (Lanes.empty() || !Lanes.intersects(Description->Lanes)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RV64GC spill requires a nonempty class lane mask");
    }
    const auto FormValue = spillForm(Class);
    if (!FormValue) {
      return makeUnexpected(ErrorCode::InternalError, "RV64GC spill lost its validated spill form");
    }
    const SpillForm Form = FormValue.value();
    auto Memory =
        createStackMemoryOperand(Builder, StackSlot, MemoryAccessFlag::Store, Form.SizeInBytes,
                                 Description->SpillAlignmentInBits / 8U);
    if (!Memory)
      return std::unexpected(Memory.error());
    auto Pending = Builder.buildInstr(instructionOpcode(Form.Store));
    if (!Pending)
      return std::unexpected(Pending.error());
    if (auto Result = Pending->addUse(Source); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addFrameIndex(StackSlot); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addImmediate(0); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addMemOperand(**Memory); !Result)
      return std::unexpected(Result.error());
    return Pending->build();
  }

  [[nodiscard]] Expected<bool> isRematerializable(const MachineInstr &Instruction,
                                                  Register VirtualReg, const MachineFunction &,
                                                  const TargetDescription &) const override {
    const MachineOperand *Definition = Instruction.operandAt(0);
    const Register *DefinedReg = Definition == nullptr ? nullptr : Definition->getIf<Register>();
    if (DefinedReg == nullptr || *DefinedReg != VirtualReg || !Definition->isDef())
      return false;

    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::LUI))
      return Instruction.operandCount() == 2 && Instruction.operandAt(1) != nullptr;
    if (Instruction.opcode() != instructionOpcode(InstructionOpcode::ADDI) &&
        Instruction.opcode() != instructionOpcode(InstructionOpcode::ADDIW)) {
      return false;
    }
    if (Instruction.operandCount() != 3)
      return false;
    const MachineOperand *Source = Instruction.operandAt(1);
    const Register *SourceReg = Source == nullptr ? nullptr : Source->getIf<Register>();
    const auto Zero = physicalRegister(PhysicalRegisterID::X0);
    return Zero && SourceReg != nullptr && *SourceReg == Register::fromPhysical(*Zero) &&
           Source->isUse();
  }

  [[nodiscard]] Expected<MachineInstr *>
  rematerialize(MachineIRBuilder &Builder, const MachineInstr &Original, Register Destination,
                Register OriginalRegister, const TargetDescription &Target) const override {
    auto Accepted = isRematerializable(Original, OriginalRegister, Builder.function(), Target);
    if (!Accepted)
      return std::unexpected(Accepted.error());
    if (!*Accepted) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RV64GC instruction is not rematerializable");
    }

    auto Pending = Builder.buildInstr(Original.opcode());
    if (!Pending)
      return std::unexpected(Pending.error());
    const MachineOperand *OriginalDef = Original.operandAt(0);
    if (auto Result = Pending->addDef(Destination, {}, {}, OriginalDef->targetFlags()); !Result)
      return std::unexpected(Result.error());
    for (std::size_t Index = 1; Index < Original.operandCount(); ++Index) {
      const MachineOperand *Operand = Original.operandAt(Index);
      if (Operand == nullptr) {
        return makeUnexpected(ErrorCode::InternalError,
                              "RV64GC rematerialization found a missing operand");
      }
      if (auto Result = Pending->addOperand(Operand->cloneDetached()); !Result)
        return std::unexpected(Result.error());
    }
    return Pending->build();
  }

  [[nodiscard]] std::uint64_t physicalRegisterCost(const MachineFunction &, Register,
                                                   PhysicalRegister Reg, RegisterClassID,
                                                   bool) const noexcept override {
    if (Reg.number() == static_cast<std::uint32_t>(PhysicalRegisterID::X1))
      return 16;
    if (isCalleeSaved(Reg))
      return 4;
    return isCompressedRegister(Reg) ? 0 : 1;
  }

  [[nodiscard]] std::uint64_t spillCost(const MachineFunction &, const LiveInterval &,
                                        RegisterClassID Class) const noexcept override {
    const auto Form = spillForm(Class);
    return Form ? std::max<std::uint64_t>(1, Form->SizeInBytes / 4U) : 1;
  }
};

} // namespace

const TargetRegisterAllocationInfo &registerAllocationInfo() noexcept {
  static const RISCVRegisterAllocationInfo Info;
  return Info;
}

} // namespace rcc::riscv

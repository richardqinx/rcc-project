// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVRegisterBankInfo.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"

#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] bool isScalarGPRType(LowLevelType Type) noexcept {
  return (Type.isPointer() && Type.addressSpace() == 0 && Type.scalarBitWidth() == 64) ||
         (Type.isCategoryNeutralScalar() && Type.scalarBitWidth() >= 1 &&
          Type.scalarBitWidth() <= 64);
}

[[nodiscard]] constexpr RegisterBankID gprBank() noexcept {
  return RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::GPR));
}

[[nodiscard]] constexpr RegisterBankID fprBank() noexcept {
  return RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::FPR));
}

[[nodiscard]] std::optional<std::size_t> operandIndex(const MachineInstr &Instruction,
                                                      const MachineOperand &Operand) noexcept {
  for (std::size_t Index = 0; Index < Instruction.operandCount(); ++Index) {
    if (Instruction.operandAt(Index) == &Operand)
      return Index;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<RegisterBankID>
physicalRegisterBank(Register Reg, const TargetDescription &Target) noexcept {
  const std::optional<PhysicalRegister> Physical = Reg.physicalRegister();
  if (!Physical)
    return std::nullopt;
  for (const RegisterBankDesc &Bank : Target.registers().registerBanks()) {
    if (Target.registers().bankContainsRegister(Bank.ID, *Physical))
      return Bank.ID;
  }
  return std::nullopt;
}

[[nodiscard]] bool isFloatingGenericOpcode(GenericOpcode OpcodeValue) noexcept {
  switch (OpcodeValue) {
  case GenericOpcode::G_FRAME_INDEX:
  case GenericOpcode::G_FREEZE:
  case GenericOpcode::G_FENCE:
  case GenericOpcode::G_FSHL:
  case GenericOpcode::G_FSHR:
    return false;
  default:
    return getGenericOpcodeName(OpcodeValue).starts_with("G_F");
  }
}

[[nodiscard]] std::optional<RegisterBankID>
requiredBankForOperand(const MachineInstr &Instruction, const MachineOperand &Operand,
                       const TargetDescription &Target) {
  const std::optional<std::size_t> Index = operandIndex(Instruction, Operand);
  if (!Index)
    return std::nullopt;

  if (const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
      Descriptor != nullptr && *Index < Descriptor->Operands.size() &&
      Descriptor->Operands[*Index].RegBank) {
    return Descriptor->Operands[*Index].RegBank;
  }

  if (Instruction.opcode() == opcode(GenericOpcode::COPY)) {
    if (!Operand.isDef())
      return std::nullopt;
    for (const MachineOperand &Other : Instruction.operands()) {
      if (&Other == &Operand)
        continue;
      const Register *OtherReg = Other.getIf<Register>();
      if (OtherReg != nullptr && Other.isUse() && OtherReg->isPhysical())
        return physicalRegisterBank(*OtherReg, Target);
    }
    return std::nullopt;
  }

  if (!isGenericOpcode(Instruction.opcode()))
    return std::nullopt;
  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  switch (Generic) {
  case GenericOpcode::G_FPEXTLOAD:
  case GenericOpcode::G_FPTRUNCSTORE:
    return *Index == 0 ? fprBank() : gprBank();
  case GenericOpcode::G_FPTOSI:
  case GenericOpcode::G_FPTOUI:
  case GenericOpcode::G_FPTOSI_SAT:
  case GenericOpcode::G_FPTOUI_SAT:
  case GenericOpcode::G_LROUND:
  case GenericOpcode::G_LLROUND:
    return *Index == 0 ? gprBank() : fprBank();
  case GenericOpcode::G_SITOFP:
  case GenericOpcode::G_UITOFP:
    return *Index == 0 ? fprBank() : gprBank();
  case GenericOpcode::G_FCMP:
  case GenericOpcode::G_IS_FPCLASS:
    return *Index == 0 ? gprBank() : fprBank();
  case GenericOpcode::G_FPOWI:
  case GenericOpcode::G_FLDEXP:
    return *Index < 2 ? fprBank() : gprBank();
  case GenericOpcode::G_FFREXP:
    return *Index == 1 ? gprBank() : fprBank();
  case GenericOpcode::G_LOAD:
  case GenericOpcode::G_SEXTLOAD:
  case GenericOpcode::G_ZEXTLOAD:
    return *Index == 1 ? std::optional<RegisterBankID>(gprBank()) : std::nullopt;
  case GenericOpcode::G_STORE:
    return *Index == 1 ? std::optional<RegisterBankID>(gprBank()) : std::nullopt;
  case GenericOpcode::G_SELECT:
    return *Index == 1 ? std::optional<RegisterBankID>(gprBank()) : std::nullopt;
  case GenericOpcode::G_IMPLICIT_DEF:
  case GenericOpcode::G_PHI:
    return std::nullopt;
  default:
    return isFloatingGenericOpcode(Generic) ? fprBank() : gprBank();
  }
}

void appendConnectedRegisters(const MachineInstr &Instruction, const MachineOperand &Operand,
                              std::vector<Register> &Pending) {
  const std::optional<std::size_t> Index = operandIndex(Instruction, Operand);
  if (!Index)
    return;
  const bool IsPhi = Instruction.opcode() == opcode(GenericOpcode::G_PHI);
  if (!IsPhi)
    return;
  for (std::size_t OtherIndex = 0; OtherIndex < Instruction.operandCount(); ++OtherIndex) {
    if (OtherIndex == *Index)
      continue;
    const MachineOperand *Other = Instruction.operandAt(OtherIndex);
    const Register *OtherReg = Other == nullptr ? nullptr : Other->getIf<Register>();
    if (OtherReg != nullptr && OtherReg->isVirtual())
      Pending.push_back(*OtherReg);
  }
}

[[nodiscard]] Expected<std::optional<RegisterBankID>>
requiredBankForRegister(const MachineFunction &Function, Register Reg,
                        const TargetDescription &Target) {
  std::vector<Register> Pending{Reg};
  std::unordered_set<Register> Visited;
  std::optional<RegisterBankID> Required;
  while (!Pending.empty()) {
    const Register Current = Pending.back();
    Pending.pop_back();
    if (!Visited.insert(Current).second)
      continue;
    for (const MachineOperand &Operand : Function.registerInfo().regOperands(Current)) {
      const MachineInstr *Instruction = Operand.parent();
      if (Instruction == nullptr)
        continue;
      const std::optional<RegisterBankID> OperandBank =
          requiredBankForOperand(*Instruction, Operand, Target);
      if (OperandBank) {
        if (Required && *Required != *OperandBank) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "RISC-V virtual register has conflicting register-bank uses");
        }
        Required = *OperandBank;
      }
      appendConnectedRegisters(*Instruction, Operand, Pending);
    }
  }
  return Required;
}

[[nodiscard]] bool hasFloatingCopyUse(const MachineFunction &Function, Register Reg,
                                      const TargetDescription &Target) {
  for (const MachineOperand &Operand : Function.registerInfo().regOperands(Reg)) {
    const MachineInstr *Instruction = Operand.parent();
    if (!Operand.isUse() || Instruction == nullptr ||
        Instruction->opcode() != opcode(GenericOpcode::COPY))
      continue;
    for (const MachineOperand &Other : Instruction->operands()) {
      const Register *OtherReg = Other.getIf<Register>();
      if (!Other.isDef() || OtherReg == nullptr || !OtherReg->isPhysical())
        continue;
      if (physicalRegisterBank(*OtherReg, Target) == fprBank())
        return true;
    }
  }
  return false;
}

[[nodiscard]] Expected<RegisterClassID> requiredRegisterClass(const MachineFunction &Function,
                                                              Register Reg,
                                                              const TargetDescription &Target) {
  if (!Reg.isVirtual())
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V register-class selection requires a virtual register");
  auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  if (!Info->get().Type) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V selection requires a type for every virtual register");
  }
  const VirtualRegisterInfo &RegisterInfo = Info->get();
  const LowLevelType Type = RegisterInfo.Type.value_or(LowLevelType{});
  RegisterClassID RequiredClass;
  if (RegisterInfo.Bank && *RegisterInfo.Bank == fprBank()) {
    if (Type.scalarBitWidth() != 32 && Type.scalarBitWidth() != 64) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V RV64GC has no floating register class for type '" +
                                Type.toString() + "'");
    }
    RequiredClass =
        Type.scalarBitWidth() == 32
            ? RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::FPR32))
            : RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::FPR64));
  } else if (isScalarGPRType(Type)) {
    RequiredClass = RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::GPR));
  } else {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V RV64GC has no register class for type '" + Type.toString() + "'");
  }

  if (Target.registers().registerClass(RequiredClass) == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V target description is missing a required register class");
  }
  if (RegisterInfo.Bank && !Target.registers().bankCoversClass(*RegisterInfo.Bank, RequiredClass)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V virtual register bank cannot cover its required class");
  }
  if (RegisterInfo.RegClass &&
      !Target.registers().isSubclass(*RegisterInfo.RegClass, RequiredClass)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V virtual register has a conflicting register class");
  }
  return RequiredClass;
}

} // namespace

Expected<RegisterBankSelection>
RISCVRegisterBankInfo::selectBank(const MachineFunction &Function, Register Reg,
                                  const VirtualRegisterInfo &Info, const MachineInstr *,
                                  const TargetDescription &Target) const {
  if (!Info.Type) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V register-bank selection requires a virtual-register type");
  }

  auto Required = requiredBankForRegister(Function, Reg, Target);
  if (!Required)
    return std::unexpected(Required.error());
  const bool IsConstrained = Required->has_value() || Info.Bank.has_value();
  const RegisterBankID Selected = Required->value_or(gprBank());
  if (Info.Bank && *Info.Bank != Selected) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V virtual register has a conflicting register bank");
  }
  return RegisterBankSelection{.Bank = Selected, .IsConstrained = IsConstrained};
}

Expected<RegisterBankInstructionMapping>
RISCVRegisterBankInfo::getInstructionMapping(const MachineInstr &Instruction,
                                             const MachineFunction &Function,
                                             const TargetDescription &Target) const {
  if (Instruction.opcode() != opcode(GenericOpcode::G_SELECT))
    return RegisterBankInstructionMapping{};
  if (Instruction.operandCount() != 4) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V G_SELECT register-bank mapping requires four operands");
  }

  RegisterBankInstructionMapping Mapping(Instruction.operandCount());
  Mapping[1] = gprBank();
  unsigned FloatingConstraints = 0;

  const Register *Result = Instruction.operandAt(0)->getIf<Register>();
  if (Result == nullptr || !Result->isVirtual()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V G_SELECT result must be a virtual register");
  }
  auto ResultRequirement = requiredBankForRegister(Function, *Result, Target);
  if (!ResultRequirement)
    return std::unexpected(ResultRequirement.error());
  if (*ResultRequirement == fprBank() || hasFloatingCopyUse(Function, *Result, Target))
    ++FloatingConstraints;

  for (const std::size_t Index : {std::size_t{2}, std::size_t{3}}) {
    const Register *Source = Instruction.operandAt(Index)->getIf<Register>();
    if (Source == nullptr || !Source->isVirtual()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V G_SELECT sources must be virtual registers");
    }
    auto SourceInfo = Function.registerInfo().virtualRegisterInfo(*Source);
    if (!SourceInfo)
      return std::unexpected(SourceInfo.error());
    if (SourceInfo->get().Bank == fprBank())
      ++FloatingConstraints;
  }

  const RegisterBankID ValueBank = FloatingConstraints >= 2 ? fprBank() : gprBank();
  Mapping[0] = ValueBank;
  Mapping[2] = ValueBank;
  Mapping[3] = ValueBank;
  return Mapping;
}

Expected<void> RISCVRegisterBankInfo::constrainSelectedInstruction(
    MachineInstr &Instruction, MachineFunction &Function, const TargetDescription &Target) const {
  std::vector<std::pair<Register, RegisterClassID>> PlannedClasses;
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual())
      continue;
    auto Required = requiredRegisterClass(Function, *Reg, Target);
    if (!Required)
      return std::unexpected(Required.error());
    if (std::ranges::find_if(PlannedClasses, [Reg](const auto &Entry) {
          return Entry.first == *Reg;
        }) == PlannedClasses.end())
      PlannedClasses.emplace_back(*Reg, *Required);
  }
  for (const auto &[Reg, Required] : PlannedClasses) {
    auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
    if (!Info)
      return std::unexpected(Info.error());
    if (Info->get().RegClass)
      continue;
    auto SetClass = Function.registerInfo().setRegisterClass(Reg, Required);
    if (!SetClass)
      return std::unexpected(SetClass.error());
  }
  return {};
}

Expected<void> RISCVRegisterBankInfo::constrainRegister(MachineFunction &Function, Register Reg,
                                                        const TargetDescription &Target) const {
  auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  auto Bank = selectBank(Function, Reg, Info->get(), nullptr, Target);
  if (!Bank)
    return std::unexpected(Bank.error());
  auto RequiredClass = requiredRegisterClass(Function, Reg, Target);
  if (!RequiredClass)
    return std::unexpected(RequiredClass.error());
  if (!Info->get().Bank) {
    auto SetBank = Function.registerInfo().setRegisterBank(Reg, Bank->Bank);
    if (!SetBank)
      return std::unexpected(SetBank.error());
  }
  if (!Info->get().RegClass) {
    auto SetClass = Function.registerInfo().setRegisterClass(Reg, *RequiredClass);
    if (!SetClass)
      return std::unexpected(SetClass.error());
  }
  return {};
}

Expected<void> RISCVRegisterBankInfo::validateInstruction(const MachineInstr &Instruction,
                                                          const MachineFunction &Function,
                                                          const TargetDescription &Target) const {
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual())
      continue;
    auto Required = requiredRegisterClass(Function, *Reg, Target);
    if (!Required)
      return std::unexpected(Required.error());
  }
  return {};
}

} // namespace rcc::riscv

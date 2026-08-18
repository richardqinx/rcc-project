// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionRecords.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rcc::riscv {
namespace {

constexpr std::uint32_t PCRelativeHighFlag = 2;
constexpr std::uint32_t PCRelativeLowFlag = 4;
constexpr std::uint32_t CopyF32Flag = 8;
constexpr std::uint32_t CopyF64Flag = 16;
constexpr std::uint32_t GOTPCRelativeHighFlag = 128;

[[nodiscard]] std::optional<RegisterBankID>
registerBank(Register Reg, const MachineFunction &Function, const TargetDescription &Target) {
  if (Reg.isVirtual()) {
    auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
    return Info ? Info->get().Bank : std::nullopt;
  }
  const std::optional<PhysicalRegister> Physical = Reg.physicalRegister();
  if (!Physical)
    return std::nullopt;
  for (const RegisterBankDesc &Bank : Target.registers().registerBanks()) {
    if (Target.registers().bankContainsRegister(Bank.ID, *Physical))
      return Bank.ID;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<LowLevelType> registerType(Register Reg,
                                                       const MachineFunction &Function) {
  if (!Reg.isVirtual())
    return std::nullopt;
  auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  return Info ? Info->get().Type : std::nullopt;
}

[[nodiscard]] Expected<void> copyLocation(const MachineInstr &Source, MachineInstr &Destination) {
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

[[nodiscard]] Expected<void> copyCommonDecoration(const MachineInstr &Source,
                                                  MachineInstr &Destination, bool CopyPreSymbol,
                                                  bool CopyPostSymbol, bool CopyDebugNumber) {
  auto Location = copyLocation(Source, Destination);
  if (!Location)
    return std::unexpected(Location.error());
  Destination.setFlags(Source.flags());
  if (CopyDebugNumber && Source.debugInstrNumber() != 0) {
    auto Number = Destination.setDebugInstrNumber(Source.debugInstrNumber());
    if (!Number)
      return std::unexpected(Number.error());
  }
  if (CopyPreSymbol) {
    if (const auto Symbol = Source.preInstrSymbol()) {
      auto Set = Destination.setPreInstrSymbol(std::string(*Symbol));
      if (!Set)
        return std::unexpected(Set.error());
    }
  }
  if (CopyPostSymbol) {
    if (const auto Symbol = Source.postInstrSymbol()) {
      auto Set = Destination.setPostInstrSymbol(std::string(*Symbol));
      if (!Set)
        return std::unexpected(Set.error());
    }
  }
  if (Source.heapAllocMarker()) {
    auto Set = Destination.setHeapAllocMarker(*Source.heapAllocMarker());
    if (!Set)
      return std::unexpected(Set.error());
  }
  if (Source.pcSections()) {
    auto Set = Destination.setPCSections(*Source.pcSections());
    if (!Set)
      return std::unexpected(Set.error());
  }
  if (Source.mmra()) {
    auto Set = Destination.setMMRA(*Source.mmra());
    if (!Set)
      return std::unexpected(Set.error());
  }
  if (Source.cfiType()) {
    auto Set = Destination.setCFIType(*Source.cfiType());
    if (!Set)
      return std::unexpected(Set.error());
  }
  if (const auto Symbol = Source.deactivationSymbol()) {
    auto Set = Destination.setDeactivationSymbol(std::string(*Symbol));
    if (!Set)
      return std::unexpected(Set.error());
  }
  return {};
}

[[nodiscard]] Expected<void> removeOldInstruction(MachineInstr &Instruction) {
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "pseudo instruction is not linked");
  return Block->removeInstruction(Instruction);
}

[[nodiscard]] Expected<std::pair<Register, Register>>
copyRegisters(const MachineInstr &Instruction) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *SourceOperand = Instruction.operandAt(1);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  if (Destination == nullptr || Source == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "post-RA copy pseudo requires two register operands");
  return std::pair{*Destination, *Source};
}

[[nodiscard]] Expected<bool> prepareCopy(MachineInstr &Instruction, MachineFunction &Function,
                                         const TargetDescription &Target) {
  if (Instruction.opcode() != opcode(GenericOpcode::COPY))
    return false;
  auto Registers = copyRegisters(Instruction);
  if (!Registers)
    return std::unexpected(Registers.error());
  const auto DestinationBank = registerBank(Registers->first, Function, Target);
  const auto SourceBank = registerBank(Registers->second, Function, Target);
  if (!DestinationBank || !SourceBank)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V copy preparation requires constrained register banks");
  if (*DestinationBank == *SourceBank)
    return false;

  const auto DestinationType = registerType(Registers->first, Function);
  const auto SourceType = registerType(Registers->second, Function);
  const std::optional<LowLevelType> Type = DestinationType ? DestinationType : SourceType;
  if (!Type || Type->isVector() || (Type->scalarBitWidth() != 32 && Type->scalarBitWidth() != 64)) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V cross-bank COPY requires a scalar 32-bit or 64-bit type");
  }
  if (DestinationType && SourceType &&
      DestinationType->scalarBitWidth() != SourceType->scalarBitWidth()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V cross-bank COPY has mismatched value widths");
  }
  const std::uint32_t Flag = Type->scalarBitWidth() == 32 ? CopyF32Flag : CopyF64Flag;
  MachineOperand *DestinationOperand = Instruction.operandAt(0);
  MachineOperand *SourceOperand = Instruction.operandAt(1);
  const bool Changed =
      DestinationOperand->targetFlags() != Flag || SourceOperand->targetFlags() != Flag;
  DestinationOperand->setTargetFlags(Flag);
  SourceOperand->setTargetFlags(Flag);
  return Changed;
}

[[nodiscard]] Expected<bool> expandCopy(MachineInstr &Instruction, MachineFunction &Function,
                                        const TargetDescription &Target) {
  auto Registers = copyRegisters(Instruction);
  if (!Registers)
    return std::unexpected(Registers.error());
  const Register Destination = Registers->first;
  const Register Source = Registers->second;
  if (!Destination.isPhysical() || !Source.isPhysical()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V post-RA COPY retains a virtual register");
  }
  if (Destination == Source) {
    auto Removed = removeOldInstruction(Instruction);
    if (!Removed)
      return std::unexpected(Removed.error());
    return true;
  }
  const auto DestinationBank = registerBank(Destination, Function, Target);
  const auto SourceBank = registerBank(Source, Function, Target);
  if (!DestinationBank || !SourceBank)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V post-RA COPY references an unknown register bank");
  const RegisterBankID GPR =
      RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::GPR));
  const RegisterBankID FPR =
      RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::FPR));

  InstructionOpcode Selected = InstructionOpcode::Invalid;
  bool DuplicateSource = false;
  bool AddZeroImmediate = false;
  if (*DestinationBank == GPR && *SourceBank == GPR) {
    Selected = InstructionOpcode::ADDI;
    AddZeroImmediate = true;
  } else if (*DestinationBank == FPR && *SourceBank == FPR) {
    Selected = InstructionOpcode::FSGNJ_D;
    DuplicateSource = true;
  } else {
    const std::uint32_t DestinationFlags = Instruction.operandAt(0)->targetFlags();
    const std::uint32_t SourceFlags = Instruction.operandAt(1)->targetFlags();
    if (DestinationFlags != SourceFlags ||
        (DestinationFlags != CopyF32Flag && DestinationFlags != CopyF64Flag)) {
      return makeUnexpected(ErrorCode::PseudoExpansionFailed,
                            "RISC-V cross-bank COPY has no prepared transfer width");
    }
    if (*DestinationBank == GPR && *SourceBank == FPR) {
      Selected =
          DestinationFlags == CopyF32Flag ? InstructionOpcode::FMV_X_W : InstructionOpcode::FMV_X_D;
    } else if (*DestinationBank == FPR && *SourceBank == GPR) {
      Selected =
          DestinationFlags == CopyF32Flag ? InstructionOpcode::FMV_W_X : InstructionOpcode::FMV_D_X;
    }
  }
  if (Selected == InstructionOpcode::Invalid)
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V COPY crosses unsupported banks");

  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(instructionOpcode(Selected));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Destination);
  if (!Def)
    return std::unexpected(Def.error());
  auto Use = Pending->addUse(Source);
  if (!Use)
    return std::unexpected(Use.error());
  if (DuplicateSource) {
    auto SecondUse = Pending->addUse(Source);
    if (!SecondUse)
      return std::unexpected(SecondUse.error());
  }
  if (AddZeroImmediate) {
    auto Immediate = Pending->addImmediate(0);
    if (!Immediate)
      return std::unexpected(Immediate.error());
  }
  auto Replacement = Pending->build();
  if (!Replacement)
    return std::unexpected(Replacement.error());
  auto Decoration = copyCommonDecoration(Instruction, **Replacement, true, true, true);
  if (!Decoration)
    return std::unexpected(Decoration.error());
  auto Removed = removeOldInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

[[nodiscard]] Expected<bool> expandSubregisterCopy(MachineInstr &Instruction,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target) {
  const Opcode Value = Instruction.opcode();
  if (Value == opcode(GenericOpcode::EXTRACT_SUBREG) ||
      Value == opcode(GenericOpcode::SUBREG_TO_REG)) {
    const MachineOperand *Index = Instruction.operandAt(2);
    const auto *Immediate = Index == nullptr ? nullptr : Index->getIf<std::int64_t>();
    if (Immediate == nullptr || *Immediate != 0)
      return makeUnexpected(ErrorCode::Unsupported,
                            "RV64GC does not support nonzero subregister indices");
  } else if (Value == opcode(GenericOpcode::COPY_LANEMASK)) {
    const MachineOperand *MaskOperand = Instruction.operandAt(2);
    const auto *Mask = MaskOperand == nullptr ? nullptr : MaskOperand->getIf<LaneMaskOperand>();
    if (Mask == nullptr || Mask->Mask.empty())
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V lane copy requires a nonempty lane mask");
  } else {
    return false;
  }
  return expandCopy(Instruction, Function, Target);
}

[[nodiscard]] Expected<void> addDirectTarget(MachineInstrBuilder &Builder,
                                             const MachineOperand &Callee) {
  if (const auto *Global = Callee.getIf<GlobalSymbolOperand>())
    return Builder.addGlobalSymbol(Global->Name, Global->Offset, PCRelativeHighFlag);
  if (const auto *External = Callee.getIf<ExternalSymbolOperand>())
    return Builder.addExternalSymbol(External->Name, External->Offset, PCRelativeHighFlag);
  return makeUnexpected(ErrorCode::VerificationError,
                        "RISC-V direct call pseudo has no symbol target");
}

[[nodiscard]] Expected<Register> createGPRScratchRegister(MachineFunction &Function,
                                                          const TargetDescription &Target) {
  const RegisterBankID GPRBank =
      RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::GPR));
  const RegisterClassID GPRClass =
      RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::GPR));
  if (Target.registers().registerBank(GPRBank) == nullptr ||
      Target.registers().registerClass(GPRClass) == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V pre-RA address expansion requires the GPR register class");
  }
  auto Scratch = Function.registerInfo().createVirtualRegister();
  if (!Scratch)
    return std::unexpected(Scratch.error());
  auto SetBank = Function.registerInfo().setRegisterBank(*Scratch, GPRBank);
  if (!SetBank)
    return std::unexpected(SetBank.error());
  auto SetClass = Function.registerInfo().setRegisterClass(*Scratch, GPRClass);
  if (!SetClass)
    return std::unexpected(SetClass.error());
  return *Scratch;
}

[[nodiscard]] Expected<bool> expandAddressPseudo(MachineInstr &Instruction,
                                                 MachineFunction &Function,
                                                 const TargetDescription &Target,
                                                 bool LoadFromGOT) {
  const MachineOperand *Destination = Instruction.operandAt(0);
  const MachineOperand *Symbol = Instruction.operandAt(1);
  const Register *DestinationRegister =
      Destination == nullptr ? nullptr : Destination->getIf<Register>();
  if (DestinationRegister == nullptr || Symbol == nullptr || Instruction.operandCount() != 2) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V address pseudo has malformed explicit operands");
  }
  if (LoadFromGOT && Instruction.memOperands().size() != 1) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V global-address pseudo requires one GOT load memory operand");
  }
  if (!LoadFromGOT && !Instruction.memOperands().empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V local-address pseudo cannot carry memory operands");
  }

  auto Scratch = createGPRScratchRegister(Function, Target);
  if (!Scratch)
    return std::unexpected(Scratch.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());

  auto HighPending = Builder.buildInstr(instructionOpcode(InstructionOpcode::AUIPC));
  if (!HighPending)
    return std::unexpected(HighPending.error());
  auto HighDef = HighPending->addDef(*Scratch);
  if (!HighDef)
    return std::unexpected(HighDef.error());
  MachineOperand HighSymbol = Symbol->cloneDetached();
  HighSymbol.setTargetFlags(LoadFromGOT ? GOTPCRelativeHighFlag : PCRelativeHighFlag);
  auto HighTarget = HighPending->addOperand(std::move(HighSymbol));
  if (!HighTarget)
    return std::unexpected(HighTarget.error());
  auto High = HighPending->build();
  if (!High)
    return std::unexpected(High.error());
  const std::string Anchor = Instruction.preInstrSymbol()
                                 ? std::string(*Instruction.preInstrSymbol())
                                 : ".Lrcc_pcrel_hi_" + std::to_string(Function.stableID().value()) +
                                       "_" + std::to_string((*High)->stableID().value());
  auto HighAnchor = (*High)->setPreInstrSymbol(Anchor);
  if (!HighAnchor)
    return std::unexpected(HighAnchor.error());
  auto HighDecoration = copyCommonDecoration(Instruction, **High, false, false, false);
  if (!HighDecoration)
    return std::unexpected(HighDecoration.error());

  const InstructionOpcode LowOpcode = LoadFromGOT ? InstructionOpcode::LD : InstructionOpcode::ADDI;
  auto LowPending = Builder.buildInstr(instructionOpcode(LowOpcode));
  if (!LowPending)
    return std::unexpected(LowPending.error());
  auto LowDef = LowPending->addOperand(Destination->cloneDetached());
  if (!LowDef)
    return std::unexpected(LowDef.error());
  auto LowBase = LowPending->addUse(*Scratch);
  if (!LowBase)
    return std::unexpected(LowBase.error());
  auto LowOffset = LowPending->addMCSymbol(Anchor, 0, PCRelativeLowFlag);
  if (!LowOffset)
    return std::unexpected(LowOffset.error());
  if (LoadFromGOT) {
    const MachineMemOperand *Memory = Instruction.memOperands().front();
    if (Memory == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V global-address pseudo has a null memory operand");
    auto AddedMemory = LowPending->addMemOperand(*Memory);
    if (!AddedMemory)
      return std::unexpected(AddedMemory.error());
  }
  auto Low = LowPending->build();
  if (!Low)
    return std::unexpected(Low.error());
  auto LowDecoration = copyCommonDecoration(Instruction, **Low, false, true, true);
  if (!LowDecoration)
    return std::unexpected(LowDecoration.error());
  auto Records =
      Function.records().replaceInstructionReference(Instruction.stableID(), (*Low)->stableID());
  if (!Records)
    return std::unexpected(Records.error());
  auto Removed = removeOldInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

[[nodiscard]] bool isRegister(const MachineOperand &Operand, PhysicalRegister Reg) noexcept {
  const Register *Value = Operand.getIf<Register>();
  return Value != nullptr && Value->physicalRegister() == Reg;
}

[[nodiscard]] Expected<void> copyExtraImplicitOperands(
    const MachineInstr &Source, std::size_t FirstImplicit, MachineInstrBuilder &Destination,
    std::optional<PhysicalRegister> ExplicitDef, std::optional<PhysicalRegister> ExplicitUse) {
  for (std::size_t Index = FirstImplicit; Index < Source.operandCount(); ++Index) {
    const MachineOperand *Operand = Source.operandAt(Index);
    if (Operand == nullptr || !Operand->isImplicit())
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V call pseudo has malformed implicit operands");
    if ((ExplicitDef && Operand->isDef() && isRegister(*Operand, *ExplicitDef)) ||
        (ExplicitUse && Operand->isUse() && isRegister(*Operand, *ExplicitUse))) {
      continue;
    }
    auto Added = Destination.addImplicitOperand(Operand->cloneDetached());
    if (!Added)
      return std::unexpected(Added.error());
  }
  return {};
}

[[nodiscard]] Expected<bool> expandFFLAGSAccess(MachineInstr &Instruction,
                                                MachineFunction &Function,
                                                const TargetDescription &Target, bool IsRead) {
  const MachineOperand *ValueOperand = Instruction.operandAt(0);
  const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
  if (Value == nullptr || !Value->isPhysical()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V FFLAGS pseudo retains a nonphysical value register");
  }
  auto Zero = physicalRegister(PhysicalRegisterID::X0);
  if (!Zero)
    return std::unexpected(Zero.error());

  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(
      instructionOpcode(IsRead ? InstructionOpcode::CSRRS : InstructionOpcode::CSRRW));
  if (!Pending)
    return std::unexpected(Pending.error());
  Expected<void> Added = IsRead ? Pending->addOperand(ValueOperand->cloneDetached())
                                : Pending->addDef(Register::fromPhysical(*Zero));
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pending->addImmediate(1);
  if (!Added)
    return std::unexpected(Added.error());
  Added = IsRead ? Pending->addUse(Register::fromPhysical(*Zero))
                 : Pending->addOperand(ValueOperand->cloneDetached());
  if (!Added)
    return std::unexpected(Added.error());
  auto Implicit = copyExtraImplicitOperands(Instruction, 1, *Pending, std::nullopt, std::nullopt);
  if (!Implicit)
    return std::unexpected(Implicit.error());
  auto Replacement = Pending->build();
  if (!Replacement)
    return std::unexpected(Replacement.error());
  auto Decoration = copyCommonDecoration(Instruction, **Replacement, true, true, true);
  if (!Decoration)
    return std::unexpected(Decoration.error());
  auto Removed = removeOldInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

[[nodiscard]] Expected<bool> expandCall(MachineInstr &Instruction, MachineFunction &Function,
                                        const TargetDescription &Target, bool IsTail) {
  MachineBasicBlock *Block = Instruction.parent();
  const MachineOperand *Callee = Instruction.operandAt(0);
  const MachineOperand *Mask = Instruction.operandAt(1);
  if (Block == nullptr || Callee == nullptr || Mask == nullptr ||
      Mask->kind() != MachineOperand::Kind::RegisterMask) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V call pseudo has malformed explicit operands");
  }
  auto Zero = physicalRegister(PhysicalRegisterID::X0);
  auto Link = physicalRegister(PhysicalRegisterID::X1);
  auto Scratch = physicalRegister(PhysicalRegisterID::X6);
  if (!Zero || !Link || !Scratch)
    return makeUnexpected(ErrorCode::InternalError, "RISC-V call registers are unavailable");

  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  Register Base;
  std::optional<std::string> Anchor;
  const bool Direct = Callee->kind() == MachineOperand::Kind::GlobalSymbol ||
                      Callee->kind() == MachineOperand::Kind::ExternalSymbol;
  if (Direct) {
    const PhysicalRegister HighRegister = IsTail ? *Scratch : *Link;
    Base = Register::fromPhysical(HighRegister);
    Anchor = Instruction.preInstrSymbol()
                 ? std::optional<std::string>{std::string(*Instruction.preInstrSymbol())}
                 : std::optional<std::string>{".Lrcc_call_" +
                                              std::to_string(Function.stableID().value()) + "_" +
                                              std::to_string(Instruction.stableID().value())};
    auto HighPending = Builder.buildInstr(instructionOpcode(InstructionOpcode::AUIPC));
    if (!HighPending)
      return std::unexpected(HighPending.error());
    auto HighDef = HighPending->addDef(Base);
    if (!HighDef)
      return std::unexpected(HighDef.error());
    auto HighTarget = addDirectTarget(*HighPending, *Callee);
    if (!HighTarget)
      return std::unexpected(HighTarget.error());
    auto High = HighPending->build();
    if (!High)
      return std::unexpected(High.error());
    auto Symbol = (*High)->setPreInstrSymbol(*Anchor);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    auto Decoration = copyLocation(Instruction, **High);
    if (!Decoration)
      return std::unexpected(Decoration.error());
  } else {
    const Register *CalleeRegister = Callee->getIf<Register>();
    if (CalleeRegister == nullptr || !CalleeRegister->isPhysical())
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V indirect call retains a virtual callee register");
    Base = *CalleeRegister;
  }

  const InstructionOpcode LowOpcode =
      IsTail ? InstructionOpcode::JALR_TAIL : InstructionOpcode::JALR_CALL;
  auto LowPending = Builder.buildInstr(instructionOpcode(LowOpcode));
  if (!LowPending)
    return std::unexpected(LowPending.error());
  const PhysicalRegister DestinationRegister = IsTail ? *Zero : *Link;
  auto LowDef = LowPending->addDef(Register::fromPhysical(DestinationRegister));
  if (!LowDef)
    return std::unexpected(LowDef.error());
  auto LowBase = LowPending->addUse(Base);
  if (!LowBase)
    return std::unexpected(LowBase.error());
  Expected<void> LowOffset =
      Anchor ? LowPending->addMCSymbol(*Anchor, 0, PCRelativeLowFlag) : LowPending->addImmediate(0);
  if (!LowOffset)
    return std::unexpected(LowOffset.error());
  auto LowMask = LowPending->addOperand(Mask->cloneDetached());
  if (!LowMask)
    return std::unexpected(LowMask.error());
  auto Implicit = copyExtraImplicitOperands(
      Instruction, 2, *LowPending, IsTail ? std::nullopt : std::optional<PhysicalRegister>{*Link},
      std::nullopt);
  if (!Implicit)
    return std::unexpected(Implicit.error());
  auto Low = LowPending->build();
  if (!Low)
    return std::unexpected(Low.error());
  auto Decoration = copyCommonDecoration(Instruction, **Low, !Direct, true, true);
  if (!Decoration)
    return std::unexpected(Decoration.error());
  auto Records =
      Function.records().replaceInstructionReference(Instruction.stableID(), (*Low)->stableID());
  if (!Records)
    return std::unexpected(Records.error());
  auto Removed = removeOldInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

[[nodiscard]] Expected<bool> expandReturn(MachineInstr &Instruction, MachineFunction &Function,
                                          const TargetDescription &Target) {
  auto Zero = physicalRegister(PhysicalRegisterID::X0);
  auto Link = physicalRegister(PhysicalRegisterID::X1);
  if (!Zero || !Link)
    return makeUnexpected(ErrorCode::InternalError, "RISC-V return registers are unavailable");
  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(instructionOpcode(InstructionOpcode::JALR_RET));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Register::fromPhysical(*Zero));
  if (!Def)
    return std::unexpected(Def.error());
  auto Use = Pending->addUse(Register::fromPhysical(*Link));
  if (!Use)
    return std::unexpected(Use.error());
  auto Immediate = Pending->addImmediate(0);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  auto Implicit = copyExtraImplicitOperands(Instruction, 0, *Pending, std::nullopt, *Link);
  if (!Implicit)
    return std::unexpected(Implicit.error());
  auto Replacement = Pending->build();
  if (!Replacement)
    return std::unexpected(Replacement.error());
  auto Decoration = copyCommonDecoration(Instruction, **Replacement, true, true, true);
  if (!Decoration)
    return std::unexpected(Decoration.error());
  auto Removed = removeOldInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

[[nodiscard]] Expected<bool> expandBranch(MachineInstr &Instruction, MachineFunction &Function,
                                          const TargetDescription &Target) {
  const MachineOperand *TargetOperand = Instruction.operandAt(0);
  const BasicBlockOperand *TargetBlock =
      TargetOperand == nullptr ? nullptr : TargetOperand->getIf<BasicBlockOperand>();
  if (TargetBlock == nullptr || TargetBlock->Block == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V PseudoBR has no basic-block target");
  }

  auto Zero = physicalRegister(PhysicalRegisterID::X0);
  if (!Zero)
    return std::unexpected(Zero.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(instructionOpcode(InstructionOpcode::JAL));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Register::fromPhysical(*Zero));
  if (!Def)
    return std::unexpected(Def.error());
  auto Destination = Pending->addBasicBlock(*TargetBlock->Block);
  if (!Destination)
    return std::unexpected(Destination.error());
  auto Replacement = Pending->build();
  if (!Replacement)
    return std::unexpected(Replacement.error());
  auto Decoration = copyCommonDecoration(Instruction, **Replacement, true, true, true);
  if (!Decoration)
    return std::unexpected(Decoration.error());
  auto Removed = removeOldInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

[[nodiscard]] Expected<bool> expandIndirectBranch(MachineInstr &Instruction,
                                                  MachineFunction &Function,
                                                  const TargetDescription &Target) {
  const MachineOperand *Address = Instruction.operandAt(0);
  const MachineOperand *Offset = Instruction.operandAt(1);
  if (Address == nullptr || Address->getIf<Register>() == nullptr || Offset == nullptr ||
      Offset->getIf<std::int64_t>() == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V PseudoBRIND has malformed operands");
  }

  auto Zero = physicalRegister(PhysicalRegisterID::X0);
  if (!Zero)
    return std::unexpected(Zero.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(instructionOpcode(InstructionOpcode::JALR));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Added = Pending->addDef(Register::fromPhysical(*Zero));
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pending->addOperand(Address->cloneDetached());
  if (!Added)
    return std::unexpected(Added.error());
  Added = Pending->addOperand(Offset->cloneDetached());
  if (!Added)
    return std::unexpected(Added.error());
  auto Implicit = copyExtraImplicitOperands(Instruction, 2, *Pending, std::nullopt, std::nullopt);
  if (!Implicit)
    return std::unexpected(Implicit.error());
  auto Replacement = Pending->build();
  if (!Replacement)
    return std::unexpected(Replacement.error());
  auto Decoration = copyCommonDecoration(Instruction, **Replacement, true, true, true);
  if (!Decoration)
    return std::unexpected(Decoration.error());
  auto Removed = removeOldInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

class RISCVPseudoExpansionInfo final : public TargetPseudoExpansionInfo {
public:
  [[nodiscard]] Expected<bool> preparePostRAPseudo(MachineInstr &Instruction,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target) const override {
    return prepareCopy(Instruction, Function, Target);
  }

  [[nodiscard]] Expected<bool> expandPseudo(MachineInstr &Instruction,
                                            MachinePseudoExpansionPhase Phase,
                                            MachineFunction &Function,
                                            const TargetDescription &Target) const override {
    if (Phase == MachinePseudoExpansionPhase::PreRA) {
      if (Instruction.opcode() == instructionOpcode(InstructionOpcode::PseudoLLA))
        return expandAddressPseudo(Instruction, Function, Target, false);
      if (Instruction.opcode() == instructionOpcode(InstructionOpcode::PseudoLGA))
        return expandAddressPseudo(Instruction, Function, Target, true);
      return false;
    }
    if (Phase != MachinePseudoExpansionPhase::PostRA)
      return false;
    if (Instruction.opcode() == opcode(GenericOpcode::COPY))
      return expandCopy(Instruction, Function, Target);
    auto Subregister = expandSubregisterCopy(Instruction, Function, Target);
    if (!Subregister || *Subregister)
      return Subregister;
    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::PseudoCALL))
      return expandCall(Instruction, Function, Target, false);
    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::PseudoTAIL))
      return expandCall(Instruction, Function, Target, true);
    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::PseudoRET))
      return expandReturn(Instruction, Function, Target);
    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::PseudoBR))
      return expandBranch(Instruction, Function, Target);
    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::PseudoBRIND))
      return expandIndirectBranch(Instruction, Function, Target);
    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::ReadFFLAGS))
      return expandFFLAGSAccess(Instruction, Function, Target, true);
    if (Instruction.opcode() == instructionOpcode(InstructionOpcode::WriteFFLAGS))
      return expandFFLAGSAccess(Instruction, Function, Target, false);
    return false;
  }
};

} // namespace

const TargetPseudoExpansionInfo &pseudoExpansionInfo() noexcept {
  static const RISCVPseudoExpansionInfo Value;
  return Value;
}

} // namespace rcc::riscv

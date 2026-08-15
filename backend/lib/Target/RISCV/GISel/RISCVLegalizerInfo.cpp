// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVLegalizerInfo.h"

#include "RISCVImmediateMaterialization.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "rcc/CodeGen/GlobalISel/LegalizerHelper.h"
#include "rcc/CodeGen/GlobalISel/Utils.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/IR/DataLayout.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] std::optional<LowLevelType> registerType(const MachineFunction &Function,
                                                       Register Reg) {
  if (!Reg.isVirtual())
    return std::nullopt;
  auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  if (!Info)
    return std::nullopt;
  return Info->get().Type;
}

[[nodiscard]] std::optional<LowLevelType> registerType(const MachineFunction &Function,
                                                       const MachineOperand &Operand) {
  const Register *Reg = Operand.getIf<Register>();
  return Reg == nullptr ? std::nullopt : registerType(Function, *Reg);
}

[[nodiscard]] std::optional<LowLevelType> resultType(const MachineFunction &Function,
                                                     const MachineInstr &Instruction) {
  const MachineOperand *Result = Instruction.operandAt(0);
  return Result == nullptr ? std::nullopt : registerType(Function, *Result);
}

[[nodiscard]] Expected<Opcode> getRISCVWOpcode(GenericOpcode Generic) {
  switch (Generic) {
  case GenericOpcode::G_ASHR:
    return instructionOpcode(InstructionOpcode::G_SRAW);
  case GenericOpcode::G_LSHR:
    return instructionOpcode(InstructionOpcode::G_SRLW);
  case GenericOpcode::G_SHL:
    return instructionOpcode(InstructionOpcode::G_SLLW);
  case GenericOpcode::G_SDIV:
    return instructionOpcode(InstructionOpcode::G_DIVW);
  case GenericOpcode::G_UDIV:
    return instructionOpcode(InstructionOpcode::G_DIVUW);
  case GenericOpcode::G_UREM:
    return instructionOpcode(InstructionOpcode::G_REMUW);
  case GenericOpcode::G_ROTL:
    return instructionOpcode(InstructionOpcode::G_ROLW);
  case GenericOpcode::G_ROTR:
    return instructionOpcode(InstructionOpcode::G_RORW);
  case GenericOpcode::G_CTLZ:
    return instructionOpcode(InstructionOpcode::G_CLZW);
  case GenericOpcode::G_CTTZ:
    return instructionOpcode(InstructionOpcode::G_CTZW);
  case GenericOpcode::G_CTLS:
    return instructionOpcode(InstructionOpcode::G_CLSW);
  case GenericOpcode::G_FPTOSI:
    return instructionOpcode(InstructionOpcode::G_FCVT_W_RV64);
  case GenericOpcode::G_FPTOUI:
    return instructionOpcode(InstructionOpcode::G_FCVT_WU_RV64);
  default:
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V word opcode mapping received an unexpected opcode");
  }
}

[[nodiscard]] Expected<APInt>
extendIntegerImmediate(const APInt &Value, std::uint32_t DestinationWidth, bool SignExtend) {
  if (DestinationWidth == 0 || DestinationWidth > 64)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V constant extension requires a destination no wider than 64 bits");

  std::uint64_t Raw = Value.words().empty() ? 0 : Value.words().front();
  const std::uint32_t SourceWidth = Value.bitWidth();
  if (SignExtend && Value.isNegative() && SourceWidth < 64)
    Raw |= ~std::uint64_t{0} << SourceWidth;
  if (DestinationWidth < 64)
    Raw &= (std::uint64_t{1} << DestinationWidth) - 1U;
  return APInt::create(DestinationWidth, std::span<const std::uint64_t>(&Raw, 1));
}

[[nodiscard]] Expected<void>
foldConstantExtension(MachineInstr &Instruction, MachineFunction &Function, GenericOpcode Generic) {
  const MachineOperand *SourceOperand = Instruction.operandAt(1);
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Source == nullptr || Destination == nullptr || !Source->isVirtual() ||
      !Destination->isVirtual()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V constant extension requires virtual-register operands");
  }
  const Register SourceRegister = *Source;
  const Register DestinationRegister = *Destination;
  const MachineOperand *SourceDefinition = Function.registerInfo().getUniqueVRegDef(SourceRegister);
  const MachineInstr *SourceDef =
      SourceDefinition == nullptr ? nullptr : SourceDefinition->parent();
  if (SourceDef == nullptr || SourceDef->opcode() != opcode(GenericOpcode::G_CONSTANT))
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V constant extension requires a single constant definition");
  const MachineOperand *ConstantOperand = SourceDef->operandAt(1);
  const auto *Integer =
      ConstantOperand == nullptr ? nullptr : ConstantOperand->getIf<IntegerImmediateOperand>();
  if (Integer == nullptr)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V constant extension requires an integer constant");
  const auto DestinationType = registerType(Function, *Destination);
  if (!DestinationType)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V constant extension destination has no type");
  const std::uint32_t DestinationWidth = DestinationType->scalarBitWidth();
  const bool SignExtend = Generic != GenericOpcode::G_ZEXT;
  auto Extended = extendIntegerImmediate(Integer->Value, DestinationWidth, SignExtend);
  if (!Extended)
    return std::unexpected(Extended.error());

  if (Function.registerInfo().hasOneUse(SourceRegister)) {
    MachineInstr *Def = const_cast<MachineInstr *>(SourceDef);
    if (auto Replaced = Def->operandAt(0)->setRegister(DestinationRegister); !Replaced)
      return std::unexpected(Replaced.error());
    auto Erased = Def->eraseOperand(1);
    if (!Erased)
      return std::unexpected(Erased.error());
    auto Added = Def->addOperand(MachineOperand::integerImmediate(std::move(*Extended)));
    if (!Added)
      return std::unexpected(Added.error());
    MachineBasicBlock *Parent = Instruction.parent();
    if (Parent == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V constant extension is detached during folding");
    return Parent->removeInstruction(Instruction);
  }

  auto SetOpcode = Instruction.setOpcode(opcode(GenericOpcode::G_CONSTANT));
  if (!SetOpcode)
    return std::unexpected(SetOpcode.error());
  while (Instruction.operandCount() > 1) {
    auto Erased = Instruction.eraseOperand(1);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  auto Added = Instruction.addOperand(MachineOperand::integerImmediate(std::move(*Extended)));
  if (!Added)
    return std::unexpected(Added.error());

  if (Function.registerInfo().useEmpty(SourceRegister)) {
    MachineInstr *Def = const_cast<MachineInstr *>(SourceDef);
    MachineBasicBlock *Parent = Def->parent();
    if (Parent == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V constant extension source definition is detached");
    auto Removed = Parent->removeInstruction(*Def);
    if (!Removed)
      return std::unexpected(Removed.error());
    // Keep the source register record as a tombstone-free historical vreg until
    // the legalizer worklist transaction has completed.
  }
  return {};
}

[[nodiscard]] Expected<void> lowerConstantToPool(MachineInstr &Instruction,
                                                 MachineFunction &Function,
                                                 const TargetDescription &Target) {
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const MachineOperand *ImmediateOperand = Instruction.operandAt(1);
  const auto *Immediate =
      ImmediateOperand == nullptr ? nullptr : ImmediateOperand->getIf<IntegerImmediateOperand>();
  if (Destination == nullptr || !Destination->isVirtual() || Immediate == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V constant-pool legalization has invalid operands");
  const auto DestinationType = registerType(Function, *Destination);
  if (!DestinationType || DestinationType->scalarBitWidth() != 64)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V constant-pool legalization requires a 64-bit destination");
  auto Value = GenericConstantData::integer(Immediate->Value);
  if (!Value)
    return std::unexpected(Value.error());
  auto Align = Alignment::create(8);
  if (!Align)
    return std::unexpected(Align.error());
  MachineConstantPoolEntryDesc Description;
  Description.Value = std::move(*Value);
  Description.Size = 8;
  Description.Align = *Align;
  auto PoolIndex = Function.constantPool().addEntry(std::move(Description));
  if (!PoolIndex)
    return std::unexpected(PoolIndex.error());

  auto PointerType = LowLevelType::makePointer(0, 64);
  if (!PointerType)
    return std::unexpected(PointerType.error());
  auto Address = Function.registerInfo().createVirtualRegister(*PointerType);
  if (!Address)
    return std::unexpected(Address.error());
  MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPointBefore(Instruction);
  if (!Point)
    return std::unexpected(Point.error());
  auto AddressInstruction = Builder.buildInstr(opcode(GenericOpcode::G_CONSTANT_POOL));
  if (!AddressInstruction)
    return std::unexpected(AddressInstruction.error());
  auto AddressDef = AddressInstruction->addDef(*Address);
  if (!AddressDef)
    return std::unexpected(AddressDef.error());
  auto AddressPool = AddressInstruction->addConstantPool(*PoolIndex);
  if (!AddressPool)
    return std::unexpected(AddressPool.error());
  auto BuiltAddress = AddressInstruction->build();
  if (!BuiltAddress)
    return std::unexpected(BuiltAddress.error());

  MachineMemOperandDesc MemoryDescription;
  MemoryDescription.Accesses = {MemoryAccessFlag::Load};
  MemoryDescription.Size = *MemorySize::fixed(8);
  MemoryDescription.MemoryType = *DestinationType;
  MemoryDescription.Align = *Align;
  MemoryDescription.Base = MemoryBase::constantPoolPseudo();
  auto Memory = Function.createMemOperand(std::move(MemoryDescription));
  if (!Memory)
    return std::unexpected(Memory.error());
  auto SetOpcode = Instruction.setOpcode(opcode(GenericOpcode::G_LOAD));
  if (!SetOpcode)
    return std::unexpected(SetOpcode.error());
  while (Instruction.operandCount() > 1) {
    auto Erased = Instruction.eraseOperand(1);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  auto AddressUse =
      MachineOperand::registerOperand(*Address, RegisterOperandFlags{RegisterOperandFlag::Use});
  if (!AddressUse)
    return std::unexpected(AddressUse.error());
  auto AddedAddress = Instruction.addOperand(std::move(*AddressUse));
  if (!AddedAddress)
    return std::unexpected(AddedAddress.error());
  auto AddedMemory = Instruction.addMemOperand(**Memory);
  if (!AddedMemory)
    return std::unexpected(AddedMemory.error());
  return {};
}

[[nodiscard]] Expected<std::int64_t> signedImmediate(const MachineOperand &Operand) {
  if (const std::int64_t *Value = Operand.getIf<std::int64_t>())
    return *Value;
  const auto *Integer = Operand.getIf<IntegerImmediateOperand>();
  if (Integer == nullptr || Integer->Value.bitWidth() == 0 || Integer->Value.bitWidth() > 64)
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V constant selection requires an immediate no wider than 64 bits");
  std::uint64_t Raw = Integer->Value.words().empty() ? 0 : Integer->Value.words().front();
  const std::uint32_t Width = Integer->Value.bitWidth();
  if (Width < 64 && (Raw & (std::uint64_t{1} << (Width - 1U))) != 0)
    Raw |= ~std::uint64_t{0} << Width;
  return std::bit_cast<std::int64_t>(Raw);
}

} // namespace

RISCVLegalizerInfo::RISCVLegalizerInfo(const RISCVSubtarget &Subtarget) : Subtarget(Subtarget) {
  const LowLevelType S8 = *LowLevelType::makeCategoryNeutralScalar(8);
  const LowLevelType S16 = *LowLevelType::makeCategoryNeutralScalar(16);
  const LowLevelType S32 = *LowLevelType::makeCategoryNeutralScalar(32);
  const LowLevelType S64 = *LowLevelType::makeCategoryNeutralScalar(64);
  const LowLevelType S128 = *LowLevelType::makeCategoryNeutralScalar(128);
  const LowLevelType P0 = *LowLevelType::makePointer(0, 64);
  const bool HasM = Subtarget.features().has(RISCVExtension::M);
  const bool HasA = Subtarget.features().has(RISCVExtension::A);
  const bool HasF = Subtarget.features().has(RISCVExtension::F);
  const bool HasD = Subtarget.features().has(RISCVExtension::D);
  const bool HasZbb = Subtarget.features().has(RISCVExtension::Zbb);

  getActionDefinitionsBuilder({opcode(GenericOpcode::G_ASSERT_SEXT),
                               opcode(GenericOpcode::G_ASSERT_ZEXT),
                               opcode(GenericOpcode::G_ASSERT_ALIGN)})
      .alwaysLegal();

  getActionDefinitionsBuilder({opcode(GenericOpcode::G_ADD), opcode(GenericOpcode::G_SUB)})
      .legalFor({S64})
      .customFor({S32})
      .widenScalarToNextPow2(0)
      .clampScalar(0, S64, S64);

  getActionDefinitionsBuilder(
      {opcode(GenericOpcode::G_AND), opcode(GenericOpcode::G_OR), opcode(GenericOpcode::G_XOR)})
      .legalFor({S64})
      .widenScalarToNextPow2(0)
      .clampScalar(0, S64, S64);

  getActionDefinitionsBuilder({opcode(GenericOpcode::G_UADDE), opcode(GenericOpcode::G_UADDO),
                               opcode(GenericOpcode::G_USUBE), opcode(GenericOpcode::G_USUBO),
                               opcode(GenericOpcode::G_READ_REGISTER),
                               opcode(GenericOpcode::G_WRITE_REGISTER)})
      .lower();
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_SADDE), opcode(GenericOpcode::G_SADDO),
                               opcode(GenericOpcode::G_SSUBE), opcode(GenericOpcode::G_SSUBO)})
      .minScalar(0, S64)
      .lower();
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_UADDSAT), opcode(GenericOpcode::G_SADDSAT),
                               opcode(GenericOpcode::G_USUBSAT), opcode(GenericOpcode::G_SSUBSAT),
                               opcode(GenericOpcode::G_SSHLSAT), opcode(GenericOpcode::G_USHLSAT)})
      .lower();

  getActionDefinitionsBuilder(
      {opcode(GenericOpcode::G_SHL), opcode(GenericOpcode::G_ASHR), opcode(GenericOpcode::G_LSHR)})
      .legalFor({{S64, S64}})
      .customFor({{S32, S32}})
      .widenScalarToNextPow2(0)
      .clampScalar(1, S64, S64)
      .clampScalar(0, S64, S64);

  getActionDefinitionsBuilder({opcode(GenericOpcode::G_ZEXT), opcode(GenericOpcode::G_SEXT),
                               opcode(GenericOpcode::G_ANYEXT)})
      .legalFor({{S32, S16}, {S64, S16}, {S64, S32}})
      .maxScalar(0, S64);
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_TRUNC)).alwaysLegal();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_SEXT_INREG))
      .customFor({S64})
      .clampScalar(0, S64, S64)
      .lower();

  for (const GenericOpcode Generic :
       {GenericOpcode::G_MERGE_VALUES, GenericOpcode::G_UNMERGE_VALUES}) {
    const std::size_t BigTypeIndex = Generic == GenericOpcode::G_MERGE_VALUES ? 0 : 1;
    const std::size_t LittleTypeIndex = Generic == GenericOpcode::G_MERGE_VALUES ? 1 : 0;
    getActionDefinitionsBuilder(opcode(Generic))
        .widenScalarToNextPow2(LittleTypeIndex, 64)
        .widenScalarToNextPow2(BigTypeIndex, 64)
        .clampScalar(LittleTypeIndex, S64, S64)
        .clampScalar(BigTypeIndex, S64, S64);
  }
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_FSHL), opcode(GenericOpcode::G_FSHR)})
      .lower();
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_ROTR), opcode(GenericOpcode::G_ROTL)})
      .legalFor(HasZbb, {{S64, S64}})
      .customFor(HasZbb, {{S32, S32}})
      .lower();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_BITREVERSE)).maxScalar(0, S64).lower();
  auto &ByteSwapActions = getActionDefinitionsBuilder(opcode(GenericOpcode::G_BSWAP));
  if (HasZbb)
    ByteSwapActions.legalFor({S64}).clampScalar(0, S64, S64);
  else
    ByteSwapActions.maxScalar(0, S64).lower();

  auto &CountZerosActions =
      getActionDefinitionsBuilder({opcode(GenericOpcode::G_CTLZ), opcode(GenericOpcode::G_CTTZ)});
  auto &CountZerosPoisonActions = getActionDefinitionsBuilder(
      {opcode(GenericOpcode::G_CTLZ_ZERO_POISON), opcode(GenericOpcode::G_CTTZ_ZERO_POISON)});
  if (HasZbb) {
    CountZerosActions.legalFor({{S64, S64}})
        .customFor({{S32, S32}})
        .clampScalar(0, S32, S64)
        .widenScalarToNextPow2(0)
        .scalarSameSizeAs(1, 0);
  } else {
    CountZerosActions.maxScalar(0, S64).scalarSameSizeAs(1, 0).lower();
    CountZerosPoisonActions.maxScalar(0, S64).scalarSameSizeAs(1, 0);
  }
  CountZerosPoisonActions.lower();

  getActionDefinitionsBuilder(opcode(GenericOpcode::G_CTLS))
      .maxScalar(0, S64)
      .scalarSameSizeAs(1, 0)
      .lower();

  auto &CountPopulationActions = getActionDefinitionsBuilder(opcode(GenericOpcode::G_CTPOP));
  if (HasZbb) {
    CountPopulationActions.legalFor({{S64, S64}}).clampScalar(0, S64, S64).scalarSameSizeAs(1, 0);
  } else {
    CountPopulationActions.widenScalarToNextPow2(0, 8)
        .clampScalar(0, S8, S64)
        .scalarSameSizeAs(1, 0)
        .lower();
  }

  getActionDefinitionsBuilder(opcode(GenericOpcode::G_CONSTANT))
      .legalFor({P0})
      .customFor({S64})
      .widenScalarToNextPow2(0)
      .clampScalar(0, S64, S64);
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FREEZE))
      .legalFor({S16, S32, S64, P0})
      .widenScalarToNextPow2(0)
      .clampScalar(0, S16, S64);
  getActionDefinitionsBuilder(
      {opcode(GenericOpcode::G_IMPLICIT_DEF), opcode(GenericOpcode::G_CONSTANT_FOLD_BARRIER)})
      .legalFor({S32, S64, P0})
      .widenScalarToNextPow2(0)
      .clampScalar(0, S32, S64);

  getActionDefinitionsBuilder(opcode(GenericOpcode::G_ICMP))
      .legalFor({{S64, S64}, {S64, P0}})
      .widenScalarToNextPow2(1, 8)
      .clampScalar(1, S64, S64)
      .clampScalar(0, S64, S64);
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_SELECT))
      .legalFor({{S32, S64}, {S64, S64}, {P0, S64}})
      .widenScalarToNextPow2(0)
      .clampScalar(0, S32, S64)
      .clampScalar(1, S64, S64);

  auto &LoadRules = getActionDefinitionsBuilder(opcode(GenericOpcode::G_LOAD));
  auto &StoreRules = getActionDefinitionsBuilder(opcode(GenericOpcode::G_STORE));
  auto &ExtLoadRules = getActionDefinitionsBuilder(
      {opcode(GenericOpcode::G_SEXTLOAD), opcode(GenericOpcode::G_ZEXTLOAD)});
  const auto GetScalarMemAlign = [&Subtarget](std::uint64_t Size) {
    return Subtarget.features().has(RISCVSubtargetOption::UnalignedScalarMemory) ? 8 : Size;
  };
  LoadRules
      .legalForTypesWithMemDesc({{S16, P0, S8, GetScalarMemAlign(8)},
                                 {S32, P0, S8, GetScalarMemAlign(8)},
                                 {S16, P0, S16, GetScalarMemAlign(16)},
                                 {S32, P0, S16, GetScalarMemAlign(16)},
                                 {S32, P0, S32, GetScalarMemAlign(32)},
                                 {S64, P0, S8, GetScalarMemAlign(8)},
                                 {S64, P0, S16, GetScalarMemAlign(16)},
                                 {S64, P0, S32, GetScalarMemAlign(32)},
                                 {S64, P0, S64, GetScalarMemAlign(64)},
                                 {P0, P0, S64, GetScalarMemAlign(64)}})
      .widenScalarToNextPow2(0, 8)
      .lowerIfMemSizeNotByteSizePow2()
      .clampScalar(0, S16, S64)
      .lower();
  StoreRules
      .legalForTypesWithMemDesc({{S16, P0, S8, GetScalarMemAlign(8)},
                                 {S32, P0, S8, GetScalarMemAlign(8)},
                                 {S16, P0, S16, GetScalarMemAlign(16)},
                                 {S32, P0, S16, GetScalarMemAlign(16)},
                                 {S32, P0, S32, GetScalarMemAlign(32)},
                                 {S64, P0, S8, GetScalarMemAlign(8)},
                                 {S64, P0, S16, GetScalarMemAlign(16)},
                                 {S64, P0, S32, GetScalarMemAlign(32)},
                                 {S64, P0, S64, GetScalarMemAlign(64)},
                                 {P0, P0, S64, GetScalarMemAlign(64)}})
      .clampScalar(0, S16, S64)
      .lowerIfMemSizeNotByteSizePow2()
      .lower();
  ExtLoadRules
      .legalForTypesWithMemDesc({{S64, P0, S8, GetScalarMemAlign(8)},
                                 {S64, P0, S16, GetScalarMemAlign(16)},
                                 {S64, P0, S32, GetScalarMemAlign(32)}})
      .widenScalarToNextPow2(0)
      .clampScalar(0, S64, S64)
      .lower();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FPEXTLOAD))
      .legalForTypesWithMemDesc({{S64, P0, S32, GetScalarMemAlign(32)}})
      .lower();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FPTRUNCSTORE))
      .legalForTypesWithMemDesc({{S64, P0, S32, GetScalarMemAlign(32)}})
      .lower();

  getActionDefinitionsBuilder({opcode(GenericOpcode::G_PTR_ADD), opcode(GenericOpcode::G_PTRMASK)})
      .legalFor({{P0, S64}});
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_PTRTOINT))
      .legalFor({{S64, P0}})
      .clampScalar(0, S64, S64);
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_INTTOPTR))
      .legalFor({{P0, S64}})
      .clampScalar(1, S64, S64);
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_BR)).alwaysLegal();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_BRCOND)).legalFor({S64}).minScalar(0, S64);
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_BRJT)).customFor({{P0, S64}});
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_BRINDIRECT)).legalFor({P0});
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_PHI))
      .legalFor({P0, S32, S64})
      .widenScalarToNextPow2(0)
      .clampScalar(0, S32, S64);
  getActionDefinitionsBuilder(
      {opcode(GenericOpcode::G_GLOBAL_VALUE), opcode(GenericOpcode::G_JUMP_TABLE),
       opcode(GenericOpcode::G_CONSTANT_POOL), opcode(GenericOpcode::G_FRAME_INDEX)})
      .legalFor({P0});

  if (HasM) {
    getActionDefinitionsBuilder(opcode(GenericOpcode::G_MUL))
        .legalFor({S64})
        .widenScalarToNextPow2(0)
        .clampScalar(0, S64, S64);
    getActionDefinitionsBuilder({opcode(GenericOpcode::G_SMULH), opcode(GenericOpcode::G_UMULH)})
        .legalFor({S64})
        .lower();
    getActionDefinitionsBuilder({opcode(GenericOpcode::G_SMULO), opcode(GenericOpcode::G_UMULO)})
        .minScalar(0, S64)
        .lower();
  } else {
    getActionDefinitionsBuilder(opcode(GenericOpcode::G_MUL))
        .libcallFor({S64, S128})
        .widenScalarToNextPow2(0)
        .clampScalar(0, S64, S128);
    getActionDefinitionsBuilder({opcode(GenericOpcode::G_SMULH), opcode(GenericOpcode::G_UMULH)})
        .lowerFor({S64});
    getActionDefinitionsBuilder({opcode(GenericOpcode::G_SMULO), opcode(GenericOpcode::G_UMULO)})
        .minScalar(0, S64)
        .lower();
  }
  if (HasM) {
    getActionDefinitionsBuilder({opcode(GenericOpcode::G_SDIV), opcode(GenericOpcode::G_UDIV),
                                 opcode(GenericOpcode::G_UREM)})
        .legalFor({S64})
        .customFor({S32})
        .libcallFor({S128})
        .clampScalar(0, S32, S128)
        .widenScalarToNextPow2(0);
    getActionDefinitionsBuilder(opcode(GenericOpcode::G_SREM))
        .legalFor({S64})
        .libcallFor({S128})
        .clampScalar(0, S64, S128)
        .widenScalarToNextPow2(0);
  } else {
    getActionDefinitionsBuilder({opcode(GenericOpcode::G_SDIV), opcode(GenericOpcode::G_UDIV),
                                 opcode(GenericOpcode::G_SREM), opcode(GenericOpcode::G_UREM)})
        .libcallFor({S64, S128})
        .clampScalar(0, S64, S128)
        .widenScalarToNextPow2(0);
  }
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_SDIVREM), opcode(GenericOpcode::G_UDIVREM)})
      .lower();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_ABS)).lower();
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_ABDS), opcode(GenericOpcode::G_ABDU)})
      .minScalar(HasZbb, 0, S64)
      .lower();
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_UMAX), opcode(GenericOpcode::G_UMIN),
                               opcode(GenericOpcode::G_SMAX), opcode(GenericOpcode::G_SMIN)})
      .legalFor(HasZbb, {S64})
      .minScalar(HasZbb, 0, S64)
      .lower();
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_SCMP), opcode(GenericOpcode::G_UCMP)})
      .lower();
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_MEMCPY), opcode(GenericOpcode::G_MEMMOVE),
                               opcode(GenericOpcode::G_MEMSET)})
      .libcall();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_MEMCPY_INLINE)).lower();
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_DYN_STACKALLOC),
                               opcode(GenericOpcode::G_STACKSAVE),
                               opcode(GenericOpcode::G_STACKRESTORE)})
      .lower();

  getActionDefinitionsBuilder(
      {opcode(GenericOpcode::G_FADD), opcode(GenericOpcode::G_FSUB), opcode(GenericOpcode::G_FMUL),
       opcode(GenericOpcode::G_FDIV), opcode(GenericOpcode::G_FMA), opcode(GenericOpcode::G_FSQRT),
       opcode(GenericOpcode::G_FMAXNUM), opcode(GenericOpcode::G_FMINNUM),
       opcode(GenericOpcode::G_FMAXIMUMNUM), opcode(GenericOpcode::G_FMINIMUMNUM)})
      .legalFor(HasF, {S32})
      .legalFor(HasD, {S64})
      .libcallFor({S32, S64})
      .libcallFor({S128});
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_FNEG), opcode(GenericOpcode::G_FABS)})
      .legalFor(HasF, {S32})
      .legalFor(HasD, {S64})
      .lowerFor({S32, S64, S128});
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FREM))
      .libcallFor({S32, S64, S128})
      .minScalar(0, S32);
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FCOPYSIGN))
      .legalFor(HasF, {{S32, S32}})
      .legalFor(HasD, {{S64, S64}, {S32, S64}, {S64, S32}})
      .lower();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FPTRUNC))
      .legalFor(HasD, {{S32, S64}})
      .libcallFor({{S32, S64}, {S32, S128}, {S64, S128}});
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FPEXT))
      .legalFor(HasD, {{S64, S32}})
      .libcallFor({{S64, S32}, {S128, S32}, {S128, S64}});
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FCMP))
      .legalFor(HasF, {{S64, S32}})
      .legalFor(HasD, {{S64, S64}})
      .clampScalar(0, S64, S64)
      .libcallFor({{S64, S32}, {S64, S64}, {S64, S128}});
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_IS_FPCLASS)).lower();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FCONSTANT))
      .legalFor(HasF, {S32})
      .legalFor(HasD, {S64})
      .customFor({S32, S64})
      .lowerFor({S64, S128});
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_FPTOSI), opcode(GenericOpcode::G_FPTOUI)})
      .legalFor(HasF, {{S64, S32}})
      .legalFor(HasD, {{S64, S64}})
      .customFor(HasF, {{S32, S32}})
      .customFor(HasD, {{S32, S64}})
      .widenScalarToNextPow2(0)
      .minScalar(0, S32)
      .libcallFor({{S32, S32},
                   {S64, S32},
                   {S32, S64},
                   {S64, S64},
                   {S32, S128},
                   {S64, S128},
                   {S128, S32},
                   {S128, S64},
                   {S128, S128}});
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_SITOFP), opcode(GenericOpcode::G_UITOFP)})
      .legalFor(HasF, {{S32, S64}})
      .legalFor(HasD, {{S64, S64}})
      .widenScalarToNextPow2(1)
      .widenScalarIf(
          [HasF, HasD](const LegalizationQuery &Query) {
            if (Query.Types.size() < 2 || !Query.Types[0].isCategoryNeutralScalar() ||
                !Query.Types[1].isCategoryNeutralScalar() ||
                Query.Types[1].scalarBitWidth() >= 64) {
              return false;
            }
            return (HasF && Query.Types[0].scalarBitWidth() == 32) ||
                   (HasD && Query.Types[0].scalarBitWidth() == 64);
          },
          [S64](const LegalizationQuery &) { return std::pair{std::size_t{1}, S64}; })
      .minScalar(1, S32)
      .libcallFor({{S32, S32},
                   {S64, S32},
                   {S32, S64},
                   {S64, S64},
                   {S128, S32},
                   {S128, S64},
                   {S32, S128},
                   {S64, S128},
                   {S128, S128}});
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_FCEIL), opcode(GenericOpcode::G_FFLOOR),
                               opcode(GenericOpcode::G_FRINT), opcode(GenericOpcode::G_FNEARBYINT),
                               opcode(GenericOpcode::G_INTRINSIC_TRUNC),
                               opcode(GenericOpcode::G_INTRINSIC_ROUND),
                               opcode(GenericOpcode::G_INTRINSIC_ROUNDEVEN)})
      .libcallFor({S32, S64, S128});
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_FCOS), opcode(GenericOpcode::G_FSIN),
                               opcode(GenericOpcode::G_FTAN), opcode(GenericOpcode::G_FPOW),
                               opcode(GenericOpcode::G_FLOG), opcode(GenericOpcode::G_FLOG2),
                               opcode(GenericOpcode::G_FLOG10), opcode(GenericOpcode::G_FEXP),
                               opcode(GenericOpcode::G_FEXP2), opcode(GenericOpcode::G_FEXP10),
                               opcode(GenericOpcode::G_FACOS), opcode(GenericOpcode::G_FASIN),
                               opcode(GenericOpcode::G_FATAN), opcode(GenericOpcode::G_FATAN2),
                               opcode(GenericOpcode::G_FCOSH), opcode(GenericOpcode::G_FSINH),
                               opcode(GenericOpcode::G_FTANH), opcode(GenericOpcode::G_FMODF)})
      .libcallFor({S32, S64, S128});
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_FPOWI), opcode(GenericOpcode::G_FLDEXP)})
      .libcallFor({{S32, S32}, {S64, S32}, {S128, S32}});
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FCANONICALIZE))
      .legalFor(HasF, {S32})
      .legalFor(HasD, {S64});

  getActionDefinitionsBuilder(opcode(GenericOpcode::G_VASTART)).customFor({P0});
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_VAARG)).clampScalar(0, S64, S64).lower();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_ATOMIC_CMPXCHG_WITH_SUCCESS))
      .lowerIf([S8, S16, S32, S64, P0](const LegalizationQuery &Query) {
        return Query.Types.size() >= 3 &&
               (Query.Types[0] == S8 || Query.Types[0] == S16 || Query.Types[0] == S32 ||
                Query.Types[0] == S64) &&
               Query.Types[2] == P0;
      });
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_ATOMIC_CMPXCHG))
      .legalFor(HasA, {{S64, P0}})
      .libcallFor(!HasA, {{S8, P0}, {S16, P0}, {S32, P0}, {S64, P0}})
      .clampScalar(0, S64, S64);
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_ATOMICRMW_ADD))
      .legalFor(HasA, {{S64, P0}})
      .libcallFor(!HasA, {{S8, P0}, {S16, P0}, {S32, P0}, {S64, P0}})
      .clampScalar(0, S64, S64);
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_ATOMICRMW_SUB))
      .libcallFor(!HasA, {{S8, P0}, {S16, P0}, {S32, P0}, {S64, P0}})
      .clampScalar(0, S64, S64)
      .lower();
  getActionDefinitionsBuilder(
      {opcode(GenericOpcode::G_ATOMICRMW_XCHG), opcode(GenericOpcode::G_ATOMICRMW_AND),
       opcode(GenericOpcode::G_ATOMICRMW_OR), opcode(GenericOpcode::G_ATOMICRMW_XOR),
       opcode(GenericOpcode::G_ATOMICRMW_MAX), opcode(GenericOpcode::G_ATOMICRMW_MIN),
       opcode(GenericOpcode::G_ATOMICRMW_UMAX), opcode(GenericOpcode::G_ATOMICRMW_UMIN)})
      .legalFor(HasA, {{S64, P0}})
      .libcallFor(!HasA, {{S8, P0}, {S16, P0}, {S32, P0}, {S64, P0}})
      .clampScalar(0, S64, S64);
  getActionDefinitionsBuilder(
      {opcode(GenericOpcode::G_INTRINSIC), opcode(GenericOpcode::G_INTRINSIC_W_SIDE_EFFECTS)})
      .alwaysLegal();
  getActionDefinitionsBuilder(opcode(GenericOpcode::G_FENCE)).alwaysLegal();
  getActionDefinitionsBuilder({opcode(GenericOpcode::G_TRAP), opcode(GenericOpcode::G_DEBUGTRAP),
                               opcode(GenericOpcode::G_UBSANTRAP)})
      .alwaysLegal();
}

Expected<void> RISCVLegalizerInfo::legalizeCustom(LegalizerHelper &Helper,
                                                  MachineInstr &Instruction,
                                                  const LegalizationDecision &Decision) const {
  if (Decision.Action != LegalizationAction::Custom)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V custom legalizer received a non-custom action");

  MachineFunction &Function = Helper.MIRBuilder.function();
  const TargetDescription &Target = Helper.MIRBuilder.target();
  if (Instruction.opcode() == opcode(GenericOpcode::G_ADD) ||
      Instruction.opcode() == opcode(GenericOpcode::G_SUB)) {
    auto XLenType = LowLevelType::makeCategoryNeutralScalar(64);
    if (!XLenType)
      return std::unexpected(XLenType.error());

    Helper.Observer.changingInstr(Instruction);
    if (auto Widened = Helper.widenScalarSrc(Instruction, *XLenType, 1, GenericOpcode::G_ANYEXT);
        !Widened)
      return std::unexpected(Widened.error());
    if (auto Widened = Helper.widenScalarSrc(Instruction, *XLenType, 2, GenericOpcode::G_ANYEXT);
        !Widened)
      return std::unexpected(Widened.error());

    auto ALUDestination = Function.registerInfo().createVirtualRegister(*XLenType);
    if (!ALUDestination)
      return std::unexpected(ALUDestination.error());
    auto SignExtendedDestination = Function.registerInfo().createVirtualRegister(*XLenType);
    if (!SignExtendedDestination)
      return std::unexpected(SignExtendedDestination.error());

    MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V word arithmetic requires a register destination");
    const Register OriginalDestination = *Destination;
    if (auto Point = Helper.MIRBuilder.setInsertPointAfter(Instruction); !Point)
      return std::unexpected(Point.error());
    if (auto Built =
            Helper.MIRBuilder.buildSExtInReg(*SignExtendedDestination, *ALUDestination, 32);
        !Built)
      return std::unexpected(Built.error());
    if (auto Built = Helper.MIRBuilder.buildTrunc(OriginalDestination, *SignExtendedDestination);
        !Built)
      return std::unexpected(Built.error());
    if (auto Changed = DestinationOperand->setRegister(*ALUDestination); !Changed)
      return std::unexpected(Changed.error());
    Helper.Observer.changedInstr(Instruction);
    return {};
  }
  if (Instruction.opcode() == opcode(GenericOpcode::G_SEXT_INREG)) {
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *SizeOperand = Instruction.operandAt(2);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const std::int64_t *Size =
        SizeOperand == nullptr ? nullptr : SizeOperand->getIf<std::int64_t>();
    if (Destination == nullptr || Size == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V G_SEXT_INREG has invalid operands");
    auto DestinationType = Function.registerInfo().getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());
    if (DestinationType->minimumBitWidth() == 64 && *Size == 32)
      return {};
    if (Subtarget.features().has(RISCVExtension::Zbb) && (*Size == 8 || *Size == 16))
      return {};
    return Helper.lower(Instruction, LegalizationDecision{
                                         .Action = LegalizationAction::Lower,
                                         .TypeIndex = 0,
                                         .NewType = {},
                                     });
  }
  if (Instruction.opcode() == opcode(GenericOpcode::G_ASHR) ||
      Instruction.opcode() == opcode(GenericOpcode::G_LSHR) ||
      Instruction.opcode() == opcode(GenericOpcode::G_SHL)) {
    auto XLenType = LowLevelType::makeCategoryNeutralScalar(64);
    if (!XLenType)
      return std::unexpected(XLenType.error());
    const MachineOperand *ShiftOperand = Instruction.operandAt(2);
    const Register *ShiftAmount =
        ShiftOperand == nullptr ? nullptr : ShiftOperand->getIf<Register>();
    if (ShiftAmount == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V shift legalization requires a register shift amount");

    const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
    if (getIConstantVRegValWithLookThrough(*ShiftAmount, Function.registerInfo())) {
      GenericOpcode ExtensionOpcode = GenericOpcode::G_ANYEXT;
      if (Generic == GenericOpcode::G_ASHR)
        ExtensionOpcode = GenericOpcode::G_SEXT;
      else if (Generic == GenericOpcode::G_LSHR)
        ExtensionOpcode = GenericOpcode::G_ZEXT;

      Helper.Observer.changingInstr(Instruction);
      if (auto Widened = Helper.widenScalarSrc(Instruction, *XLenType, 1, ExtensionOpcode);
          !Widened)
        return std::unexpected(Widened.error());
      if (auto Widened = Helper.widenScalarSrc(Instruction, *XLenType, 2, GenericOpcode::G_ZEXT);
          !Widened)
        return std::unexpected(Widened.error());
      if (auto Widened = Helper.widenScalarDst(Instruction, *XLenType); !Widened)
        return std::unexpected(Widened.error());
      Helper.Observer.changedInstr(Instruction);
      return {};
    }

    Helper.Observer.changingInstr(Instruction);
    if (auto Widened = Helper.widenScalarSrc(Instruction, *XLenType, 1, GenericOpcode::G_ANYEXT);
        !Widened)
      return std::unexpected(Widened.error());
    if (auto Widened = Helper.widenScalarSrc(Instruction, *XLenType, 2, GenericOpcode::G_ANYEXT);
        !Widened)
      return std::unexpected(Widened.error());
    if (auto Widened = Helper.widenScalarDst(Instruction, *XLenType); !Widened)
      return std::unexpected(Widened.error());
    auto WordOpcode = getRISCVWOpcode(Generic);
    if (!WordOpcode)
      return std::unexpected(WordOpcode.error());
    if (auto Changed = Instruction.setOpcode(*WordOpcode); !Changed)
      return std::unexpected(Changed.error());
    Helper.Observer.changedInstr(Instruction);
    return {};
  }
  if (Instruction.opcode() == opcode(GenericOpcode::G_SDIV) ||
      Instruction.opcode() == opcode(GenericOpcode::G_UDIV) ||
      Instruction.opcode() == opcode(GenericOpcode::G_UREM) ||
      Instruction.opcode() == opcode(GenericOpcode::G_ROTL) ||
      Instruction.opcode() == opcode(GenericOpcode::G_ROTR)) {
    auto XLenType = LowLevelType::makeCategoryNeutralScalar(64);
    if (!XLenType)
      return std::unexpected(XLenType.error());

    Helper.Observer.changingInstr(Instruction);
    if (auto Widened = Helper.widenScalarSrc(Instruction, *XLenType, 1, GenericOpcode::G_ANYEXT);
        !Widened)
      return std::unexpected(Widened.error());
    if (auto Widened = Helper.widenScalarSrc(Instruction, *XLenType, 2, GenericOpcode::G_ANYEXT);
        !Widened)
      return std::unexpected(Widened.error());
    if (auto Widened = Helper.widenScalarDst(Instruction, *XLenType); !Widened)
      return std::unexpected(Widened.error());
    auto WordOpcode = getRISCVWOpcode(static_cast<GenericOpcode>(Instruction.opcode().value()));
    if (!WordOpcode)
      return std::unexpected(WordOpcode.error());
    if (auto Changed = Instruction.setOpcode(*WordOpcode); !Changed)
      return std::unexpected(Changed.error());
    Helper.Observer.changedInstr(Instruction);
    return {};
  }
  if (Instruction.opcode() == opcode(GenericOpcode::G_CTLZ) ||
      Instruction.opcode() == opcode(GenericOpcode::G_CTTZ) ||
      Instruction.opcode() == opcode(GenericOpcode::G_CTLS)) {
    auto XLenType = LowLevelType::makeCategoryNeutralScalar(64);
    if (!XLenType)
      return std::unexpected(XLenType.error());

    Helper.Observer.changingInstr(Instruction);
    if (auto Widened = Helper.widenScalarSrc(Instruction, *XLenType, 1, GenericOpcode::G_ANYEXT);
        !Widened) {
      return std::unexpected(Widened.error());
    }
    if (auto Widened = Helper.widenScalarDst(Instruction, *XLenType); !Widened)
      return std::unexpected(Widened.error());
    auto WordOpcode = getRISCVWOpcode(static_cast<GenericOpcode>(Instruction.opcode().value()));
    if (!WordOpcode)
      return std::unexpected(WordOpcode.error());
    if (auto Changed = Instruction.setOpcode(*WordOpcode); !Changed)
      return std::unexpected(Changed.error());
    Helper.Observer.changedInstr(Instruction);
    return {};
  }
  if (Instruction.opcode() == opcode(GenericOpcode::G_FPTOSI) ||
      Instruction.opcode() == opcode(GenericOpcode::G_FPTOUI)) {
    auto XLenType = LowLevelType::makeCategoryNeutralScalar(64);
    if (!XLenType)
      return std::unexpected(XLenType.error());

    Helper.Observer.changingInstr(Instruction);
    if (auto Widened = Helper.widenScalarDst(Instruction, *XLenType); !Widened)
      return std::unexpected(Widened.error());
    auto WordOpcode = getRISCVWOpcode(static_cast<GenericOpcode>(Instruction.opcode().value()));
    if (!WordOpcode)
      return std::unexpected(WordOpcode.error());
    if (auto Changed = Instruction.setOpcode(*WordOpcode); !Changed)
      return std::unexpected(Changed.error());
    if (auto RoundingMode = Instruction.addOperand(MachineOperand::immediate(1)); !RoundingMode)
      return std::unexpected(RoundingMode.error());
    Helper.Observer.changedInstr(Instruction);
    return {};
  }
  if (Instruction.opcode() == opcode(GenericOpcode::G_ANYEXT) ||
      Instruction.opcode() == opcode(GenericOpcode::G_SEXT) ||
      Instruction.opcode() == opcode(GenericOpcode::G_ZEXT)) {
    return foldConstantExtension(Instruction, Function,
                                 static_cast<GenericOpcode>(Instruction.opcode().value()));
  }
  if (Instruction.opcode() == opcode(GenericOpcode::G_CONSTANT)) {
    const MachineOperand *Immediate = Instruction.operandAt(1);
    const std::optional<LowLevelType> Type = resultType(Function, Instruction);
    if (Immediate != nullptr && Type && Type->isCategoryNeutralScalar() &&
        Type->scalarBitWidth() == 64) {
      auto Value = signedImmediate(*Immediate);
      if (Value && shouldUseRV64IntegerConstantPool(*Value)) {
        Helper.Observer.changingInstr(Instruction);
        auto Lowered = lowerConstantToPool(Instruction, Function, Target);
        if (!Lowered)
          return std::unexpected(Lowered.error());
        Helper.Observer.changedInstr(Instruction);
        return {};
      }
      return {};
    }
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V constant custom legalization is unavailable");
  }
  return makeUnexpected(ErrorCode::Unsupported,
                        "RISC-V legalizer has no lowering rule for this instruction");
}

Expected<bool> RISCVLegalizerInfo::legalizeIntrinsic(LegalizerHelper &Helper,
                                                     MachineInstr &Instruction) const {
  const MachineOperand *IntrinsicMachineOperand = Instruction.operandAt(0);
  const auto *Intrinsic = IntrinsicMachineOperand == nullptr
                              ? nullptr
                              : IntrinsicMachineOperand->getIf<rcc::IntrinsicOperand>();
  if (Intrinsic == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V intrinsic legalization requires an intrinsic operand");
  if (Intrinsic->ID != intrinsicID(GenericIntrinsic::VACopy))
    return false;
  if (Instruction.operandCount() != 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "llvm.va_copy requires destination and source operands");

  const MachineOperand *DestinationOperand = Instruction.operandAt(1);
  const MachineOperand *SourceOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  if (Destination == nullptr || Source == nullptr || !Destination->isVirtual() ||
      !Source->isVirtual()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "llvm.va_copy requires virtual-register pointer operands");
  }

  MachineFunction &Function = Helper.MIRBuilder.function();
  auto DestinationType = Function.registerInfo().getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  auto SourceType = Function.registerInfo().getType(*Source);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  if (!DestinationType->isPointer() || !SourceType->isPointer() ||
      *DestinationType != *SourceType || DestinationType->scalarBitWidth() == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "llvm.va_copy operands must have the same scalar pointer type");
  }

  const auto AddressSpace = DestinationType->addressSpace();
  if (!AddressSpace)
    return makeUnexpected(ErrorCode::VerificationError,
                          "llvm.va_copy pointer type has no address space");
  auto Layout = DataLayout::parse(TargetDataLayout);
  if (!Layout)
    return std::unexpected(Layout.error());
  const GenericConstantType PointerType = GenericConstantType::pointer(*AddressSpace);
  auto PointerWidth = Layout->getPointerSizeInBits(*AddressSpace);
  if (!PointerWidth)
    return std::unexpected(PointerWidth.error());
  if (*PointerWidth != DestinationType->scalarBitWidth())
    return makeUnexpected(ErrorCode::VerificationError,
                          "llvm.va_copy pointer type disagrees with the target data layout");
  auto ABIAlignment = Layout->getABITypeAlign(PointerType);
  if (!ABIAlignment)
    return std::unexpected(ABIAlignment.error());
  auto Size = MemorySize::fixed((static_cast<std::uint64_t>(*PointerWidth) + 7U) / 8U);
  auto Alignment = rcc::Alignment::create(*ABIAlignment);
  if (!Size || !Alignment)
    return makeUnexpected(ErrorCode::InternalError,
                          "llvm.va_copy pointer type has an invalid memory layout");

  MachineMemOperandDesc LoadDescription;
  LoadDescription.Accesses = {MemoryAccessFlag::Load};
  LoadDescription.Size = *Size;
  LoadDescription.MemoryType = *DestinationType;
  LoadDescription.Align = *Alignment;
  auto LoadMemory = Function.createMemOperand(std::move(LoadDescription));
  if (!LoadMemory)
    return std::unexpected(LoadMemory.error());

  MachineMemOperandDesc StoreDescription;
  StoreDescription.Accesses = {MemoryAccessFlag::Store};
  StoreDescription.Size = *Size;
  StoreDescription.MemoryType = *DestinationType;
  StoreDescription.Align = *Alignment;
  auto StoreMemory = Function.createMemOperand(std::move(StoreDescription));
  if (!StoreMemory)
    return std::unexpected(StoreMemory.error());

  auto Temporary = Function.registerInfo().createVirtualRegister(*DestinationType);
  if (!Temporary)
    return std::unexpected(Temporary.error());

  if (auto Point = Helper.MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto LoadBuilder = Helper.MIRBuilder.buildInstr(opcode(GenericOpcode::G_LOAD));
  if (!LoadBuilder)
    return std::unexpected(LoadBuilder.error());
  if (auto Added = LoadBuilder->addDef(*Temporary); !Added)
    return std::unexpected(Added.error());
  if (auto Added = LoadBuilder->addUse(*Source); !Added)
    return std::unexpected(Added.error());
  if (auto Added = LoadBuilder->addMemOperand(**LoadMemory); !Added)
    return std::unexpected(Added.error());
  if (auto Built = LoadBuilder->build(); !Built)
    return std::unexpected(Built.error());

  auto StoreBuilder = Helper.MIRBuilder.buildInstr(opcode(GenericOpcode::G_STORE));
  if (!StoreBuilder)
    return std::unexpected(StoreBuilder.error());
  if (auto Added = StoreBuilder->addUse(*Temporary); !Added)
    return std::unexpected(Added.error());
  if (auto Added = StoreBuilder->addUse(*Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = StoreBuilder->addMemOperand(**StoreMemory); !Added)
    return std::unexpected(Added.error());
  if (auto Built = StoreBuilder->build(); !Built)
    return std::unexpected(Built.error());

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "llvm.va_copy intrinsic became detached during lowering");
  if (auto Removed = Block->removeInstruction(Instruction); !Removed)
    return std::unexpected(Removed.error());
  return true;
}

bool RISCVLegalizerInfo::isSExtCheaperThanZExt(LowLevelType SourceType,
                                               LowLevelType DestinationType) const {
  return SourceType.isCategoryNeutralScalar() && DestinationType.isCategoryNeutralScalar() &&
         SourceType.scalarBitWidth() == 32 && DestinationType.scalarBitWidth() == 64;
}

} // namespace rcc::riscv

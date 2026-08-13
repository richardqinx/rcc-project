// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/LegalizerHelper.h"

#include "rcc/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace rcc {
namespace {

struct ScalarLayout final {
  std::uint64_t StoreSize = 0;
  std::uint64_t AllocationSize = 0;
  std::uint64_t ABIAlignment = 0;
};

[[nodiscard]] Expected<std::int64_t> signedImmediate(const MachineOperand &Operand) {
  if (const std::int64_t *Value = Operand.getIf<std::int64_t>())
    return *Value;
  const auto *Integer = Operand.getIf<IntegerImmediateOperand>();
  if (Integer == nullptr || Integer->Value.bitWidth() == 0 || Integer->Value.bitWidth() > 64) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "legalizer requires an immediate no wider than 64 bits");
  }
  std::uint64_t Raw = Integer->Value.words().empty() ? 0 : Integer->Value.words().front();
  const std::uint32_t Width = Integer->Value.bitWidth();
  if (Width < 64 && (Raw & (std::uint64_t{1} << (Width - 1U))) != 0)
    Raw |= ~std::uint64_t{0} << Width;
  return std::bit_cast<std::int64_t>(Raw);
}

[[nodiscard]] std::optional<ScalarLayout> scalarLayout(LowLevelType Type) noexcept {
  if (!Type.isScalarValue() || Type.scalarBitWidth() == 0)
    return std::nullopt;
  const std::uint64_t StoreSize = (Type.scalarBitWidth() + 7U) / 8U;
  const std::uint64_t AllocationSize = std::bit_ceil(StoreSize);
  return ScalarLayout{StoreSize, AllocationSize, std::min<std::uint64_t>(AllocationSize, 8)};
}

[[nodiscard]] Expected<const MachineMemOperand *> createMemoryAccess(MachineFunction &Function,
                                                                     MemoryAccessFlag Access,
                                                                     LowLevelType Type,
                                                                     const ScalarLayout &Layout) {
  auto Size = MemorySize::fixed(Layout.StoreSize);
  auto Align = Alignment::create(Layout.ABIAlignment);
  if (!Size || !Align)
    return makeUnexpected(ErrorCode::InternalError,
                          "legalizer cannot represent scalar memory layout");
  MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *Size;
  Description.MemoryType = Type;
  Description.Align = *Align;
  return Function.createMemOperand(std::move(Description));
}

[[nodiscard]] Expected<Register> buildConstantBefore(MachineIRBuilder &Builder,
                                                     MachineInstr &Anchor, LowLevelType Type,
                                                     std::uint64_t Value) {
  auto Result = Builder.function().registerInfo().createVirtualRegister(Type);
  if (!Result)
    return std::unexpected(Result.error());
  auto Immediate = APInt::fromUnsigned(Type.scalarBitWidth(), Value);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  if (auto Point = Builder.setInsertPointBefore(Anchor); !Point)
    return std::unexpected(Point.error());
  auto Built = Builder.buildConstant(*Result, std::move(*Immediate));
  if (!Built)
    return std::unexpected(Built.error());
  return *Result;
}

[[nodiscard]] Expected<Register> buildBinaryBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                                                   GenericOpcode Generic, LowLevelType Type,
                                                   Register Left, Register Right) {
  auto Result = Builder.function().registerInfo().createVirtualRegister(Type);
  if (!Result)
    return std::unexpected(Result.error());
  if (auto Point = Builder.setInsertPointBefore(Anchor); !Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(Generic));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(*Result); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Left); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Right); !Added)
    return std::unexpected(Added.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return *Result;
}

} // namespace

LegalizerHelper::LegalizerHelper(MachineFunction &Function, const LegalizerInfo &Info,
                                 GISelChangeObserver &Observer, MachineIRBuilder &MIRBuilder)
    : MIRBuilder(MIRBuilder), Observer(Observer), Function(Function), MRI(Function.registerInfo()),
      Info(Info) {}

Expected<void> LegalizerHelper::widenScalarSrc(MachineInstr &Instruction, LowLevelType WideType,
                                               std::size_t OperandIndex,
                                               GenericOpcode ExtensionOpcode) {
  MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  const Register *Source = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Source == nullptr || !Source->isVirtual() || !Operand->isUse())
    return makeUnexpected(ErrorCode::VerificationError,
                          "scalar widening source is not a virtual-register use");

  auto WideSource = MRI.createVirtualRegister(WideType);
  if (!WideSource)
    return std::unexpected(WideSource.error());
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto Extension = MIRBuilder.buildInstr(opcode(ExtensionOpcode));
  if (!Extension)
    return std::unexpected(Extension.error());
  if (auto Added = Extension->addDef(*WideSource); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Extension->addUse(*Source); !Added)
    return std::unexpected(Added.error());
  if (auto Built = Extension->build(); !Built)
    return std::unexpected(Built.error());
  return Operand->setRegister(*WideSource);
}

Expected<void> LegalizerHelper::widenScalarDst(MachineInstr &Instruction, LowLevelType WideType,
                                               std::size_t OperandIndex,
                                               GenericOpcode TruncOpcode) {
  MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  const Register *Destination = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Destination == nullptr || !Destination->isVirtual() || !Operand->isDef())
    return makeUnexpected(ErrorCode::VerificationError,
                          "scalar widening destination is not a virtual-register definition");

  auto WideDestination = MRI.createVirtualRegister(WideType);
  if (!WideDestination)
    return std::unexpected(WideDestination.error());
  if (auto Point = MIRBuilder.setInsertPointAfter(Instruction); !Point)
    return std::unexpected(Point.error());
  auto Truncation = MIRBuilder.buildInstr(opcode(TruncOpcode));
  if (!Truncation)
    return std::unexpected(Truncation.error());
  if (auto Added = Truncation->addDef(*Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Truncation->addUse(*WideDestination); !Added)
    return std::unexpected(Added.error());
  if (auto Built = Truncation->build(); !Built)
    return std::unexpected(Built.error());
  return Operand->setRegister(*WideDestination);
}

Expected<void> LegalizerHelper::widenScalar(MachineInstr &Instruction, std::size_t TypeIndex,
                                            LowLevelType WideType) {
  auto Query = LegalizerInfo::buildQuery(Instruction, Function, MIRBuilder.target());
  if (!Query)
    return std::unexpected(Query.error());
  if (!WideType.isCategoryNeutralScalar() || TypeIndex >= Query->Types.size() ||
      !Query->Types[TypeIndex].isCategoryNeutralScalar() ||
      WideType.scalarBitWidth() <= Query->Types[TypeIndex].scalarBitWidth()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "scalar widening has an invalid type index or target type");
  }

  const auto widenSource = [&](std::size_t OperandIndex, GenericOpcode ExtensionOpcode) {
    return widenScalarSrc(Instruction, WideType, OperandIndex, ExtensionOpcode);
  };
  const auto widenDestination = [&](std::size_t OperandIndex = 0,
                                    GenericOpcode TruncOpcode = GenericOpcode::G_TRUNC) {
    return widenScalarDst(Instruction, WideType, OperandIndex, TruncOpcode);
  };
  const auto apply = [](Expected<void> Result) -> Expected<void> {
    if (!Result)
      return std::unexpected(Result.error());
    return {};
  };

  Observer.changingInstr(Instruction);
  switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
  case GenericOpcode::G_ATOMICRMW_XCHG:
  case GenericOpcode::G_ATOMICRMW_ADD:
  case GenericOpcode::G_ATOMICRMW_SUB:
  case GenericOpcode::G_ATOMICRMW_AND:
  case GenericOpcode::G_ATOMICRMW_OR:
  case GenericOpcode::G_ATOMICRMW_XOR:
  case GenericOpcode::G_ATOMICRMW_MIN:
  case GenericOpcode::G_ATOMICRMW_MAX:
  case GenericOpcode::G_ATOMICRMW_UMIN:
  case GenericOpcode::G_ATOMICRMW_UMAX:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "atomic RMW value widening has an invalid type index");
    if (auto Result = apply(widenSource(2, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = apply(widenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_ATOMIC_CMPXCHG:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "compare-exchange widening has an invalid type index");
    if (auto Result = apply(widenSource(2, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = apply(widenSource(3, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = apply(widenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_ATOMIC_CMPXCHG_WITH_SUCCESS:
    if (TypeIndex == 0) {
      if (auto Result = apply(widenSource(3, GenericOpcode::G_ANYEXT)); !Result)
        return Result;
      if (auto Result = apply(widenSource(4, GenericOpcode::G_ANYEXT)); !Result)
        return Result;
      if (auto Result = apply(widenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "compare-exchange success widening has an invalid type index");
    if (auto Result = apply(widenDestination(1)); !Result)
      return Result;
    break;

  case GenericOpcode::G_ADD:
  case GenericOpcode::G_SUB:
  case GenericOpcode::G_MUL:
  case GenericOpcode::G_AND:
  case GenericOpcode::G_OR:
  case GenericOpcode::G_XOR:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "binary operation widening has an invalid type index");
    if (auto Result = apply(widenSource(1, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = apply(widenSource(2, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = apply(widenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_SDIV:
  case GenericOpcode::G_SREM:
  case GenericOpcode::G_SMIN:
  case GenericOpcode::G_SMAX:
  case GenericOpcode::G_ABDS:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "signed operation widening has an invalid type index");
    if (auto Result = apply(widenSource(1, GenericOpcode::G_SEXT)); !Result)
      return Result;
    if (auto Result = apply(widenSource(2, GenericOpcode::G_SEXT)); !Result)
      return Result;
    if (auto Result = apply(widenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_UDIV:
  case GenericOpcode::G_UREM:
  case GenericOpcode::G_UMIN:
  case GenericOpcode::G_UMAX:
  case GenericOpcode::G_ABDU:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "unsigned operation widening has an invalid type index");
    if (auto Result = apply(widenSource(1, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    if (auto Result = apply(widenSource(2, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    if (auto Result = apply(widenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_SHL:
  case GenericOpcode::G_LSHR:
  case GenericOpcode::G_ASHR:
    if (TypeIndex == 0) {
      GenericOpcode ExtensionOpcode = GenericOpcode::G_ANYEXT;
      if (Instruction.opcode() == opcode(GenericOpcode::G_LSHR))
        ExtensionOpcode = GenericOpcode::G_ZEXT;
      else if (Instruction.opcode() == opcode(GenericOpcode::G_ASHR))
        ExtensionOpcode = GenericOpcode::G_SEXT;
      if (auto Result = apply(widenSource(1, ExtensionOpcode)); !Result)
        return Result;
      if (auto Result = apply(widenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "shift widening has an invalid type index");
    if (auto Result = apply(widenSource(2, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_ROTL:
  case GenericOpcode::G_ROTR:
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::Unsupported, "rotate value widening is not defined");
    if (auto Result = apply(widenSource(2, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_FREEZE:
  case GenericOpcode::G_CONSTANT_FOLD_BARRIER:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "unary operation widening has an invalid type index");
    if (auto Result = apply(widenSource(1, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = apply(widenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_ABS:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "absolute-value widening has an invalid type index");
    if (auto Result = apply(widenSource(1, GenericOpcode::G_SEXT)); !Result)
      return Result;
    if (auto Result = apply(widenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_SELECT:
    if (TypeIndex == 0) {
      if (auto Result = apply(widenSource(2, GenericOpcode::G_ANYEXT)); !Result)
        return Result;
      if (auto Result = apply(widenSource(3, GenericOpcode::G_ANYEXT)); !Result)
        return Result;
      if (auto Result = apply(widenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "select widening has an invalid type index");
    if (auto Result = apply(widenSource(1, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_FPEXT:
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::Unsupported,
                            "floating-point extension destination widening is not defined");
    if (auto Result = apply(widenSource(1, GenericOpcode::G_FPEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_FPTOSI:
  case GenericOpcode::G_FPTOUI:
  case GenericOpcode::G_INTRINSIC_LRINT:
  case GenericOpcode::G_INTRINSIC_LLRINT:
  case GenericOpcode::G_IS_FPCLASS:
    if (TypeIndex == 0) {
      if (auto Result = apply(widenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "floating-to-integer widening has an invalid type index");
    if (auto Result = apply(widenSource(1, GenericOpcode::G_FPEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_SITOFP:
  case GenericOpcode::G_UITOFP:
    if (TypeIndex == 0) {
      if (auto Result = apply(widenDestination(0, GenericOpcode::G_FPTRUNC)); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "integer-to-floating widening has an invalid type index");
    if (auto Result = apply(widenSource(1, Instruction.opcode() == opcode(GenericOpcode::G_SITOFP)
                                               ? GenericOpcode::G_SEXT
                                               : GenericOpcode::G_ZEXT));
        !Result)
      return Result;
    break;

  default:
    return makeUnexpected(ErrorCode::Unsupported,
                          "scalar widening is not implemented for this opcode");
  }

  Observer.changedInstr(Instruction);
  return {};
}

Expected<void> LegalizerHelper::lowerVAArg(MachineInstr &Instruction) {
  if (Instruction.opcode() != opcode(GenericOpcode::G_VAARG) || Instruction.operandCount() != 3 ||
      !Instruction.memOperands().empty()) {
    return makeUnexpected(ErrorCode::VerificationError, "G_VAARG has invalid operands");
  }

  MachineOperand *DestinationOperand = Instruction.operandAt(0);
  MachineOperand *ListOperand = Instruction.operandAt(1);
  MachineOperand *AlignmentOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *ListPointer = ListOperand == nullptr ? nullptr : ListOperand->getIf<Register>();
  if (Destination == nullptr || ListPointer == nullptr || !Destination->isVirtual() ||
      !ListPointer->isVirtual() || !DestinationOperand->isDef() || !ListOperand->isUse() ||
      AlignmentOperand == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_VAARG requires virtual-register operands");
  }

  auto DestinationInfo = MRI.virtualRegisterInfo(*Destination);
  auto PointerInfo = MRI.virtualRegisterInfo(*ListPointer);
  auto RequestedAlignment = signedImmediate(*AlignmentOperand);
  if (!DestinationInfo || !PointerInfo || !DestinationInfo->get().Type ||
      !PointerInfo->get().Type || !RequestedAlignment || *RequestedAlignment <= 0 ||
      !std::has_single_bit(static_cast<std::uint64_t>(*RequestedAlignment))) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_VAARG has invalid type or alignment information");
  }

  const LowLevelType DestinationType = *DestinationInfo->get().Type;
  const LowLevelType PointerType = *PointerInfo->get().Type;
  const auto DestinationLayout = scalarLayout(DestinationType);
  const auto PointerLayout = scalarLayout(PointerType);
  if (!DestinationLayout || !PointerLayout || !PointerType.isPointer()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "G_VAARG requires scalar result and pointer list types");
  }

  auto ListLoadMemory =
      createMemoryAccess(Function, MemoryAccessFlag::Load, PointerType, *PointerLayout);
  auto ListStoreMemory =
      createMemoryAccess(Function, MemoryAccessFlag::Store, PointerType, *PointerLayout);
  if (!ListLoadMemory)
    return std::unexpected(ListLoadMemory.error());
  if (!ListStoreMemory)
    return std::unexpected(ListStoreMemory.error());

  auto CurrentList = MRI.createVirtualRegister(PointerType);
  if (!CurrentList)
    return std::unexpected(CurrentList.error());
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto ListLoad = MIRBuilder.buildInstr(opcode(GenericOpcode::G_LOAD));
  if (!ListLoad)
    return std::unexpected(ListLoad.error());
  if (auto Added = ListLoad->addDef(*CurrentList); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListLoad->addUse(*ListPointer); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListLoad->addMemOperand(**ListLoadMemory); !Added)
    return std::unexpected(Added.error());
  if (auto Built = ListLoad->build(); !Built)
    return std::unexpected(Built.error());

  Register ValueAddress = *CurrentList;
  const std::uint64_t AlignmentValue = static_cast<std::uint64_t>(*RequestedAlignment);
  auto PointerScalarType = LowLevelType::makeCategoryNeutralScalar(PointerType.scalarBitWidth());
  if (!PointerScalarType)
    return std::unexpected(PointerScalarType.error());
  if (AlignmentValue > 1) {
    auto AlignmentOffset =
        buildConstantBefore(MIRBuilder, Instruction, *PointerScalarType, AlignmentValue - 1U);
    if (!AlignmentOffset)
      return std::unexpected(AlignmentOffset.error());
    auto BiasedAddress = buildBinaryBefore(MIRBuilder, Instruction, GenericOpcode::G_PTR_ADD,
                                           PointerType, ValueAddress, *AlignmentOffset);
    if (!BiasedAddress)
      return std::unexpected(BiasedAddress.error());
    auto AlignmentMask =
        buildConstantBefore(MIRBuilder, Instruction, *PointerScalarType, ~(AlignmentValue - 1U));
    if (!AlignmentMask)
      return std::unexpected(AlignmentMask.error());
    auto AlignedAddress = buildBinaryBefore(MIRBuilder, Instruction, GenericOpcode::G_PTRMASK,
                                            PointerType, *BiasedAddress, *AlignmentMask);
    if (!AlignedAddress)
      return std::unexpected(AlignedAddress.error());
    ValueAddress = *AlignedAddress;
  }

  auto Increment = buildConstantBefore(MIRBuilder, Instruction, *PointerScalarType,
                                       DestinationLayout->AllocationSize);
  if (!Increment)
    return std::unexpected(Increment.error());
  auto NextList = buildBinaryBefore(MIRBuilder, Instruction, GenericOpcode::G_PTR_ADD, PointerType,
                                    ValueAddress, *Increment);
  if (!NextList)
    return std::unexpected(NextList.error());

  auto ListStore = MIRBuilder.buildInstr(opcode(GenericOpcode::G_STORE));
  if (!ListStore)
    return std::unexpected(ListStore.error());
  if (auto Added = ListStore->addUse(*NextList); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListStore->addUse(*ListPointer); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListStore->addMemOperand(**ListStoreMemory); !Added)
    return std::unexpected(Added.error());
  if (auto Built = ListStore->build(); !Built)
    return std::unexpected(Built.error());

  if (!MRI.useEmpty(*Destination)) {
    auto ValueLoadMemory =
        createMemoryAccess(Function, MemoryAccessFlag::Load, DestinationType, *DestinationLayout);
    if (!ValueLoadMemory)
      return std::unexpected(ValueLoadMemory.error());
    auto ValueLoad = MIRBuilder.buildInstr(opcode(GenericOpcode::G_LOAD));
    if (!ValueLoad)
      return std::unexpected(ValueLoad.error());
    if (auto Added = ValueLoad->addDef(*Destination); !Added)
      return std::unexpected(Added.error());
    if (auto Added = ValueLoad->addUse(ValueAddress); !Added)
      return std::unexpected(Added.error());
    if (auto Added = ValueLoad->addMemOperand(**ValueLoadMemory); !Added)
      return std::unexpected(Added.error());
    if (auto Built = ValueLoad->build(); !Built)
      return std::unexpected(Built.error());
  }

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "G_VAARG is detached during legalization");
  return Block->removeInstruction(Instruction);
}

Expected<void> LegalizerHelper::lower(MachineInstr &Instruction,
                                      const LegalizationDecision &Decision) {
  if (Decision.Action != LegalizationAction::Lower)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "lower received a non-lower legalization action");
  switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
  case GenericOpcode::G_VAARG:
    return lowerVAArg(Instruction);
  default: {
    const InstructionDescriptor *Descriptor =
        MIRBuilder.target().instructionDescriptor(Instruction.opcode());
    const std::string_view Name =
        Descriptor == nullptr ? std::string_view{"<unknown>"} : Descriptor->Name;
    return makeUnexpected(ErrorCode::Unsupported,
                          "generic lowering is not implemented for opcode '" + std::string(Name) +
                              "'");
  }
  }
}

Expected<void> LegalizerHelper::legalize(MachineInstr &Instruction,
                                         const LegalizationDecision &Decision) {
  switch (Decision.Action) {
  case LegalizationAction::WidenScalar:
    return widenScalar(Instruction, Decision.TypeIndex, Decision.NewType);
  case LegalizationAction::Custom:
    return Info.legalizeCustom(*this, Instruction, Decision);
  case LegalizationAction::Lower:
    return lower(Instruction, Decision);
  case LegalizationAction::Legal:
    return {};
  case LegalizationAction::Unsupported:
  case LegalizationAction::Libcall:
  case LegalizationAction::NarrowScalar:
  case LegalizationAction::MoreElements:
  case LegalizationAction::FewerElements:
  case LegalizationAction::Bitcast:
  case LegalizationAction::ChangeElementType:
  case LegalizationAction::ChangeAddressSpace:
    return makeUnexpected(ErrorCode::Unsupported,
                          "legalizer helper does not implement the requested action");
  }
  return makeUnexpected(ErrorCode::InternalError, "unknown legalization action");
}

} // namespace rcc

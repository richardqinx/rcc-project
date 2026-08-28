// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <vector>

namespace rcc::riscv {
namespace {

constexpr std::uint32_t PCRelativeHighFlag = 2;
constexpr std::uint32_t PCRelativeLowFlag = 4;
constexpr std::uint32_t AbsoluteHighFlag = 32;
constexpr std::uint32_t AbsoluteLowFlag = 64;

struct AddressPair final {
  MachineInstr *High = nullptr;
  MachineInstr *Low = nullptr;
};

[[nodiscard]] bool isInt32(std::int64_t Value) noexcept {
  return Value >= std::numeric_limits<std::int32_t>::min() &&
         Value <= std::numeric_limits<std::int32_t>::max();
}

[[nodiscard]] std::int64_t signExtend32(std::uint64_t Value) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] bool isZeroRegister(Register Reg) noexcept {
  const auto Physical = Reg.physicalRegister();
  return Physical && Physical->number() == static_cast<std::uint32_t>(PhysicalRegisterID::X0);
}

[[nodiscard]] std::optional<std::int64_t> addressOffset(const MachineOperand &Operand) noexcept {
  if (const auto *Constant = Operand.getIf<ConstantPoolOperand>())
    return Constant->Offset;
  if (const auto *Global = Operand.getIf<GlobalSymbolOperand>())
    return Global->Offset;
  if (const auto *Block = Operand.getIf<BlockAddressOperand>())
    return Block->Offset;
  if (const auto *Symbol = Operand.getIf<MCSymbolOperand>())
    return Symbol->Offset;
  return std::nullopt;
}

[[nodiscard]] bool isFoldableAddress(const MachineOperand &Operand) noexcept {
  return Operand.getIf<ConstantPoolOperand>() != nullptr ||
         Operand.getIf<GlobalSymbolOperand>() != nullptr ||
         Operand.getIf<BlockAddressOperand>() != nullptr;
}

[[nodiscard]] Expected<MachineOperand> addressWithOffset(const MachineOperand &Operand,
                                                         std::int64_t Offset) {
  if (const auto *Constant = Operand.getIf<ConstantPoolOperand>())
    return MachineOperand::constantPool(Constant->Index, Offset, Operand.targetFlags());
  if (const auto *Global = Operand.getIf<GlobalSymbolOperand>()) {
    return MachineOperand::globalSymbol(Global->Name, Offset, Operand.targetFlags(),
                                        Global->Properties);
  }
  if (const auto *Block = Operand.getIf<BlockAddressOperand>()) {
    return MachineOperand::blockAddress(Block->FunctionName, Block->BlockName, Offset,
                                        Operand.targetFlags());
  }
  if (const auto *Symbol = Operand.getIf<MCSymbolOperand>())
    return MachineOperand::mcSymbol(Symbol->Name, Offset, Operand.targetFlags());
  return makeUnexpected(ErrorCode::InvalidArgument,
                        "RISC-V base-offset folding requires a symbolic address operand");
}

[[nodiscard]] Expected<void> replaceOperand(MachineInstr &Instruction, std::size_t Index,
                                            MachineOperand Operand) {
  auto Erased = Instruction.eraseOperand(Index);
  if (!Erased)
    return std::unexpected(Erased.error());
  return Instruction.insertOperand(Index, std::move(Operand));
}

[[nodiscard]] MachineInstr *uniqueUseInstruction(MachineRegisterInfo &Registers,
                                                 Register Reg) noexcept {
  if (!Registers.hasOneUse(Reg, false))
    return nullptr;
  for (MachineOperand &Use : Registers.useOperands(Reg, false))
    return Use.parent();
  return nullptr;
}

[[nodiscard]] std::optional<AddressPair> detectFoldable(MachineInstr &High,
                                                        MachineRegisterInfo &Registers) {
  const bool IsLUI = High.opcode() == instructionOpcode(InstructionOpcode::LUI);
  const bool IsAUIPC = High.opcode() == instructionOpcode(InstructionOpcode::AUIPC);
  if (!IsLUI && !IsAUIPC)
    return std::nullopt;
  MachineOperand *HighAddress = High.operandAt(1);
  if (HighAddress == nullptr || !isFoldableAddress(*HighAddress) ||
      addressOffset(*HighAddress) != 0 ||
      HighAddress->targetFlags() != (IsAUIPC ? PCRelativeHighFlag : AbsoluteHighFlag)) {
    return std::nullopt;
  }
  MachineOperand *HighDefinition = High.operandAt(0);
  const auto HighRegister =
      HighDefinition == nullptr ? std::nullopt : HighDefinition->registerValue();
  if (!HighRegister)
    return std::nullopt;
  MachineInstr *Low = uniqueUseInstruction(Registers, *HighRegister);
  if (Low == nullptr || Low->opcode() != instructionOpcode(InstructionOpcode::ADDI))
    return std::nullopt;
  const auto LowBase =
      Low->operandAt(1) == nullptr ? std::nullopt : Low->operandAt(1)->registerValue();
  MachineOperand *LowAddress = Low->operandAt(2);
  if (!LowBase || *LowBase != *HighRegister || LowAddress == nullptr)
    return std::nullopt;
  if (IsLUI) {
    if (!isFoldableAddress(*LowAddress) || addressOffset(*LowAddress) != 0 ||
        LowAddress->targetFlags() != AbsoluteLowFlag) {
      return std::nullopt;
    }
  } else if (LowAddress->getIf<MCSymbolOperand>() == nullptr ||
             LowAddress->targetFlags() != PCRelativeLowFlag) {
    return std::nullopt;
  }
  return AddressPair{.High = &High, .Low = Low};
}

[[nodiscard]] Expected<bool> foldOffset(AddressPair Pair, MachineInstr &Tail, std::int64_t Offset,
                                        MachineRegisterInfo &Registers) {
  if (!isInt32(Offset))
    return false;
  MachineOperand *HighAddress = Pair.High->operandAt(1);
  MachineOperand *LowAddress = Pair.Low->operandAt(2);
  MachineOperand *LowDefinition = Pair.Low->operandAt(0);
  MachineOperand *TailDefinition = Tail.operandAt(0);
  if (HighAddress == nullptr || LowAddress == nullptr || LowDefinition == nullptr ||
      TailDefinition == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V base-offset sequence has incomplete operands");
  }
  auto LowRegister = LowDefinition->registerValue();
  auto TailRegister = TailDefinition->registerValue();
  if (!LowRegister || !TailRegister)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V base-offset sequence has non-register definitions");
  auto NewHighAddress = addressWithOffset(*HighAddress, Offset);
  if (!NewHighAddress)
    return std::unexpected(NewHighAddress.error());
  std::optional<MachineOperand> NewLowAddress;
  if (Pair.High->opcode() != instructionOpcode(InstructionOpcode::AUIPC)) {
    auto Address = addressWithOffset(*LowAddress, Offset);
    if (!Address)
      return std::unexpected(Address.error());
    NewLowAddress.emplace(std::move(*Address));
  }
  auto ReplacedHigh = replaceOperand(*Pair.High, 1, std::move(*NewHighAddress));
  if (!ReplacedHigh)
    return std::unexpected(ReplacedHigh.error());
  if (NewLowAddress) {
    auto ReplacedLow = replaceOperand(*Pair.Low, 2, std::move(*NewLowAddress));
    if (!ReplacedLow)
      return std::unexpected(ReplacedLow.error());
  }
  auto ReplacedRegisters = Registers.replaceRegWith(*TailRegister, *LowRegister);
  if (!ReplacedRegisters)
    return std::unexpected(ReplacedRegisters.error());
  auto Removed = Tail.eraseFromParent();
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

[[nodiscard]] Expected<bool> foldLargeOffset(AddressPair Pair, MachineInstr &Tail,
                                             Register GlobalAddress,
                                             MachineRegisterInfo &Registers) {
  MachineOperand *LeftOperand = Tail.operandAt(1);
  MachineOperand *RightOperand = Tail.operandAt(2);
  const auto Left = LeftOperand == nullptr ? std::nullopt : LeftOperand->registerValue();
  const auto Right = RightOperand == nullptr ? std::nullopt : RightOperand->registerValue();
  if (!Left || !Right)
    return false;
  const Register OffsetRegister = *Left == GlobalAddress ? *Right : *Left;
  if (!OffsetRegister.isVirtual() || !Registers.hasOneUse(OffsetRegister, false))
    return false;
  MachineInstr *OffsetTail = Registers.getVRegDef(OffsetRegister);
  if (OffsetTail == nullptr)
    return false;

  const bool IsADDI = OffsetTail->opcode() == instructionOpcode(InstructionOpcode::ADDI);
  const bool IsADDIW = OffsetTail->opcode() == instructionOpcode(InstructionOpcode::ADDIW);
  if (IsADDI || IsADDIW) {
    MachineOperand *BaseOperand = OffsetTail->operandAt(1);
    MachineOperand *ImmediateOperand = OffsetTail->operandAt(2);
    const auto Base = BaseOperand == nullptr ? std::nullopt : BaseOperand->registerValue();
    const auto *Low =
        ImmediateOperand == nullptr ? nullptr : ImmediateOperand->getIf<std::int64_t>();
    if (!Base || Low == nullptr || ImmediateOperand->targetFlags() != 0)
      return false;
    if (isZeroRegister(*Base)) {
      auto Folded = foldOffset(Pair, Tail, *Low, Registers);
      if (!Folded || !*Folded)
        return Folded;
      auto Removed = OffsetTail->eraseFromParent();
      if (!Removed)
        return std::unexpected(Removed.error());
      return true;
    }
    MachineInstr *OffsetHigh = Registers.getVRegDef(*Base);
    if (OffsetHigh == nullptr ||
        OffsetHigh->opcode() != instructionOpcode(InstructionOpcode::LUI) ||
        !Registers.hasOneUse(*Base, false)) {
      return false;
    }
    MachineOperand *HighImmediate = OffsetHigh->operandAt(1);
    const auto *High = HighImmediate == nullptr ? nullptr : HighImmediate->getIf<std::int64_t>();
    if (High == nullptr || HighImmediate->targetFlags() != 0)
      return false;
    std::int64_t Offset = signExtend32(static_cast<std::uint64_t>(*High) << 12U) + *Low;
    if (IsADDIW)
      Offset = signExtend32(static_cast<std::uint64_t>(Offset));
    auto Folded = foldOffset(Pair, Tail, Offset, Registers);
    if (!Folded || !*Folded)
      return Folded;
    auto Removed = OffsetTail->eraseFromParent();
    if (!Removed)
      return std::unexpected(Removed.error());
    Removed = OffsetHigh->eraseFromParent();
    if (!Removed)
      return std::unexpected(Removed.error());
    return true;
  }

  if (OffsetTail->opcode() != instructionOpcode(InstructionOpcode::LUI))
    return false;
  MachineOperand *ImmediateOperand = OffsetTail->operandAt(1);
  const auto *High =
      ImmediateOperand == nullptr ? nullptr : ImmediateOperand->getIf<std::int64_t>();
  if (High == nullptr || ImmediateOperand->targetFlags() != 0)
    return false;
  const std::int64_t Offset = signExtend32(static_cast<std::uint64_t>(*High) << 12U);
  auto Folded = foldOffset(Pair, Tail, Offset, Registers);
  if (!Folded || !*Folded)
    return Folded;
  auto Removed = OffsetTail->eraseFromParent();
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

[[nodiscard]] Expected<bool> foldShiftedOffset(AddressPair Pair, MachineInstr &Tail,
                                               Register GlobalAddress,
                                               MachineRegisterInfo &Registers) {
  if (Tail.operandAt(2) == nullptr || Tail.operandAt(2)->registerValue() != GlobalAddress)
    return false;
  unsigned Shift = 0;
  if (Tail.opcode() == instructionOpcode(InstructionOpcode::SH1ADD))
    Shift = 1;
  else if (Tail.opcode() == instructionOpcode(InstructionOpcode::SH2ADD))
    Shift = 2;
  else if (Tail.opcode() == instructionOpcode(InstructionOpcode::SH3ADD))
    Shift = 3;
  else
    return false;
  const auto Shifted =
      Tail.operandAt(1) == nullptr ? std::nullopt : Tail.operandAt(1)->registerValue();
  if (!Shifted || !Shifted->isVirtual() || !Registers.hasOneUse(*Shifted, false))
    return false;
  MachineInstr *OffsetTail = Registers.getVRegDef(*Shifted);
  if (OffsetTail == nullptr || OffsetTail->opcode() != instructionOpcode(InstructionOpcode::ADDI)) {
    return false;
  }
  const auto Base = OffsetTail->operandAt(1) == nullptr ? std::nullopt
                                                        : OffsetTail->operandAt(1)->registerValue();
  const auto *Immediate = OffsetTail->operandAt(2) == nullptr
                              ? nullptr
                              : OffsetTail->operandAt(2)->getIf<std::int64_t>();
  if (!Base || !isZeroRegister(*Base) || Immediate == nullptr)
    return false;
  const std::int64_t Offset =
      static_cast<std::int64_t>(static_cast<std::uint64_t>(*Immediate) << Shift);
  auto Folded = foldOffset(Pair, Tail, Offset, Registers);
  if (!Folded || !*Folded)
    return Folded;
  auto Removed = OffsetTail->eraseFromParent();
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

[[nodiscard]] Expected<bool> foldArithmeticOffset(AddressPair Pair,
                                                  MachineRegisterInfo &Registers) {
  MachineOperand *LowDefinition = Pair.Low->operandAt(0);
  const auto LowRegister = LowDefinition == nullptr ? std::nullopt : LowDefinition->registerValue();
  if (!LowRegister)
    return false;
  MachineInstr *Tail = uniqueUseInstruction(Registers, *LowRegister);
  if (Tail == nullptr)
    return false;
  if (Tail->opcode() == instructionOpcode(InstructionOpcode::ADDI)) {
    const auto *Immediate =
        Tail->operandAt(2) == nullptr ? nullptr : Tail->operandAt(2)->getIf<std::int64_t>();
    if (Immediate == nullptr)
      return false;
    MachineOperand *TailDefinition = Tail->operandAt(0);
    const auto TailRegister =
        TailDefinition == nullptr ? std::nullopt : TailDefinition->registerValue();
    MachineInstr *SecondTail =
        TailRegister ? uniqueUseInstruction(Registers, *TailRegister) : nullptr;
    if (SecondTail != nullptr &&
        SecondTail->opcode() == instructionOpcode(InstructionOpcode::ADDI)) {
      const auto *SecondImmediate = SecondTail->operandAt(2) == nullptr
                                        ? nullptr
                                        : SecondTail->operandAt(2)->getIf<std::int64_t>();
      if (SecondImmediate != nullptr) {
        auto Folded = foldOffset(Pair, *SecondTail, *Immediate + *SecondImmediate, Registers);
        if (!Folded || !*Folded)
          return Folded;
        auto Removed = Tail->eraseFromParent();
        if (!Removed)
          return std::unexpected(Removed.error());
        return true;
      }
    }
    return foldOffset(Pair, *Tail, *Immediate, Registers);
  }
  if (Tail->opcode() == instructionOpcode(InstructionOpcode::ADD))
    return foldLargeOffset(Pair, *Tail, *LowRegister, Registers);
  if (Tail->opcode() == instructionOpcode(InstructionOpcode::SH1ADD) ||
      Tail->opcode() == instructionOpcode(InstructionOpcode::SH2ADD) ||
      Tail->opcode() == instructionOpcode(InstructionOpcode::SH3ADD)) {
    return foldShiftedOffset(Pair, *Tail, *LowRegister, Registers);
  }
  return false;
}

[[nodiscard]] bool isOffsetMemoryInstruction(const MachineInstr &Instruction,
                                             const TargetDescription &Target) noexcept {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr || (!Descriptor->Flags.has(InstructionFlag::MayLoad) &&
                                !Descriptor->Flags.has(InstructionFlag::MayStore))) {
    return false;
  }
  return Instruction.operandAt(0) != nullptr && Instruction.operandAt(1) != nullptr &&
         Instruction.operandAt(2) != nullptr;
}

[[nodiscard]] Expected<bool> foldIntoMemoryOperations(AddressPair Pair,
                                                      MachineRegisterInfo &Registers,
                                                      const TargetDescription &Target) {
  MachineOperand *LowDefinition = Pair.Low->operandAt(0);
  const auto LowRegister = LowDefinition == nullptr ? std::nullopt : LowDefinition->registerValue();
  if (!LowRegister)
    return false;
  std::vector<MachineInstr *> Uses;
  for (MachineOperand &Use : Registers.useOperands(*LowRegister, false)) {
    MachineInstr *Instruction = Use.parent();
    if (Instruction != nullptr && std::ranges::find(Uses, Instruction) == Uses.end())
      Uses.push_back(Instruction);
  }
  if (Uses.empty())
    return false;

  std::optional<std::int64_t> CommonOffset;
  for (MachineInstr *Use : Uses) {
    if (!isOffsetMemoryInstruction(*Use, Target))
      return false;
    const auto Value = Use->operandAt(0)->registerValue();
    const auto Base = Use->operandAt(1)->registerValue();
    const auto *Offset = Use->operandAt(2)->getIf<std::int64_t>();
    if (!Base || *Base != *LowRegister || (Value && *Value == *LowRegister) || Offset == nullptr)
      return false;
    if (CommonOffset && *CommonOffset != *Offset)
      return false;
    CommonOffset = *Offset;
  }
  assert(CommonOffset && "nonempty validated use list must define a common offset");

  MachineOperand *HighAddress = Pair.High->operandAt(1);
  MachineOperand *LowAddress = Pair.Low->operandAt(2);
  MachineOperand *HighDefinition = Pair.High->operandAt(0);
  if (HighAddress == nullptr || LowAddress == nullptr || HighDefinition == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V base-offset sequence has incomplete address operands");
  const auto HighRegister = HighDefinition->registerValue();
  const auto OldOffset = addressOffset(*HighAddress);
  if (!HighRegister || !OldOffset)
    return false;
  const std::int64_t NewOffset = *OldOffset + *CommonOffset;
  if (!isInt32(NewOffset))
    return false;

  auto NewHighAddress = addressWithOffset(*HighAddress, NewOffset);
  if (!NewHighAddress)
    return std::unexpected(NewHighAddress.error());
  auto NewLowAddress = Pair.High->opcode() == instructionOpcode(InstructionOpcode::AUIPC)
                           ? Expected<MachineOperand>(LowAddress->cloneDetached())
                           : addressWithOffset(*LowAddress, NewOffset);
  if (!NewLowAddress)
    return std::unexpected(NewLowAddress.error());
  auto ReplacedHigh = replaceOperand(*Pair.High, 1, std::move(*NewHighAddress));
  if (!ReplacedHigh)
    return std::unexpected(ReplacedHigh.error());
  if (Pair.High->opcode() != instructionOpcode(InstructionOpcode::AUIPC)) {
    auto ReplacedLow = replaceOperand(*Pair.Low, 2, NewLowAddress->cloneDetached());
    if (!ReplacedLow)
      return std::unexpected(ReplacedLow.error());
  }
  for (MachineInstr *Use : Uses) {
    auto SetBase = Use->operandAt(1)->setRegister(*HighRegister);
    if (!SetBase)
      return std::unexpected(SetBase.error());
    auto ReplacedOffset = replaceOperand(*Use, 2, NewLowAddress->cloneDetached());
    if (!ReplacedOffset)
      return std::unexpected(ReplacedOffset.error());
  }
  auto Removed = Pair.Low->eraseFromParent();
  if (!Removed)
    return std::unexpected(Removed.error());
  return true;
}

class RISCVMergeBaseOffsetPass final : public MachineFunctionPass {
public:
  [[nodiscard]] const MachinePassContract &contract() const noexcept override {
    static const MachinePassContract Contract = [] {
      MachinePassContract Value{
          .Name = "riscv-merge-base-offset",
          .Requires = {MachineFunctionProperty::IsSSA},
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
    MachineRegisterInfo &Registers = Function.registerInfo();
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
        auto Pair = detectFoldable(*Instruction, Registers);
        if (!Pair) {
          Instruction = Instruction->next();
          continue;
        }
        auto Arithmetic = foldArithmeticOffset(*Pair, Registers);
        if (!Arithmetic)
          return std::unexpected(Arithmetic.error());
        Result.Changed |= *Arithmetic;
        auto Memory = foldIntoMemoryOperations(*Pair, Registers, Target);
        if (!Memory)
          return std::unexpected(Memory.error());
        Result.Changed |= *Memory;
        Instruction = Pair->High->next();
      }
    }
    if (Result.Changed) {
      Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                        MachinePassChange::Registers};
      Result.Preserved = PreservedAnalyses::none();
    }
    return Result;
  }
};

} // namespace

std::unique_ptr<MachineFunctionPass> createMergeBaseOffsetPass() {
  return std::make_unique<RISCVMergeBaseOffsetPass>();
}

} // namespace rcc::riscv

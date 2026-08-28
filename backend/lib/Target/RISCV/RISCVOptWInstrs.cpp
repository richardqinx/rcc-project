// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] bool isOpcode(const MachineInstr &Instruction, InstructionOpcode Opcode) noexcept {
  return Instruction.opcode() == instructionOpcode(Opcode);
}

[[nodiscard]] const Register *registerOperand(const MachineInstr &Instruction,
                                              std::size_t Index) noexcept {
  const MachineOperand *Operand = Instruction.operandAt(Index);
  return Operand == nullptr ? nullptr : Operand->getIf<Register>();
}

[[nodiscard]] const std::int64_t *immediateOperand(const MachineInstr &Instruction,
                                                   std::size_t Index) noexcept {
  const MachineOperand *Operand = Instruction.operandAt(Index);
  return Operand == nullptr ? nullptr : Operand->getIf<std::int64_t>();
}

[[nodiscard]] bool isZeroRegister(Register Reg) noexcept {
  const auto Physical = Reg.physicalRegister();
  return Physical && Physical->number() == static_cast<std::uint32_t>(PhysicalRegisterID::X0);
}

[[nodiscard]] std::optional<std::size_t> operandIndex(const MachineInstr &Instruction,
                                                      const MachineOperand &Operand) noexcept {
  for (std::size_t Index = 0; Index < Instruction.operandCount(); ++Index) {
    if (Instruction.operandAt(Index) == &Operand)
      return Index;
  }
  return std::nullopt;
}

[[nodiscard]] bool hasOneExplicitDef(const MachineInstr &Instruction) noexcept {
  return std::ranges::count_if(Instruction.operands(), [](const MachineOperand &Operand) {
           return Operand.isDef() && !Operand.isImplicit();
         }) == 1;
}

[[nodiscard]] bool isWordConsumer(Opcode Opcode) noexcept {
  constexpr InstructionOpcode Opcodes[]{
      InstructionOpcode::ADDIW,     InstructionOpcode::ADDW,     InstructionOpcode::DIVUW,
      InstructionOpcode::DIVW,      InstructionOpcode::MULW,     InstructionOpcode::REMUW,
      InstructionOpcode::REMW,      InstructionOpcode::SLLW,     InstructionOpcode::SRAIW,
      InstructionOpcode::SRAW,      InstructionOpcode::SRLIW,    InstructionOpcode::SRLW,
      InstructionOpcode::SUBW,      InstructionOpcode::FMV_W_X,  InstructionOpcode::FCVT_S_W,
      InstructionOpcode::FCVT_S_WU, InstructionOpcode::FCVT_D_W, InstructionOpcode::FCVT_D_WU,
  };
  return std::ranges::any_of(
      Opcodes, [Opcode](InstructionOpcode Value) { return Opcode == instructionOpcode(Value); });
}

struct UserDemand final {
  const MachineInstr *Instruction = nullptr;
  unsigned Bits = 0;

  friend bool operator==(const UserDemand &, const UserDemand &) = default;
};

[[nodiscard]] bool hasAllNBitUsers(const MachineInstr &Origin, const MachineFunction &Function,
                                   unsigned OriginBits) {
  std::vector<UserDemand> Visited;
  std::vector<UserDemand> Worklist{{&Origin, OriginBits}};
  const MachineRegisterInfo &Registers = Function.registerInfo();

  while (!Worklist.empty()) {
    const UserDemand Demand = Worklist.back();
    Worklist.pop_back();
    if (std::ranges::find(Visited, Demand) != Visited.end())
      continue;
    Visited.push_back(Demand);

    const MachineInstr &Instruction = *Demand.Instruction;
    if (!hasOneExplicitDef(Instruction))
      return false;
    const Register *Destination = registerOperand(Instruction, 0);
    if (Destination == nullptr || !Destination->isVirtual())
      return false;

    for (const MachineOperand &Use :
         const_cast<MachineRegisterInfo &>(Registers).useOperands(*Destination, false)) {
      const MachineInstr *User = Use.parent();
      if (User == nullptr)
        return false;
      const auto UseIndex = operandIndex(*User, Use);
      if (!UseIndex)
        return false;
      const Opcode UserOpcode = User->opcode();

      if (isWordConsumer(UserOpcode)) {
        if (Demand.Bits < 32)
          return false;
        continue;
      }
      if (UserOpcode == instructionOpcode(InstructionOpcode::SEXT_B)) {
        if (Demand.Bits < 8)
          return false;
        continue;
      }
      if (UserOpcode == instructionOpcode(InstructionOpcode::SEXT_H) ||
          UserOpcode == instructionOpcode(InstructionOpcode::ZEXT_H_RV64)) {
        if (Demand.Bits < 16)
          return false;
        continue;
      }
      if (UserOpcode == instructionOpcode(InstructionOpcode::SRLI)) {
        const std::int64_t *Amount = immediateOperand(*User, 2);
        if (Amount == nullptr || *Amount < 0 || *Amount >= 64 ||
            Demand.Bits <= static_cast<unsigned>(*Amount)) {
          return false;
        }
        Worklist.push_back({User, Demand.Bits - static_cast<unsigned>(*Amount)});
        continue;
      }
      if (UserOpcode == instructionOpcode(InstructionOpcode::SLLI) ||
          UserOpcode == instructionOpcode(InstructionOpcode::SLLIW)) {
        const std::int64_t *Amount = immediateOperand(*User, 2);
        const unsigned Width =
            UserOpcode == instructionOpcode(InstructionOpcode::SLLIW) ? 32U : 64U;
        if (Amount == nullptr || *Amount < 0 || static_cast<unsigned>(*Amount) >= Width)
          return false;
        const unsigned Shift = static_cast<unsigned>(*Amount);
        if (Demand.Bits < Width - Shift)
          Worklist.push_back({User, Demand.Bits + Shift});
        continue;
      }
      if (UserOpcode == instructionOpcode(InstructionOpcode::ANDI) ||
          UserOpcode == instructionOpcode(InstructionOpcode::ORI)) {
        const std::int64_t *Immediate = immediateOperand(*User, 2);
        if (Immediate == nullptr)
          return false;
        const std::uint64_t Mask = static_cast<std::uint64_t>(*Immediate);
        const unsigned Required = UserOpcode == instructionOpcode(InstructionOpcode::ANDI)
                                      ? static_cast<unsigned>(std::bit_width(Mask))
                                      : static_cast<unsigned>(std::bit_width(~Mask));
        if (Demand.Bits < Required)
          Worklist.push_back({User, Demand.Bits});
        continue;
      }
      if (UserOpcode == instructionOpcode(InstructionOpcode::SLL) ||
          UserOpcode == instructionOpcode(InstructionOpcode::SH1ADD) ||
          UserOpcode == instructionOpcode(InstructionOpcode::SH2ADD) ||
          UserOpcode == instructionOpcode(InstructionOpcode::SH3ADD)) {
        if (*UseIndex == 2) {
          if (Demand.Bits < 6)
            return false;
        } else {
          Worklist.push_back({User, Demand.Bits});
        }
        continue;
      }
      if (UserOpcode == instructionOpcode(InstructionOpcode::SRA) ||
          UserOpcode == instructionOpcode(InstructionOpcode::SRL)) {
        if (*UseIndex != 2 || Demand.Bits < 6)
          return false;
        continue;
      }
      if (UserOpcode == instructionOpcode(InstructionOpcode::ADD_UW) ||
          UserOpcode == instructionOpcode(InstructionOpcode::SH1ADD_UW) ||
          UserOpcode == instructionOpcode(InstructionOpcode::SH2ADD_UW) ||
          UserOpcode == instructionOpcode(InstructionOpcode::SH3ADD_UW)) {
        if (*UseIndex == 1 && Demand.Bits >= 32)
          continue;
        Worklist.push_back({User, Demand.Bits});
        continue;
      }
      if (UserOpcode == instructionOpcode(InstructionOpcode::SB) ||
          UserOpcode == instructionOpcode(InstructionOpcode::SH) ||
          UserOpcode == instructionOpcode(InstructionOpcode::SW)) {
        const unsigned StoredBits = UserOpcode == instructionOpcode(InstructionOpcode::SB)   ? 8U
                                    : UserOpcode == instructionOpcode(InstructionOpcode::SH) ? 16U
                                                                                             : 32U;
        if (*UseIndex != 0 || Demand.Bits < StoredBits)
          return false;
        continue;
      }

      const bool PropagatesLowBits = UserOpcode == opcode(GenericOpcode::COPY) ||
                                     UserOpcode == opcode(GenericOpcode::PHI) ||
                                     UserOpcode == instructionOpcode(InstructionOpcode::ADD) ||
                                     UserOpcode == instructionOpcode(InstructionOpcode::ADDI) ||
                                     UserOpcode == instructionOpcode(InstructionOpcode::AND) ||
                                     UserOpcode == instructionOpcode(InstructionOpcode::MUL) ||
                                     UserOpcode == instructionOpcode(InstructionOpcode::OR) ||
                                     UserOpcode == instructionOpcode(InstructionOpcode::SUB) ||
                                     UserOpcode == instructionOpcode(InstructionOpcode::XOR) ||
                                     UserOpcode == instructionOpcode(InstructionOpcode::XORI);
      if (!PropagatesLowBits)
        return false;
      Worklist.push_back({User, Demand.Bits});
    }
  }
  return true;
}

[[nodiscard]] bool hasAllWordUsers(const MachineInstr &Instruction,
                                   const MachineFunction &Function) {
  return hasAllNBitUsers(Instruction, Function, 32);
}

[[nodiscard]] bool isInherentlySignExtended(const MachineInstr &Instruction) noexcept {
  constexpr InstructionOpcode Opcodes[]{
      InstructionOpcode::LB,        InstructionOpcode::LBU,       InstructionOpcode::LH,
      InstructionOpcode::LHU,       InstructionOpcode::LW,        InstructionOpcode::ADDIW,
      InstructionOpcode::ADDW,      InstructionOpcode::SUBW,      InstructionOpcode::SLLIW,
      InstructionOpcode::SRLIW,     InstructionOpcode::SRAIW,     InstructionOpcode::SLLW,
      InstructionOpcode::SRLW,      InstructionOpcode::SRAW,      InstructionOpcode::MULW,
      InstructionOpcode::DIVW,      InstructionOpcode::DIVUW,     InstructionOpcode::REMW,
      InstructionOpcode::REMUW,     InstructionOpcode::LR_W,      InstructionOpcode::SC_W,
      InstructionOpcode::AMOSWAP_W, InstructionOpcode::AMOADD_W,  InstructionOpcode::AMOXOR_W,
      InstructionOpcode::AMOAND_W,  InstructionOpcode::AMOOR_W,   InstructionOpcode::AMOMIN_W,
      InstructionOpcode::AMOMAX_W,  InstructionOpcode::AMOMINU_W, InstructionOpcode::AMOMAXU_W,
      InstructionOpcode::FCVT_W_S,  InstructionOpcode::FCVT_WU_S, InstructionOpcode::FCVT_W_D,
      InstructionOpcode::FCVT_WU_D, InstructionOpcode::FMV_X_W,
  };
  return std::ranges::any_of(Opcodes,
                             [&](InstructionOpcode Value) { return isOpcode(Instruction, Value); });
}

[[nodiscard]] bool addVirtualRegister(std::vector<Register> &Worklist, Register Reg) {
  if (!Reg.isVirtual())
    return false;
  Worklist.push_back(Reg);
  return true;
}

[[nodiscard]] bool isSignExtendedWord(Register Source, MachineFunction &Function,
                                      std::vector<MachineInstr *> &FixableDefinitions) {
  if (isZeroRegister(Source))
    return true;
  if (!Source.isVirtual())
    return false;

  std::vector<Register> Visited;
  std::vector<Register> Worklist{Source};
  MachineRegisterInfo &Registers = Function.registerInfo();
  while (!Worklist.empty()) {
    const Register Reg = Worklist.back();
    Worklist.pop_back();
    if (std::ranges::find(Visited, Reg) != Visited.end())
      continue;
    Visited.push_back(Reg);

    MachineInstr *Definition = Registers.getVRegDef(Reg);
    if (Definition == nullptr)
      return false;
    if (isInherentlySignExtended(*Definition))
      continue;

    const Register *FirstSource = registerOperand(*Definition, 1);
    if (isOpcode(*Definition, InstructionOpcode::SRAI) ||
        isOpcode(*Definition, InstructionOpcode::SRLI)) {
      const std::int64_t *Amount = immediateOperand(*Definition, 2);
      if (Amount != nullptr && ((isOpcode(*Definition, InstructionOpcode::SRAI) && *Amount >= 32) ||
                                (isOpcode(*Definition, InstructionOpcode::SRLI) && *Amount > 32))) {
        continue;
      }
    }
    if (isOpcode(*Definition, InstructionOpcode::ADDI) && FirstSource != nullptr &&
        isZeroRegister(*FirstSource)) {
      continue;
    }
    if (isOpcode(*Definition, InstructionOpcode::ANDI) ||
        isOpcode(*Definition, InstructionOpcode::ORI)) {
      const std::int64_t *Immediate = immediateOperand(*Definition, 2);
      if (Immediate != nullptr) {
        const bool UnsignedElevenBit = *Immediate >= 0 && *Immediate < 2048;
        if ((isOpcode(*Definition, InstructionOpcode::ANDI) && UnsignedElevenBit) ||
            (isOpcode(*Definition, InstructionOpcode::ORI) && !UnsignedElevenBit)) {
          continue;
        }
      }
    }
    if (Definition->opcode() == opcode(GenericOpcode::COPY)) {
      if (FirstSource == nullptr ||
          (!isZeroRegister(*FirstSource) && !addVirtualRegister(Worklist, *FirstSource))) {
        return false;
      }
      continue;
    }

    const bool PropagatesFirst = isOpcode(*Definition, InstructionOpcode::REM) ||
                                 isOpcode(*Definition, InstructionOpcode::ANDI) ||
                                 isOpcode(*Definition, InstructionOpcode::ORI) ||
                                 isOpcode(*Definition, InstructionOpcode::XORI) ||
                                 isOpcode(*Definition, InstructionOpcode::SRAI);
    if (PropagatesFirst) {
      if (FirstSource == nullptr || !addVirtualRegister(Worklist, *FirstSource))
        return false;
      continue;
    }

    const bool PropagatesAll = isOpcode(*Definition, InstructionOpcode::REMU) ||
                               isOpcode(*Definition, InstructionOpcode::AND) ||
                               isOpcode(*Definition, InstructionOpcode::OR) ||
                               isOpcode(*Definition, InstructionOpcode::XOR) ||
                               isOpcode(*Definition, InstructionOpcode::MIN) ||
                               isOpcode(*Definition, InstructionOpcode::MINU) ||
                               isOpcode(*Definition, InstructionOpcode::MAX) ||
                               isOpcode(*Definition, InstructionOpcode::MAXU) ||
                               Definition->opcode() == opcode(GenericOpcode::PHI);
    if (PropagatesAll) {
      const std::size_t Step = Definition->opcode() == opcode(GenericOpcode::PHI) ? 2U : 1U;
      const std::size_t End =
          Definition->opcode() == opcode(GenericOpcode::PHI) ? Definition->operandCount() : 3U;
      for (std::size_t Index = 1; Index < End; Index += Step) {
        const Register *Input = registerOperand(*Definition, Index);
        if (Input == nullptr ||
            (!isZeroRegister(*Input) && !addVirtualRegister(Worklist, *Input))) {
          return false;
        }
      }
      continue;
    }

    const bool IsFixable = isOpcode(*Definition, InstructionOpcode::ADDI) ||
                           isOpcode(*Definition, InstructionOpcode::ADD) ||
                           isOpcode(*Definition, InstructionOpcode::LD) ||
                           isOpcode(*Definition, InstructionOpcode::LWU) ||
                           isOpcode(*Definition, InstructionOpcode::MUL) ||
                           isOpcode(*Definition, InstructionOpcode::SLLI) ||
                           isOpcode(*Definition, InstructionOpcode::SUB);
    if (!IsFixable || !hasAllWordUsers(*Definition, Function))
      return false;
    if (isOpcode(*Definition, InstructionOpcode::SLLI)) {
      const std::int64_t *Amount = immediateOperand(*Definition, 2);
      if (Amount == nullptr || *Amount < 0 || *Amount >= 32)
        return false;
    }
    if (isOpcode(*Definition, InstructionOpcode::LD)) {
      if (Definition->memOperands().size() != 1 || Definition->memOperands().front() == nullptr ||
          Definition->memOperands().front()->isVolatile()) {
        return false;
      }
    }
    if (std::ranges::find(FixableDefinitions, Definition) == FixableDefinitions.end())
      FixableDefinitions.push_back(Definition);
  }
  return true;
}

[[nodiscard]] std::optional<InstructionOpcode> wordOpcode(Opcode Opcode) noexcept {
  constexpr std::pair<InstructionOpcode, InstructionOpcode> Mappings[]{
      {InstructionOpcode::ADDI, InstructionOpcode::ADDIW},
      {InstructionOpcode::ADD, InstructionOpcode::ADDW},
      {InstructionOpcode::LD, InstructionOpcode::LW},
      {InstructionOpcode::LWU, InstructionOpcode::LW},
      {InstructionOpcode::MUL, InstructionOpcode::MULW},
      {InstructionOpcode::SLLI, InstructionOpcode::SLLIW},
      {InstructionOpcode::SUB, InstructionOpcode::SUBW},
  };
  const auto *const Mapping = std::ranges::find_if(
      Mappings, [Opcode](const auto &Entry) { return instructionOpcode(Entry.first) == Opcode; });
  return Mapping == std::end(Mappings) ? std::nullopt
                                       : std::optional<InstructionOpcode>{Mapping->second};
}

void clearWordInvalidFlags(MachineInstr &Instruction) {
  MachineInstrFlags Flags = Instruction.flags();
  Flags.clear(MachineInstrFlag::NoSignedWrap);
  Flags.clear(MachineInstrFlag::NoUnsignedWrap);
  Flags.clear(MachineInstrFlag::Exact);
  Instruction.setFlags(Flags);
}

[[nodiscard]] Expected<bool> constrainReplacementRegister(MachineFunction &Function,
                                                          const TargetDescription &Target,
                                                          Register Source, Register Destination) {
  auto SourceInfo = Function.registerInfo().virtualRegisterInfo(Source);
  if (!SourceInfo)
    return std::unexpected(SourceInfo.error());
  auto DestinationInfo = Function.registerInfo().virtualRegisterInfo(Destination);
  if (!DestinationInfo)
    return std::unexpected(DestinationInfo.error());
  const std::optional<RegisterClassID> SourceClassValue = SourceInfo->get().RegClass;
  const std::optional<RegisterClassID> DestinationClassValue = DestinationInfo->get().RegClass;
  if (!SourceClassValue || !DestinationClassValue)
    return true;
  const RegisterClassID SourceClass = *SourceClassValue;
  const RegisterClassID DestinationClass = *DestinationClassValue;
  if (Target.registers().isSubclass(SourceClass, DestinationClass))
    return true;
  if (!Target.registers().isSubclass(DestinationClass, SourceClass))
    return false;
  auto Constrained = Function.registerInfo().setRegisterClass(Source, DestinationClass);
  if (!Constrained)
    return std::unexpected(Constrained.error());
  return true;
}

[[nodiscard]] Expected<void> clearKillFlags(MachineFunction &Function, Register Reg) {
  for (MachineOperand &Use : Function.registerInfo().useOperands(Reg, true)) {
    if (!Use.registerFlags().has(RegisterOperandFlag::Kill))
      continue;
    RegisterOperandFlags Flags = Use.registerFlags();
    Flags.clear(RegisterOperandFlag::Kill);
    auto Updated = Use.setRegisterFlags(Flags);
    if (!Updated)
      return std::unexpected(Updated.error());
  }
  return {};
}

[[nodiscard]] Expected<bool> removeRedundantSignExtensions(MachineFunction &Function,
                                                           const TargetDescription &Target) {
  bool Changed = false;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
      MachineInstr *Next = Instruction->next();
      const std::int64_t *Immediate = immediateOperand(*Instruction, 2);
      if (!isOpcode(*Instruction, InstructionOpcode::ADDIW) || Immediate == nullptr ||
          *Immediate != 0) {
        Instruction = Next;
        continue;
      }
      const Register *Destination = registerOperand(*Instruction, 0);
      const Register *Source = registerOperand(*Instruction, 1);
      if (Destination == nullptr || Source == nullptr || !Destination->isVirtual() ||
          !Source->isVirtual()) {
        Instruction = Next;
        continue;
      }

      std::vector<MachineInstr *> FixableDefinitions;
      if (!hasAllWordUsers(*Instruction, Function) &&
          !isSignExtendedWord(*Source, Function, FixableDefinitions)) {
        Instruction = Next;
        continue;
      }
      auto Constrained = constrainReplacementRegister(Function, Target, *Source, *Destination);
      if (!Constrained)
        return std::unexpected(Constrained.error());
      if (!*Constrained) {
        Instruction = Next;
        continue;
      }
      for (MachineInstr *Definition : FixableDefinitions) {
        const auto Replacement = wordOpcode(Definition->opcode());
        if (!Replacement)
          return makeUnexpected(ErrorCode::InternalError,
                                "W-instruction optimization lost a fixable opcode mapping");
        auto Updated = Definition->setOpcode(instructionOpcode(*Replacement));
        if (!Updated)
          return std::unexpected(Updated.error());
        clearWordInvalidFlags(*Definition);
      }
      auto Replaced = Function.registerInfo().replaceRegWith(*Destination, *Source);
      if (!Replaced)
        return std::unexpected(Replaced.error());
      auto Cleared = clearKillFlags(Function, *Source);
      if (!Cleared)
        return std::unexpected(Cleared.error());
      auto Removed = Instruction->eraseFromParent();
      if (!Removed)
        return std::unexpected(Removed.error());
      Changed = true;
      Instruction = Next;
    }
  }
  return Changed;
}

[[nodiscard]] Expected<bool> canonicalizeWordSuffixes(MachineFunction &Function,
                                                      bool PreferWordInstructions) {
  bool Changed = false;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      std::optional<InstructionOpcode> WordOpcode;
      std::optional<InstructionOpcode> NonWordOpcode;
      const Opcode Original = Instruction->opcode();
      if (Original == instructionOpcode(InstructionOpcode::ADDW))
        NonWordOpcode = InstructionOpcode::ADD;
      else if (Original == instructionOpcode(InstructionOpcode::ADDIW))
        NonWordOpcode = InstructionOpcode::ADDI;
      else if (Original == instructionOpcode(InstructionOpcode::MULW))
        NonWordOpcode = InstructionOpcode::MUL;
      else if (Original == instructionOpcode(InstructionOpcode::SLLIW))
        NonWordOpcode = InstructionOpcode::SLLI;
      else if (Original == instructionOpcode(InstructionOpcode::SUBW))
        NonWordOpcode = InstructionOpcode::SUB;
      else if (Original == instructionOpcode(InstructionOpcode::ADD))
        WordOpcode = InstructionOpcode::ADDW;
      else if (Original == instructionOpcode(InstructionOpcode::ADDI))
        WordOpcode = InstructionOpcode::ADDIW;
      else if (Original == instructionOpcode(InstructionOpcode::SUB))
        WordOpcode = InstructionOpcode::SUBW;
      else if (Original == instructionOpcode(InstructionOpcode::MUL))
        WordOpcode = InstructionOpcode::MULW;
      else if (Original == instructionOpcode(InstructionOpcode::SLLI)) {
        const std::int64_t *Amount = immediateOperand(*Instruction, 2);
        if (Amount == nullptr || *Amount < 0 || *Amount >= 32)
          continue;
        WordOpcode = InstructionOpcode::SLLIW;
      } else if (Original == instructionOpcode(InstructionOpcode::LD)) {
        if (Instruction->memOperands().size() != 1 ||
            Instruction->memOperands().front() == nullptr ||
            Instruction->memOperands().front()->isVolatile()) {
          continue;
        }
        WordOpcode = InstructionOpcode::LW;
      } else if (Original == instructionOpcode(InstructionOpcode::LWU)) {
        WordOpcode = InstructionOpcode::LW;
      } else {
        continue;
      }

      if (!PreferWordInstructions && NonWordOpcode && hasAllWordUsers(*Instruction, Function)) {
        auto Updated = Instruction->setOpcode(instructionOpcode(*NonWordOpcode));
        if (!Updated)
          return std::unexpected(Updated.error());
        Changed = true;
        continue;
      }
      if ((PreferWordInstructions || Original == instructionOpcode(InstructionOpcode::LWU)) &&
          WordOpcode && hasAllWordUsers(*Instruction, Function)) {
        auto Updated = Instruction->setOpcode(instructionOpcode(*WordOpcode));
        if (!Updated)
          return std::unexpected(Updated.error());
        clearWordInvalidFlags(*Instruction);
        Changed = true;
      }
    }
  }
  return Changed;
}

[[nodiscard]] Expected<bool> runImplementation(MachineFunction &Function,
                                               const TargetDescription &Target,
                                               bool PreferWordInstructions) {
  auto Removed = removeRedundantSignExtensions(Function, Target);
  if (!Removed)
    return std::unexpected(Removed.error());
  auto Canonicalized = canonicalizeWordSuffixes(Function, PreferWordInstructions);
  if (!Canonicalized)
    return std::unexpected(Canonicalized.error());
  return *Removed || *Canonicalized;
}

class RISCVOptWInstrsPass final : public MachineFunctionPass {
public:
  explicit RISCVOptWInstrsPass(const RISCVSubtarget &Subtarget) noexcept : Subtarget(&Subtarget) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override {
    static const MachinePassContract Contract = [] {
      MachinePassContract Value{
          .Name = "riscv-opt-w-instrs",
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
    if (Subtarget == nullptr)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "RISC-V W-instruction optimization requires a subtarget");
    const bool PreferWordInstructions =
        Subtarget->features().has(RISCVSubtargetOption::PreferWordInstructions);
    auto Changed = runImplementation(Function, Target, PreferWordInstructions);
    if (!Changed)
      return std::unexpected(Changed.error());

    MachinePassResult Result;
    Result.Changed = *Changed;
    if (Result.Changed) {
      Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                        MachinePassChange::Registers};
      Result.Preserved = PreservedAnalyses::none();
    } else {
      Result.Preserved = PreservedAnalyses::all();
    }
    return Result;
  }

private:
  const RISCVSubtarget *Subtarget = nullptr;
};

} // namespace

std::unique_ptr<MachineFunctionPass> createOptWInstrsPass(const RISCVSubtarget &Subtarget) {
  return std::make_unique<RISCVOptWInstrsPass>(Subtarget);
}

} // namespace rcc::riscv

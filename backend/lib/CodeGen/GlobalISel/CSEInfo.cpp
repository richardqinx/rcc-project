// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/CSEInfo.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace rcc {
namespace {

enum class ProfileTag : std::uint64_t {
  Definition,
  Use,
  RegisterNumber,
  PhysicalRegister,
  Type,
  RegisterBank,
  NoRegisterBank,
  RegisterClass,
  NoRegisterClass,
  Immediate,
  IntegerImmediate,
  FloatingImmediate,
  IntegerPredicate,
  FloatingPredicate,
  Flags,
};

void appendTypeProfile(std::vector<std::uint64_t> &Profile, LowLevelType Type,
                       std::optional<RegisterBankID> Bank,
                       std::optional<RegisterClassID> RegClass) {
  Profile.push_back(static_cast<std::uint64_t>(ProfileTag::Type));
  Profile.push_back(Type.rawValue());
  if (Bank) {
    Profile.push_back(static_cast<std::uint64_t>(ProfileTag::RegisterBank));
    Profile.push_back(Bank->value());
  } else {
    Profile.push_back(static_cast<std::uint64_t>(ProfileTag::NoRegisterBank));
  }
  if (RegClass) {
    Profile.push_back(static_cast<std::uint64_t>(ProfileTag::RegisterClass));
    Profile.push_back(RegClass->value());
  } else {
    Profile.push_back(static_cast<std::uint64_t>(ProfileTag::NoRegisterClass));
  }
}

void appendAPIntProfile(std::vector<std::uint64_t> &Profile, const APInt &Value) {
  Profile.push_back(static_cast<std::uint64_t>(ProfileTag::IntegerImmediate));
  Profile.push_back(Value.bitWidth());
  Profile.push_back(Value.words().size());
  Profile.insert(Profile.end(), Value.words().begin(), Value.words().end());
}

void appendAPFloatProfile(std::vector<std::uint64_t> &Profile, const APFloat &Value) {
  Profile.push_back(static_cast<std::uint64_t>(ProfileTag::FloatingImmediate));
  Profile.push_back(static_cast<std::uint64_t>(Value.semantics()));
  Profile.push_back(Value.words().size());
  Profile.insert(Profile.end(), Value.words().begin(), Value.words().end());
}

void hashCombine(std::size_t &Seed, std::size_t Value) noexcept {
  Seed ^= Value + 0x9e3779b97f4a7c15ULL + (Seed << 6U) + (Seed >> 2U);
}

} // namespace

bool CSEConfigFull::shouldCSEOpcode(Opcode InstructionOpcode) const noexcept {
  switch (static_cast<GenericOpcode>(InstructionOpcode.value())) {
  default:
    return false;
  case GenericOpcode::G_ADD:
  case GenericOpcode::G_AND:
  case GenericOpcode::G_ASHR:
  case GenericOpcode::G_LSHR:
  case GenericOpcode::G_MUL:
  case GenericOpcode::G_OR:
  case GenericOpcode::G_SHL:
  case GenericOpcode::G_SUB:
  case GenericOpcode::G_XOR:
  case GenericOpcode::G_UDIV:
  case GenericOpcode::G_SDIV:
  case GenericOpcode::G_UREM:
  case GenericOpcode::G_SREM:
  case GenericOpcode::G_CONSTANT:
  case GenericOpcode::G_FCONSTANT:
  case GenericOpcode::G_IMPLICIT_DEF:
  case GenericOpcode::G_ZEXT:
  case GenericOpcode::G_SEXT:
  case GenericOpcode::G_ANYEXT:
  case GenericOpcode::G_UNMERGE_VALUES:
  case GenericOpcode::G_TRUNC:
  case GenericOpcode::G_PTR_ADD:
  case GenericOpcode::G_EXTRACT:
  case GenericOpcode::G_SELECT:
  case GenericOpcode::G_BUILD_VECTOR:
  case GenericOpcode::G_BUILD_VECTOR_TRUNC:
  case GenericOpcode::G_SEXT_INREG:
  case GenericOpcode::G_FADD:
  case GenericOpcode::G_FSUB:
  case GenericOpcode::G_FMUL:
  case GenericOpcode::G_FDIV:
  case GenericOpcode::G_FABS:
  case GenericOpcode::G_FMAXNUM:
  case GenericOpcode::G_FMINNUM:
  case GenericOpcode::G_FMAXNUM_IEEE:
  case GenericOpcode::G_FMINNUM_IEEE:
    return true;
  }
}

bool CSEConfigConstantOnly::shouldCSEOpcode(Opcode InstructionOpcode) const noexcept {
  return InstructionOpcode == opcode(GenericOpcode::G_CONSTANT) ||
         InstructionOpcode == opcode(GenericOpcode::G_FCONSTANT) ||
         InstructionOpcode == opcode(GenericOpcode::G_IMPLICIT_DEF);
}

std::unique_ptr<CSEConfigBase> getStandardCSEConfigForOpt(OptimizationLevel Level) {
  if (Level == OptimizationLevel::O0)
    return std::make_unique<CSEConfigConstantOnly>();
  return std::make_unique<CSEConfigFull>();
}

std::size_t GISelCSEInfo::CSEKeyHash::operator()(const CSEKey &Key) const noexcept {
  std::size_t Result = std::hash<const MachineBasicBlock *>{}(Key.Block);
  hashCombine(Result, std::hash<std::uint32_t>{}(Key.InstructionOpcode.value()));
  for (std::uint64_t Value : Key.Profile)
    hashCombine(Result, std::hash<std::uint64_t>{}(Value));
  return Result;
}

void GISelCSEInfo::setCSEConfig(std::unique_ptr<CSEConfigBase> NewConfig) noexcept {
  Config = std::move(NewConfig);
}

bool GISelCSEInfo::shouldCSE(Opcode InstructionOpcode) const noexcept {
  return Config != nullptr && isGenericOpcode(InstructionOpcode) &&
         Config->shouldCSEOpcode(InstructionOpcode);
}

Expected<void> GISelCSEInfo::appendRegisterProfile(std::vector<std::uint64_t> &Profile,
                                                   Register Reg, bool IsDefinition) const {
  Profile.push_back(
      static_cast<std::uint64_t>(IsDefinition ? ProfileTag::Definition : ProfileTag::Use));
  if (!IsDefinition) {
    Profile.push_back(static_cast<std::uint64_t>(ProfileTag::RegisterNumber));
    Profile.push_back(Reg.opaqueValue());
  }
  if (!Reg.isVirtual()) {
    Profile.push_back(static_cast<std::uint64_t>(ProfileTag::PhysicalRegister));
    Profile.push_back(Reg.opaqueValue());
    return {};
  }
  if (Function == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "GlobalISel CSE has no machine function");
  auto Info = Function->registerInfo().virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  const std::optional<LowLevelType> &Type = Info->get().Type;
  if (!Type.has_value()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "GlobalISel CSE requires typed virtual registers");
  }
  appendTypeProfile(Profile, Type.value(), Info->get().Bank, Info->get().RegClass);
  return {};
}

Expected<void> GISelCSEInfo::appendResultProfile(std::vector<std::uint64_t> &Profile,
                                                 const MachineIRBuilderResult &Result) const {
  if (Result.kind() == MachineIRBuilderResult::Kind::Register)
    return appendRegisterProfile(Profile, Result.reg(), true);
  Profile.push_back(static_cast<std::uint64_t>(ProfileTag::Definition));
  appendTypeProfile(Profile, Result.type(), std::nullopt, std::nullopt);
  return {};
}

Expected<GISelCSEInfo::CSEKey>
GISelCSEInfo::profileInstruction(const MachineInstr &Instruction) const {
  if (Instruction.parent() == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "cannot profile a detached instruction");
  if (!Instruction.memOperands().empty()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "GlobalISel CSE does not profile memory operands");
  }

  CSEKey Key{
      .Block = Instruction.parent(), .InstructionOpcode = Instruction.opcode(), .Profile = {}};
  for (const MachineOperand &Operand : Instruction.operands()) {
    switch (Operand.kind()) {
    case MachineOperand::Kind::Register: {
      const Register *Reg = Operand.getIf<Register>();
      if (Reg == nullptr || Operand.isImplicit()) {
        return makeUnexpected(ErrorCode::Unsupported,
                              "GlobalISel CSE does not profile implicit register operands");
      }
      auto Appended = appendRegisterProfile(Key.Profile, *Reg, Operand.isDef());
      if (!Appended)
        return std::unexpected(Appended.error());
      break;
    }
    case MachineOperand::Kind::Immediate: {
      const std::int64_t *Value = Operand.getIf<std::int64_t>();
      if (Value == nullptr)
        return makeUnexpected(ErrorCode::InternalError, "invalid immediate operand storage");
      Key.Profile.push_back(static_cast<std::uint64_t>(ProfileTag::Immediate));
      Key.Profile.push_back(std::bit_cast<std::uint64_t>(*Value));
      break;
    }
    case MachineOperand::Kind::IntegerImmediate: {
      const auto *Value = Operand.getIf<IntegerImmediateOperand>();
      if (Value == nullptr)
        return makeUnexpected(ErrorCode::InternalError,
                              "invalid integer immediate operand storage");
      appendAPIntProfile(Key.Profile, Value->Value);
      break;
    }
    case MachineOperand::Kind::FloatingImmediate: {
      const auto *Value = Operand.getIf<FloatingImmediateOperand>();
      if (Value == nullptr)
        return makeUnexpected(ErrorCode::InternalError,
                              "invalid floating immediate operand storage");
      appendAPFloatProfile(Key.Profile, Value->Bits);
      break;
    }
    case MachineOperand::Kind::Predicate: {
      const auto *Predicate = Operand.getIf<PredicateOperand>();
      if (Predicate == nullptr)
        return makeUnexpected(ErrorCode::InternalError, "invalid predicate operand storage");
      if (const IntegerPredicate *Integer = Predicate->integerPredicate()) {
        Key.Profile.push_back(static_cast<std::uint64_t>(ProfileTag::IntegerPredicate));
        Key.Profile.push_back(static_cast<std::uint64_t>(*Integer));
      } else if (const FloatingPredicate *Floating = Predicate->floatingPredicate()) {
        Key.Profile.push_back(static_cast<std::uint64_t>(ProfileTag::FloatingPredicate));
        Key.Profile.push_back(static_cast<std::uint64_t>(*Floating));
      } else {
        return makeUnexpected(ErrorCode::InternalError, "empty predicate operand");
      }
      break;
    }
    default:
      return makeUnexpected(ErrorCode::Unsupported,
                            "GlobalISel CSE encountered an unsupported operand kind");
    }
  }
  Key.Profile.push_back(static_cast<std::uint64_t>(ProfileTag::Flags));
  Key.Profile.push_back(Instruction.flags().rawValue());
  return Key;
}

Expected<GISelCSEInfo::CSEKey> GISelCSEInfo::profileBuilderInstruction(
    Opcode InstructionOpcode, std::span<const MachineIRBuilderResult> Destinations,
    std::span<const Register> Sources, std::span<const std::int64_t> Immediates,
    MachineInstrFlags Flags, const MachineBasicBlock &Block) const {
  CSEKey Key{.Block = &Block, .InstructionOpcode = InstructionOpcode, .Profile = {}};
  for (const MachineIRBuilderResult &Destination : Destinations) {
    auto Appended = appendResultProfile(Key.Profile, Destination);
    if (!Appended)
      return std::unexpected(Appended.error());
  }
  for (Register Source : Sources) {
    auto Appended = appendRegisterProfile(Key.Profile, Source, false);
    if (!Appended)
      return std::unexpected(Appended.error());
  }
  for (std::int64_t Immediate : Immediates) {
    Key.Profile.push_back(static_cast<std::uint64_t>(ProfileTag::Immediate));
    Key.Profile.push_back(std::bit_cast<std::uint64_t>(Immediate));
  }
  Key.Profile.push_back(static_cast<std::uint64_t>(ProfileTag::Flags));
  Key.Profile.push_back(Flags.rawValue());
  return Key;
}

Expected<GISelCSEInfo::CSEKey>
GISelCSEInfo::profileConstant(const MachineIRBuilderResult &Destination, const APInt &Value,
                              const MachineBasicBlock &Block) const {
  CSEKey Key{
      .Block = &Block, .InstructionOpcode = opcode(GenericOpcode::G_CONSTANT), .Profile = {}};
  auto Appended = appendResultProfile(Key.Profile, Destination);
  if (!Appended)
    return std::unexpected(Appended.error());
  appendAPIntProfile(Key.Profile, Value);
  Key.Profile.push_back(static_cast<std::uint64_t>(ProfileTag::Flags));
  Key.Profile.push_back(0);
  return Key;
}

Expected<void> GISelCSEInfo::insertInstruction(MachineInstr &Instruction) {
  TemporaryInstructions.remove(&Instruction);
  auto Key = profileInstruction(Instruction);
  if (!Key)
    return std::unexpected(Key.error());
  auto [Position, Inserted] = CSEMap.emplace(*Key, &Instruction);
  if (Inserted)
    InstructionMapping.emplace(&Instruction, std::move(*Key));
  return {};
}

void GISelCSEInfo::recordNewInstruction(MachineInstr &Instruction) {
  if (shouldCSE(Instruction.opcode()))
    TemporaryInstructions.insert(&Instruction);
}

Expected<void> GISelCSEInfo::handleRecordedInstruction(MachineInstr &Instruction) {
  TemporaryInstructions.remove(&Instruction);
  handleRemoveInstruction(Instruction);
  if (Instruction.parent() == nullptr || !shouldCSE(Instruction.opcode()))
    return {};
  return insertInstruction(Instruction);
}

Expected<void> GISelCSEInfo::handleRecordedInstructions() {
  if (HandlingRecordedInstructions)
    return {};
  HandlingRecordedInstructions = true;
  while (!TemporaryInstructions.empty()) {
    MachineInstr *Instruction = TemporaryInstructions.popBack();
    if (Instruction == nullptr)
      continue;
    auto Handled = handleRecordedInstruction(*Instruction);
    if (!Handled) {
      HandlingRecordedInstructions = false;
      return std::unexpected(Handled.error());
    }
  }
  HandlingRecordedInstructions = false;
  return {};
}

void GISelCSEInfo::handleRemoveInstruction(MachineInstr &Instruction) noexcept {
  const auto Mapping = InstructionMapping.find(&Instruction);
  if (Mapping != InstructionMapping.end()) {
    const auto Position = CSEMap.find(Mapping->second);
    if (Position != CSEMap.end() && Position->second == &Instruction)
      CSEMap.erase(Position);
    InstructionMapping.erase(Mapping);
  }
  TemporaryInstructions.remove(&Instruction);
}

Expected<MachineInstr *> GISelCSEInfo::getMachineInstrIfExists(
    Opcode InstructionOpcode, std::span<const MachineIRBuilderResult> Destinations,
    std::span<const Register> Sources, std::span<const std::int64_t> Immediates,
    MachineInstrFlags Flags, MachineBasicBlock &Block) {
  auto Handled = handleRecordedInstructions();
  if (!Handled)
    return std::unexpected(Handled.error());
  auto Key =
      profileBuilderInstruction(InstructionOpcode, Destinations, Sources, Immediates, Flags, Block);
  if (!Key)
    return std::unexpected(Key.error());
  const auto Position = CSEMap.find(*Key);
  if (Position == CSEMap.end())
    return nullptr;
  countOpcodeHit(InstructionOpcode);
  return Position->second;
}

Expected<MachineInstr *>
GISelCSEInfo::getConstantIfExists(const MachineIRBuilderResult &Destination, const APInt &Value,
                                  MachineBasicBlock &Block) {
  auto Handled = handleRecordedInstructions();
  if (!Handled)
    return std::unexpected(Handled.error());
  auto Key = profileConstant(Destination, Value, Block);
  if (!Key)
    return std::unexpected(Key.error());
  const auto Position = CSEMap.find(*Key);
  if (Position == CSEMap.end())
    return nullptr;
  countOpcodeHit(opcode(GenericOpcode::G_CONSTANT));
  return Position->second;
}

void GISelCSEInfo::countOpcodeHit(Opcode InstructionOpcode) noexcept {
  ++OpcodeHitTable[InstructionOpcode.value()];
}

Expected<void> GISelCSEInfo::analyze(MachineFunction &NewFunction) {
  if (Config == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "GlobalISel CSE configuration is not set");
  CSEMap.clear();
  InstructionMapping.clear();
  TemporaryInstructions.clear();
  OpcodeHitTable.clear();
  HandlingRecordedInstructions = false;
  Function = &NewFunction;
  for (MachineBasicBlock *Block = NewFunction.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      if (!shouldCSE(Instruction->opcode()))
        continue;
      auto Inserted = insertInstruction(*Instruction);
      if (!Inserted)
        return std::unexpected(Inserted.error());
    }
  }
  return {};
}

Expected<void> GISelCSEInfo::verify() {
  auto Handled = handleRecordedInstructions();
  if (!Handled)
    return std::unexpected(Handled.error());
  for (const auto &[Instruction, Key] : InstructionMapping) {
    if (Instruction == nullptr || Instruction->parent() == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "GlobalISel CSE maps a detached instruction");
    auto CurrentKey = profileInstruction(*Instruction);
    if (!CurrentKey)
      return std::unexpected(CurrentKey.error());
    const auto Position = CSEMap.find(*CurrentKey);
    if (*CurrentKey != Key || Position == CSEMap.end() || Position->second != Instruction) {
      return makeUnexpected(ErrorCode::InternalError,
                            "GlobalISel CSE instruction mapping is inconsistent");
    }
  }
  for (const auto &[Key, Instruction] : CSEMap) {
    const auto Mapping = InstructionMapping.find(Instruction);
    if (Mapping == InstructionMapping.end() || Mapping->second != Key) {
      return makeUnexpected(ErrorCode::InternalError,
                            "GlobalISel CSE table has no instruction mapping");
    }
  }
  return {};
}

void GISelCSEInfo::releaseMemory() noexcept {
  CSEMap.clear();
  InstructionMapping.clear();
  TemporaryInstructions.clear();
  OpcodeHitTable.clear();
  Config.reset();
  Function = nullptr;
  HandlingRecordedInstructions = false;
}

void GISelCSEInfo::erasingInstr(MachineInstr &Instruction) {
  handleRemoveInstruction(Instruction);
}

void GISelCSEInfo::createdInstr(MachineInstr &Instruction) {
  recordNewInstruction(Instruction);
}

void GISelCSEInfo::changingInstr(MachineInstr &Instruction) {
  erasingInstr(Instruction);
  createdInstr(Instruction);
}

void GISelCSEInfo::changedInstr(MachineInstr &Instruction) {
  changingInstr(Instruction);
}

} // namespace rcc

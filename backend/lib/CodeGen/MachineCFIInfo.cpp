// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCFIInfo.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace rcc {
namespace {

[[nodiscard]] Expected<void> verifyVectorRegisters(const MachineCFIVectorRegistersRule &Rule,
                                                   ErrorCode Code) {
  if (Rule.VectorRegisters.empty()) {
    return makeUnexpected(Code, "CFI vector-register rule must contain at least one lane");
  }
  return {};
}

} // namespace

MachineCFIInstruction MachineCFIInstruction::sameValue(DwarfRegisterNumber Register) {
  return MachineCFIInstruction(MachineCFIOperation::SameValue, MachineCFIRegisterRule{Register});
}

MachineCFIInstruction MachineCFIInstruction::rememberState() {
  return MachineCFIInstruction(MachineCFIOperation::RememberState, MachineCFINoOperands{});
}

MachineCFIInstruction MachineCFIInstruction::restoreState() {
  return MachineCFIInstruction(MachineCFIOperation::RestoreState, MachineCFINoOperands{});
}

MachineCFIInstruction MachineCFIInstruction::offset(DwarfRegisterNumber Register,
                                                    std::int64_t Offset) {
  return MachineCFIInstruction(MachineCFIOperation::Offset,
                               MachineCFIRegisterOffsetRule{Register, Offset});
}

MachineCFIInstruction MachineCFIInstruction::llvmDefAddressSpaceCFA(DwarfRegisterNumber Register,
                                                                    std::int64_t Offset,
                                                                    std::uint32_t AddressSpace) {
  return MachineCFIInstruction(MachineCFIOperation::LLVMDefAddressSpaceCFA,
                               MachineCFIAddressSpaceCFARule{Register, Offset, AddressSpace});
}

MachineCFIInstruction MachineCFIInstruction::defCFARegister(DwarfRegisterNumber Register) {
  return MachineCFIInstruction(MachineCFIOperation::DefCFARegister,
                               MachineCFIRegisterRule{Register});
}

MachineCFIInstruction MachineCFIInstruction::defCFAOffset(std::int64_t Offset) {
  return MachineCFIInstruction(MachineCFIOperation::DefCFAOffset, MachineCFIOffsetRule{Offset});
}

MachineCFIInstruction MachineCFIInstruction::defCFA(DwarfRegisterNumber Register,
                                                    std::int64_t Offset) {
  return MachineCFIInstruction(MachineCFIOperation::DefCFA,
                               MachineCFIRegisterOffsetRule{Register, Offset});
}

MachineCFIInstruction MachineCFIInstruction::relativeOffset(DwarfRegisterNumber Register,
                                                            std::int64_t Offset) {
  return MachineCFIInstruction(MachineCFIOperation::RelativeOffset,
                               MachineCFIRegisterOffsetRule{Register, Offset});
}

MachineCFIInstruction MachineCFIInstruction::adjustCFAOffset(std::int64_t Offset) {
  return MachineCFIInstruction(MachineCFIOperation::AdjustCFAOffset, MachineCFIOffsetRule{Offset});
}

Expected<MachineCFIInstruction> MachineCFIInstruction::escape(std::vector<std::uint8_t> Bytes) {
  if (Bytes.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "CFI escape byte sequence cannot be empty");
  return MachineCFIInstruction(MachineCFIOperation::Escape, MachineCFIEscapeRule{std::move(Bytes)});
}

MachineCFIInstruction MachineCFIInstruction::restore(DwarfRegisterNumber Register) {
  return MachineCFIInstruction(MachineCFIOperation::Restore, MachineCFIRegisterRule{Register});
}

MachineCFIInstruction MachineCFIInstruction::undefined(DwarfRegisterNumber Register) {
  return MachineCFIInstruction(MachineCFIOperation::Undefined, MachineCFIRegisterRule{Register});
}

MachineCFIInstruction MachineCFIInstruction::registerRule(DwarfRegisterNumber Register,
                                                          DwarfRegisterNumber SourceRegister) {
  return MachineCFIInstruction(MachineCFIOperation::Register,
                               MachineCFIRegisterRegisterRule{Register, SourceRegister});
}

MachineCFIInstruction MachineCFIInstruction::windowSave() {
  return MachineCFIInstruction(MachineCFIOperation::WindowSave, MachineCFINoOperands{});
}

MachineCFIInstruction MachineCFIInstruction::negateReturnAddressState() {
  return MachineCFIInstruction(MachineCFIOperation::NegateReturnAddressState,
                               MachineCFINoOperands{});
}

MachineCFIInstruction MachineCFIInstruction::negateReturnAddressStateWithPC() {
  return MachineCFIInstruction(MachineCFIOperation::NegateReturnAddressStateWithPC,
                               MachineCFINoOperands{});
}

MachineCFIInstruction MachineCFIInstruction::gnuArgsSize(std::int64_t Size) {
  return MachineCFIInstruction(MachineCFIOperation::GNUArgsSize, MachineCFIOffsetRule{Size});
}

Expected<MachineCFIInstruction> MachineCFIInstruction::label(std::string Symbol) {
  if (Symbol.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "CFI label symbol cannot be empty");
  return MachineCFIInstruction(MachineCFIOperation::Label, MachineCFILabelRule{std::move(Symbol)});
}

MachineCFIInstruction MachineCFIInstruction::valueOffset(DwarfRegisterNumber Register,
                                                         std::int64_t Offset) {
  return MachineCFIInstruction(MachineCFIOperation::ValueOffset,
                               MachineCFIRegisterOffsetRule{Register, Offset});
}

MachineCFIInstruction MachineCFIInstruction::llvmRegisterPair(MachineCFIRegisterPairRule Rule) {
  return MachineCFIInstruction(MachineCFIOperation::LLVMRegisterPair, Rule);
}

Expected<MachineCFIInstruction>
MachineCFIInstruction::llvmVectorRegisters(MachineCFIVectorRegistersRule Rule) {
  auto Verification = verifyVectorRegisters(Rule, ErrorCode::InvalidArgument);
  if (!Verification)
    return std::unexpected(Verification.error());
  return MachineCFIInstruction(MachineCFIOperation::LLVMVectorRegisters, std::move(Rule));
}

MachineCFIInstruction MachineCFIInstruction::llvmVectorOffset(MachineCFIVectorOffsetRule Rule) {
  return MachineCFIInstruction(MachineCFIOperation::LLVMVectorOffset, Rule);
}

MachineCFIInstruction
MachineCFIInstruction::llvmVectorRegisterMask(MachineCFIVectorRegisterMaskRule Rule) {
  return MachineCFIInstruction(MachineCFIOperation::LLVMVectorRegisterMask, Rule);
}

Expected<void> MachineCFIInstruction::verify() const {
  switch (Operation) {
  case MachineCFIOperation::SameValue:
  case MachineCFIOperation::DefCFARegister:
  case MachineCFIOperation::Restore:
  case MachineCFIOperation::Undefined:
    if (getIf<MachineCFIRegisterRule>() == nullptr)
      break;
    return {};
  case MachineCFIOperation::RememberState:
  case MachineCFIOperation::RestoreState:
  case MachineCFIOperation::WindowSave:
  case MachineCFIOperation::NegateReturnAddressState:
  case MachineCFIOperation::NegateReturnAddressStateWithPC:
    if (getIf<MachineCFINoOperands>() == nullptr)
      break;
    return {};
  case MachineCFIOperation::Offset:
  case MachineCFIOperation::DefCFA:
  case MachineCFIOperation::RelativeOffset:
  case MachineCFIOperation::ValueOffset:
    if (getIf<MachineCFIRegisterOffsetRule>() == nullptr)
      break;
    return {};
  case MachineCFIOperation::LLVMDefAddressSpaceCFA:
    if (getIf<MachineCFIAddressSpaceCFARule>() == nullptr)
      break;
    return {};
  case MachineCFIOperation::DefCFAOffset:
  case MachineCFIOperation::AdjustCFAOffset:
  case MachineCFIOperation::GNUArgsSize:
    if (getIf<MachineCFIOffsetRule>() == nullptr)
      break;
    return {};
  case MachineCFIOperation::Escape: {
    const auto *Rule = getIf<MachineCFIEscapeRule>();
    if (Rule == nullptr)
      break;
    if (Rule->Bytes.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "CFI escape byte sequence cannot be empty");
    }
    return {};
  }
  case MachineCFIOperation::Register:
    if (getIf<MachineCFIRegisterRegisterRule>() == nullptr)
      break;
    return {};
  case MachineCFIOperation::Label: {
    const auto *Rule = getIf<MachineCFILabelRule>();
    if (Rule == nullptr)
      break;
    if (Rule->Symbol.empty())
      return makeUnexpected(ErrorCode::VerificationError, "CFI label symbol cannot be empty");
    return {};
  }
  case MachineCFIOperation::LLVMRegisterPair: {
    const auto *Rule = getIf<MachineCFIRegisterPairRule>();
    if (Rule == nullptr)
      break;
    return {};
  }
  case MachineCFIOperation::LLVMVectorRegisters: {
    const auto *Rule = getIf<MachineCFIVectorRegistersRule>();
    if (Rule == nullptr)
      break;
    return verifyVectorRegisters(*Rule, ErrorCode::VerificationError);
  }
  case MachineCFIOperation::LLVMVectorOffset: {
    const auto *Rule = getIf<MachineCFIVectorOffsetRule>();
    if (Rule == nullptr)
      break;
    return {};
  }
  case MachineCFIOperation::LLVMVectorRegisterMask: {
    const auto *Rule = getIf<MachineCFIVectorRegisterMaskRule>();
    if (Rule == nullptr)
      break;
    return {};
  }
  case MachineCFIOperation::Count:
    break;
  }
  return makeUnexpected(ErrorCode::VerificationError,
                        "CFI instruction operation and payload do not match");
}

Expected<CFIInstructionIndex> MachineCFIInfo::addInstruction(MachineCFIInstruction Instruction) {
  auto Verification = Instruction.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  if (Instructions.size() > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::Unsupported, "CFI instruction index space is exhausted");
  }

  const auto Index =
      CFIInstructionIndex::fromValue(static_cast<std::uint32_t>(Instructions.size()));
  Instructions.push_back(std::move(Instruction));
  return Index;
}

const MachineCFIInstruction *MachineCFIInfo::instruction(CFIInstructionIndex Index) const noexcept {
  if (Index.value() >= Instructions.size())
    return nullptr;
  return &Instructions[Index.value()];
}

Expected<void> MachineCFIInfo::verify() const {
  for (const MachineCFIInstruction &Instruction : Instructions) {
    auto Verification = Instruction.verify();
    if (!Verification)
      return std::unexpected(Verification.error());
  }
  return {};
}

Expected<MachineCFIInfo> MachineCFIInfo::clone() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());

  MachineCFIInfo Result;
  for (const MachineCFIInstruction &Instruction : Instructions) {
    auto Index = Result.addInstruction(Instruction);
    if (!Index)
      return std::unexpected(Index.error());
  }
  return Result;
}

} // namespace rcc

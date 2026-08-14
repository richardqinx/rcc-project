// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/CSEMIRBuilder.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/CSEInfo.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace rcc {

bool CSEMIRBuilder::canPerformCSE(Opcode InstructionOpcode) const noexcept {
  const GISelCSEInfo *Info = getCSEInfo();
  return Info != nullptr && Info->shouldCSE(InstructionOpcode);
}

bool CSEMIRBuilder::dominatesInsertionPoint(const MachineInstr &Instruction) const noexcept {
  const MachineBasicBlock *Block = insertionBlock();
  const MachineInstr *InsertBefore = insertionInstruction();
  if (Block == nullptr || Instruction.parent() != Block)
    return false;
  if (InsertBefore == nullptr)
    return true;
  for (const MachineInstr *Current = Block->firstInstruction(); Current != nullptr;
       Current = Current->next()) {
    if (Current == &Instruction)
      return true;
    if (Current == InsertBefore)
      return false;
  }
  return false;
}

Expected<void> CSEMIRBuilder::makeInstructionDominate(MachineInstr &Instruction) const {
  MachineBasicBlock *Block = insertionBlock();
  MachineInstr *InsertBefore = insertionInstruction();
  if (Block == nullptr || Instruction.parent() != Block) {
    return makeUnexpected(ErrorCode::InternalError,
                          "GlobalISel CSE candidate is not in the insertion block");
  }
  if (&Instruction == InsertBefore) {
    return const_cast<CSEMIRBuilder *>(this)->setInsertPointAfter(Instruction);
  }
  if (InsertBefore == nullptr || dominatesInsertionPoint(Instruction))
    return {};
  return Block->spliceInstructionBefore(*InsertBefore, Instruction);
}

Expected<MachineInstr *>
CSEMIRBuilder::generateCopyIfRequired(const MachineIRBuilderResult &Destination,
                                      MachineInstr &Instruction) const {
  if (Destination.kind() == MachineIRBuilderResult::Kind::Type)
    return &Instruction;
  const MachineOperand *Definition = Instruction.operandAt(0);
  const Register *Source = Definition == nullptr ? nullptr : Definition->getIf<Register>();
  if (Source == nullptr || !Definition->isDef()) {
    return makeUnexpected(ErrorCode::InternalError,
                          "GlobalISel CSE candidate has no register definition");
  }
  if (Destination.reg() == *Source)
    return &Instruction;
  return MachineIRBuilder::buildCopy(Destination, *Source);
}

Expected<MachineInstr *> CSEMIRBuilder::memoize(MachineInstr &Instruction) const {
  GISelCSEInfo *Info = getCSEInfo();
  if (Info == nullptr)
    return &Instruction;
  auto Inserted = Info->handleRecordedInstruction(Instruction);
  if (!Inserted)
    return std::unexpected(Inserted.error());
  return &Instruction;
}

Expected<MachineInstr *>
CSEMIRBuilder::buildInstr(Opcode InstructionOpcode,
                          std::span<const MachineIRBuilderResult> Destinations,
                          std::span<const Register> Sources) const {
  if (!canPerformCSE(InstructionOpcode))
    return MachineIRBuilder::buildInstr(InstructionOpcode, Destinations, Sources);
  MachineBasicBlock *Block = insertionBlock();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "machine IR builder has no insertion point");

  GISelCSEInfo *Info = getCSEInfo();
  const bool CanGenerateCopies =
      Destinations.size() == 1 || std::ranges::all_of(Destinations, [](const auto &Destination) {
        return Destination.kind() == MachineIRBuilderResult::Kind::Type;
      });
  if (!CanGenerateCopies) {
    auto Built = MachineIRBuilder::buildInstr(InstructionOpcode, Destinations, Sources);
    if (!Built)
      return std::unexpected(Built.error());
    Info->handleRemoveInstruction(**Built);
    return *Built;
  }

  auto Existing =
      Info->getMachineInstrIfExists(InstructionOpcode, Destinations, Sources,
                                    std::span<const std::int64_t>(), MachineInstrFlags(), *Block);
  if (!Existing)
    return std::unexpected(Existing.error());
  if (*Existing != nullptr) {
    auto Dominating = makeInstructionDominate(**Existing);
    if (!Dominating)
      return std::unexpected(Dominating.error());
    if (Destinations.size() == 1)
      return generateCopyIfRequired(Destinations.front(), **Existing);
    return *Existing;
  }

  auto Built = MachineIRBuilder::buildInstr(InstructionOpcode, Destinations, Sources);
  if (!Built)
    return std::unexpected(Built.error());
  return memoize(**Built);
}

Expected<MachineInstr *> CSEMIRBuilder::buildConstant(MachineIRBuilderResult Destination,
                                                      APInt Value) const {
  const Opcode ConstantOpcode = opcode(GenericOpcode::G_CONSTANT);
  if (!canPerformCSE(ConstantOpcode))
    return MachineIRBuilder::buildConstant(Destination, std::move(Value));
  MachineBasicBlock *Block = insertionBlock();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "machine IR builder has no insertion point");

  GISelCSEInfo *Info = getCSEInfo();
  auto Existing = Info->getConstantIfExists(Destination, Value, *Block);
  if (!Existing)
    return std::unexpected(Existing.error());
  if (*Existing != nullptr) {
    auto Dominating = makeInstructionDominate(**Existing);
    if (!Dominating)
      return std::unexpected(Dominating.error());
    return generateCopyIfRequired(Destination, **Existing);
  }

  auto Built = MachineIRBuilder::buildConstant(Destination, std::move(Value));
  if (!Built)
    return std::unexpected(Built.error());
  return memoize(**Built);
}

Expected<MachineInstr *> CSEMIRBuilder::buildSExtInReg(MachineIRBuilderResult Destination,
                                                       Register Source, std::uint32_t Width) const {
  const Opcode InstructionOpcode = opcode(GenericOpcode::G_SEXT_INREG);
  if (!canPerformCSE(InstructionOpcode))
    return MachineIRBuilder::buildSExtInReg(Destination, Source, Width);
  MachineBasicBlock *Block = insertionBlock();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "machine IR builder has no insertion point");

  GISelCSEInfo *Info = getCSEInfo();
  const std::array Destinations{Destination};
  const std::array Sources{Source};
  const std::array Immediates{static_cast<std::int64_t>(Width)};
  auto Existing = Info->getMachineInstrIfExists(
      InstructionOpcode, std::span<const MachineIRBuilderResult>(Destinations),
      std::span<const Register>(Sources), std::span<const std::int64_t>(Immediates),
      MachineInstrFlags(), *Block);
  if (!Existing)
    return std::unexpected(Existing.error());
  if (*Existing != nullptr) {
    auto Dominating = makeInstructionDominate(**Existing);
    if (!Dominating)
      return std::unexpected(Dominating.error());
    return generateCopyIfRequired(Destination, **Existing);
  }

  auto Built = MachineIRBuilder::buildSExtInReg(Destination, Source, Width);
  if (!Built)
    return std::unexpected(Built.error());
  return memoize(**Built);
}

Expected<MachineInstr *> CSEMIRBuilder::buildExtract(MachineIRBuilderResult Destination,
                                                     Register Source, std::uint64_t Offset) const {
  if (Offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return MachineIRBuilder::buildExtract(Destination, Source, Offset);
  const Opcode InstructionOpcode = opcode(GenericOpcode::G_EXTRACT);
  if (!canPerformCSE(InstructionOpcode))
    return MachineIRBuilder::buildExtract(Destination, Source, Offset);
  MachineBasicBlock *Block = insertionBlock();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "machine IR builder has no insertion point");

  GISelCSEInfo *Info = getCSEInfo();
  const std::array Destinations{Destination};
  const std::array Sources{Source};
  const std::array Immediates{static_cast<std::int64_t>(Offset)};
  auto Existing = Info->getMachineInstrIfExists(
      InstructionOpcode, std::span<const MachineIRBuilderResult>(Destinations),
      std::span<const Register>(Sources), std::span<const std::int64_t>(Immediates),
      MachineInstrFlags(), *Block);
  if (!Existing)
    return std::unexpected(Existing.error());
  if (*Existing != nullptr) {
    auto Dominating = makeInstructionDominate(**Existing);
    if (!Dominating)
      return std::unexpected(Dominating.error());
    return generateCopyIfRequired(Destination, **Existing);
  }

  auto Built = MachineIRBuilder::buildExtract(Destination, Source, Offset);
  if (!Built)
    return std::unexpected(Built.error());
  return memoize(**Built);
}

} // namespace rcc

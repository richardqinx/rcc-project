// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineIRBuilder.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rcc {

MachineInstrBuilder::MachineInstrBuilder(MachineFunction &Function,
                                         const InstructionSet &Instructions,
                                         const InstructionDescriptor &Descriptor,
                                         MachineBasicBlock &InsertBlock,
                                         MachineInstr *InsertBefore) noexcept
    : Function(&Function), Instructions(&Instructions), Descriptor(&Descriptor),
      InsertBlock(&InsertBlock), InsertBefore(InsertBefore) {}

MachineInstrBuilder::MachineInstrBuilder(MachineInstrBuilder &&Other) noexcept
    : Function(Other.Function), Instructions(Other.Instructions), Descriptor(Other.Descriptor),
      InsertBlock(Other.InsertBlock), InsertBefore(Other.InsertBefore),
      ExplicitOperands(std::move(Other.ExplicitOperands)),
      ExtraImplicitOperands(std::move(Other.ExtraImplicitOperands)),
      RequestedTies(std::move(Other.RequestedTies)), MemOperands(std::move(Other.MemOperands)),
      Flags(Other.Flags), Location(std::move(Other.Location)), Origin(Other.Origin),
      IsBuilt(Other.IsBuilt) {
  Other.Function = nullptr;
  Other.Instructions = nullptr;
  Other.Descriptor = nullptr;
  Other.InsertBlock = nullptr;
  Other.InsertBefore = nullptr;
  Other.IsBuilt = true;
}

MachineInstrBuilder &MachineInstrBuilder::operator=(MachineInstrBuilder &&Other) noexcept {
  if (this == &Other)
    return *this;

  Function = Other.Function;
  Instructions = Other.Instructions;
  Descriptor = Other.Descriptor;
  InsertBlock = Other.InsertBlock;
  InsertBefore = Other.InsertBefore;
  ExplicitOperands = std::move(Other.ExplicitOperands);
  ExtraImplicitOperands = std::move(Other.ExtraImplicitOperands);
  RequestedTies = std::move(Other.RequestedTies);
  MemOperands = std::move(Other.MemOperands);
  Flags = Other.Flags;
  Location = std::move(Other.Location);
  Origin = Other.Origin;
  IsBuilt = Other.IsBuilt;

  Other.Function = nullptr;
  Other.Instructions = nullptr;
  Other.Descriptor = nullptr;
  Other.InsertBlock = nullptr;
  Other.InsertBefore = nullptr;
  Other.IsBuilt = true;
  return *this;
}

Expected<void> MachineInstrBuilder::ensurePending() const {
  if (IsBuilt) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine instruction builder was already consumed");
  }
  return {};
}

Expected<void> MachineInstrBuilder::addOperand(MachineOperand &&Operand) {
  auto Pending = ensurePending();
  if (!Pending)
    return std::unexpected(Pending.error());
  if (Operand.parent() != nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder operand already belongs to an instruction");
  }
  if (Operand.isImplicit()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "implicit register requires addImplicitOperand");
  }
  ExplicitOperands.push_back(std::move(Operand));
  return {};
}

Expected<void> MachineInstrBuilder::addImplicitOperand(MachineOperand &&Operand) {
  auto Pending = ensurePending();
  if (!Pending)
    return std::unexpected(Pending.error());
  if (Operand.parent() != nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder operand already belongs to an instruction");
  }
  if (!Operand.isRegister() || !Operand.isImplicit()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "dynamic implicit operand must be an implicit register");
  }
  ExtraImplicitOperands.push_back(std::move(Operand));
  return {};
}

Expected<void> MachineInstrBuilder::appendRegister(Register Reg, RegisterOperandFlags RegisterFlags,
                                                   SubRegIndex SubReg, std::uint32_t TargetFlags,
                                                   bool IsImplicit) {
  auto Operand = MachineOperand::registerOperand(Reg, RegisterFlags, SubReg, TargetFlags);
  if (!Operand)
    return std::unexpected(Operand.error());
  if (IsImplicit)
    return addImplicitOperand(std::move(*Operand));
  return addOperand(std::move(*Operand));
}

Expected<void> MachineInstrBuilder::addDef(Register Reg, RegisterOperandFlags AdditionalFlags,
                                           SubRegIndex SubReg, std::uint32_t TargetFlags) {
  AdditionalFlags.set(RegisterOperandFlag::Define);
  return appendRegister(Reg, AdditionalFlags, SubReg, TargetFlags, false);
}

Expected<void> MachineInstrBuilder::addUse(Register Reg, RegisterOperandFlags AdditionalFlags,
                                           SubRegIndex SubReg, std::uint32_t TargetFlags) {
  AdditionalFlags.set(RegisterOperandFlag::Use);
  return appendRegister(Reg, AdditionalFlags, SubReg, TargetFlags, false);
}

Expected<void> MachineInstrBuilder::addDefUse(Register Reg, RegisterOperandFlags AdditionalFlags,
                                              SubRegIndex SubReg, std::uint32_t TargetFlags) {
  AdditionalFlags.set(RegisterOperandFlag::Define);
  AdditionalFlags.set(RegisterOperandFlag::Use);
  AdditionalFlags.set(RegisterOperandFlag::InternalRead);
  return appendRegister(Reg, AdditionalFlags, SubReg, TargetFlags, false);
}

Expected<void> MachineInstrBuilder::addImplicitDef(Register Reg,
                                                   RegisterOperandFlags AdditionalFlags,
                                                   SubRegIndex SubReg, std::uint32_t TargetFlags) {
  AdditionalFlags.set(RegisterOperandFlag::Define);
  AdditionalFlags.set(RegisterOperandFlag::Implicit);
  return appendRegister(Reg, AdditionalFlags, SubReg, TargetFlags, true);
}

Expected<void> MachineInstrBuilder::addImplicitUse(Register Reg,
                                                   RegisterOperandFlags AdditionalFlags,
                                                   SubRegIndex SubReg, std::uint32_t TargetFlags) {
  AdditionalFlags.set(RegisterOperandFlag::Use);
  AdditionalFlags.set(RegisterOperandFlag::Implicit);
  return appendRegister(Reg, AdditionalFlags, SubReg, TargetFlags, true);
}

Expected<void> MachineInstrBuilder::addImmediate(std::int64_t Value, std::uint32_t TargetFlags) {
  return addOperand(MachineOperand::immediate(Value, TargetFlags));
}

Expected<void> MachineInstrBuilder::addFloatingImmediate(FloatingPointBits Bits,
                                                         std::uint32_t TargetFlags) {
  return addOperand(MachineOperand::floatingImmediate(Bits, TargetFlags));
}

Expected<void> MachineInstrBuilder::addBasicBlock(MachineBasicBlock &Block,
                                                  std::uint32_t TargetFlags) {
  return addOperand(MachineOperand::basicBlock(Block, TargetFlags));
}

Expected<void> MachineInstrBuilder::addFrameIndex(FrameIndex Index, std::int64_t Offset,
                                                  std::uint32_t TargetFlags) {
  return addOperand(MachineOperand::frameIndex(Index, Offset, TargetFlags));
}

Expected<void> MachineInstrBuilder::addConstantPool(ConstantPoolIndex Index, std::int64_t Offset,
                                                    std::uint32_t TargetFlags) {
  return addOperand(MachineOperand::constantPool(Index, Offset, TargetFlags));
}

Expected<void> MachineInstrBuilder::addJumpTable(JumpTableIndex Index, std::uint32_t TargetFlags) {
  return addOperand(MachineOperand::jumpTable(Index, TargetFlags));
}

Expected<void> MachineInstrBuilder::addGlobalSymbol(std::string Name, std::int64_t Offset,
                                                    std::uint32_t TargetFlags) {
  auto Operand = MachineOperand::globalSymbol(std::move(Name), Offset, TargetFlags);
  if (!Operand)
    return std::unexpected(Operand.error());
  return addOperand(std::move(*Operand));
}

Expected<void> MachineInstrBuilder::addExternalSymbol(std::string Name, std::int64_t Offset,
                                                      std::uint32_t TargetFlags) {
  auto Operand = MachineOperand::externalSymbol(std::move(Name), Offset, TargetFlags);
  if (!Operand)
    return std::unexpected(Operand.error());
  return addOperand(std::move(*Operand));
}

Expected<void> MachineInstrBuilder::addBlockAddress(std::string FunctionName, std::string BlockName,
                                                    std::int64_t Offset,
                                                    std::uint32_t TargetFlags) {
  auto Operand = MachineOperand::blockAddress(std::move(FunctionName), std::move(BlockName), Offset,
                                              TargetFlags);
  if (!Operand)
    return std::unexpected(Operand.error());
  return addOperand(std::move(*Operand));
}

Expected<void> MachineInstrBuilder::addRegisterMask(std::shared_ptr<const RegisterMask> Mask,
                                                    std::uint32_t TargetFlags) {
  auto Operand = MachineOperand::registerMask(std::move(Mask), TargetFlags);
  if (!Operand)
    return std::unexpected(Operand.error());
  return addOperand(std::move(*Operand));
}

Expected<void> MachineInstrBuilder::addMetadata(std::string Marker, std::uint32_t TargetFlags) {
  auto Operand = MachineOperand::metadata(std::move(Marker), TargetFlags);
  if (!Operand)
    return std::unexpected(Operand.error());
  return addOperand(std::move(*Operand));
}

Expected<void> MachineInstrBuilder::addTargetIndex(TargetIndex Index, std::int64_t Offset,
                                                   std::uint32_t TargetFlags) {
  return addOperand(MachineOperand::targetIndex(Index, Offset, TargetFlags));
}

Expected<void> MachineInstrBuilder::addMCSymbol(std::string Name, std::int64_t Offset,
                                                std::uint32_t TargetFlags) {
  auto Operand = MachineOperand::mcSymbol(std::move(Name), Offset, TargetFlags);
  if (!Operand)
    return std::unexpected(Operand.error());
  return addOperand(std::move(*Operand));
}

Expected<void> MachineInstrBuilder::tieOperands(std::size_t DefOperand, std::size_t UseOperand) {
  auto Pending = ensurePending();
  if (!Pending)
    return std::unexpected(Pending.error());
  if (DefOperand >= ExplicitOperands.size() || UseOperand >= ExplicitOperands.size() ||
      DefOperand == UseOperand) {
    return makeUnexpected(ErrorCode::InvalidArgument, "builder operand tie indices are invalid");
  }

  const MachineOperand &Def = ExplicitOperands[DefOperand];
  const MachineOperand &Use = ExplicitOperands[UseOperand];
  if (!Def.isRegister() || !Use.isRegister() || !Def.isDef() || Def.isUse() || !Use.isUse() ||
      Use.isDef()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder tie must connect a pure register def and use");
  }

  const MachineOperandTie Tie{DefOperand, UseOperand};
  if (std::ranges::find(RequestedTies, Tie) == RequestedTies.end())
    RequestedTies.push_back(Tie);
  return {};
}

Expected<void> MachineInstrBuilder::addMemOperand(const MachineMemOperand &MemOperand) {
  auto Pending = ensurePending();
  if (!Pending)
    return std::unexpected(Pending.error());
  if (!Function->ownsMemOperand(MemOperand)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder memory operand belongs to another function");
  }
  MemOperands.push_back(&MemOperand);
  return {};
}

Expected<void> MachineInstrBuilder::setFlags(MachineInstrFlags NewFlags) {
  auto Pending = ensurePending();
  if (!Pending)
    return std::unexpected(Pending.error());
  Flags = NewFlags;
  return {};
}

Expected<void> MachineInstrBuilder::setSourceLocation(SourceLocation NewLocation) {
  auto Pending = ensurePending();
  if (!Pending)
    return std::unexpected(Pending.error());
  if (!NewLocation.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "builder source location is incomplete");
  }
  Location = std::move(NewLocation);
  return {};
}

Expected<void> MachineInstrBuilder::clearSourceLocation() {
  auto Pending = ensurePending();
  if (!Pending)
    return std::unexpected(Pending.error());
  Location.reset();
  return {};
}

Expected<void> MachineInstrBuilder::setOrigin(OriginID NewOrigin) {
  auto Pending = ensurePending();
  if (!Pending)
    return std::unexpected(Pending.error());
  if (NewOrigin.value() == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument, "builder origin ID must be nonzero");
  }
  Origin = NewOrigin;
  return {};
}

Expected<void> MachineInstrBuilder::clearOrigin() {
  auto Pending = ensurePending();
  if (!Pending)
    return std::unexpected(Pending.error());
  Origin.reset();
  return {};
}

Expected<void> MachineInstrBuilder::validateInsertionPoint() const {
  if (InsertBlock == nullptr || &InsertBlock->function() != Function || !InsertBlock->isLinked()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder insertion block is not linked to its function");
  }
  if (InsertBefore != nullptr && InsertBefore->parent() != InsertBlock) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder insertion cursor is no longer in its block");
  }
  return {};
}

Expected<void> MachineInstrBuilder::validateFrameReference(FrameIndex Index) const {
  const StackObject *Object = Function->frameInfo().object(Index);
  if (Object == nullptr || Object->description()->IsDead) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder references an unknown or dead stack object");
  }
  return {};
}

Expected<void> MachineInstrBuilder::validateConstantPoolReference(ConstantPoolIndex Index) const {
  if (Function->constantPool().entry(Index) == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder references an unknown constant pool entry");
  }
  return {};
}

Expected<void> MachineInstrBuilder::validateJumpTableReference(JumpTableIndex Index) const {
  if (Function->jumpTableInfo().table(Index) == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument, "builder references an unknown jump table");
  }
  return {};
}

Expected<void> MachineInstrBuilder::validateOperandOwnership(const MachineOperand &Operand) const {
  if (Operand.parent() != nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder operand already belongs to an instruction");
  }
  if (const auto *Block = Operand.getIf<BasicBlockOperand>();
      Block != nullptr && (Block->Block == nullptr || &Block->Block->function() != Function ||
                           !Block->Block->isLinked())) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder basic block operand is not linked to its function");
  }
  if (const Register *Reg = Operand.getIf<Register>();
      Reg != nullptr && Reg->isVirtual() &&
      !Function->registerInfo().isKnownVirtualRegister(*Reg)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder operand references an unknown virtual register");
  }
  if (const auto *Frame = Operand.getIf<FrameIndexOperand>(); Frame != nullptr) {
    auto FrameResult = validateFrameReference(Frame->Index);
    if (!FrameResult)
      return FrameResult;
  }
  if (const auto *Constant = Operand.getIf<ConstantPoolOperand>(); Constant != nullptr) {
    auto ConstantResult = validateConstantPoolReference(Constant->Index);
    if (!ConstantResult)
      return ConstantResult;
  }
  if (const auto *JumpTable = Operand.getIf<JumpTableOperand>(); JumpTable != nullptr) {
    auto JumpTableResult = validateJumpTableReference(JumpTable->Index);
    if (!JumpTableResult)
      return JumpTableResult;
  }
  return {};
}

std::vector<MachineOperandTie> MachineInstrBuilder::collectOperandTies() const {
  std::vector<MachineOperandTie> Result;
  for (std::size_t Index = 0; Index < Descriptor->Operands.size(); ++Index) {
    const OperandDescriptor &Operand = Descriptor->Operands[Index];
    if (!Operand.DefaultTiedOperand || Index > *Operand.DefaultTiedOperand)
      continue;
    const std::size_t Other = *Operand.DefaultTiedOperand;
    const bool IndexIsDef = Operand.RegisterRole == RegisterOperandRole::Def;
    Result.push_back(IndexIsDef ? MachineOperandTie{Index, Other}
                                : MachineOperandTie{Other, Index});
  }
  for (const MachineOperandTie Tie : RequestedTies) {
    if (std::ranges::find(Result, Tie) == Result.end())
      Result.push_back(Tie);
  }
  return Result;
}

Expected<MachineInstr *> MachineInstrBuilder::build() {
  auto Pending = ensurePending();
  if (!Pending)
    return std::unexpected(Pending.error());

  auto InsertionValidation = validateInsertionPoint();
  if (!InsertionValidation)
    return std::unexpected(InsertionValidation.error());

  for (const MachineOperand &Operand : ExplicitOperands) {
    auto Ownership = validateOperandOwnership(Operand);
    if (!Ownership)
      return std::unexpected(Ownership.error());
  }
  for (const MachineOperand &Operand : ExtraImplicitOperands) {
    auto Ownership = validateOperandOwnership(Operand);
    if (!Ownership)
      return std::unexpected(Ownership.error());
  }
  for (const MachineMemOperand *MemOperand : MemOperands) {
    const MemoryBase &Base = MemOperand->description().Base;
    if (const auto Frame = Base.frameIndex()) {
      auto FrameResult = validateFrameReference(*Frame);
      if (!FrameResult)
        return std::unexpected(FrameResult.error());
    }
    if (const auto Constant = Base.constantPoolIndex()) {
      auto ConstantResult = validateConstantPoolReference(*Constant);
      if (!ConstantResult)
        return std::unexpected(ConstantResult.error());
    }
    if (const auto JumpTable = Base.jumpTableIndex()) {
      auto JumpTableResult = validateJumpTableReference(*JumpTable);
      if (!JumpTableResult)
        return std::unexpected(JumpTableResult.error());
    }
  }

  auto ExplicitValidation = Instructions->validateExplicitOperands(
      Descriptor->InstructionOpcode, ExplicitOperands, Function->registerInfo());
  if (!ExplicitValidation)
    return std::unexpected(ExplicitValidation.error());

  auto DefaultImplicit = Instructions->createDefaultImplicitOperands(*Descriptor);
  if (!DefaultImplicit)
    return std::unexpected(DefaultImplicit.error());

  std::vector<const MachineOperand *> ImplicitOperands;
  ImplicitOperands.reserve(DefaultImplicit->size() + ExtraImplicitOperands.size());
  for (const MachineOperand &Operand : *DefaultImplicit)
    ImplicitOperands.push_back(&Operand);
  for (const MachineOperand &Operand : ExtraImplicitOperands)
    ImplicitOperands.push_back(&Operand);
  auto ImplicitValidation =
      Instructions->validateImplicitOperands(Descriptor->InstructionOpcode, ImplicitOperands);
  if (!ImplicitValidation)
    return std::unexpected(ImplicitValidation.error());

  const std::vector<MachineOperandTie> Ties = collectOperandTies();
  auto TieValidation =
      Instructions->validateOperandTies(Descriptor->InstructionOpcode, ExplicitOperands, Ties);
  if (!TieValidation)
    return std::unexpected(TieValidation.error());

  auto MemoryValidation =
      Instructions->validateMemOperands(Descriptor->InstructionOpcode, MemOperands);
  if (!MemoryValidation)
    return std::unexpected(MemoryValidation.error());

  auto Instruction = Function->createInstruction(Descriptor->InstructionOpcode);
  if (!Instruction)
    return std::unexpected(Instruction.error());
  MachineInstr *Result = *Instruction;
  Result->reserveOperands(ExplicitOperands.size() + DefaultImplicit->size() +
                          ExtraImplicitOperands.size());

  for (MachineOperand &Operand : ExplicitOperands) {
    auto AddResult = Result->addOperand(std::move(Operand));
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }
  for (MachineOperand &Operand : *DefaultImplicit) {
    auto AddResult = Result->addOperand(std::move(Operand));
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }
  for (MachineOperand &Operand : ExtraImplicitOperands) {
    auto AddResult = Result->addOperand(std::move(Operand));
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }
  for (const MachineOperandTie Tie : Ties) {
    auto TieResult = Result->tieOperands(Tie.DefOperand, Tie.UseOperand);
    if (!TieResult)
      return std::unexpected(TieResult.error());
  }
  for (const MachineMemOperand *MemOperand : MemOperands) {
    auto AddResult = Result->addMemOperand(*MemOperand);
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }

  Result->setFlags(Flags);
  if (Location)
    Result->setSourceLocation(std::move(*Location));
  if (Origin)
    Result->setOrigin(*Origin);

  Expected<void> InsertResult;
  if (InsertBefore == nullptr)
    InsertResult = InsertBlock->appendInstruction(*Result);
  else
    InsertResult = InsertBlock->insertInstructionBefore(*InsertBefore, *Result);
  if (!InsertResult)
    return std::unexpected(InsertResult.error());

  IsBuilt = true;
  return Result;
}

Expected<void> MachineIRBuilder::validateBlock(const MachineBasicBlock &Block) const {
  if (&Block.function() != Function || !Block.isLinked()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder insertion block is not linked to its function");
  }
  return {};
}

Expected<void> MachineIRBuilder::validateInstruction(const MachineInstr &Instruction) const {
  if (&Instruction.function() != Function || Instruction.parent() == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "builder insertion instruction is not linked to its function");
  }
  return {};
}

Expected<void> MachineIRBuilder::setInsertPoint(MachineBasicBlock &Block) {
  auto Validation = validateBlock(Block);
  if (!Validation)
    return std::unexpected(Validation.error());
  InsertBlock = &Block;
  InsertBefore = nullptr;
  return {};
}

Expected<void> MachineIRBuilder::setInsertPointBefore(MachineInstr &Instruction) {
  auto Validation = validateInstruction(Instruction);
  if (!Validation)
    return std::unexpected(Validation.error());
  InsertBlock = Instruction.parent();
  InsertBefore = &Instruction;
  return {};
}

Expected<void> MachineIRBuilder::setInsertPointAfter(MachineInstr &Instruction) {
  auto Validation = validateInstruction(Instruction);
  if (!Validation)
    return std::unexpected(Validation.error());
  InsertBlock = Instruction.parent();
  InsertBefore = Instruction.next();
  return {};
}

Expected<MachineInstrBuilder> MachineIRBuilder::buildInstr(Opcode InstructionOpcode) const {
  if (InsertBlock == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument, "machine IR builder has no insertion point");
  }
  const InstructionDescriptor *Descriptor = Instructions->descriptor(InstructionOpcode);
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine IR builder opcode is not registered");
  }
  auto BlockValidation = validateBlock(*InsertBlock);
  if (!BlockValidation)
    return std::unexpected(BlockValidation.error());
  if (InsertBefore != nullptr && InsertBefore->parent() != InsertBlock) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine IR builder insertion cursor is stale");
  }
  return MachineInstrBuilder(*Function, *Instructions, *Descriptor, *InsertBlock, InsertBefore);
}

} // namespace rcc

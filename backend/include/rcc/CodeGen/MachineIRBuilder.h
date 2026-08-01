// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEIRBUILDER_H
#define RCC_CODEGEN_MACHINEIRBUILDER_H

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class MachineMemOperand;
class RegisterMask;

class MachineInstrBuilder final {
public:
  MachineInstrBuilder(const MachineInstrBuilder &) = delete;
  MachineInstrBuilder &operator=(const MachineInstrBuilder &) = delete;
  MachineInstrBuilder(MachineInstrBuilder &&Other) noexcept;
  MachineInstrBuilder &operator=(MachineInstrBuilder &&Other) noexcept;
  ~MachineInstrBuilder() = default;

  [[nodiscard]] Expected<void> addOperand(MachineOperand &&Operand);
  [[nodiscard]] Expected<void> addImplicitOperand(MachineOperand &&Operand);

  [[nodiscard]] Expected<void> addDef(Register Reg, RegisterOperandFlags AdditionalFlags = {},
                                      SubRegIndex SubReg = {}, std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addUse(Register Reg, RegisterOperandFlags AdditionalFlags = {},
                                      SubRegIndex SubReg = {}, std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addDefUse(Register Reg, RegisterOperandFlags AdditionalFlags = {},
                                         SubRegIndex SubReg = {}, std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addImplicitDef(Register Reg,
                                              RegisterOperandFlags AdditionalFlags = {},
                                              SubRegIndex SubReg = {},
                                              std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addImplicitUse(Register Reg,
                                              RegisterOperandFlags AdditionalFlags = {},
                                              SubRegIndex SubReg = {},
                                              std::uint32_t TargetFlags = 0);

  [[nodiscard]] Expected<void> addImmediate(std::int64_t Value, std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addFloatingImmediate(FloatingPointBits Bits,
                                                    std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addBasicBlock(MachineBasicBlock &Block,
                                             std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addFrameIndex(FrameIndex Index, std::int64_t Offset = 0,
                                             std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addConstantPool(ConstantPoolIndex Index, std::int64_t Offset = 0,
                                               std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addJumpTable(JumpTableIndex Index, std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addGlobalSymbol(std::string Name, std::int64_t Offset = 0,
                                               std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addExternalSymbol(std::string Name, std::int64_t Offset = 0,
                                                 std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addBlockAddress(std::string FunctionName, std::string BlockName,
                                               std::int64_t Offset = 0,
                                               std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addRegisterMask(std::shared_ptr<const RegisterMask> Mask,
                                               std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addMetadata(std::string Marker, std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addTargetIndex(TargetIndex Index, std::int64_t Offset = 0,
                                              std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addMCSymbol(std::string Name, std::int64_t Offset = 0,
                                           std::uint32_t TargetFlags = 0);

  [[nodiscard]] Expected<void> tieOperands(std::size_t DefOperand, std::size_t UseOperand);
  [[nodiscard]] Expected<void> addMemOperand(const MachineMemOperand &MemOperand);

  [[nodiscard]] Expected<void> setFlags(MachineInstrFlags NewFlags);

  [[nodiscard]] Expected<void> setSourceLocation(SourceLocation NewLocation);
  [[nodiscard]] Expected<void> clearSourceLocation();

  [[nodiscard]] Expected<void> setOrigin(OriginID NewOrigin);
  [[nodiscard]] Expected<void> clearOrigin();

  [[nodiscard]] Expected<MachineInstr *> build();

private:
  MachineInstrBuilder(MachineFunction &Function, const InstructionSet &Instructions,
                      const InstructionDescriptor &Descriptor, MachineBasicBlock &InsertBlock,
                      MachineInstr *InsertBefore) noexcept;

  [[nodiscard]] Expected<void> ensurePending() const;
  [[nodiscard]] Expected<void> validateInsertionPoint() const;
  [[nodiscard]] Expected<void> validateFrameReference(FrameIndex Index) const;
  [[nodiscard]] Expected<void> validateOperandOwnership(const MachineOperand &Operand) const;
  [[nodiscard]] std::vector<MachineOperandTie> collectOperandTies() const;
  [[nodiscard]] Expected<void> appendRegister(Register Reg, RegisterOperandFlags Flags,
                                              SubRegIndex SubReg, std::uint32_t TargetFlags,
                                              bool IsImplicit);

  MachineFunction *Function;
  const InstructionSet *Instructions;
  const InstructionDescriptor *Descriptor;
  MachineBasicBlock *InsertBlock;
  MachineInstr *InsertBefore;
  std::vector<MachineOperand> ExplicitOperands;
  std::vector<MachineOperand> ExtraImplicitOperands;
  std::vector<MachineOperandTie> RequestedTies;
  std::vector<const MachineMemOperand *> MemOperands;
  MachineInstrFlags Flags;
  std::optional<SourceLocation> Location;
  std::optional<OriginID> Origin;
  bool IsBuilt = false;

  friend class MachineIRBuilder;
};

class MachineIRBuilder final {
public:
  MachineIRBuilder(MachineFunction &Function, const InstructionSet &Instructions) noexcept
      : Function(&Function), Instructions(&Instructions) {}
  MachineIRBuilder(const MachineIRBuilder &) = delete;
  MachineIRBuilder &operator=(const MachineIRBuilder &) = delete;
  MachineIRBuilder(MachineIRBuilder &&) = delete;
  MachineIRBuilder &operator=(MachineIRBuilder &&) = delete;

  [[nodiscard]] MachineFunction &function() noexcept {
    return *Function;
  }

  [[nodiscard]] const MachineFunction &function() const noexcept {
    return *Function;
  }

  [[nodiscard]] Expected<void> setInsertPoint(MachineBasicBlock &Block);
  [[nodiscard]] Expected<void> setInsertPointBefore(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> setInsertPointAfter(MachineInstr &Instruction);
  void clearInsertPoint() noexcept {
    InsertBlock = nullptr;
    InsertBefore = nullptr;
  }

  [[nodiscard]] Expected<MachineInstrBuilder> buildInstr(Opcode InstructionOpcode) const;

private:
  [[nodiscard]] Expected<void> validateBlock(const MachineBasicBlock &Block) const;
  [[nodiscard]] Expected<void> validateInstruction(const MachineInstr &Instruction) const;

  MachineFunction *Function;
  const InstructionSet *Instructions;
  MachineBasicBlock *InsertBlock = nullptr;
  MachineInstr *InsertBefore = nullptr;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEIRBUILDER_H

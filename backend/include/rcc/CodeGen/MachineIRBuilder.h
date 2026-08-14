// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEIRBUILDER_H
#define RCC_CODEGEN_MACHINEIRBUILDER_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class MachineMemOperand;
class MachineRegisterInfo;
class RegisterMask;
class GISelCSEInfo;

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
  [[nodiscard]] Expected<void> addIntegerImmediate(APInt Value, std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addFloatingImmediate(APFloat Bits, std::uint32_t TargetFlags = 0);
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
  [[nodiscard]] Expected<void> addLiveOutRegisterMask(std::shared_ptr<const RegisterMask> Mask,
                                                      std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addMetadata(MachineMetadataValue Value,
                                           std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addTargetIndex(TargetIndex Index, std::int64_t Offset = 0,
                                              std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addMCSymbol(std::string Name, std::int64_t Offset = 0,
                                           std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addCFIInstruction(CFIInstructionIndex Index,
                                                 std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addIntrinsic(IntrinsicID ID, std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addIntegerPredicate(IntegerPredicate Predicate,
                                                   std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addFloatingPredicate(FloatingPredicate Predicate,
                                                    std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addShuffleMask(std::vector<std::int32_t> Elements,
                                              std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addDebugInstrRef(std::uint64_t InstructionNumber,
                                                std::uint64_t OperandNumber,
                                                std::uint32_t TargetFlags = 0);
  [[nodiscard]] Expected<void> addLaneMask(LaneMask Mask, std::uint32_t TargetFlags = 0);

  [[nodiscard]] Expected<void> tieOperands(std::size_t DefOperand, std::size_t UseOperand);
  [[nodiscard]] Expected<void> addMemOperand(const MachineMemOperand &MemOperand);

  [[nodiscard]] Expected<void> setFlags(MachineInstrFlags NewFlags);

  [[nodiscard]] Expected<void> setSourceLocation(SourceLocation NewLocation);
  [[nodiscard]] Expected<void> clearSourceLocation();
  [[nodiscard]] Expected<void> setDebugLocation(MachineDebugLocation NewLocation);
  [[nodiscard]] Expected<void> clearDebugLocation();

  [[nodiscard]] Expected<void> setOrigin(OriginID NewOrigin);
  [[nodiscard]] Expected<void> clearOrigin();

  [[nodiscard]] Expected<MachineInstr *> build();

private:
  MachineInstrBuilder(MachineFunction &Function, const TargetDescription &Target,
                      const InstructionDescriptor &Descriptor, MachineBasicBlock &InsertBlock,
                      MachineInstr *InsertBefore) noexcept;

  [[nodiscard]] Expected<void> ensurePending() const;
  [[nodiscard]] Expected<void> validateInsertionPoint() const;
  [[nodiscard]] Expected<void> validateFrameReference(FrameIndex Index) const;
  [[nodiscard]] Expected<void> validateConstantPoolReference(ConstantPoolIndex Index) const;
  [[nodiscard]] Expected<void> validateJumpTableReference(JumpTableIndex Index) const;
  [[nodiscard]] Expected<void> validateCFIReference(CFIInstructionIndex Index) const;
  [[nodiscard]] Expected<void> validateOperandOwnership(const MachineOperand &Operand) const;
  [[nodiscard]] std::vector<MachineOperandTie> collectOperandTies() const;
  [[nodiscard]] Expected<void> appendRegister(Register Reg, RegisterOperandFlags Flags,
                                              SubRegIndex SubReg, std::uint32_t TargetFlags,
                                              bool IsImplicit);

  MachineFunction *Function;
  const TargetDescription *Target;
  const InstructionDescriptor *Descriptor;
  MachineBasicBlock *InsertBlock;
  MachineInstr *InsertBefore;
  std::vector<MachineOperand> ExplicitOperands;
  std::vector<MachineOperand> ExtraImplicitOperands;
  std::vector<MachineOperandTie> RequestedTies;
  std::vector<const MachineMemOperand *> MemOperands;
  MachineInstrFlags Flags;
  std::optional<SourceLocation> Location;
  std::optional<MachineDebugLocation> DebugLocation;
  std::optional<OriginID> Origin;
  bool IsBuilt = false;

  friend class MachineIRBuilder;
};

class MachineIRBuilderResult final {
public:
  enum class Kind : std::uint8_t {
    Register,
    Type,
  };

  MachineIRBuilderResult(Register Reg) noexcept : Reg(Reg), ResultKind(Kind::Register) {}
  MachineIRBuilderResult(LowLevelType Type) noexcept : Type(Type), ResultKind(Kind::Type) {}

  [[nodiscard]] Kind kind() const noexcept {
    return ResultKind;
  }

  [[nodiscard]] Register reg() const noexcept;
  [[nodiscard]] LowLevelType type() const noexcept;
  [[nodiscard]] Expected<LowLevelType> type(const MachineRegisterInfo &MRI) const;
  [[nodiscard]] Expected<void> addDef(MachineRegisterInfo &MRI, MachineInstrBuilder &Builder) const;

private:
  Register Reg;
  LowLevelType Type;
  Kind ResultKind;
};

class MachineIRBuilder {
public:
  MachineIRBuilder(MachineFunction &Function, const TargetDescription &Target) noexcept
      : Function(&Function), Target(&Target) {}
  MachineIRBuilder(const MachineIRBuilder &) = delete;
  MachineIRBuilder &operator=(const MachineIRBuilder &) = delete;
  MachineIRBuilder(MachineIRBuilder &&) = delete;
  MachineIRBuilder &operator=(MachineIRBuilder &&) = delete;
  virtual ~MachineIRBuilder() = default;

  [[nodiscard]] MachineFunction &function() noexcept {
    return *Function;
  }

  [[nodiscard]] MachineFunction &function() const noexcept {
    return *Function;
  }

  [[nodiscard]] const TargetDescription &target() const noexcept {
    return *Target;
  }

  [[nodiscard]] MachineBasicBlock *insertionBlock() const noexcept {
    return InsertBlock;
  }

  [[nodiscard]] MachineInstr *insertionInstruction() const noexcept {
    return InsertBefore;
  }

  [[nodiscard]] GISelCSEInfo *getCSEInfo() noexcept {
    return CSEInfo;
  }

  [[nodiscard]] GISelCSEInfo *getCSEInfo() const noexcept {
    return CSEInfo;
  }

  void setCSEInfo(GISelCSEInfo *Info) noexcept {
    CSEInfo = Info;
  }

  [[nodiscard]] Expected<void> verifyInsertPoint() const;

  [[nodiscard]] Expected<void> setInsertPoint(MachineBasicBlock &Block);
  [[nodiscard]] Expected<void> setInsertPointBefore(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> setInsertPointAfter(MachineInstr &Instruction);
  void clearInsertPoint() noexcept {
    InsertBlock = nullptr;
    InsertBefore = nullptr;
  }

  [[nodiscard]] Expected<MachineInstrBuilder> buildInstr(Opcode InstructionOpcode) const;
  [[nodiscard]] Expected<MachineInstr *>
  buildInstr(Opcode InstructionOpcode, std::initializer_list<MachineIRBuilderResult> Destinations,
             std::initializer_list<Register> Sources) const;
  [[nodiscard]] virtual Expected<MachineInstr *>
  buildInstr(Opcode InstructionOpcode, std::span<const MachineIRBuilderResult> Destinations,
             std::span<const Register> Sources) const;
  [[nodiscard]] Expected<MachineInstr *> buildTrunc(MachineIRBuilderResult Destination,
                                                    Register Source) const;
  [[nodiscard]] Expected<MachineInstr *> buildCopy(MachineIRBuilderResult Destination,
                                                   Register Source) const;
  [[nodiscard]] Expected<MachineInstr *> buildImplicitDef(MachineIRBuilderResult Destination) const;
  [[nodiscard]] Expected<MachineInstr *> buildAnyExt(MachineIRBuilderResult Destination,
                                                     Register Source) const;
  [[nodiscard]] Expected<MachineInstr *> buildZExt(MachineIRBuilderResult Destination,
                                                   Register Source) const;
  [[nodiscard]] Expected<MachineInstr *> buildSExt(MachineIRBuilderResult Destination,
                                                   Register Source) const;
  [[nodiscard]] Expected<MachineInstr *> buildUndef(MachineIRBuilderResult Destination) const;
  [[nodiscard]] Expected<MachineInstr *> buildAnyExtOrTrunc(MachineIRBuilderResult Destination,
                                                            Register Source) const;
  [[nodiscard]] Expected<MachineInstr *> buildSExtOrTrunc(MachineIRBuilderResult Destination,
                                                          Register Source) const;
  [[nodiscard]] virtual Expected<MachineInstr *> buildConstant(MachineIRBuilderResult Destination,
                                                               APInt Value) const;
  [[nodiscard]] Expected<MachineInstr *> buildConstant(MachineIRBuilderResult Destination,
                                                       std::int64_t Value) const;
  [[nodiscard]] virtual Expected<MachineInstr *>
  buildSExtInReg(MachineIRBuilderResult Destination, Register Source, std::uint32_t Width) const;
  [[nodiscard]] Expected<MachineInstr *> buildAnd(MachineIRBuilderResult Destination, Register Left,
                                                  Register Right) const;
  [[nodiscard]] Expected<MachineInstr *> buildOr(MachineIRBuilderResult Destination, Register Left,
                                                 Register Right) const;
  [[nodiscard]] Expected<MachineInstr *> buildXor(MachineIRBuilderResult Destination, Register Left,
                                                  Register Right) const;
  [[nodiscard]] Expected<MachineInstr *> buildAdd(MachineIRBuilderResult Destination, Register Left,
                                                  Register Right) const;
  [[nodiscard]] Expected<MachineInstr *> buildSub(MachineIRBuilderResult Destination, Register Left,
                                                  Register Right) const;
  [[nodiscard]] Expected<MachineInstr *> buildShl(MachineIRBuilderResult Destination, Register Left,
                                                  Register Right) const;
  [[nodiscard]] Expected<MachineInstr *> buildLShr(MachineIRBuilderResult Destination,
                                                   Register Left, Register Right) const;
  [[nodiscard]] Expected<MachineInstr *> buildAShr(MachineIRBuilderResult Destination,
                                                   Register Left, Register Right) const;
  [[nodiscard]] Expected<MachineInstr *> buildPtrToInt(MachineIRBuilderResult Destination,
                                                       Register Source) const;
  [[nodiscard]] Expected<MachineInstr *> buildIntToPtr(MachineIRBuilderResult Destination,
                                                       Register Source) const;
  [[nodiscard]] Expected<MachineInstr *> buildICmp(IntegerPredicate Predicate,
                                                   MachineIRBuilderResult Destination,
                                                   Register Left, Register Right) const;
  [[nodiscard]] Expected<MachineInstr *>
  buildMergeLikeInstr(MachineIRBuilderResult Destination,
                      const std::vector<Register> &Sources) const;
  [[nodiscard]] Expected<MachineInstr *>
  buildMergeValues(MachineIRBuilderResult Destination, const std::vector<Register> &Sources) const;
  [[nodiscard]] Expected<MachineInstr *>
  buildUnmergeValues(const std::vector<Register> &Destinations, Register Source) const;
  [[nodiscard]] virtual Expected<MachineInstr *>
  buildExtract(MachineIRBuilderResult Destination, Register Source, std::uint64_t Offset) const;
  [[nodiscard]] Expected<MachineInstr *> buildInsert(MachineIRBuilderResult Destination,
                                                     Register Source, Register Inserted,
                                                     std::uint64_t Offset) const;

private:
  [[nodiscard]] Expected<MachineInstr *>
  buildUnary(Opcode InstructionOpcode, MachineIRBuilderResult Destination, Register Source) const;
  [[nodiscard]] Expected<MachineInstr *> buildBinary(Opcode InstructionOpcode,
                                                     MachineIRBuilderResult Destination,
                                                     Register Left, Register Right) const;
  [[nodiscard]] Expected<MachineInstr *> buildExtOrTrunc(Opcode ExtensionOpcode,
                                                         MachineIRBuilderResult Destination,
                                                         Register Source) const;
  [[nodiscard]] Expected<LowLevelType> registerType(Register Reg) const;
  [[nodiscard]] Expected<void> validateBlock(const MachineBasicBlock &Block) const;
  [[nodiscard]] Expected<void> validateInstruction(const MachineInstr &Instruction) const;

  MachineFunction *Function;
  const TargetDescription *Target;
  MachineBasicBlock *InsertBlock = nullptr;
  MachineInstr *InsertBefore = nullptr;
  GISelCSEInfo *CSEInfo = nullptr;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEIRBUILDER_H

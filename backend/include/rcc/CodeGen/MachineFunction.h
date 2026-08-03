// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEFUNCTION_H
#define RCC_CODEGEN_MACHINEFUNCTION_H

#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineCompilationStatus.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunctionAttributes.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/CodeGen/MachineFunctionRecords.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMetadata.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/CodeGen/TargetFunctionState.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

class MachineArena;
class MachineBasicBlock;
class MachineCFGUpdater;
class MachineInstr;
class MachineMemOperand;
struct MachineMemOperandDesc;
class Opcode;

class MachineFunction final {
public:
  [[nodiscard]] static Expected<std::unique_ptr<MachineFunction>>
  create(std::string Name, MachineFunctionStableID StableID);

  MachineFunction(const MachineFunction &) = delete;
  MachineFunction &operator=(const MachineFunction &) = delete;
  MachineFunction(MachineFunction &&) = delete;
  MachineFunction &operator=(MachineFunction &&) = delete;
  ~MachineFunction();

  [[nodiscard]] std::string_view name() const noexcept {
    return Name;
  }

  [[nodiscard]] MachineFunctionStableID stableID() const noexcept {
    return StableID;
  }

  [[nodiscard]] MachineRegisterInfo &registerInfo() noexcept {
    return RegInfo;
  }

  [[nodiscard]] const MachineRegisterInfo &registerInfo() const noexcept {
    return RegInfo;
  }

  [[nodiscard]] MachineFrameInfo &frameInfo() noexcept {
    return FrameInfo;
  }

  [[nodiscard]] const MachineFrameInfo &frameInfo() const noexcept {
    return FrameInfo;
  }

  [[nodiscard]] MachineConstantPool &constantPool() noexcept {
    return ConstantPool;
  }

  [[nodiscard]] const MachineConstantPool &constantPool() const noexcept {
    return ConstantPool;
  }

  [[nodiscard]] MachineJumpTableInfo &jumpTableInfo() noexcept {
    return JumpTables;
  }

  [[nodiscard]] const MachineJumpTableInfo &jumpTableInfo() const noexcept {
    return JumpTables;
  }

  [[nodiscard]] MachineFunctionProperties &properties() noexcept {
    return Properties;
  }

  [[nodiscard]] const MachineFunctionProperties &properties() const noexcept {
    return Properties;
  }

  [[nodiscard]] MachineFunctionAttributes &attributes() noexcept {
    return Attributes;
  }

  [[nodiscard]] const MachineFunctionAttributes &attributes() const noexcept {
    return Attributes;
  }

  [[nodiscard]] MachineFunctionRecords &records() noexcept {
    return Records;
  }

  [[nodiscard]] const MachineFunctionRecords &records() const noexcept {
    return Records;
  }

  [[nodiscard]] MachineMetadataInfo &metadata() noexcept {
    return Metadata;
  }

  [[nodiscard]] const MachineMetadataInfo &metadata() const noexcept {
    return Metadata;
  }

  [[nodiscard]] MachineCFIInfo &cfiInfo() noexcept {
    return CFIInfo;
  }

  [[nodiscard]] const MachineCFIInfo &cfiInfo() const noexcept {
    return CFIInfo;
  }

  [[nodiscard]] const TargetFunctionState &targetState() const noexcept {
    return TargetState;
  }

  [[nodiscard]] Expected<void> setTargetState(TargetFunctionState State);

  template <TargetFunctionStateValue ValueT> [[nodiscard]] ValueT *getTargetInfo() noexcept {
    return TargetState.getIf<ValueT>();
  }

  template <TargetFunctionStateValue ValueT>
  [[nodiscard]] const ValueT *getTargetInfo() const noexcept {
    return TargetState.getIf<ValueT>();
  }

  [[nodiscard]] MachineCompilationStatus &compilationStatus() noexcept {
    return CompilationStatus;
  }

  [[nodiscard]] const MachineCompilationStatus &compilationStatus() const noexcept {
    return CompilationStatus;
  }

  [[nodiscard]] Expected<MachineBasicBlock *> createBasicBlock();
  [[nodiscard]] Expected<MachineInstr *> createInstruction(Opcode InstructionOpcode);
  [[nodiscard]] Expected<const MachineMemOperand *>
  createMemOperand(MachineMemOperandDesc Description);

  [[nodiscard]] Expected<void> appendBlock(MachineBasicBlock &Block);
  [[nodiscard]] Expected<void> insertBlockBefore(MachineBasicBlock &Position,
                                                 MachineBasicBlock &Block);
  [[nodiscard]] Expected<void> removeBlock(MachineBasicBlock &Block);

  [[nodiscard]] MachineBasicBlock *firstBlock() noexcept {
    return FirstBlock;
  }

  [[nodiscard]] const MachineBasicBlock *firstBlock() const noexcept {
    return FirstBlock;
  }

  [[nodiscard]] MachineBasicBlock *lastBlock() noexcept {
    return LastBlock;
  }

  [[nodiscard]] const MachineBasicBlock *lastBlock() const noexcept {
    return LastBlock;
  }

  [[nodiscard]] std::size_t blockCount() const noexcept;
  [[nodiscard]] std::span<MachineBasicBlock *const> allBlocks() const noexcept {
    return AllBlocks;
  }

  [[nodiscard]] std::span<MachineInstr *const> allInstructions() const noexcept {
    return AllInstructions;
  }

  [[nodiscard]] std::span<const MachineMemOperand *const> allMemOperands() const noexcept {
    return AllMemOperands;
  }

  [[nodiscard]] bool ownsMemOperand(const MachineMemOperand &MemOperand) const noexcept;

  [[nodiscard]] MachineCFGUpdater cfgUpdater() noexcept;
  void renumberBlocks() noexcept;

private:
  MachineFunction(std::string Name, MachineFunctionStableID StableID);
  [[nodiscard]] Expected<std::uint32_t> allocateDebugInstrNumber();
  void observeDebugInstrNumber(std::uint32_t Number) noexcept;

  std::string Name;
  MachineFunctionStableID StableID;
  MachineRegisterInfo RegInfo;
  MachineFrameInfo FrameInfo;
  MachineConstantPool ConstantPool;
  MachineJumpTableInfo JumpTables;
  MachineFunctionProperties Properties;
  MachineFunctionAttributes Attributes;
  MachineFunctionRecords Records;
  MachineMetadataInfo Metadata;
  MachineCFIInfo CFIInfo;
  TargetFunctionState TargetState;
  MachineCompilationStatus CompilationStatus;
  MachineBasicBlock *FirstBlock = nullptr;
  MachineBasicBlock *LastBlock = nullptr;
  std::vector<MachineBasicBlock *> AllBlocks;
  std::vector<MachineInstr *> AllInstructions;
  std::vector<const MachineMemOperand *> AllMemOperands;
  StableIDGenerator<MachineBlockStableID> BlockIDGenerator;
  StableIDGenerator<MachineInstrStableID> InstrIDGenerator;
  std::uint32_t DebugInstrNumberingCount = 0;
  std::unique_ptr<MachineArena> Arena;

  friend class MachineBasicBlock;
  friend class MachineCFGUpdater;
  friend class MachineInstr;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFUNCTION_H

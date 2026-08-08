// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEBASICBLOCK_H
#define RCC_CODEGEN_MACHINEBASICBLOCK_H

#include "rcc/CodeGen/MachineBasicBlockAttributes.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineCFGUpdater;
class MachineFunction;
class MachineInstr;

struct MachineSuccessor final {
  MachineBasicBlock *Block = nullptr;
  BranchProbability Probability;

  friend bool operator==(const MachineSuccessor &Left, const MachineSuccessor &Right) = default;
};

struct MachineBasicBlockLiveIn final {
  PhysicalRegister PhysReg;
  LaneMask Lanes = LaneMask::all();

  friend bool operator==(const MachineBasicBlockLiveIn &Left,
                         const MachineBasicBlockLiveIn &Right) = default;
};

class MachineBasicBlock final {
public:
  MachineBasicBlock(const MachineBasicBlock &) = delete;
  MachineBasicBlock &operator=(const MachineBasicBlock &) = delete;
  MachineBasicBlock(MachineBasicBlock &&) = delete;
  MachineBasicBlock &operator=(MachineBasicBlock &&) = delete;
  ~MachineBasicBlock() = default;

  [[nodiscard]] MachineBlockStableID stableID() const noexcept {
    return StableID;
  }

  [[nodiscard]] MachineFunction &function() noexcept {
    return *OwnerFunction;
  }

  [[nodiscard]] const MachineFunction &function() const noexcept {
    return *OwnerFunction;
  }

  [[nodiscard]] bool isLinked() const noexcept {
    return IsLinked;
  }

  [[nodiscard]] MachineBasicBlock *previous() noexcept {
    return PreviousBlock;
  }

  [[nodiscard]] const MachineBasicBlock *previous() const noexcept {
    return PreviousBlock;
  }

  [[nodiscard]] MachineBasicBlock *next() noexcept {
    return NextBlock;
  }

  [[nodiscard]] const MachineBasicBlock *next() const noexcept {
    return NextBlock;
  }

  [[nodiscard]] MachineInstr *firstInstruction() noexcept {
    return FirstInstruction;
  }

  [[nodiscard]] const MachineInstr *firstInstruction() const noexcept {
    return FirstInstruction;
  }

  [[nodiscard]] MachineInstr *lastInstruction() noexcept {
    return LastInstruction;
  }

  [[nodiscard]] const MachineInstr *lastInstruction() const noexcept {
    return LastInstruction;
  }

  [[nodiscard]] std::size_t instructionCount() const noexcept;
  [[nodiscard]] Expected<void> appendInstruction(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> insertInstructionBefore(MachineInstr &Position,
                                                       MachineInstr &Instruction);
  [[nodiscard]] Expected<void> insertInstructionAfter(MachineInstr &Position,
                                                      MachineInstr &Instruction);
  [[nodiscard]] Expected<void> removeInstruction(MachineInstr &Instruction);
  void clearInstructions() noexcept;

  [[nodiscard]] std::span<MachineBasicBlock *const> predecessors() const noexcept {
    return Predecessors;
  }

  [[nodiscard]] std::span<const MachineSuccessor> successors() const noexcept {
    return Successors;
  }

  [[nodiscard]] Expected<void> addLiveIn(PhysicalRegister PhysReg,
                                         LaneMask Lanes = LaneMask::all());
  [[nodiscard]] Expected<void> removeLiveIn(PhysicalRegister PhysReg);
  void clearLiveIns() noexcept {
    LiveIns.clear();
  }
  [[nodiscard]] std::span<const MachineBasicBlockLiveIn> liveIns() const noexcept {
    return LiveIns;
  }

  [[nodiscard]] std::optional<std::uint32_t> denseNumber() const noexcept {
    return DenseNumber;
  }

  [[nodiscard]] std::optional<std::uint32_t> layoutIndex() const noexcept {
    return LayoutIndex;
  }

  [[nodiscard]] MachineBasicBlockAttributes &attributes() noexcept {
    return Attributes;
  }

  [[nodiscard]] const MachineBasicBlockAttributes &attributes() const noexcept {
    return Attributes;
  }

private:
  MachineBasicBlock(MachineFunction &OwnerFunction, MachineBlockStableID StableID)
      : OwnerFunction(&OwnerFunction), StableID(StableID) {}

  MachineFunction *OwnerFunction;
  MachineBlockStableID StableID;
  bool IsLinked = false;
  MachineBasicBlock *PreviousBlock = nullptr;
  MachineBasicBlock *NextBlock = nullptr;
  MachineInstr *FirstInstruction = nullptr;
  MachineInstr *LastInstruction = nullptr;
  std::vector<MachineBasicBlock *> Predecessors;
  std::vector<MachineSuccessor> Successors;
  std::vector<MachineBasicBlockLiveIn> LiveIns;
  MachineBasicBlockAttributes Attributes;
  std::optional<std::uint32_t> DenseNumber;
  std::optional<std::uint32_t> LayoutIndex;

  friend class MachineArena;
  friend class MachineCFGUpdater;
  friend class MachineFunction;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEBASICBLOCK_H

// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEINSTR_H
#define RCC_CODEGEN_MACHINEINSTR_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineMemOperand;
class MachineRegisterInfo;

class MachineInstr {
public:
  MachineInstr(const MachineInstr &) = delete;
  MachineInstr &operator=(const MachineInstr &) = delete;
  MachineInstr(MachineInstr &&) = delete;
  MachineInstr &operator=(MachineInstr &&) = delete;
  ~MachineInstr();

  [[nodiscard]] Opcode opcode() const noexcept {
    return InstructionOpcode;
  }

  [[nodiscard]] Expected<void> setOpcode(Opcode NewOpcode);

  [[nodiscard]] MachineInstrStableID stableID() const noexcept {
    return StableID;
  }

  [[nodiscard]] MachineFunction &function() noexcept {
    return *OwnerFunction;
  }

  [[nodiscard]] const MachineFunction &function() const noexcept {
    return *OwnerFunction;
  }

  [[nodiscard]] MachineBasicBlock *parent() noexcept {
    return ParentBlock;
  }

  [[nodiscard]] const MachineBasicBlock *parent() const noexcept {
    return ParentBlock;
  }

  [[nodiscard]] MachineInstr *previous() noexcept {
    return PreviousInstruction;
  }

  [[nodiscard]] const MachineInstr *previous() const noexcept {
    return PreviousInstruction;
  }

  [[nodiscard]] MachineInstr *next() noexcept {
    return NextInstruction;
  }

  [[nodiscard]] const MachineInstr *next() const noexcept {
    return NextInstruction;
  }

  [[nodiscard]] bool isLinked() const noexcept {
    return ParentBlock != nullptr;
  }

  [[nodiscard]] Expected<void> eraseFromParent();

  [[nodiscard]] bool isDebugInstr() const noexcept;

  [[nodiscard]] std::size_t operandCount() const noexcept {
    return Operands.size();
  }

  [[nodiscard]] MachineOperand *operandAt(std::size_t Index) noexcept;
  [[nodiscard]] const MachineOperand *operandAt(std::size_t Index) const noexcept;
  [[nodiscard]] std::span<const MachineOperand> operands() const noexcept {
    return Operands;
  }

  void reserveOperands(std::size_t Capacity);
  [[nodiscard]] Expected<void> addOperand(MachineOperand &&Operand);
  [[nodiscard]] Expected<void> insertOperand(std::size_t Index, MachineOperand &&Operand);
  [[nodiscard]] Expected<void> eraseOperand(std::size_t Index);

  [[nodiscard]] Expected<void> tieOperands(std::size_t DefIndex, std::size_t UseIndex);
  [[nodiscard]] Expected<void> untieOperand(std::size_t Index);

  [[nodiscard]] Expected<void> addMemOperand(const MachineMemOperand &MemOperand);
  [[nodiscard]] Expected<void> replaceMemOperand(std::size_t Index,
                                                 const MachineMemOperand &MemOperand);
  [[nodiscard]] Expected<void> eraseMemOperand(std::size_t Index);
  [[nodiscard]] std::span<const MachineMemOperand *const> memOperands() const noexcept {
    return MemOperands;
  }

  [[nodiscard]] MachineInstrFlags flags() const noexcept {
    return Flags;
  }

  void setFlags(MachineInstrFlags NewFlags) noexcept {
    Flags = NewFlags;
  }

  [[nodiscard]] const std::optional<SourceLocation> &sourceLocation() const noexcept {
    return Location;
  }

  void setSourceLocation(SourceLocation NewLocation) {
    Location = std::move(NewLocation);
  }

  void clearSourceLocation() noexcept {
    Location.reset();
  }

  [[nodiscard]] const std::optional<MachineDebugLocation> &debugLocation() const noexcept {
    return DebugLocation;
  }

  [[nodiscard]] Expected<void> setDebugLocation(MachineDebugLocation NewLocation);

  void clearDebugLocation() noexcept {
    DebugLocation.reset();
  }

  [[nodiscard]] std::optional<OriginID> origin() const noexcept {
    return Origin;
  }

  void setOrigin(OriginID NewOrigin) noexcept {
    Origin = NewOrigin;
  }

  void clearOrigin() noexcept {
    Origin.reset();
  }

  [[nodiscard]] std::uint32_t debugInstrNumber() const noexcept {
    return DebugInstrNumber;
  }

  [[nodiscard]] Expected<void> setDebugInstrNumber(std::uint64_t Number);
  [[nodiscard]] Expected<std::uint32_t> assignDebugInstrNumber();

  void clearDebugInstrNumber() noexcept {
    DebugInstrNumber = 0;
  }

  [[nodiscard]] std::optional<std::string_view> preInstrSymbol() const noexcept;
  [[nodiscard]] Expected<void> setPreInstrSymbol(std::string Symbol);
  void clearPreInstrSymbol() noexcept {
    PreInstrSymbol.reset();
  }

  [[nodiscard]] std::optional<std::string_view> postInstrSymbol() const noexcept;
  [[nodiscard]] Expected<void> setPostInstrSymbol(std::string Symbol);
  void clearPostInstrSymbol() noexcept {
    PostInstrSymbol.reset();
  }

  [[nodiscard]] const std::optional<MachineMetadataReference> &heapAllocMarker() const noexcept {
    return HeapAllocMarker;
  }
  [[nodiscard]] Expected<void> setHeapAllocMarker(MachineMetadataReference Marker);
  void clearHeapAllocMarker() noexcept {
    HeapAllocMarker.reset();
  }

  [[nodiscard]] const std::optional<MachineMetadataReference> &pcSections() const noexcept {
    return PCSections;
  }
  [[nodiscard]] Expected<void> setPCSections(MachineMetadataReference Sections);
  void clearPCSections() noexcept {
    PCSections.reset();
  }

  [[nodiscard]] const std::optional<MachineMetadataReference> &mmra() const noexcept {
    return MMRA;
  }
  [[nodiscard]] Expected<void> setMMRA(MachineMetadataReference Metadata);
  void clearMMRA() noexcept {
    MMRA.reset();
  }

  [[nodiscard]] std::optional<std::uint32_t> cfiType() const noexcept {
    return CFIType;
  }
  [[nodiscard]] Expected<void> setCFIType(std::uint64_t Type);
  void clearCFIType() noexcept {
    CFIType.reset();
  }

  [[nodiscard]] std::optional<std::string_view> deactivationSymbol() const noexcept;
  [[nodiscard]] Expected<void> setDeactivationSymbol(std::string Symbol);
  void clearDeactivationSymbol() noexcept {
    DeactivationSymbol.reset();
  }

private:
  MachineInstr(MachineFunction &OwnerFunction, Opcode InstructionOpcode,
               MachineInstrStableID StableID);

  [[nodiscard]] MachineRegisterInfo *linkedRegisterInfo() noexcept;
  [[nodiscard]] Expected<void> attachToBlock(MachineBasicBlock &Block);
  void detachFromBlock() noexcept;
  void adjustTiesForInsertion(std::size_t Index) noexcept;
  void adjustTiesForErasure(std::size_t Index) noexcept;

  MachineFunction *OwnerFunction;
  Opcode InstructionOpcode;
  MachineInstrStableID StableID;
  MachineBasicBlock *ParentBlock = nullptr;
  MachineInstr *PreviousInstruction = nullptr;
  MachineInstr *NextInstruction = nullptr;
  std::vector<MachineOperand> Operands;
  std::vector<const MachineMemOperand *> MemOperands;
  MachineInstrFlags Flags;
  std::optional<SourceLocation> Location;
  std::optional<MachineDebugLocation> DebugLocation;
  std::optional<OriginID> Origin;
  std::uint32_t DebugInstrNumber = 0;
  std::optional<std::string> PreInstrSymbol;
  std::optional<std::string> PostInstrSymbol;
  std::optional<MachineMetadataReference> HeapAllocMarker;
  std::optional<MachineMetadataReference> PCSections;
  std::optional<MachineMetadataReference> MMRA;
  std::optional<std::uint32_t> CFIType;
  std::optional<std::string> DeactivationSymbol;

  friend class MachineArena;
  friend class MachineBasicBlock;
  friend class MachineFunction;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEINSTR_H

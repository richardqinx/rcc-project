// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEREGISTERINFO_H
#define RCC_CODEGEN_MACHINEREGISTERINFO_H

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineOperand;

class RegOperandIterator final {
public:
  using difference_type = std::ptrdiff_t; // NOLINT(readability-identifier-naming)
  using value_type = MachineOperand;      // NOLINT(readability-identifier-naming)
  using pointer = MachineOperand *;       // NOLINT(readability-identifier-naming)
  using reference = MachineOperand &;     // NOLINT(readability-identifier-naming)
  using iterator_category =               // NOLINT(readability-identifier-naming)
      std::forward_iterator_tag;

  RegOperandIterator() = default;

  [[nodiscard]] reference operator*() const noexcept;
  [[nodiscard]] pointer operator->() const noexcept;
  RegOperandIterator &operator++() noexcept;
  RegOperandIterator operator++(int) noexcept;

  friend bool operator==(const RegOperandIterator &Left, const RegOperandIterator &Right) noexcept {
    return Left.Current == Right.Current;
  }

private:
  RegOperandIterator(MachineOperand *Current, bool RequireUse, bool RequireDef,
                     bool SkipDebug) noexcept;

  void skipFiltered() noexcept;

  MachineOperand *Current = nullptr;
  bool RequireUse = false;
  bool RequireDef = false;
  bool SkipDebug = false;

  friend class RegOperandRange;
  friend class MachineRegisterInfo;
};

class RegOperandRange final {
public:
  [[nodiscard]] RegOperandIterator begin() const noexcept {
    return RegOperandIterator(Head, RequireUse, RequireDef, SkipDebug);
  }

  [[nodiscard]] RegOperandIterator end() const noexcept {
    return {};
  }

private:
  RegOperandRange(MachineOperand *Head, bool RequireUse, bool RequireDef, bool SkipDebug) noexcept
      : Head(Head), RequireUse(RequireUse), RequireDef(RequireDef), SkipDebug(SkipDebug) {}

  MachineOperand *Head = nullptr;
  bool RequireUse = false;
  bool RequireDef = false;
  bool SkipDebug = false;

  friend class MachineRegisterInfo;
};

struct VirtualRegisterInfo final {
  std::optional<LLT> Type;
  std::optional<RegisterBankID> Bank;
  std::optional<RegisterClassID> RegClass;
  std::vector<PhysicalRegister> AllocationHints;
  std::uint8_t TargetFlags = 0;
  bool IsTombstone = false;
};

struct PhysicalRegisterLiveIn final {
  PhysicalRegister PhysReg;
  std::optional<Register> InitialVirtualRegister;

  friend bool operator==(const PhysicalRegisterLiveIn &Left,
                         const PhysicalRegisterLiveIn &Right) = default;
};

class MachineRegisterInfo final {
public:
  MachineRegisterInfo() = default;
  MachineRegisterInfo(const MachineRegisterInfo &) = delete;
  MachineRegisterInfo &operator=(const MachineRegisterInfo &) = delete;
  MachineRegisterInfo(MachineRegisterInfo &&) = delete;
  MachineRegisterInfo &operator=(MachineRegisterInfo &&) = delete;
  ~MachineRegisterInfo();

  [[nodiscard]] Expected<Register> createVirtualRegister();
  [[nodiscard]] Expected<Register> createVirtualRegister(LLT Type);
  [[nodiscard]] Expected<void> eraseVirtualRegister(Register Reg);

  [[nodiscard]] bool isKnownVirtualRegister(Register Reg) const noexcept;
  [[nodiscard]] std::span<const VirtualRegisterInfo> virtualRegisters() const noexcept {
    return VirtualRegisters;
  }
  [[nodiscard]] Expected<std::reference_wrapper<VirtualRegisterInfo>>
  virtualRegisterInfo(Register Reg);
  [[nodiscard]] Expected<std::reference_wrapper<const VirtualRegisterInfo>>
  virtualRegisterInfo(Register Reg) const;

  [[nodiscard]] Expected<void> setType(Register Reg, LLT Type);
  [[nodiscard]] Expected<void> clearType(Register Reg);
  [[nodiscard]] Expected<void> setRegisterBank(Register Reg, RegisterBankID Bank);
  [[nodiscard]] Expected<void> clearRegisterBank(Register Reg);
  [[nodiscard]] Expected<void> setRegisterClass(Register Reg, RegisterClassID RegClass);
  [[nodiscard]] Expected<void> clearRegisterClass(Register Reg);
  [[nodiscard]] Expected<void> addAllocationHint(Register Reg, PhysicalRegister Hint);
  [[nodiscard]] Expected<void> setTargetRegisterFlags(Register Reg, std::uint8_t Flags);
  [[nodiscard]] Expected<void> clearTargetRegisterFlags(Register Reg);
  void clearAllVirtualRegisterTypes() noexcept;

  [[nodiscard]] Expected<void>
  addLiveIn(PhysicalRegister PhysReg,
            std::optional<Register> InitialVirtualRegister = std::nullopt);
  [[nodiscard]] std::span<const PhysicalRegisterLiveIn> liveIns() const noexcept {
    return LiveIns;
  }

  [[nodiscard]] Expected<void> reservePhysicalRegister(PhysicalRegister PhysReg);
  [[nodiscard]] bool isReserved(PhysicalRegister PhysReg) const noexcept;
  [[nodiscard]] std::span<const PhysicalRegister> reservedRegisters() const noexcept {
    return ReservedRegisters;
  }

  [[nodiscard]] RegOperandRange regOperands(Register Reg) noexcept;
  [[nodiscard]] RegOperandRange regNoDebugOperands(Register Reg) noexcept;
  [[nodiscard]] RegOperandRange useOperands(Register Reg, bool IncludeDebug = true) noexcept;
  [[nodiscard]] RegOperandRange defOperands(Register Reg) noexcept;

  [[nodiscard]] bool useEmpty(Register Reg, bool IncludeDebug = true) const noexcept;
  [[nodiscard]] bool hasOneUse(Register Reg, bool IncludeDebug = true) const noexcept;
  [[nodiscard]] const MachineOperand *getUniqueVRegDef(Register Reg) const noexcept;
  [[nodiscard]] Expected<std::size_t> replaceRegWith(Register OldReg, Register NewReg);

  [[nodiscard]] Expected<void> verifyDefUseChains() const;
  [[nodiscard]] bool isLinkedOperand(const MachineOperand &Operand) const noexcept;

private:
  struct RegOperandListHead final {
    MachineOperand *Head = nullptr;
  };

  [[nodiscard]] MachineOperand *headFor(Register Reg) const noexcept;
  void linkOperand(MachineOperand &Operand);

  std::vector<VirtualRegisterInfo> VirtualRegisters;
  std::vector<PhysicalRegisterLiveIn> LiveIns;
  std::vector<PhysicalRegister> ReservedRegisters;
  std::unordered_map<Register, std::unique_ptr<RegOperandListHead>> DefUseHeads;

  friend class MachineOperand;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEREGISTERINFO_H

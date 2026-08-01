// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEFUNCTIONRECORDS_H
#define RCC_CODEGEN_MACHINEFUNCTIONRECORDS_H

#include "rcc/CodeGen/MachineBasicBlockAttributes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rcc {

class MachineFunctionCloneContext;

struct MachineCallSiteArgument final {
  PhysicalRegister Reg;
  std::uint16_t ArgumentNumber = 0;

  friend constexpr bool operator==(MachineCallSiteArgument Left,
                                   MachineCallSiteArgument Right) noexcept = default;
};

struct MachineCallSiteInfo final {
  MachineInstrStableID CallInstruction;
  std::vector<MachineCallSiteArgument> ForwardedArguments;
  std::vector<std::uint64_t> CalleeTypeIDs;

  friend bool operator==(const MachineCallSiteInfo &Left,
                         const MachineCallSiteInfo &Right) = default;
};

struct MachineCalledGlobal final {
  MachineInstrStableID CallInstruction;
  std::string Callee;
  std::uint32_t Flags = 0;

  friend bool operator==(const MachineCalledGlobal &Left,
                         const MachineCalledGlobal &Right) = default;
};

struct MachinePrefetchTarget final {
  MachineBasicBlockProfileID Block;
  std::uint32_t CallsiteIndex = 0;

  friend constexpr bool operator==(MachinePrefetchTarget Left,
                                   MachinePrefetchTarget Right) noexcept = default;
};

class MachineFunctionRecords final {
public:
  MachineFunctionRecords() = default;
  MachineFunctionRecords(const MachineFunctionRecords &) = delete;
  MachineFunctionRecords &operator=(const MachineFunctionRecords &) = delete;
  MachineFunctionRecords(MachineFunctionRecords &&) noexcept = default;
  MachineFunctionRecords &operator=(MachineFunctionRecords &&) noexcept = default;
  ~MachineFunctionRecords() = default;

  [[nodiscard]] bool hasCalleeSavedRegisters() const noexcept {
    return CalleeSavedRegisters.has_value();
  }

  [[nodiscard]] std::span<const PhysicalRegister> calleeSavedRegisters() const noexcept {
    if (!CalleeSavedRegisters)
      return {};
    return *CalleeSavedRegisters;
  }

  [[nodiscard]] Expected<void> setCalleeSavedRegisters(std::vector<PhysicalRegister> Registers);

  void clearCalleeSavedRegisters() noexcept {
    CalleeSavedRegisters.reset();
  }

  [[nodiscard]] std::span<const MachineCallSiteInfo> callSites() const noexcept {
    return CallSites;
  }

  [[nodiscard]] Expected<void> addCallSite(MachineCallSiteInfo CallSite);

  [[nodiscard]] std::span<const MachineCalledGlobal> calledGlobals() const noexcept {
    return CalledGlobals;
  }

  [[nodiscard]] Expected<void> addCalledGlobal(MachineCalledGlobal CalledGlobal);

  [[nodiscard]] std::span<const MachinePrefetchTarget> prefetchTargets() const noexcept {
    return PrefetchTargets;
  }

  [[nodiscard]] Expected<void> addPrefetchTarget(MachinePrefetchTarget Target);

  [[nodiscard]] Expected<void> verify() const;
  [[nodiscard]] Expected<MachineFunctionRecords>
  clone(const MachineFunctionCloneContext &CloneContext) const;

  friend bool operator==(const MachineFunctionRecords &Left,
                         const MachineFunctionRecords &Right) = default;

private:
  std::optional<std::vector<PhysicalRegister>> CalleeSavedRegisters;
  std::vector<MachineCallSiteInfo> CallSites;
  std::vector<MachineCalledGlobal> CalledGlobals;
  std::vector<MachinePrefetchTarget> PrefetchTargets;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFUNCTIONRECORDS_H

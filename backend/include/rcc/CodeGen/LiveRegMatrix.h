// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_LIVEREGMATRIX_H
#define RCC_CODEGEN_LIVEREGMATRIX_H

#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc {

class MachineFunction;
class TargetDescription;

enum class RegisterInterferenceKind : std::uint8_t {
  Free,
  VirtualRegister,
  FixedRegister,
  RegisterMask,
  RegisterUnit,
};

struct RegisterInterference final {
  RegisterInterferenceKind Kind = RegisterInterferenceKind::Free;
  std::optional<Register> VirtualReg;
  std::optional<PhysicalRegister> FixedReg;
  std::optional<RegisterUnitID> Unit;
  std::optional<MachineInstrStableID> Instruction;
};

class LiveRegMatrix final {
public:
  LiveRegMatrix(const MachineFunction &Function, const TargetDescription &Target,
                const LiveIntervalsAnalysis::Result &Intervals) noexcept
      : Function(&Function), Target(&Target), Intervals(&Intervals) {}

  [[nodiscard]] Expected<void> assign(Register VirtualReg, PhysicalRegister PhysicalReg);
  [[nodiscard]] Expected<void> unassign(Register VirtualReg);
  [[nodiscard]] std::optional<PhysicalRegister> assignment(Register VirtualReg) const noexcept;
  [[nodiscard]] RegisterInterference checkInterference(Register VirtualReg,
                                                       PhysicalRegister PhysicalReg) const noexcept;
  [[nodiscard]] std::vector<RegisterInterference>
  collectInterferences(Register VirtualReg, PhysicalRegister PhysicalReg) const;

  [[nodiscard]] std::span<const std::pair<Register, PhysicalRegister>>
  assignments() const noexcept {
    return OrderedAssignments;
  }

private:
  [[nodiscard]] bool intervalsOverlapOnCandidates(const LiveInterval &Left,
                                                  PhysicalRegister LeftReg,
                                                  const LiveInterval &Right,
                                                  PhysicalRegister RightReg) const noexcept;
  [[nodiscard]] bool intervalOverlapsUnit(const LiveInterval &Interval, RegisterUnitLane Unit,
                                          const LiveRange &UnitRange) const noexcept;

  const MachineFunction *Function;
  const TargetDescription *Target;
  const LiveIntervalsAnalysis::Result *Intervals;
  std::unordered_map<Register, PhysicalRegister> Assignments;
  std::vector<std::pair<Register, PhysicalRegister>> OrderedAssignments;
};

} // namespace rcc

#endif // RCC_CODEGEN_LIVEREGMATRIX_H

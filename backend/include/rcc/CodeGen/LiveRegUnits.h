// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_LIVEREGUNITS_H
#define RCC_CODEGEN_LIVEREGUNITS_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineFunction;
class MachineInstr;
class MachineOperand;
class RegisterMask;
class TargetDescription;
struct PhysicalRegisterDesc;

class LiveRegUnits final {
public:
  [[nodiscard]] LaneMask lanes(RegisterUnitID Unit) const noexcept;
  [[nodiscard]] bool contains(RegisterUnitID Unit, LaneMask Lanes = LaneMask::all()) const noexcept;
  [[nodiscard]] bool intersects(RegisterUnitID Unit,
                                LaneMask Lanes = LaneMask::all()) const noexcept;
  [[nodiscard]] bool empty() const noexcept {
    return Values.empty();
  }

  void add(RegisterUnitID Unit, LaneMask Lanes);
  void remove(RegisterUnitID Unit, LaneMask Lanes);
  void unite(const LiveRegUnits &Other);
  void subtract(const LiveRegUnits &Other);

  [[nodiscard]] Expected<bool> available(PhysicalRegister Reg,
                                         const TargetDescription &Target) const;
  [[nodiscard]] static Expected<void> accumulateUsedDefed(const MachineInstr &Instruction,
                                                          LiveRegUnits &Modified,
                                                          LiveRegUnits &Used,
                                                          const TargetDescription &Target);

  [[nodiscard]] const std::unordered_map<RegisterUnitID, LaneMask> &entries() const noexcept {
    return Values;
  }

  friend bool operator==(const LiveRegUnits &, const LiveRegUnits &) = default;

private:
  std::unordered_map<RegisterUnitID, LaneMask> Values;
};

struct PhysicalUnitAccess final {
  RegisterUnitID Unit;
  LaneMask Lanes;
};

[[nodiscard]] LaneMask normalizeUnitLanes(LaneMask UnitViewLanes,
                                          LaneMask RegisterLanes = LaneMask::all()) noexcept;
[[nodiscard]] LaneMask denormalizeUnitLanes(LaneMask UnitLanes, LaneMask UnitViewLanes) noexcept;
[[nodiscard]] LaneMask physicalRegisterLanes(const PhysicalRegisterDesc &Description) noexcept;
[[nodiscard]] LiveRegUnits physicalRegisterCoverage(const PhysicalRegisterDesc &Description,
                                                    LaneMask Requested = LaneMask::all());
[[nodiscard]] bool isSubset(const LiveRegUnits &Subset, const LiveRegUnits &Superset) noexcept;
[[nodiscard]] std::size_t laneCount(const LiveRegUnits &Set) noexcept;

[[nodiscard]] Expected<LaneMask> virtualRegisterLanes(Register Reg, const MachineFunction &Function,
                                                      const TargetDescription &Target);
[[nodiscard]] Expected<LaneMask> virtualOperandLanes(const MachineOperand &Operand,
                                                     const MachineFunction &Function,
                                                     const TargetDescription &Target);
[[nodiscard]] Expected<std::vector<PhysicalUnitAccess>>
physicalOperandUnits(const MachineOperand &Operand, const TargetDescription &Target);

void addPhysicalRegister(LiveRegUnits &Set, const PhysicalRegisterDesc &Description,
                         LaneMask Requested = LaneMask::all());
[[nodiscard]] bool isReservedPhysicalRegister(Register Reg, const MachineFunction &Function,
                                              const TargetDescription &Target) noexcept;
[[nodiscard]] Expected<void> addRegisterMaskDefs(LiveRegUnits &Set, const RegisterMask &Mask,
                                                 const TargetDescription &Target);
[[nodiscard]] Expected<void> addRegisterMaskUses(LiveRegUnits &Set, const RegisterMask &Mask,
                                                 const TargetDescription &Target);

} // namespace rcc

#endif // RCC_CODEGEN_LIVEREGUNITS_H

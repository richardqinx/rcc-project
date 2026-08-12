// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveRegUnits.h"

#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace rcc {

namespace {
[[nodiscard]] LaneMask subtractLanes(LaneMask Left, LaneMask Right) noexcept {
  return LaneMask::fromRaw(Left.rawValue() & ~Right.rawValue());
}

template <typename KeyT>
void addLanes(std::unordered_map<KeyT, LaneMask> &Values, KeyT Key, LaneMask Lanes) {
  if (Lanes.empty())
    return;
  const auto Position = Values.find(Key);
  if (Position == Values.end()) {
    Values.emplace(Key, Lanes);
    return;
  }
  Position->second = Position->second | Lanes;
}

template <typename KeyT>
void removeLanes(std::unordered_map<KeyT, LaneMask> &Values, KeyT Key, LaneMask Lanes) {
  const auto Position = Values.find(Key);
  if (Position == Values.end() || Lanes.empty())
    return;
  Position->second = subtractLanes(Position->second, Lanes);
  if (Position->second.empty())
    Values.erase(Position);
}

template <typename KeyT>
[[nodiscard]] LaneMask lanesFor(const std::unordered_map<KeyT, LaneMask> &Values,
                                KeyT Key) noexcept {
  const auto Position = Values.find(Key);
  return Position == Values.end() ? LaneMask{} : Position->second;
}

template <typename KeyT>
[[nodiscard]] bool containsLanes(const std::unordered_map<KeyT, LaneMask> &Values, KeyT Key,
                                 LaneMask Lanes) noexcept {
  const LaneMask Present = lanesFor(Values, Key);
  return !Present.empty() && Present.contains(Lanes);
}

template <typename KeyT>
[[nodiscard]] bool intersectsLanes(const std::unordered_map<KeyT, LaneMask> &Values, KeyT Key,
                                   LaneMask Lanes) noexcept {
  return lanesFor(Values, Key).intersects(Lanes);
}

template <typename KeyT>
void uniteLanes(std::unordered_map<KeyT, LaneMask> &Values,
                const std::unordered_map<KeyT, LaneMask> &Other) {
  for (const auto &[Key, Lanes] : Other)
    addLanes(Values, Key, Lanes);
}

template <typename KeyT>
void subtractLaneSets(std::unordered_map<KeyT, LaneMask> &Values,
                      const std::unordered_map<KeyT, LaneMask> &Other) {
  for (const auto &[Key, Lanes] : Other)
    removeLanes(Values, Key, Lanes);
}

} // namespace

LaneMask normalizeUnitLanes(LaneMask UnitViewLanes, LaneMask RegisterLanes) noexcept {
  std::uint64_t View = UnitViewLanes.rawValue();
  const std::uint64_t Selected = View & RegisterLanes.rawValue();
  std::uint64_t Normalized = 0;
  std::uint32_t UnitLane = 0;
  while (View != 0) {
    const std::uint64_t RegisterLane = std::uint64_t{1} << std::countr_zero(View);
    if ((Selected & RegisterLane) != 0)
      Normalized |= std::uint64_t{1} << UnitLane;
    View &= View - 1U;
    ++UnitLane;
  }
  return LaneMask::fromRaw(Normalized);
}

LaneMask denormalizeUnitLanes(LaneMask UnitLanes, LaneMask UnitViewLanes) noexcept {
  std::uint64_t View = UnitViewLanes.rawValue();
  const std::uint64_t Selected = UnitLanes.rawValue();
  std::uint64_t RegisterLanes = 0;
  std::uint32_t UnitLane = 0;
  while (View != 0) {
    const std::uint64_t RegisterLane = std::uint64_t{1} << std::countr_zero(View);
    if ((Selected & (std::uint64_t{1} << UnitLane)) != 0)
      RegisterLanes |= RegisterLane;
    View &= View - 1U;
    ++UnitLane;
  }
  return LaneMask::fromRaw(RegisterLanes);
}

LaneMask physicalRegisterLanes(const PhysicalRegisterDesc &Description) noexcept {
  LaneMask Result;
  for (const RegisterUnitLane &Unit : Description.Units)
    Result = Result | Unit.Lanes;
  return Result;
}

LiveRegUnits physicalRegisterCoverage(const PhysicalRegisterDesc &Description, LaneMask Requested) {
  LiveRegUnits Result;
  for (const RegisterUnitLane &Unit : Description.Units)
    Result.add(Unit.Unit, normalizeUnitLanes(Unit.Lanes, Requested));
  return Result;
}

bool isSubset(const LiveRegUnits &Subset, const LiveRegUnits &Superset) noexcept {
  return std::ranges::all_of(Subset.entries(), [&Superset](const auto &Entry) {
    return Superset.contains(Entry.first, Entry.second);
  });
}

std::size_t laneCount(const LiveRegUnits &Set) noexcept {
  std::size_t Result = 0;
  for (const auto &[Unit, Lanes] : Set.entries()) {
    (void)Unit;
    Result += static_cast<std::size_t>(std::popcount(Lanes.rawValue()));
  }
  return Result;
}

[[nodiscard]] Expected<LaneMask> virtualRegisterLanes(Register Reg, const MachineFunction &Function,
                                                      const TargetDescription &Target) {
  if (!Reg.isVirtual()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "virtual lane query requires a virtual register");
  }
  auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  const std::optional<RegisterClassID> ClassID = Info->get().RegClass;
  if (!ClassID)
    return LaneMask::all();
  const RegisterClassDesc *Class = Target.registers().registerClass(*ClassID);
  if (Class == nullptr || Class->Lanes.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "liveness encountered an unknown or empty register class");
  }
  return Class->Lanes;
}

[[nodiscard]] Expected<LaneMask> virtualOperandLanes(const MachineOperand &Operand,
                                                     const MachineFunction &Function,
                                                     const TargetDescription &Target) {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || !Reg->isVirtual()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "virtual lane query requires a virtual-register operand");
  }
  if (Operand.subReg().value() != 0) {
    const SubRegIndexDesc *SubReg = Target.registers().subRegIndex(Operand.subReg());
    if (SubReg == nullptr || SubReg->Lanes.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "liveness encountered an unknown or empty subregister lane mask");
    }
    return SubReg->Lanes;
  }

  return virtualRegisterLanes(*Reg, Function, Target);
}

[[nodiscard]] Expected<std::vector<PhysicalUnitAccess>>
physicalOperandUnits(const MachineOperand &Operand, const TargetDescription &Target) {
  const Register *Reg = Operand.getIf<Register>();
  const std::optional<PhysicalRegister> Original =
      Reg == nullptr ? std::nullopt : Reg->physicalRegister();
  if (!Original) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "physical unit query requires a physical-register operand");
  }

  PhysicalRegister Effective = *Original;
  if (Operand.subReg().value() != 0) {
    const std::optional<PhysicalRegister> SubRegister =
        Target.registers().subRegister(*Original, Operand.subReg());
    if (!SubRegister) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical register has no requested subregister projection");
    }
    Effective = *SubRegister;
  }
  const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(Effective);
  if (Description == nullptr || Description->Units.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "liveness encountered an unknown or unitless physical register");
  }

  std::vector<PhysicalUnitAccess> Result;
  Result.reserve(Description->Units.size());
  for (const RegisterUnitLane &Unit : Description->Units) {
    if (Unit.Lanes.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical register unit has an empty lane mask");
    }
    Result.push_back({Unit.Unit, normalizeUnitLanes(Unit.Lanes, LaneMask::all())});
  }
  return Result;
}

void addPhysicalRegister(LiveRegUnits &Set, const PhysicalRegisterDesc &Description,
                         LaneMask Requested) {
  Set.unite(physicalRegisterCoverage(Description, Requested));
}

[[nodiscard]] bool isReservedPhysicalRegister(Register Reg, const MachineFunction &Function,
                                              const TargetDescription &Target) noexcept {
  const std::optional<PhysicalRegister> Physical = Reg.physicalRegister();
  return Physical && (Function.registerInfo().isReserved(*Physical) ||
                      Target.registers().isReservedRegister(*Physical));
}

[[nodiscard]] Expected<void> addRegisterMaskDefs(LiveRegUnits &Set, const RegisterMask &Mask,
                                                 const TargetDescription &Target) {
  for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
    if (Mask.clobbers(Description.Reg))
      addPhysicalRegister(Set, Description);
  }
  return {};
}

[[nodiscard]] Expected<void> addRegisterMaskUses(LiveRegUnits &Set, const RegisterMask &Mask,
                                                 const TargetDescription &Target) {
  for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
    if (Mask.preserves(Description.Reg))
      addPhysicalRegister(Set, Description);
  }
  return {};
}
LaneMask LiveRegUnits::lanes(RegisterUnitID Unit) const noexcept {
  return lanesFor(Values, Unit);
}

bool LiveRegUnits::contains(RegisterUnitID Unit, LaneMask Lanes) const noexcept {
  return containsLanes(Values, Unit, Lanes);
}

bool LiveRegUnits::intersects(RegisterUnitID Unit, LaneMask Lanes) const noexcept {
  return intersectsLanes(Values, Unit, Lanes);
}

void LiveRegUnits::add(RegisterUnitID Unit, LaneMask Lanes) {
  if (Unit.value() != 0 && !Lanes.empty())
    addLanes(Values, Unit, Lanes);
}

void LiveRegUnits::remove(RegisterUnitID Unit, LaneMask Lanes) {
  removeLanes(Values, Unit, Lanes);
}

void LiveRegUnits::unite(const LiveRegUnits &Other) {
  uniteLanes(Values, Other.Values);
}

void LiveRegUnits::subtract(const LiveRegUnits &Other) {
  subtractLaneSets(Values, Other.Values);
}

} // namespace rcc

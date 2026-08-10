// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/RegisterPressure.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/CodeGen/TargetSchedulingModel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

using EntityID = std::size_t;

[[nodiscard]] std::uint64_t saturatingAdd(std::uint64_t Left, std::uint64_t Right) noexcept {
  return std::numeric_limits<std::uint64_t>::max() - Left < Right
             ? std::numeric_limits<std::uint64_t>::max()
             : Left + Right;
}

template <typename ValueT> void sortAndUnique(std::vector<ValueT> &Values) {
  std::ranges::sort(Values);
  const auto [FirstDuplicate, End] = std::ranges::unique(Values);
  Values.erase(FirstDuplicate, End);
}

} // namespace

namespace detail {

struct RegisterPressureBuilder final {
  RegisterPressureTracker Result;
  const ScheduleDAG *DAG = nullptr;
  const MachineFunction *Function = nullptr;
  const TargetDescription *Target = nullptr;
  const TargetSchedulingModel *Model = nullptr;
  ScheduleDAGMode Mode = ScheduleDAGMode::PreRegisterAllocation;
  std::unordered_map<Register, EntityID> CurrentVirtualEntities;
  std::unordered_map<RegisterUnitID, EntityID> CurrentPhysicalEntities;
  std::unordered_map<RegisterUnitID, std::vector<RegisterPressureSetID>> PhysicalSets;

  [[nodiscard]] Expected<void> initializePressureSets() {
    Result.Pressure.reserve(Model->pressureSets().size());
    for (const RegisterPressureSetDesc &Set : Model->pressureSets()) {
      Result.PressureIndexes.emplace(Set.ID, Result.Pressure.size());
      Result.Pressure.push_back({Set.ID, 0, 0, Set.Limit});
      for (const RegisterClassID ClassID : Set.RegisterClasses) {
        const RegisterClassDesc *Class = Target->registers().registerClass(ClassID);
        if (Class == nullptr) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "pressure set references an unknown target register class");
        }
        for (const PhysicalRegister Physical : Class->Registers) {
          const PhysicalRegisterDesc *Description = Target->registers().physicalRegister(Physical);
          if (Description == nullptr) {
            return makeUnexpected(ErrorCode::VerificationError,
                                  "register class references an unknown physical register");
          }
          for (const RegisterUnitLane &Unit : Description->Units)
            PhysicalSets[Unit.Unit].push_back(Set.ID);
        }
      }
    }
    for (auto &[Unit, Sets] : PhysicalSets) {
      (void)Unit;
      sortAndUnique(Sets);
    }
    return {};
  }

  [[nodiscard]] EntityID createEntity(std::vector<RegisterPressureSetID> Sets) {
    sortAndUnique(Sets);
    const EntityID ID = Result.Entities.size();
    Result.Entities.push_back({std::move(Sets), 0, false, false, false, false});
    return ID;
  }

  [[nodiscard]] Expected<std::vector<RegisterPressureSetID>>
  virtualPressureSets(Register Reg) const {
    if (Mode == ScheduleDAGMode::PostRegisterAllocation) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "post-RA pressure tracking encountered a virtual register");
    }
    auto Info = Function->registerInfo().virtualRegisterInfo(Reg);
    if (!Info)
      return std::unexpected(Info.error());
    const auto &RegClass = Info->get().RegClass;
    if (!RegClass) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "pre-RA pressure tracking requires constrained virtual registers");
    }
    const std::span<const RegisterPressureSetID> Sets = Model->pressureSetsForClass(*RegClass);
    return std::vector<RegisterPressureSetID>{Sets.begin(), Sets.end()};
  }

  [[nodiscard]] Expected<EntityID> boundaryVirtualEntity(Register Reg) {
    if (const auto Position = CurrentVirtualEntities.find(Reg);
        Position != CurrentVirtualEntities.end()) {
      return Position->second;
    }
    auto Sets = virtualPressureSets(Reg);
    if (!Sets)
      return std::unexpected(Sets.error());
    const EntityID ID = createEntity(std::move(*Sets));
    CurrentVirtualEntities.emplace(Reg, ID);
    return ID;
  }

  [[nodiscard]] Expected<EntityID> defineVirtualEntity(Register Reg) {
    auto Sets = virtualPressureSets(Reg);
    if (!Sets)
      return std::unexpected(Sets.error());
    const EntityID ID = createEntity(std::move(*Sets));
    CurrentVirtualEntities.insert_or_assign(Reg, ID);
    return ID;
  }

  [[nodiscard]] EntityID boundaryPhysicalEntity(RegisterUnitID Unit) {
    if (const auto Position = CurrentPhysicalEntities.find(Unit);
        Position != CurrentPhysicalEntities.end()) {
      return Position->second;
    }
    const auto Sets = PhysicalSets.find(Unit);
    const EntityID ID = createEntity(
        Sets == PhysicalSets.end() ? std::vector<RegisterPressureSetID>{} : Sets->second);
    CurrentPhysicalEntities.emplace(Unit, ID);
    return ID;
  }

  [[nodiscard]] EntityID definePhysicalEntity(RegisterUnitID Unit) {
    const auto Sets = PhysicalSets.find(Unit);
    const EntityID ID = createEntity(
        Sets == PhysicalSets.end() ? std::vector<RegisterPressureSetID>{} : Sets->second);
    CurrentPhysicalEntities.insert_or_assign(Unit, ID);
    return ID;
  }

  [[nodiscard]] Expected<std::vector<RegisterUnitID>> physicalUnits(Register Reg,
                                                                    SubRegIndex SubReg) const {
    std::optional<PhysicalRegister> Physical = Reg.physicalRegister();
    if (!Physical) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "pressure tracking encountered an invalid physical register");
    }
    if (SubReg.value() != 0) {
      Physical = Target->registers().subRegister(*Physical, SubReg);
      if (!Physical) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "pressure tracking encountered an invalid physical subregister");
      }
    }
    const PhysicalRegisterDesc *Description = Target->registers().physicalRegister(*Physical);
    if (Description == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "pressure tracking encountered an unknown physical register");
    }
    std::vector<RegisterUnitID> Units;
    Units.reserve(Description->Units.size());
    for (const RegisterUnitLane &Unit : Description->Units)
      Units.push_back(Unit.Unit);
    sortAndUnique(Units);
    return Units;
  }

  [[nodiscard]] Expected<void> appendRegisterUse(RegisterPressureTracker::Event &Event,
                                                 const MachineOperand &Operand) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr || Reg->isNoRegister())
      return {};
    if (Reg->isVirtual()) {
      auto Entity = boundaryVirtualEntity(*Reg);
      if (!Entity)
        return std::unexpected(Entity.error());
      Event.Uses.push_back(*Entity);
      return {};
    }
    auto Units = physicalUnits(*Reg, Operand.subReg());
    if (!Units)
      return std::unexpected(Units.error());
    for (const RegisterUnitID Unit : *Units)
      Event.Uses.push_back(boundaryPhysicalEntity(Unit));
    return {};
  }

  [[nodiscard]] Expected<void> appendRegisterDefinition(RegisterPressureTracker::Event &Event,
                                                        const MachineOperand &Operand) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr || Reg->isNoRegister())
      return {};
    if (Reg->isVirtual()) {
      auto Entity = defineVirtualEntity(*Reg);
      if (!Entity)
        return std::unexpected(Entity.error());
      Event.Definitions.push_back(*Entity);
      Result.Entities[*Entity].HasDefinition = true;
      Result.Entities[*Entity].DefinitionPending = true;
      return {};
    }
    auto Units = physicalUnits(*Reg, Operand.subReg());
    if (!Units)
      return std::unexpected(Units.error());
    for (const RegisterUnitID Unit : *Units) {
      const EntityID Entity = definePhysicalEntity(Unit);
      Event.Definitions.push_back(Entity);
      Result.Entities[Entity].HasDefinition = true;
      Result.Entities[Entity].DefinitionPending = true;
    }
    return {};
  }

  [[nodiscard]] Expected<void> appendMaskDefinition(RegisterPressureTracker::Event &Event,
                                                    const RegisterMask &Mask) {
    for (const PhysicalRegisterDesc &Register : Target->registers().physicalRegisters()) {
      if (!Mask.clobbers(Register.Reg))
        continue;
      for (const RegisterUnitLane &Unit : Register.Units) {
        const EntityID Entity = definePhysicalEntity(Unit.Unit);
        Event.Definitions.push_back(Entity);
        Result.Entities[Entity].HasDefinition = true;
        Result.Entities[Entity].DefinitionPending = true;
      }
    }
    return {};
  }

  void appendLiveOutMaskUse(RegisterPressureTracker::Event &Event, const RegisterMask &Mask) {
    for (const PhysicalRegisterDesc &Register : Target->registers().physicalRegisters()) {
      if (!Mask.preserves(Register.Reg))
        continue;
      for (const RegisterUnitLane &Unit : Register.Units)
        Event.Uses.push_back(boundaryPhysicalEntity(Unit.Unit));
    }
  }

  [[nodiscard]] Expected<RegisterPressureTracker::Event>
  buildEvent(const MachineInstr &Instruction) {
    RegisterPressureTracker::Event Event;
    for (const MachineOperand &Operand : Instruction.operands()) {
      if (const auto *Mask = Operand.getIf<LiveOutRegisterMaskOperand>()) {
        if (!Mask->Mask) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "pressure tracking encountered a null live-out register mask");
        }
        appendLiveOutMaskUse(Event, *Mask->Mask);
        continue;
      }
      if (!Operand.isRegister() || Operand.isDebugUse() ||
          Operand.registerFlags().has(RegisterOperandFlag::Undef)) {
        continue;
      }
      if (Operand.isUse()) {
        auto Added = appendRegisterUse(Event, Operand);
        if (!Added)
          return std::unexpected(Added.error());
      }
    }
    for (const MachineOperand &Operand : Instruction.operands()) {
      if (const auto *Mask = Operand.getIf<RegisterMaskOperand>()) {
        if (!Mask->Mask) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "pressure tracking encountered a null register mask");
        }
        auto Added = appendMaskDefinition(Event, *Mask->Mask);
        if (!Added)
          return std::unexpected(Added.error());
        continue;
      }
      if (!Operand.isRegister() || Operand.isDebugUse())
        continue;
      if (Operand.isDef()) {
        auto Added = appendRegisterDefinition(Event, Operand);
        if (!Added)
          return std::unexpected(Added.error());
      }
    }
    sortAndUnique(Event.Uses);
    sortAndUnique(Event.Definitions);
    return Event;
  }

  [[nodiscard]] Expected<void> buildEffects() {
    Result.Effects.resize(DAG->units().size());
    Result.Scheduled.assign(DAG->units().size(), false);
    for (const ScheduleUnit &Unit : DAG->units()) {
      RegisterPressureTracker::UnitEffects &Effects = Result.Effects[Unit.ID];
      Effects.Events.reserve(Unit.Instructions.size());
      for (const MachineInstr *Instruction : Unit.Instructions) {
        auto Event = buildEvent(*Instruction);
        if (!Event)
          return std::unexpected(Event.error());
        for (const EntityID Entity : Event->Uses)
          ++Result.Entities[Entity].RemainingUses;
        Effects.Events.push_back(std::move(*Event));
      }
    }
    return {};
  }

  void markEntityLiveOut(EntityID Entity) {
    RegisterPressureTracker::Entity &Value = Result.Entities[Entity];
    if (Value.LiveOut)
      return;
    Value.LiveOut = true;
    ++Value.RemainingUses;
  }

  [[nodiscard]] Expected<void> markBlockLiveOut(const MachineBlockLiveness &Liveness) {
    for (const auto &[Reg, Lanes] : Liveness.VirtualLiveOut.entries()) {
      (void)Lanes;
      auto Entity = boundaryVirtualEntity(Reg);
      if (!Entity)
        return std::unexpected(Entity.error());
      markEntityLiveOut(*Entity);
    }
    for (const auto &[Unit, Lanes] : Liveness.PhysicalLiveOut.entries()) {
      (void)Lanes;
      markEntityLiveOut(boundaryPhysicalEntity(Unit));
    }
    return {};
  }

  [[nodiscard]] Expected<void> markSuffixUses() {
    if (DAG->units().empty())
      return {};
    const MachineInstr *Instruction = DAG->units().back().Instructions.back()->next();
    std::unordered_set<Register> KilledVirtual;
    std::unordered_set<RegisterUnitID> KilledPhysical;
    for (; Instruction != nullptr; Instruction = Instruction->next()) {
      for (const MachineOperand &Operand : Instruction->operands()) {
        if (const auto *Mask = Operand.getIf<RegisterMaskOperand>()) {
          if (!Mask->Mask) {
            return makeUnexpected(ErrorCode::VerificationError,
                                  "pressure suffix scan encountered a null register mask");
          }
          for (const PhysicalRegisterDesc &Register : Target->registers().physicalRegisters()) {
            if (!Mask->Mask->clobbers(Register.Reg))
              continue;
            for (const RegisterUnitLane &Unit : Register.Units)
              KilledPhysical.insert(Unit.Unit);
          }
          continue;
        }
        if (!Operand.isRegister() || Operand.isDebugUse() ||
            Operand.registerFlags().has(RegisterOperandFlag::Undef)) {
          continue;
        }
        const Register *Reg = Operand.getIf<Register>();
        if (Reg == nullptr || Reg->isNoRegister())
          continue;
        if (Reg->isVirtual()) {
          if (Operand.isUse() && !KilledVirtual.contains(*Reg)) {
            const auto Position = CurrentVirtualEntities.find(*Reg);
            if (Position != CurrentVirtualEntities.end())
              markEntityLiveOut(Position->second);
          }
          if (Operand.isDef())
            KilledVirtual.insert(*Reg);
          continue;
        }
        auto Units = physicalUnits(*Reg, Operand.subReg());
        if (!Units)
          return std::unexpected(Units.error());
        for (const RegisterUnitID Unit : *Units) {
          if (Operand.isUse() && !KilledPhysical.contains(Unit)) {
            const auto Position = CurrentPhysicalEntities.find(Unit);
            if (Position != CurrentPhysicalEntities.end())
              markEntityLiveOut(Position->second);
          }
          if (Operand.isDef())
            KilledPhysical.insert(Unit);
        }
      }
    }
    return {};
  }

  [[nodiscard]] Expected<void> initializeLivePressure() {
    for (EntityID ID = 0; ID < Result.Entities.size(); ++ID) {
      RegisterPressureTracker::Entity &Entity = Result.Entities[ID];
      const bool InitiallyLive = Result.Direction == RegisterPressureDirection::TopDown
                                     ? !Entity.HasDefinition && Entity.RemainingUses != 0
                                     : Entity.LiveOut;
      if (!InitiallyLive)
        continue;
      auto Added = Result.makeLive(ID);
      if (!Added)
        return std::unexpected(Added.error());
    }
    return {};
  }
};

} // namespace detail

Expected<RegisterPressureTracker>
RegisterPressureTracker::create(const ScheduleDAG &DAG, const MachineFunction &Function,
                                const TargetDescription &Target, const TargetSchedulingModel &Model,
                                ScheduleDAGMode Mode, const MachineBlockLiveness &Liveness,
                                RegisterPressureDirection Direction) {
  detail::RegisterPressureBuilder Builder;
  Builder.DAG = &DAG;
  Builder.Function = &Function;
  Builder.Target = &Target;
  Builder.Model = &Model;
  Builder.Mode = Mode;
  Builder.Result.Direction = Direction;
  auto Sets = Builder.initializePressureSets();
  if (!Sets)
    return std::unexpected(Sets.error());
  auto Effects = Builder.buildEffects();
  if (!Effects)
    return std::unexpected(Effects.error());
  auto Suffix = Builder.markSuffixUses();
  if (!Suffix)
    return std::unexpected(Suffix.error());
  auto LiveOut = Builder.markBlockLiveOut(Liveness);
  if (!LiveOut)
    return std::unexpected(LiveOut.error());
  auto Initial = Builder.initializeLivePressure();
  if (!Initial)
    return std::unexpected(Initial.error());
  return std::move(Builder.Result);
}

Expected<RegisterPressureCost> RegisterPressureTracker::evaluate(std::size_t UnitID) const {
  RegisterPressureTracker Trial = *this;
  const RegisterPressureCost Before = cost();
  auto ScheduledUnit = Trial.schedule(UnitID);
  if (!ScheduledUnit)
    return std::unexpected(ScheduledUnit.error());
  RegisterPressureCost After = Trial.cost();
  After.PeakIncrease =
      After.PeakExcess > Before.PeakExcess ? After.PeakExcess - Before.PeakExcess : 0;
  return After;
}

Expected<void> RegisterPressureTracker::schedule(std::size_t UnitID) {
  if (UnitID >= Effects.size())
    return makeUnexpected(ErrorCode::InvalidArgument, "pressure tracker unit ID is out of range");
  if (Scheduled[UnitID])
    return makeUnexpected(ErrorCode::InvalidArgument, "schedule unit was already issued");
  RegisterPressureTracker Trial = *this;
  if (Trial.Direction == RegisterPressureDirection::TopDown) {
    for (const Event &Value : Trial.Effects[UnitID].Events) {
      auto Applied = Trial.applyEvent(Value);
      if (!Applied)
        return std::unexpected(Applied.error());
    }
  } else {
    for (auto Position = Trial.Effects[UnitID].Events.rbegin();
         Position != Trial.Effects[UnitID].Events.rend(); ++Position) {
      auto Applied = Trial.applyEvent(*Position);
      if (!Applied)
        return std::unexpected(Applied.error());
    }
  }
  Trial.Scheduled[UnitID] = true;
  *this = std::move(Trial);
  return {};
}

RegisterPressureCost RegisterPressureTracker::cost() const noexcept {
  RegisterPressureCost Result;
  for (const RegisterPressureValue &Value : Pressure) {
    Result.CurrentPressure = saturatingAdd(Result.CurrentPressure, Value.Current);
    if (Value.Current > Value.Limit) {
      Result.CurrentExcess = saturatingAdd(Result.CurrentExcess, Value.Current - Value.Limit);
    }
    if (Value.Maximum > Value.Limit)
      Result.PeakExcess = saturatingAdd(Result.PeakExcess, Value.Maximum - Value.Limit);
  }
  return Result;
}

Expected<void> RegisterPressureTracker::applyEvent(const Event &Value) {
  if (Direction == RegisterPressureDirection::BottomUp) {
    for (const EntityID Entity : Value.Definitions) {
      if (Entity >= Entities.size() || !Entities[Entity].DefinitionPending) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "bottom-up scheduler crossed an unavailable definition");
      }
      Entities[Entity].DefinitionPending = false;
      if (Entities[Entity].Live) {
        auto Removed = makeDead(Entity);
        if (!Removed)
          return std::unexpected(Removed.error());
      }
    }
    for (const EntityID Entity : Value.Uses) {
      if (Entity >= Entities.size()) {
        return makeUnexpected(ErrorCode::InternalError,
                              "pressure event references an unknown use entity");
      }
      if (!Entities[Entity].Live) {
        auto Added = makeLive(Entity);
        if (!Added)
          return std::unexpected(Added.error());
      }
    }
    return {};
  }
  for (const EntityID Entity : Value.Uses) {
    if (Entity >= Entities.size() || !Entities[Entity].Live ||
        Entities[Entity].RemainingUses == 0) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "scheduler selected a use before its pressure value became live");
    }
    --Entities[Entity].RemainingUses;
    if (Entities[Entity].RemainingUses == 0) {
      auto Removed = makeDead(Entity);
      if (!Removed)
        return std::unexpected(Removed.error());
    }
  }
  for (const EntityID Entity : Value.Definitions) {
    if (Entity >= Entities.size()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "pressure event references an unknown definition entity");
    }
    if (Entities[Entity].Live) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "scheduler redefined a pressure value while it was live");
    }
    if (Entities[Entity].RemainingUses != 0) {
      auto Added = makeLive(Entity);
      if (!Added)
        return std::unexpected(Added.error());
    }
  }
  return {};
}

Expected<void> RegisterPressureTracker::makeLive(EntityID EntityIDValue) {
  if (EntityIDValue >= Entities.size())
    return makeUnexpected(ErrorCode::InternalError, "unknown pressure entity became live");
  Entity &Value = Entities[EntityIDValue];
  if (Value.Live)
    return {};
  for (const RegisterPressureSetID Set : Value.Sets) {
    const auto Position = PressureIndexes.find(Set);
    if (Position == PressureIndexes.end()) {
      return makeUnexpected(ErrorCode::InternalError,
                            "pressure entity references an unknown pressure set");
    }
    RegisterPressureValue &SetPressure = Pressure[Position->second];
    if (SetPressure.Current == std::numeric_limits<std::uint32_t>::max()) {
      return makeUnexpected(ErrorCode::SchedulingFailed, "register pressure counter overflowed");
    }
    ++SetPressure.Current;
    SetPressure.Maximum = std::max(SetPressure.Maximum, SetPressure.Current);
  }
  Value.Live = true;
  return {};
}

Expected<void> RegisterPressureTracker::makeDead(EntityID EntityIDValue) {
  if (EntityIDValue >= Entities.size())
    return makeUnexpected(ErrorCode::InternalError, "unknown pressure entity became dead");
  Entity &Value = Entities[EntityIDValue];
  if (!Value.Live)
    return makeUnexpected(ErrorCode::InternalError, "dead pressure entity was not live");
  for (const RegisterPressureSetID Set : Value.Sets) {
    const auto Position = PressureIndexes.find(Set);
    if (Position == PressureIndexes.end() || Pressure[Position->second].Current == 0) {
      return makeUnexpected(ErrorCode::InternalError, "register pressure counter underflowed");
    }
    --Pressure[Position->second].Current;
  }
  Value.Live = false;
  return {};
}

} // namespace rcc

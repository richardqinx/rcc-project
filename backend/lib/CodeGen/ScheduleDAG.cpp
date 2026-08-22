// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/ScheduleDAG.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineAnalyses.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

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

struct RegisterDependencyKey final {
  bool IsUnit = false;
  std::uint32_t Value = 0;

  friend constexpr bool operator==(RegisterDependencyKey, RegisterDependencyKey) noexcept = default;
};

struct RegisterDependencyKeyHash final {
  [[nodiscard]] std::size_t operator()(RegisterDependencyKey Key) const noexcept {
    const std::uint64_t Value = (static_cast<std::uint64_t>(Key.IsUnit) << 32U) | Key.Value;
    return std::hash<std::uint64_t>{}(Value);
  }
};

struct RegisterDependencyState final {
  struct Definition final {
    std::size_t Unit = 0;
    MachineInstr *Instruction = nullptr;
    std::size_t OperandIndex = 0;
  };

  std::optional<Definition> LastDefinition;
  std::vector<std::size_t> LastUses;
};

struct RegisterAccess final {
  RegisterDependencyKey Key;
  MachineInstr *Instruction = nullptr;
  std::size_t OperandIndex = 0;
};

[[nodiscard]] bool keyLess(RegisterDependencyKey Left, RegisterDependencyKey Right) noexcept {
  return Left.IsUnit == Right.IsUnit ? Left.Value < Right.Value : Left.IsUnit < Right.IsUnit;
}

[[nodiscard]] std::uint32_t saturatingAdd(std::uint32_t Left, std::uint32_t Right) noexcept {
  return std::numeric_limits<std::uint32_t>::max() - Left < Right
             ? std::numeric_limits<std::uint32_t>::max()
             : Left + Right;
}

[[nodiscard]] std::uint64_t saturatingAdd(std::uint64_t Left, std::uint64_t Right) noexcept {
  return std::numeric_limits<std::uint64_t>::max() - Left < Right
             ? std::numeric_limits<std::uint64_t>::max()
             : Left + Right;
}

[[nodiscard]] Expected<std::vector<RegisterDependencyKey>>
operandKeys(const MachineOperand &Operand, const MachineFunction &Function,
            const TargetDescription &Target, ScheduleDAGMode Mode) {
  const Register *Reg = Operand.getIf<Register>();
  if (Reg == nullptr || Reg->isNoRegister())
    return std::vector<RegisterDependencyKey>{};
  if (Reg->isVirtual()) {
    if (Mode == ScheduleDAGMode::PostRegisterAllocation) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "post-RA scheduling region contains a virtual register");
    }
    if (!Function.registerInfo().isKnownVirtualRegister(*Reg)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "scheduler encountered an unknown virtual register");
    }
    return std::vector<RegisterDependencyKey>{{false, Reg->opaqueValue()}};
  }

  std::optional<PhysicalRegister> Physical = Reg->physicalRegister();
  if (!Physical)
    return makeUnexpected(ErrorCode::VerificationError,
                          "scheduler encountered an invalid physical register");
  if (Operand.subReg().value() != 0) {
    const std::optional<PhysicalRegister> SubReg =
        Target.registers().subRegister(*Physical, Operand.subReg());
    if (!SubReg) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "scheduler encountered an invalid physical subregister");
    }
    Physical = SubReg;
  }
  const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(*Physical);
  if (Description == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "scheduler encountered an unknown physical register");
  std::vector<RegisterDependencyKey> Result;
  Result.reserve(Description->Units.size());
  for (const RegisterUnitLane &Unit : Description->Units)
    Result.push_back({true, Unit.Unit.value()});
  std::ranges::sort(Result, {}, &RegisterDependencyKey::Value);
  const auto [FirstDuplicate, End] = std::ranges::unique(Result);
  Result.erase(FirstDuplicate, End);
  return Result;
}

[[nodiscard]] Expected<std::vector<RegisterDependencyKey>>
maskKeys(const RegisterMask &Mask, const TargetDescription &Target) {
  std::vector<RegisterDependencyKey> Result;
  for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
    if (!Mask.clobbers(Register.Reg))
      continue;
    for (const RegisterUnitLane &Unit : Register.Units)
      Result.push_back({true, Unit.Unit.value()});
  }
  std::ranges::sort(Result, {}, &RegisterDependencyKey::Value);
  const auto [FirstDuplicate, End] = std::ranges::unique(Result);
  Result.erase(FirstDuplicate, End);
  return Result;
}

[[nodiscard]] bool mayAlias(const MachineInstr &Earlier, const MachineInstr &Later,
                            const MachineAliasAnalysis::Result &Aliases) noexcept {
  if (Earlier.memOperands().empty() || Later.memOperands().empty())
    return true;
  for (const MachineMemOperand *Left : Earlier.memOperands()) {
    if (Left == nullptr)
      return true;
    for (const MachineMemOperand *Right : Later.memOperands()) {
      if (Right == nullptr || Aliases.alias(*Left, *Right) != MachineAliasResult::NoAlias)
        return true;
    }
  }
  return false;
}

[[nodiscard]] bool hasOrderedMemory(const MachineInstr &Instruction) noexcept {
  return std::ranges::any_of(Instruction.memOperands(), [](const MachineMemOperand *Memory) {
    return Memory == nullptr || Memory->isAtomic() || Memory->isVolatile();
  });
}

[[nodiscard]] bool hasLoad(const MachineInstr &Instruction) noexcept {
  return std::ranges::any_of(Instruction.memOperands(), [](const MachineMemOperand *Memory) {
    return Memory == nullptr || Memory->isLoad();
  });
}

[[nodiscard]] bool hasStore(const MachineInstr &Instruction) noexcept {
  return std::ranges::any_of(Instruction.memOperands(), [](const MachineMemOperand *Memory) {
    return Memory == nullptr || Memory->isStore();
  });
}

[[nodiscard]] bool isHazardDependency(ScheduleDependencyKind Kind) noexcept {
  return Kind == ScheduleDependencyKind::Anti || Kind == ScheduleDependencyKind::Output;
}

} // namespace

Expected<ScheduleDAG>
ScheduleDAG::build(MachineBasicBlock &Block, std::span<MachineInstr *const> Instructions,
                   const MachineFunction &Function, const TargetDescription &Target,
                   const TargetSchedulingModel &Model, ScheduleDAGMode Mode, bool EmitDebugOutput) {
  if (&Block.function() != &Function)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "schedule DAG block belongs to another function");
  ScheduleDAG Result(Block);
  auto Units = Result.buildUnits(Instructions, Target, Model);
  if (!Units)
    return std::unexpected(Units.error());
  auto Registers = Result.buildRegisterDependencies(Function, Target, Model, Mode);
  if (!Registers)
    return std::unexpected(Registers.error());
  auto Memory = Result.buildMemoryDependencies(Target);
  if (!Memory)
    return std::unexpected(Memory.error());
  auto Order = Result.buildOrderDependencies(Target);
  if (!Order)
    return std::unexpected(Order.error());
  auto Mutated = Model.hooks().mutateScheduleDAG(Result, Function, Target, Mode, EmitDebugOutput);
  if (!Mutated)
    return std::unexpected(Mutated.error());
  auto CriticalPaths = Result.computeCriticalPaths();
  if (!CriticalPaths)
    return std::unexpected(CriticalPaths.error());
  return Result;
}

Expected<void> ScheduleDAG::buildUnits(std::span<MachineInstr *const> Instructions,
                                       const TargetDescription &Target,
                                       const TargetSchedulingModel &Model) {
  MachineInstr *ExpectedInstruction = Instructions.empty() ? nullptr : Instructions.front();
  for (std::size_t Index = 0; Index < Instructions.size();) {
    MachineInstr *Instruction = Instructions[Index];
    if (Instruction == nullptr || Instruction->parent() != Block)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "schedule region contains a detached or foreign instruction");
    if (Instruction != ExpectedInstruction) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "schedule region is not a contiguous instruction sequence");
    }
    if (Instruction->flags().has(MachineInstrFlag::BundledWithPred)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "schedule region begins in the middle of an instruction bundle");
    }

    ScheduleUnit Unit;
    Unit.ID = Units.size();
    Unit.StableID = Instruction->stableID();
    std::unordered_map<ProcessorResourceID, std::uint32_t> ResourceCycles;
    for (;;) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "scheduler encountered an unknown instruction descriptor");
      }
      const SchedulingClassModelDesc *Class = Model.schedulingClass(Descriptor->SchedulingClass);
      if (Class == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "instruction references an unknown scheduling class");
      }
      Unit.MicroOps = saturatingAdd(Unit.MicroOps, Class->MicroOps);
      Unit.Latency = std::max(Unit.Latency, Class->Latency);
      for (const SchedulingResourceUse &Use : Class->Resources) {
        ResourceCycles[Use.Resource] = saturatingAdd(ResourceCycles[Use.Resource], Use.Cycles);
      }
      Unit.Instructions.push_back(Instruction);
      const bool HasSuccessor = Instruction->flags().has(MachineInstrFlag::BundledWithSucc);
      ++Index;
      if (!HasSuccessor)
        break;
      if (Index >= Instructions.size()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "schedule region ends in the middle of an instruction bundle");
      }
      MachineInstr *Next = Instructions[Index];
      if (Next == nullptr || Next->parent() != Block ||
          !Next->flags().has(MachineInstrFlag::BundledWithPred) || Instruction->next() != Next) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "instruction bundle links are not reciprocal and contiguous");
      }
      Instruction = Next;
    }
    Unit.Resources.reserve(ResourceCycles.size());
    for (const auto &[Resource, Cycles] : ResourceCycles)
      Unit.Resources.push_back({Resource, Cycles});
    std::ranges::sort(Unit.Resources, {}, &SchedulingResourceUse::Resource);
    ExpectedInstruction = Unit.Instructions.back()->next();
    Units.push_back(std::move(Unit));
  }
  return {};
}

Expected<void> ScheduleDAG::buildRegisterDependencies(const MachineFunction &Function,
                                                      const TargetDescription &Target,
                                                      const TargetSchedulingModel &Model,
                                                      ScheduleDAGMode Mode) {
  std::unordered_map<RegisterDependencyKey, RegisterDependencyState, RegisterDependencyKeyHash>
      States;
  for (ScheduleUnit &Unit : Units) {
    std::vector<RegisterAccess> Uses;
    std::vector<RegisterAccess> Definitions;
    for (MachineInstr *Instruction : Unit.Instructions) {
      for (std::size_t OperandIndex = 0; OperandIndex < Instruction->operandCount();
           ++OperandIndex) {
        const MachineOperand *Operand = Instruction->operandAt(OperandIndex);
        if (Operand == nullptr)
          continue;
        if (const auto *Mask = Operand->getIf<RegisterMaskOperand>()) {
          if (!Mask->Mask)
            return makeUnexpected(ErrorCode::VerificationError,
                                  "scheduler encountered a null register mask");
          auto Keys = maskKeys(*Mask->Mask, Target);
          if (!Keys)
            return std::unexpected(Keys.error());
          for (const RegisterDependencyKey Key : *Keys)
            Definitions.push_back({Key, Instruction, OperandIndex});
          continue;
        }
        if (const auto *Mask = Operand->getIf<LiveOutRegisterMaskOperand>()) {
          if (!Mask->Mask)
            return makeUnexpected(ErrorCode::VerificationError,
                                  "scheduler encountered a null live-out register mask");
          for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
            if (!Mask->Mask->preserves(Register.Reg))
              continue;
            for (const RegisterUnitLane &RegisterUnit : Register.Units)
              Uses.push_back({{true, RegisterUnit.Unit.value()}, Instruction, OperandIndex});
          }
          continue;
        }
        if (!Operand->isRegister() || Operand->isDebugUse() ||
            Operand->registerFlags().has(RegisterOperandFlag::Undef)) {
          continue;
        }
        auto Keys = operandKeys(*Operand, Function, Target, Mode);
        if (!Keys)
          return std::unexpected(Keys.error());
        if (Operand->isUse()) {
          for (const RegisterDependencyKey Key : *Keys)
            Uses.push_back({Key, Instruction, OperandIndex});
        }
        if (Operand->isDef()) {
          for (const RegisterDependencyKey Key : *Keys)
            Definitions.push_back({Key, Instruction, OperandIndex});
        }
      }
    }
    std::ranges::stable_sort(Uses, [](const RegisterAccess &Left, const RegisterAccess &Right) {
      return keyLess(Left.Key, Right.Key);
    });
    std::ranges::stable_sort(Definitions,
                             [](const RegisterAccess &Left, const RegisterAccess &Right) {
                               return keyLess(Left.Key, Right.Key);
                             });

    for (std::size_t Begin = 0; Begin < Uses.size();) {
      std::size_t End = Begin + 1;
      while (End < Uses.size() && Uses[End].Key == Uses[Begin].Key)
        ++End;
      RegisterDependencyState &State = States[Uses[Begin].Key];
      if (State.LastDefinition) {
        std::uint32_t Latency = 0;
        for (std::size_t Index = Begin; Index < End; ++Index) {
          const InstructionDescriptor *Producer =
              Target.instructionDescriptor(State.LastDefinition->Instruction->opcode());
          const InstructionDescriptor *Consumer =
              Target.instructionDescriptor(Uses[Index].Instruction->opcode());
          if (Producer == nullptr || Consumer == nullptr) {
            return makeUnexpected(ErrorCode::VerificationError,
                                  "scheduler latency lookup encountered an unknown instruction");
          }
          auto OperandLatency =
              Model.operandLatency(Producer->SchedulingClass, State.LastDefinition->OperandIndex,
                                   Consumer->SchedulingClass, Uses[Index].OperandIndex);
          if (!OperandLatency)
            return std::unexpected(OperandLatency.error());
          Latency = std::max(Latency, *OperandLatency);
        }
        addDependency(State.LastDefinition->Unit, Unit.ID,
                      Uses[Begin].Key.IsUnit ? ScheduleDependencyKind::RegisterUnit
                                             : ScheduleDependencyKind::Data,
                      Latency);
      }
      if (State.LastUses.empty() || State.LastUses.back() != Unit.ID)
        State.LastUses.push_back(Unit.ID);
      Begin = End;
    }

    for (std::size_t Begin = 0; Begin < Definitions.size();) {
      std::size_t End = Begin + 1;
      while (End < Definitions.size() && Definitions[End].Key == Definitions[Begin].Key) {
        ++End;
      }
      RegisterDependencyState &State = States[Definitions[Begin].Key];
      if (State.LastDefinition) {
        addDependency(State.LastDefinition->Unit, Unit.ID, ScheduleDependencyKind::Output, 0);
      }
      for (const std::size_t Use : State.LastUses)
        addDependency(Use, Unit.ID, ScheduleDependencyKind::Anti, 0);
      State.LastUses.clear();
      const RegisterAccess &Last = Definitions[End - 1];
      State.LastDefinition =
          RegisterDependencyState::Definition{Unit.ID, Last.Instruction, Last.OperandIndex};
      Begin = End;
    }
  }
  return {};
}

Expected<void> ScheduleDAG::addArtificialDependency(std::size_t Source, std::size_t Destination,
                                                    std::uint32_t Latency) {
  if (Source >= Units.size() || Destination >= Units.size() || Source == Destination) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "target schedule mutation requires distinct known schedule units");
  }
  if (hasPath(Destination, Source))
    return makeUnexpected(ErrorCode::SchedulingFailed,
                          "target schedule mutation would introduce a dependency cycle");
  addDependency(Source, Destination, ScheduleDependencyKind::Artificial, Latency);
  return {};
}

bool ScheduleDAG::hasPath(std::size_t Source, std::size_t Destination) const {
  if (Source >= Units.size() || Destination >= Units.size())
    return false;
  std::vector<bool> Visited(Units.size());
  std::vector<std::size_t> Pending = {Source};
  Visited[Source] = true;
  while (!Pending.empty()) {
    const std::size_t Current = Pending.back();
    Pending.pop_back();
    for (const ScheduleDependency &Dependency : Units[Current].Successors) {
      if (Dependency.Destination == Destination)
        return true;
      if (!Visited[Dependency.Destination]) {
        Visited[Dependency.Destination] = true;
        Pending.push_back(Dependency.Destination);
      }
    }
  }
  return false;
}

bool ScheduleDAG::hasDirectDependency(std::size_t Source, std::size_t Destination) const {
  if (Source >= Units.size() || Destination >= Units.size())
    return false;
  return std::ranges::any_of(Units[Source].Successors,
                             [Destination](const ScheduleDependency &Dependency) {
                               return Dependency.Destination == Destination;
                             });
}

Expected<bool> ScheduleDAG::fuseInstructionPair(std::size_t First, std::size_t Second) {
  if (First >= Units.size() || Second >= Units.size() || First == Second) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "macro fusion requires two distinct known schedule units");
  }
  ScheduleUnit &FirstUnit = Units[First];
  ScheduleUnit &SecondUnit = Units[Second];
  if (FirstUnit.ClusterPredecessor || FirstUnit.ClusterSuccessor || SecondUnit.ClusterPredecessor ||
      SecondUnit.ClusterSuccessor || hasPath(Second, First)) {
    return false;
  }

  struct Binding final {
    std::size_t Source = 0;
    std::size_t Destination = 0;
  };
  std::vector<Binding> Bindings;
  const std::vector<ScheduleDependency> FirstSuccessors = FirstUnit.Successors;
  const std::vector<ScheduleDependency> SecondPredecessors = SecondUnit.Predecessors;
  for (const ScheduleDependency &Dependency : FirstSuccessors) {
    if (Dependency.Kind == ScheduleDependencyKind::Cluster || isHazardDependency(Dependency.Kind) ||
        Dependency.Destination == Second || hasDirectDependency(Dependency.Destination, Second)) {
      continue;
    }
    if (hasPath(Dependency.Destination, Second))
      return false;
    Bindings.push_back({Second, Dependency.Destination});
  }
  for (const ScheduleDependency &Dependency : SecondPredecessors) {
    if (Dependency.Kind == ScheduleDependencyKind::Cluster || isHazardDependency(Dependency.Kind) ||
        Dependency.Source == First || hasDirectDependency(First, Dependency.Source)) {
      continue;
    }
    if (hasPath(First, Dependency.Source))
      return false;
    Bindings.push_back({Dependency.Source, First});
  }

  addDependency(First, Second, ScheduleDependencyKind::Cluster, 0);
  FirstUnit.ClusterSuccessor = Second;
  SecondUnit.ClusterPredecessor = First;
  for (ScheduleDependency &Dependency : FirstUnit.Successors) {
    if (Dependency.Destination == Second)
      Dependency.Latency = 0;
  }
  for (ScheduleDependency &Dependency : SecondUnit.Predecessors) {
    if (Dependency.Source == First)
      Dependency.Latency = 0;
  }
  for (const Binding &Binding : Bindings)
    addDependency(Binding.Source, Binding.Destination, ScheduleDependencyKind::Artificial, 0);
  return true;
}

Expected<void> ScheduleDAG::buildMemoryDependencies(const TargetDescription &Target) {
  MachineAliasAnalysis::Result Aliases;
  for (std::size_t LaterIndex = 0; LaterIndex < Units.size(); ++LaterIndex) {
    for (std::size_t EarlierIndex = 0; EarlierIndex < LaterIndex; ++EarlierIndex) {
      bool NeedsEdge = false;
      for (MachineInstr *Earlier : Units[EarlierIndex].Instructions) {
        const InstructionDescriptor *EarlierDescriptor =
            Target.instructionDescriptor(Earlier->opcode());
        if (EarlierDescriptor == nullptr)
          return makeUnexpected(ErrorCode::VerificationError,
                                "scheduler encountered an unknown instruction descriptor");
        const bool EarlierMemory = EarlierDescriptor->Flags.has(InstructionFlag::MayLoad) ||
                                   EarlierDescriptor->Flags.has(InstructionFlag::MayStore) ||
                                   !Earlier->memOperands().empty();
        if (!EarlierMemory)
          continue;
        for (MachineInstr *Later : Units[LaterIndex].Instructions) {
          const InstructionDescriptor *LaterDescriptor =
              Target.instructionDescriptor(Later->opcode());
          if (LaterDescriptor == nullptr)
            return makeUnexpected(ErrorCode::VerificationError,
                                  "scheduler encountered an unknown instruction descriptor");
          const bool LaterMemory = LaterDescriptor->Flags.has(InstructionFlag::MayLoad) ||
                                   LaterDescriptor->Flags.has(InstructionFlag::MayStore) ||
                                   !Later->memOperands().empty();
          if (!LaterMemory)
            continue;
          if (hasOrderedMemory(*Earlier) || hasOrderedMemory(*Later) ||
              ((hasStore(*Earlier) || hasStore(*Later)) && mayAlias(*Earlier, *Later, Aliases)) ||
              (Earlier->memOperands().empty() || Later->memOperands().empty()) ||
              (!hasLoad(*Earlier) && !hasStore(*Earlier)) ||
              (!hasLoad(*Later) && !hasStore(*Later))) {
            NeedsEdge = true;
            break;
          }
        }
        if (NeedsEdge)
          break;
      }
      if (NeedsEdge)
        addDependency(EarlierIndex, LaterIndex, ScheduleDependencyKind::Memory, 0);
    }
  }
  return {};
}

Expected<void> ScheduleDAG::buildOrderDependencies(const TargetDescription &Target) {
  for (std::size_t Barrier = 0; Barrier < Units.size(); ++Barrier) {
    bool IsBarrier = false;
    for (const MachineInstr *Instruction : Units[Barrier].Instructions) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "scheduler encountered an unknown instruction descriptor");
      }
      const InstructionFlags Flags = Descriptor->Flags;
      IsBarrier |= Flags.has(InstructionFlag::Terminator) || Flags.has(InstructionFlag::Branch) ||
                   Flags.has(InstructionFlag::Call) || Flags.has(InstructionFlag::Return) ||
                   Flags.has(InstructionFlag::Barrier) ||
                   Flags.has(InstructionFlag::HasSideEffects) ||
                   Flags.has(InstructionFlag::UnmodeledSideEffects) ||
                   Flags.has(InstructionFlag::Convergent) || Flags.has(InstructionFlag::Phi) ||
                   Flags.has(InstructionFlag::Meta);
    }
    if (!IsBarrier)
      continue;
    for (std::size_t Earlier = 0; Earlier < Barrier; ++Earlier)
      addDependency(Earlier, Barrier, ScheduleDependencyKind::Barrier, 0);
    for (std::size_t Later = Barrier + 1; Later < Units.size(); ++Later)
      addDependency(Barrier, Later, ScheduleDependencyKind::Barrier, 0);
  }
  return {};
}

void ScheduleDAG::addDependency(std::size_t Source, std::size_t Destination,
                                ScheduleDependencyKind Kind, std::uint32_t Latency) {
  if (Source >= Units.size() || Destination >= Units.size() || Source == Destination)
    return;
  const auto Existing = std::ranges::find_if(
      Units[Source].Successors, [Destination, Kind](const ScheduleDependency &Dependency) {
        return Dependency.Destination == Destination && Dependency.Kind == Kind;
      });
  if (Existing != Units[Source].Successors.end()) {
    Existing->Latency = std::max(Existing->Latency, Latency);
    const auto Predecessor = std::ranges::find_if(
        Units[Destination].Predecessors, [Source, Kind](const ScheduleDependency &Dependency) {
          return Dependency.Source == Source && Dependency.Kind == Kind;
        });
    if (Predecessor != Units[Destination].Predecessors.end())
      Predecessor->Latency = Existing->Latency;
    return;
  }
  const ScheduleDependency Dependency{Source, Destination, Kind, Latency};
  Units[Source].Successors.push_back(Dependency);
  Units[Destination].Predecessors.push_back(Dependency);
}

Expected<void> ScheduleDAG::computeCriticalPaths() {
  std::vector<std::size_t> RemainingPredecessors;
  RemainingPredecessors.reserve(Units.size());
  for (const ScheduleUnit &Unit : Units)
    RemainingPredecessors.push_back(Unit.Predecessors.size());
  std::vector<std::size_t> TopologicalOrder;
  TopologicalOrder.reserve(Units.size());
  std::vector<bool> Emitted(Units.size());
  for (std::size_t Count = 0; Count < Units.size(); ++Count) {
    std::optional<std::size_t> Next;
    for (const ScheduleUnit &Unit : Units) {
      if (!Emitted[Unit.ID] && RemainingPredecessors[Unit.ID] == 0) {
        Next = Unit.ID;
        break;
      }
    }
    if (!Next) {
      return makeUnexpected(ErrorCode::SchedulingFailed,
                            "schedule DAG contains a cyclic target mutation");
    }
    Emitted[*Next] = true;
    TopologicalOrder.push_back(*Next);
    for (const ScheduleDependency &Dependency : Units[*Next].Successors) {
      if (RemainingPredecessors[Dependency.Destination] == 0) {
        return makeUnexpected(ErrorCode::InternalError,
                              "schedule DAG predecessor count underflowed");
      }
      --RemainingPredecessors[Dependency.Destination];
    }
  }
  for (const std::size_t ID : TopologicalOrder) {
    ScheduleUnit &Unit = Units[ID];
    Unit.Depth = 0;
    for (const ScheduleDependency &Dependency : Unit.Predecessors) {
      Unit.Depth =
          std::max(Unit.Depth, saturatingAdd(Units[Dependency.Source].Depth,
                                             static_cast<std::uint64_t>(Dependency.Latency)));
    }
  }
  for (auto Position = TopologicalOrder.rbegin(); Position != TopologicalOrder.rend(); ++Position) {
    ScheduleUnit &Unit = Units[*Position];
    Unit.Height = Unit.Latency;
    for (const ScheduleDependency &Dependency : Unit.Successors) {
      Unit.Height =
          std::max(Unit.Height, saturatingAdd(Units[Dependency.Destination].Height,
                                              static_cast<std::uint64_t>(Dependency.Latency)));
    }
  }
  for (ScheduleUnit &Unit : Units) {
    std::ranges::sort(Unit.Predecessors, {}, &ScheduleDependency::Source);
    std::ranges::sort(Unit.Successors, {}, &ScheduleDependency::Destination);
  }
  return {};
}

} // namespace rcc

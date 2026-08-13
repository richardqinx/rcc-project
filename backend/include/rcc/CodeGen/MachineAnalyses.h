// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEANALYSES_H
#define RCC_CODEGEN_MACHINEANALYSES_H

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class MachineMemOperand;
class TargetDescription;

class MachineDominatorTreeAnalysis final {
public:
  class Result final {
  public:
    [[nodiscard]] bool isReachable(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] bool dominates(const MachineBasicBlock &Dominator,
                                 const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] bool dominates(const MachineInstr &Dominator,
                                 const MachineInstr &Instruction) const noexcept;
    [[nodiscard]] const MachineBasicBlock *
    immediateDominator(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] const MachineBasicBlock *
    nearestCommonDominator(const MachineBasicBlock &Left,
                           const MachineBasicBlock &Right) const noexcept;
    [[nodiscard]] std::span<MachineBasicBlock *const> unreachableBlocks() const noexcept {
      return UnreachableBlocks;
    }

    [[nodiscard]] Expected<void> updateAfterCFGChange(MachineFunction &Function);

  private:
    [[nodiscard]] Expected<void> build(MachineFunction &Function);

    std::vector<MachineBasicBlock *> Blocks;
    std::unordered_map<const MachineBasicBlock *, std::size_t> BlockToIndex;
    std::vector<std::vector<bool>> Dominators;
    std::vector<const MachineBasicBlock *> ImmediateDominators;
    std::vector<bool> Reachable;
    std::vector<MachineBasicBlock *> UnreachableBlocks;

    friend class MachineDominatorTreeAnalysis;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "MachineDominatorTree";
  }

  [[nodiscard]] static Expected<Result> run(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            MachineFunctionAnalysisManager &Analyses);
};

struct MachineLoop final {
  MachineBasicBlock *Header = nullptr;
  std::vector<MachineBasicBlock *> Blocks;
  std::vector<MachineBasicBlock *> Latches;
  std::size_t Parent = static_cast<std::size_t>(-1);
  std::vector<std::size_t> Children;
  std::uint32_t Depth = 1;
};

struct IrreducibleMachineRegion final {
  std::vector<MachineBasicBlock *> Blocks;
  std::vector<MachineBasicBlock *> Entries;
};

class MachineLoopAnalysis final {
public:
  class Result final {
  public:
    [[nodiscard]] std::span<const MachineLoop> loops() const noexcept {
      return Loops;
    }
    [[nodiscard]] std::span<const IrreducibleMachineRegion> irreducibleRegions() const noexcept {
      return IrreducibleRegions;
    }
    [[nodiscard]] const MachineLoop *loopFor(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] std::uint32_t loopDepth(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] bool isIrreducible(const MachineBasicBlock &Block) const noexcept;

  private:
    std::vector<MachineLoop> Loops;
    std::vector<IrreducibleMachineRegion> IrreducibleRegions;
    std::unordered_map<const MachineBasicBlock *, std::size_t> InnermostLoop;
    std::unordered_set<const MachineBasicBlock *> IrreducibleBlocks;

    friend class MachineLoopAnalysis;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "MachineLoopInfo";
  }

  [[nodiscard]] static Expected<Result> run(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            MachineFunctionAnalysisManager &Analyses);
};

class MachineBranchProbabilityAnalysis final {
public:
  class Result final {
  public:
    [[nodiscard]] BranchProbability probability(const MachineBasicBlock &From,
                                                const MachineBasicBlock &To) const noexcept;
    [[nodiscard]] std::span<const MachineSuccessor>
    successors(const MachineBasicBlock &Block) const noexcept;

  private:
    std::unordered_map<const MachineBasicBlock *, std::vector<MachineSuccessor>> Probabilities;

    friend class MachineBranchProbabilityAnalysis;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "MachineBranchProbability";
  }

  [[nodiscard]] static Expected<Result> run(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            MachineFunctionAnalysisManager &Analyses);
};

class MachineBlockFrequencyAnalysis final {
public:
  static constexpr std::uint64_t EntryFrequency = std::uint64_t{1} << 32U;

  class Result final {
  public:
    [[nodiscard]] std::uint64_t frequency(const MachineBasicBlock &Block) const noexcept;

  private:
    std::unordered_map<const MachineBasicBlock *, std::uint64_t> Frequencies;

    friend class MachineBlockFrequencyAnalysis;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "MachineBlockFrequency";
  }

  [[nodiscard]] static Expected<Result> run(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            MachineFunctionAnalysisManager &Analyses);
};

enum class MachineAliasResult : std::uint8_t {
  NoAlias,
  MayAlias,
  PartialAlias,
  MustAlias,
};

class MachineAliasAnalysis final {
public:
  class Result final {
  public:
    [[nodiscard]] MachineAliasResult alias(const MachineMemOperand &Left,
                                           const MachineMemOperand &Right) const noexcept;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "MachineAliasAnalysis";
  }

  [[nodiscard]] static Expected<Result> run(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            MachineFunctionAnalysisManager &Analyses);
};

enum class MachineMemoryDependencyKind : std::uint8_t {
  Flow,
  Anti,
  Output,
  Ordered,
};

struct MachineMemoryDependency final {
  MachineInstrStableID Source;
  MachineInstrStableID Destination;
  MachineMemoryDependencyKind Kind = MachineMemoryDependencyKind::Ordered;
};

struct MachineMemoryDependenceRegion final {
  const MachineBasicBlock *Block = nullptr;
  std::vector<MachineInstrStableID> Instructions;
};

class MachineMemoryDependenceAnalysis final {
public:
  class Result final {
  public:
    [[nodiscard]] std::span<const MachineMemoryDependenceRegion> regions() const noexcept {
      return Regions;
    }
    [[nodiscard]] const MachineMemoryDependenceRegion *
    regionFor(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] std::span<const MachineMemoryDependency>
    dependencies(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] std::span<const MachineMemoryDependency>
    dependencies(const MachineInstr &Instruction) const noexcept;
    [[nodiscard]] std::span<const MachineMemoryDependency> allDependencies() const noexcept {
      return Dependencies;
    }

  private:
    struct Range final {
      std::size_t Begin = 0;
      std::size_t Size = 0;
    };

    std::vector<MachineMemoryDependency> Dependencies;
    std::vector<MachineMemoryDependenceRegion> Regions;
    std::unordered_map<const MachineBasicBlock *, std::size_t> BlockToRegion;
    std::unordered_map<const MachineBasicBlock *, Range> BlockRanges;
    std::unordered_map<MachineInstrStableID, Range> DestinationRanges;

    friend class MachineMemoryDependenceAnalysis;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "MachineMemoryDependence";
  }

  [[nodiscard]] static Expected<Result> run(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            MachineFunctionAnalysisManager &Analyses);
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEANALYSES_H

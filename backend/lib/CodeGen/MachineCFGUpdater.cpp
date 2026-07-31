// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCFGUpdater.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] std::optional<std::size_t> findSuccessorIndex(const MachineBasicBlock &From,
                                                            const MachineBasicBlock &To) {
  const auto Successors = From.successors();
  for (std::size_t Index = 0; Index < Successors.size(); ++Index) {
    if (Successors[Index].Block == &To)
      return Index;
  }
  return std::nullopt;
}

} // namespace

Expected<void> MachineCFGUpdater::verifyOwnership(const MachineBasicBlock &Block) const {
  if (&Block.function() != Function) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "CFG block belongs to another machine function");
  }
  return {};
}

Expected<void> MachineCFGUpdater::addSuccessor(MachineBasicBlock &From, MachineBasicBlock &To,
                                               BranchProbability Probability) {
  auto FromOwnership = verifyOwnership(From);
  if (!FromOwnership)
    return std::unexpected(FromOwnership.error());
  auto ToOwnership = verifyOwnership(To);
  if (!ToOwnership)
    return std::unexpected(ToOwnership.error());

  const auto Existing = findSuccessorIndex(From, To);
  if (Existing) {
    From.Successors[*Existing].Probability =
        From.Successors[*Existing].Probability.saturatingAdd(Probability);
    return {};
  }

  From.Successors.push_back(MachineSuccessor{&To, Probability});
  if (std::ranges::find(To.Predecessors, &From) == To.Predecessors.end())
    To.Predecessors.push_back(&From);
  return {};
}

Expected<void> MachineCFGUpdater::removeSuccessor(MachineBasicBlock &From, MachineBasicBlock &To) {
  auto FromOwnership = verifyOwnership(From);
  if (!FromOwnership)
    return std::unexpected(FromOwnership.error());
  auto ToOwnership = verifyOwnership(To);
  if (!ToOwnership)
    return std::unexpected(ToOwnership.error());

  const auto Successor = findSuccessorIndex(From, To);
  if (!Successor) {
    return makeUnexpected(ErrorCode::InvalidArgument, "CFG successor edge is not present");
  }
  From.Successors.erase(From.Successors.begin() + static_cast<std::ptrdiff_t>(*Successor));

  const auto Predecessor = std::ranges::find(To.Predecessors, &From);
  if (Predecessor == To.Predecessors.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "CFG successor has no reciprocal predecessor");
  }
  To.Predecessors.erase(Predecessor);
  return {};
}

Expected<void> MachineCFGUpdater::replaceSuccessor(MachineBasicBlock &From,
                                                   MachineBasicBlock &OldTarget,
                                                   MachineBasicBlock &NewTarget) {
  auto FromOwnership = verifyOwnership(From);
  if (!FromOwnership)
    return std::unexpected(FromOwnership.error());
  auto OldOwnership = verifyOwnership(OldTarget);
  if (!OldOwnership)
    return std::unexpected(OldOwnership.error());
  auto NewOwnership = verifyOwnership(NewTarget);
  if (!NewOwnership)
    return std::unexpected(NewOwnership.error());

  if (&OldTarget == &NewTarget)
    return {};

  const auto Existing = findSuccessorIndex(From, OldTarget);
  if (!Existing) {
    return makeUnexpected(ErrorCode::InvalidArgument, "replaced CFG successor edge is not present");
  }
  const BranchProbability Probability = From.Successors[*Existing].Probability;

  auto RemoveResult = removeSuccessor(From, OldTarget);
  if (!RemoveResult)
    return std::unexpected(RemoveResult.error());
  auto AddResult = addSuccessor(From, NewTarget, Probability);
  if (!AddResult) {
    auto RestoreResult = addSuccessor(From, OldTarget, Probability);
    if (!RestoreResult) {
      return makeUnexpected(ErrorCode::InternalError,
                            "failed to restore CFG edge after replacement failure");
    }
    return std::unexpected(AddResult.error());
  }
  return {};
}

Expected<void> MachineCFGUpdater::updateBranchProbability(MachineBasicBlock &From,
                                                          MachineBasicBlock &To,
                                                          BranchProbability Probability) {
  auto FromOwnership = verifyOwnership(From);
  if (!FromOwnership)
    return std::unexpected(FromOwnership.error());
  auto ToOwnership = verifyOwnership(To);
  if (!ToOwnership)
    return std::unexpected(ToOwnership.error());

  const auto Successor = findSuccessorIndex(From, To);
  if (!Successor) {
    return makeUnexpected(ErrorCode::InvalidArgument, "updated CFG successor edge is not present");
  }
  From.Successors[*Successor].Probability = Probability;
  return {};
}

Expected<void> MachineCFGUpdater::transferSuccessors(MachineBasicBlock &From,
                                                     MachineBasicBlock &To) {
  auto FromOwnership = verifyOwnership(From);
  if (!FromOwnership)
    return std::unexpected(FromOwnership.error());
  auto ToOwnership = verifyOwnership(To);
  if (!ToOwnership)
    return std::unexpected(ToOwnership.error());
  if (&From == &To)
    return {};

  const std::vector<MachineSuccessor> Successors = From.Successors;
  for (const MachineSuccessor &Successor : Successors) {
    auto RemoveResult = removeSuccessor(From, *Successor.Block);
    if (!RemoveResult)
      return std::unexpected(RemoveResult.error());
    auto AddResult = addSuccessor(To, *Successor.Block, Successor.Probability);
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }
  return {};
}

Expected<void> MachineCFGUpdater::disconnectBlock(MachineBasicBlock &Block) {
  auto Ownership = verifyOwnership(Block);
  if (!Ownership)
    return std::unexpected(Ownership.error());

  const std::vector<MachineSuccessor> Successors = Block.Successors;
  for (const MachineSuccessor &Successor : Successors) {
    auto Result = removeSuccessor(Block, *Successor.Block);
    if (!Result)
      return std::unexpected(Result.error());
  }

  const std::vector<MachineBasicBlock *> Predecessors = Block.Predecessors;
  for (MachineBasicBlock *Predecessor : Predecessors) {
    auto Result = removeSuccessor(*Predecessor, Block);
    if (!Result)
      return std::unexpected(Result.error());
  }
  return {};
}

} // namespace rcc

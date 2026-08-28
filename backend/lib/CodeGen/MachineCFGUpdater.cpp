// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCFGUpdater.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace rcc {
namespace {

struct PhiIncomingEdit final {
  MachineInstr *Phi = nullptr;
  std::size_t ValueIndex = 0;
};

[[nodiscard]] bool isKnownPhi(const MachineInstr &Instruction) noexcept {
  return Instruction.opcode() == opcode(GenericOpcode::PHI) ||
         Instruction.opcode() == opcode(GenericOpcode::G_PHI);
}

[[nodiscard]] bool containsKnownPhi(const MachineBasicBlock &Block) noexcept {
  for (const MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (isKnownPhi(*Instruction))
      return true;
  }
  return false;
}

[[nodiscard]] Expected<std::vector<PhiIncomingEdit>>
collectPhiIncomingEdits(MachineBasicBlock &Block, const MachineBasicBlock &Predecessor) {
  std::vector<PhiIncomingEdit> Result;
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (!isKnownPhi(*Instruction))
      continue;
    if (Instruction->operandCount() < 3 || (Instruction->operandCount() - 1U) % 2U != 0) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "CFG update encountered a malformed PHI instruction");
    }

    std::optional<std::size_t> Match;
    for (std::size_t Index = 1; Index < Instruction->operandCount(); Index += 2U) {
      const MachineOperand *Value = Instruction->operandAt(Index);
      const MachineOperand *BlockOperand = Instruction->operandAt(Index + 1U);
      const Register *Reg = Value == nullptr ? nullptr : Value->getIf<Register>();
      const BasicBlockOperand *Incoming =
          BlockOperand == nullptr ? nullptr : BlockOperand->getIf<BasicBlockOperand>();
      if (Reg == nullptr || Incoming == nullptr || Incoming->Block == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "CFG update encountered malformed PHI incoming operands");
      }
      if (Incoming->Block != &Predecessor)
        continue;
      if (Match) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "CFG update encountered duplicate PHI incoming blocks");
      }
      Match = Index;
    }
    if (!Match) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "CFG edge has no corresponding PHI incoming value");
    }
    Result.push_back(PhiIncomingEdit{Instruction, *Match});
  }
  return Result;
}

[[nodiscard]] Expected<void> erasePhiIncoming(std::span<const PhiIncomingEdit> Edits) {
  for (const PhiIncomingEdit &Edit : Edits) {
    auto BlockErased = Edit.Phi->eraseOperand(Edit.ValueIndex + 1U);
    if (!BlockErased)
      return std::unexpected(BlockErased.error());
    auto ValueErased = Edit.Phi->eraseOperand(Edit.ValueIndex);
    if (!ValueErased)
      return std::unexpected(ValueErased.error());
  }
  return {};
}

[[nodiscard]] Expected<void> rewritePhiIncoming(std::span<const PhiIncomingEdit> Edits,
                                                MachineBasicBlock &NewPredecessor) {
  for (const PhiIncomingEdit &Edit : Edits) {
    MachineOperand *BlockOperand = Edit.Phi->operandAt(Edit.ValueIndex + 1U);
    if (BlockOperand == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "PHI incoming operand disappeared during CFG update");
    }
    auto Rewritten = BlockOperand->setBasicBlock(NewPredecessor);
    if (!Rewritten)
      return std::unexpected(Rewritten.error());
  }
  return {};
}

[[nodiscard]] std::optional<std::size_t> findSuccessorIndex(const MachineBasicBlock &From,
                                                            const MachineBasicBlock &To) {
  const auto Successors = From.successors();
  for (std::size_t Index = 0; Index < Successors.size(); ++Index) {
    if (Successors[Index].Block == &To)
      return Index;
  }
  return std::nullopt;
}

[[nodiscard]] Expected<void> validateOutgoingEdges(MachineBasicBlock &From) {
  for (const MachineSuccessor &Successor : From.successors()) {
    if (Successor.Block == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError, "CFG successor is null");
    }
    if (std::ranges::find(Successor.Block->predecessors(), &From) ==
        Successor.Block->predecessors().end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "CFG successor has no reciprocal predecessor");
    }
    auto PhiEdits = collectPhiIncomingEdits(*Successor.Block, From);
    if (!PhiEdits)
      return std::unexpected(PhiEdits.error());
  }
  return {};
}

[[nodiscard]] Expected<void> validateIncomingEdges(MachineBasicBlock &To) {
  for (MachineBasicBlock *Predecessor : To.predecessors()) {
    if (Predecessor == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError, "CFG predecessor is null");
    }
    if (!findSuccessorIndex(*Predecessor, To)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "CFG predecessor has no reciprocal successor");
    }
    auto PhiEdits = collectPhiIncomingEdits(To, *Predecessor);
    if (!PhiEdits)
      return std::unexpected(PhiEdits.error());
  }
  return {};
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

  if (containsKnownPhi(To)) {
    auto PhiEdits = collectPhiIncomingEdits(To, From);
    if (!PhiEdits) {
      return makeUnexpected(
          ErrorCode::InvalidArgument,
          "adding a predecessor to a PHI block requires explicit incoming values");
    }
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
  const auto Predecessor = std::ranges::find(To.Predecessors, &From);
  if (Predecessor == To.Predecessors.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "CFG successor has no reciprocal predecessor");
  }

  auto PhiEdits = collectPhiIncomingEdits(To, From);
  if (!PhiEdits)
    return std::unexpected(PhiEdits.error());
  auto PhiResult = erasePhiIncoming(*PhiEdits);
  if (!PhiResult)
    return std::unexpected(PhiResult.error());

  From.Successors.erase(From.Successors.begin() + static_cast<std::ptrdiff_t>(*Successor));
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

  const auto OldPredecessor = std::ranges::find(OldTarget.Predecessors, &From);
  if (OldPredecessor == OldTarget.Predecessors.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "replaced CFG successor has no reciprocal predecessor");
  }
  auto OldPhiEdits = collectPhiIncomingEdits(OldTarget, From);
  if (!OldPhiEdits)
    return std::unexpected(OldPhiEdits.error());

  const auto NewEdge = findSuccessorIndex(From, NewTarget);
  const auto NewPredecessor = std::ranges::find(NewTarget.Predecessors, &From);
  if (NewEdge) {
    if (NewPredecessor == NewTarget.Predecessors.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "replacement target has no reciprocal predecessor");
    }
    auto NewPhiEdits = collectPhiIncomingEdits(NewTarget, From);
    if (!NewPhiEdits)
      return std::unexpected(NewPhiEdits.error());
  } else {
    if (NewPredecessor != NewTarget.Predecessors.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "replacement target has a predecessor without a reciprocal edge");
    }
    if (containsKnownPhi(NewTarget)) {
      auto NewPhiEdits = collectPhiIncomingEdits(NewTarget, From);
      if (!NewPhiEdits) {
        return makeUnexpected(
            ErrorCode::InvalidArgument,
            "redirecting an edge to a PHI block requires explicit incoming values");
      }
    }
  }
  auto PhiResult = erasePhiIncoming(*OldPhiEdits);
  if (!PhiResult)
    return std::unexpected(PhiResult.error());
  OldTarget.Predecessors.erase(OldPredecessor);
  if (NewEdge) {
    From.Successors[*NewEdge].Probability =
        From.Successors[*NewEdge].Probability.saturatingAdd(Probability);
    From.Successors.erase(From.Successors.begin() + static_cast<std::ptrdiff_t>(*Existing));
  } else {
    From.Successors[*Existing].Block = &NewTarget;
    if (std::ranges::find(NewTarget.Predecessors, &From) == NewTarget.Predecessors.end())
      NewTarget.Predecessors.push_back(&From);
  }
  return {};
}

Expected<void> MachineCFGUpdater::splitEdge(MachineBasicBlock &From, MachineBasicBlock &To,
                                            MachineBasicBlock &EdgeBlock) {
  auto FromOwnership = verifyOwnership(From);
  if (!FromOwnership)
    return std::unexpected(FromOwnership.error());
  auto ToOwnership = verifyOwnership(To);
  if (!ToOwnership)
    return std::unexpected(ToOwnership.error());
  auto EdgeOwnership = verifyOwnership(EdgeBlock);
  if (!EdgeOwnership)
    return std::unexpected(EdgeOwnership.error());
  if (&From == &To || &From == &EdgeBlock || &To == &EdgeBlock) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "edge split requires three distinct basic blocks");
  }
  if (!EdgeBlock.isLinked() || EdgeBlock.firstInstruction() != nullptr ||
      !EdgeBlock.predecessors().empty() || !EdgeBlock.successors().empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "edge split block must be linked, empty, and disconnected");
  }

  const auto Successor = findSuccessorIndex(From, To);
  if (!Successor)
    return makeUnexpected(ErrorCode::InvalidArgument, "split CFG edge is not present");
  auto Predecessor = std::ranges::find(To.Predecessors, &From);
  if (Predecessor == To.Predecessors.end()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "split CFG edge has no reciprocal predecessor");
  }
  auto PhiEdits = collectPhiIncomingEdits(To, From);
  if (!PhiEdits)
    return std::unexpected(PhiEdits.error());

  auto PhiResult = rewritePhiIncoming(*PhiEdits, EdgeBlock);
  if (!PhiResult)
    return std::unexpected(PhiResult.error());
  From.Successors[*Successor].Block = &EdgeBlock;
  *Predecessor = &EdgeBlock;
  EdgeBlock.Predecessors.push_back(&From);
  EdgeBlock.Successors.push_back(MachineSuccessor{&To, BranchProbability::one()});
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

  struct Transfer final {
    MachineSuccessor Successor;
    std::vector<PhiIncomingEdit> PhiEdits;
    bool MergeExisting = false;
  };
  std::vector<Transfer> Transfers;
  Transfers.reserve(From.Successors.size());
  for (const MachineSuccessor &Successor : From.Successors) {
    if (Successor.Block == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError, "transferred CFG successor is null");
    }
    if (std::ranges::find(Successor.Block->Predecessors, &From) ==
        Successor.Block->Predecessors.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "transferred CFG successor has no reciprocal predecessor");
    }
    auto PhiEdits = collectPhiIncomingEdits(*Successor.Block, From);
    if (!PhiEdits)
      return std::unexpected(PhiEdits.error());
    const bool MergeExisting = findSuccessorIndex(To, *Successor.Block).has_value();
    const auto ToPredecessor = std::ranges::find(Successor.Block->Predecessors, &To);
    if (MergeExisting) {
      if (ToPredecessor == Successor.Block->Predecessors.end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "transfer target edge has no reciprocal predecessor");
      }
      auto ExistingPhiEdits = collectPhiIncomingEdits(*Successor.Block, To);
      if (!ExistingPhiEdits)
        return std::unexpected(ExistingPhiEdits.error());
    } else if (ToPredecessor != Successor.Block->Predecessors.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "transfer target is a predecessor without a reciprocal edge");
    }
    Transfers.push_back(Transfer{Successor, std::move(*PhiEdits), MergeExisting});
  }

  for (Transfer &TransferValue : Transfers) {
    MachineBasicBlock &Successor = *TransferValue.Successor.Block;
    auto OldPredecessor = std::ranges::find(Successor.Predecessors, &From);
    if (TransferValue.MergeExisting) {
      auto PhiResult = erasePhiIncoming(TransferValue.PhiEdits);
      if (!PhiResult)
        return std::unexpected(PhiResult.error());
      Successor.Predecessors.erase(OldPredecessor);
      const std::optional<std::size_t> Existing = findSuccessorIndex(To, Successor);
      if (!Existing) {
        return makeUnexpected(ErrorCode::InternalError,
                              "preflighted transfer target edge disappeared");
      }
      To.Successors[Existing.value()].Probability =
          To.Successors[Existing.value()].Probability.saturatingAdd(
              TransferValue.Successor.Probability);
    } else {
      auto PhiResult = rewritePhiIncoming(TransferValue.PhiEdits, To);
      if (!PhiResult)
        return std::unexpected(PhiResult.error());
      *OldPredecessor = &To;
      To.Successors.push_back(TransferValue.Successor);
    }
  }
  From.Successors.clear();
  return {};
}

Expected<void> MachineCFGUpdater::disconnectBlock(MachineBasicBlock &Block) {
  auto Ownership = verifyOwnership(Block);
  if (!Ownership)
    return std::unexpected(Ownership.error());

  auto Outgoing = validateOutgoingEdges(Block);
  if (!Outgoing)
    return std::unexpected(Outgoing.error());
  auto Incoming = validateIncomingEdges(Block);
  if (!Incoming)
    return std::unexpected(Incoming.error());

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

Expected<MachineBasicBlock *> MachineCFGUpdater::splitBlock(MachineBasicBlock &Block,
                                                            MachineInstr &SplitBefore) {
  auto Ownership = verifyOwnership(Block);
  if (!Ownership)
    return std::unexpected(Ownership.error());
  if (!Block.isLinked() || SplitBefore.parent() != &Block) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "split point is not linked to the requested basic block");
  }
  bool MovesPhi = false;
  for (const MachineInstr *Instruction = &SplitBefore; Instruction != nullptr;
       Instruction = Instruction->next()) {
    MovesPhi |= isKnownPhi(*Instruction);
  }
  if (MovesPhi) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "a basic block cannot be split inside its PHI region");
  }

  auto Outgoing = validateOutgoingEdges(Block);
  if (!Outgoing)
    return std::unexpected(Outgoing.error());

  auto NewBlockResult = Function->createBasicBlock();
  if (!NewBlockResult)
    return std::unexpected(NewBlockResult.error());
  MachineBasicBlock *NewBlock = *NewBlockResult;
  Expected<void> Inserted = Block.next() == nullptr
                                ? Function->appendBlock(*NewBlock)
                                : Function->insertBlockBefore(*Block.next(), *NewBlock);
  if (!Inserted)
    return std::unexpected(Inserted.error());

  MachineInstr *Instruction = &SplitBefore;
  while (Instruction != nullptr) {
    MachineInstr *Next = Instruction->next();
    auto Removed = Instruction->eraseFromParent();
    if (!Removed)
      return std::unexpected(Removed.error());
    auto Appended = NewBlock->appendInstruction(*Instruction);
    if (!Appended)
      return std::unexpected(Appended.error());
    Instruction = Next;
  }

  auto Transferred = transferSuccessors(Block, *NewBlock);
  if (!Transferred)
    return std::unexpected(Transferred.error());
  auto Connected = addSuccessor(Block, *NewBlock, BranchProbability::one());
  if (!Connected)
    return std::unexpected(Connected.error());
  return NewBlock;
}

Expected<void> MachineCFGUpdater::deleteBlock(MachineBasicBlock &Block) {
  auto Ownership = verifyOwnership(Block);
  if (!Ownership)
    return std::unexpected(Ownership.error());
  if (!Block.isLinked())
    return makeUnexpected(ErrorCode::InvalidArgument, "deleted CFG block is not linked");
  auto Disconnected = disconnectBlock(Block);
  if (!Disconnected)
    return std::unexpected(Disconnected.error());
  return Function->removeBlock(Block);
}

} // namespace rcc

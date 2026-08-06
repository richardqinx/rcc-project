// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFrameInfo.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace rcc {

Expected<StackID> StackID::fromValue(std::uint32_t Value) {
  StackID Result(Value);
  if (!Result.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "stack ID is not serializable in LLVM MIR");
  return Result;
}

std::optional<StackID> parseStackIDName(std::string_view Name) noexcept {
  if (Name == "default")
    return StackID::defaultStack();
  if (Name == "sgpr-spill")
    return StackID::sgprSpillStack();
  if (Name == "scalable-vector")
    return StackID::scalableVectorStack();
  if (Name == "scalable-predicate-vector")
    return StackID::scalablePredicateVectorStack();
  if (Name == "wasm-local")
    return StackID::wasmLocalStack();
  if (Name == "noalloc")
    return StackID::noAllocStack();
  return std::nullopt;
}

std::string_view getStackIDName(StackID ID) noexcept {
  if (ID == StackID::defaultStack())
    return "default";
  if (ID == StackID::sgprSpillStack())
    return "sgpr-spill";
  if (ID == StackID::scalableVectorStack())
    return "scalable-vector";
  if (ID == StackID::scalablePredicateVectorStack())
    return "scalable-predicate-vector";
  if (ID == StackID::wasmLocalStack())
    return "wasm-local";
  if (ID == StackID::noAllocStack())
    return "noalloc";
  return "unknown";
}

std::optional<MachineFramePointerPolicy>
parseMachineFramePointerPolicy(std::string_view Name) noexcept {
  if (Name == "none")
    return MachineFramePointerPolicy::None;
  if (Name == "non-leaf")
    return MachineFramePointerPolicy::NonLeaf;
  if (Name == "all")
    return MachineFramePointerPolicy::All;
  if (Name == "reserved")
    return MachineFramePointerPolicy::Reserved;
  if (Name == "non-leaf-no-reserve")
    return MachineFramePointerPolicy::NonLeafNoReserve;
  return std::nullopt;
}

std::string_view
getMachineFramePointerPolicyName(MachineFramePointerPolicy Policy) noexcept {
  switch (Policy) {
  case MachineFramePointerPolicy::None:
    return "none";
  case MachineFramePointerPolicy::NonLeaf:
    return "non-leaf";
  case MachineFramePointerPolicy::All:
    return "all";
  case MachineFramePointerPolicy::Reserved:
    return "reserved";
  case MachineFramePointerPolicy::NonLeafNoReserve:
    return "non-leaf-no-reserve";
  case MachineFramePointerPolicy::Count:
    return "unknown";
  }
  return "unknown";
}

bool StackObject::isSpill() const noexcept {
  const StackObjectDesc *Desc = description();
  if (Desc == nullptr)
    return false;
  return Desc->Kind == StackObjectKind::SpillSlot || Desc->Kind == StackObjectKind::CalleeSaved ||
         Desc->Kind == StackObjectKind::EmergencySpill ||
         Desc->Kind == StackObjectKind::FixedSpillSlot;
}

bool StackObject::isFixed() const noexcept {
  const StackObjectDesc *Desc = description();
  return Desc != nullptr && (Desc->Kind == StackObjectKind::FixedABI ||
                             Desc->Kind == StackObjectKind::FixedSpillSlot);
}

bool StackObject::isVariableSized() const noexcept {
  const StackObjectDesc *Desc = description();
  return Desc != nullptr && (Desc->Kind == StackObjectKind::VariableSized ||
                             Desc->Kind == StackObjectKind::DynamicAlloca);
}

Expected<void> MachineFrameInfo::verifyDescription(const StackObjectDesc &Description) {
  if (Description.Kind >= StackObjectKind::Count)
    return makeUnexpected(ErrorCode::InvalidArgument, "stack object kind is invalid");
  if (!Description.Stack.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "stack object has an invalid stack ID");

  const bool IsVariable = Description.Kind == StackObjectKind::VariableSized ||
                          Description.Kind == StackObjectKind::DynamicAlloca;
  if (IsVariable) {
    if (Description.Size || Description.Offset) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "variable-sized stack objects cannot have a fixed size or offset");
    }
  } else if (!Description.Size) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "fixed-size stack objects require an explicit size");
  }

  if ((Description.Kind == StackObjectKind::FixedABI ||
       Description.Kind == StackObjectKind::FixedSpillSlot) &&
      !Description.Offset) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "fixed ABI stack objects require a fixed offset");
  }
  if (Description.AllocaName && Description.AllocaName->empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "named stack objects require a nonempty alloca name");
  }
  if (Description.IsImmutable && Description.Kind != StackObjectKind::FixedABI) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "only fixed ABI stack objects may be immutable");
  }
  if (Description.CalleeSavedRegister && Description.CalleeSavedRegister->number() == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "callee-saved stack objects require a nonzero physical register");
  }
  if (Description.LocalOffset &&
      (Description.Kind == StackObjectKind::FixedABI ||
       Description.Kind == StackObjectKind::FixedSpillSlot)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "fixed ABI stack objects cannot have a local frame offset");
  }
  if (Description.Kind == StackObjectKind::Target && !Description.Stack.isSpecialStack() &&
      Description.TargetFlags == 0) {
    return makeUnexpected(
        ErrorCode::InvalidArgument,
        "target stack objects require a special stack ID or nonzero target flags");
  }

  std::unordered_set<MachineInstrStableID> LifetimeBoundaries;
  for (const StackObjectLifetimeRange &Range : Description.Lifetime) {
    if (!Range.Begin.isValid() || !Range.End.isValid() || Range.Begin == Range.End) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "stack object lifetime boundaries must be valid and distinct");
    }
    if (!LifetimeBoundaries.insert(Range.Begin).second ||
        !LifetimeBoundaries.insert(Range.End).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "stack object lifetime boundaries must not be reused");
    }
  }
  if (Description.DebugInfo) {
    auto DebugInfoResult = Description.DebugInfo->verifyStructure();
    if (!DebugInfoResult)
      return DebugInfoResult;
  }
  return {};
}

Expected<void> MachineFrameInfo::verifyStateStructure(const MachineFrameState &State) {
  if (State.FramePointerPolicy >= MachineFramePointerPolicy::Count) {
    return makeUnexpected(ErrorCode::InvalidArgument, "frame-pointer policy is invalid");
  }

  auto VerifyPoints = [](std::span<const MachineFrameSaveRestorePoint> Points,
                         std::string_view Kind) -> Expected<void> {
    std::unordered_set<MachineBlockStableID> Blocks;
    for (const MachineFrameSaveRestorePoint &Point : Points) {
      if (!Point.Block.isValid()) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              std::string(Kind) + " point requires a valid basic-block identity");
      }
      if (!Blocks.insert(Point.Block).second) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              std::string(Kind) + " points must use unique basic blocks");
      }
      std::unordered_set<PhysicalRegister> Registers;
      for (const PhysicalRegister Reg : Point.Registers) {
        if (Reg.number() == 0 || !Registers.insert(Reg).second) {
          return makeUnexpected(ErrorCode::InvalidArgument,
                                std::string(Kind) +
                                    " point registers must be nonzero and unique");
        }
      }
    }
    return {};
  };

  auto SaveResult = VerifyPoints(State.SavePoints, "save");
  if (!SaveResult)
    return SaveResult;
  return VerifyPoints(State.RestorePoints, "restore");
}

Expected<FrameIndex> MachineFrameInfo::createObject(StackObjectDesc Description) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "frozen frame cannot create stack objects");
  auto DescriptionResult = verifyDescription(Description);
  if (!DescriptionResult)
    return std::unexpected(DescriptionResult.error());
  if (Objects.size() > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::Unsupported, "stack object index space is exhausted");
  }

  const FrameIndex Index = FrameIndex::fromValue(static_cast<std::uint32_t>(Objects.size()));
  Objects.push_back(StackObject(Index, std::move(Description)));
  return Index;
}

Expected<StackObject *> MachineFrameInfo::mutableObject(FrameIndex Index) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "frozen frame cannot modify stack objects");
  if (Index.value() >= Objects.size())
    return makeUnexpected(ErrorCode::InvalidArgument, "stack object index is out of range");
  StackObject &Object = Objects[Index.value()];
  if (Object.isTombstone())
    return makeUnexpected(ErrorCode::InvalidArgument, "stack object index is a tombstone");
  return &Object;
}

Expected<void> MachineFrameInfo::updateObject(FrameIndex Index, StackObjectDesc Description) {
  auto DescriptionResult = verifyDescription(Description);
  if (!DescriptionResult)
    return std::unexpected(DescriptionResult.error());
  auto Object = mutableObject(Index);
  if (!Object)
    return std::unexpected(Object.error());
  (*Object)->Description.emplace<StackObjectDesc>(std::move(Description));
  return {};
}

Expected<void> MachineFrameInfo::setObjectOffset(FrameIndex Index, std::int64_t Offset) {
  auto Object = mutableObject(Index);
  if (!Object)
    return std::unexpected(Object.error());
  StackObjectDesc *CurrentDescription = std::get_if<StackObjectDesc>(&(*Object)->Description);
  if (CurrentDescription == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "live stack object unexpectedly lacks a description");
  }
  StackObjectDesc Description = *CurrentDescription;
  if ((*Object)->isVariableSized()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "variable-sized stack object cannot have a fixed offset");
  }
  Description.Offset = Offset;
  (*Object)->Description.emplace<StackObjectDesc>(std::move(Description));
  return {};
}

Expected<void> MachineFrameInfo::markObjectDead(FrameIndex Index) {
  auto Object = mutableObject(Index);
  if (!Object)
    return std::unexpected(Object.error());
  StackObjectDesc *Description = std::get_if<StackObjectDesc>(&(*Object)->Description);
  if (Description == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "live stack object unexpectedly lacks a description");
  }
  Description->IsDead = true;
  return {};
}

Expected<void> MachineFrameInfo::eraseObject(FrameIndex Index) {
  auto Object = mutableObject(Index);
  if (!Object)
    return std::unexpected(Object.error());
  (*Object)->Description.emplace<std::monostate>();
  return {};
}

const StackObject *MachineFrameInfo::object(FrameIndex Index) const noexcept {
  if (Index.value() >= Objects.size() || Objects[Index.value()].isTombstone())
    return nullptr;
  return &Objects[Index.value()];
}

std::size_t MachineFrameInfo::liveObjectCount() const noexcept {
  std::size_t Count = 0;
  for (const StackObject &Object : Objects) {
    if (!Object.isTombstone())
      ++Count;
  }
  return Count;
}

Expected<void> MachineFrameInfo::setMaxCallFrameSize(std::uint64_t Size) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "frozen frame cannot change its maximum call-frame size");
  State.MaxCallFrameSize = Size;
  return {};
}

Expected<void> MachineFrameInfo::clearMaxCallFrameSize() {
  if (Frozen) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "frozen frame cannot change its maximum call-frame size");
  }
  State.MaxCallFrameSize.reset();
  return {};
}

Expected<void> MachineFrameInfo::setState(MachineFrameState NewState) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "frozen frame cannot replace its state");
  auto Verification = verifyStateStructure(NewState);
  if (!Verification)
    return std::unexpected(Verification.error());
  State = std::move(NewState);
  return {};
}

Expected<void> MachineFrameInfo::freeze(std::uint64_t Size, Alignment Align) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "frame layout is already frozen");
  for (const StackObject &Object : Objects) {
    if (Object.isTombstone() || Object.isVariableSized())
      continue;
    if (!Object.description()->Offset) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "fixed-size stack object is missing its final offset");
    }
  }
  if (Size % Align.bytes() != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "static stack size must be aligned to the stack alignment");
  }

  State.StackSize = Size;
  State.MaxAlignment = Align;
  Frozen = true;
  return {};
}

Expected<void> MachineFrameInfo::verify() const {
  auto StateResult = verifyStateStructure(State);
  if (!StateResult)
    return StateResult;

  for (std::size_t Position = 0; Position < Objects.size(); ++Position) {
    const StackObject &Object = Objects[Position];
    if (Object.index() != FrameIndex::fromValue(static_cast<std::uint32_t>(Position))) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "stack object index does not match its stable table position");
    }
    if (Object.isTombstone())
      continue;
    auto DescriptionResult = verifyDescription(*Object.description());
    if (!DescriptionResult)
      return std::unexpected(DescriptionResult.error());
    if (Frozen && !Object.isVariableSized() && !Object.description()->Offset) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "frozen fixed-size stack object has no final offset");
    }
  }

  for (const std::optional<FrameIndex> Index : {State.StackProtector, State.FunctionContext}) {
    if (Index && object(*Index) == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "frame state references an unknown stack object");
    }
  }
  if (Frozen && !State.MaxAlignment) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "frozen frame is missing its maximum alignment");
  }
  if (Frozen && State.StackSize % State.MaxAlignment->bytes() != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "frozen static stack size violates stack alignment");
  }
  return {};
}

} // namespace rcc

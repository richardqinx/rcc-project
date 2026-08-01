// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunction.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <utility>
#include <vector>

namespace rcc {

class MachineArena final {
public:
  MachineArena() = default;
  MachineArena(const MachineArena &) = delete;
  MachineArena &operator=(const MachineArena &) = delete;
  MachineArena(MachineArena &&) = delete;
  MachineArena &operator=(MachineArena &&) = delete;

  ~MachineArena() {
    for (auto Position = Destructors.rbegin(); Position != Destructors.rend(); ++Position)
      Position->Destroy(Position->Object);
  }

  template <typename ObjectT, typename... ArgumentTs>
  [[nodiscard]] ObjectT *create(ArgumentTs &&...Arguments) {
    void *Storage = Resource.allocate(sizeof(ObjectT), alignof(ObjectT));
    ObjectT *Object = ::new (Storage) ObjectT(std::forward<ArgumentTs>(Arguments)...);
    auto DestroyObject = [](ObjectT *TypedObject) noexcept { std::destroy_at(TypedObject); };
    std::unique_ptr<ObjectT, decltype(DestroyObject)> ConstructionGuard(Object, DestroyObject);
    Destructors.push_back(DestructorRecord{
        Object,
        [](void *RawObject) noexcept { std::destroy_at(static_cast<ObjectT *>(RawObject)); },
    });
    return ConstructionGuard.release();
  }

private:
  struct DestructorRecord final {
    void *Object;
    void (*Destroy)(void *) noexcept;
  };

  std::pmr::monotonic_buffer_resource Resource;
  std::vector<DestructorRecord> Destructors;
};

Expected<std::unique_ptr<MachineFunction>>
MachineFunction::create(std::string Name, MachineFunctionStableID StableID) {
  if (Name.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "machine function name cannot be empty");
  if (!StableID.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "machine function StableID must be valid");
  return std::unique_ptr<MachineFunction>(new MachineFunction(std::move(Name), StableID));
}

MachineFunction::MachineFunction(std::string Name, MachineFunctionStableID StableID)
    : Name(std::move(Name)), StableID(StableID), JumpTables(*this),
      Arena(std::make_unique<MachineArena>()) {}

MachineFunction::~MachineFunction() {
  for (MachineBasicBlock *Block = FirstBlock; Block != nullptr; Block = Block->NextBlock) {
    Block->clearInstructions();
    Block->Predecessors.clear();
    Block->Successors.clear();
  }
}

Expected<void> MachineFunction::setTargetState(TargetFunctionState State) {
  if (TargetState.hasValue()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "machine function already has target state");
  }
  auto Verification = State.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  TargetState = std::move(State);
  return {};
}

Expected<MachineBasicBlock *> MachineFunction::createBasicBlock() {
  auto ID = BlockIDGenerator.allocate();
  if (!ID)
    return std::unexpected(ID.error());
  MachineBasicBlock *Block = Arena->create<MachineBasicBlock>(*this, *ID);
  AllBlocks.push_back(Block);
  return Block;
}

Expected<MachineInstr *> MachineFunction::createInstruction(Opcode InstructionOpcode) {
  if (!InstructionOpcode.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "machine opcode must be nonzero");

  auto ID = InstrIDGenerator.allocate();
  if (!ID)
    return std::unexpected(ID.error());
  MachineInstr *Instruction = Arena->create<MachineInstr>(*this, InstructionOpcode, *ID);
  AllInstructions.push_back(Instruction);
  return Instruction;
}

Expected<const MachineMemOperand *>
MachineFunction::createMemOperand(MachineMemOperandDesc Description) {
  auto MemOperand = MachineMemOperand::create(std::move(Description));
  if (!MemOperand)
    return std::unexpected(MemOperand.error());

  const MachineMemOperand *Stored = Arena->create<MachineMemOperand>(std::move(*MemOperand));
  AllMemOperands.push_back(Stored);
  return Stored;
}

Expected<void> MachineFunction::appendBlock(MachineBasicBlock &Block) {
  if (Block.OwnerFunction != this)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "basic block belongs to another machine function");
  if (Block.IsLinked)
    return makeUnexpected(ErrorCode::InvalidArgument, "basic block is already linked");

  Block.PreviousBlock = LastBlock;
  Block.NextBlock = nullptr;
  if (LastBlock != nullptr)
    LastBlock->NextBlock = &Block;
  else
    FirstBlock = &Block;
  LastBlock = &Block;
  Block.IsLinked = true;
  renumberBlocks();
  return {};
}

Expected<void> MachineFunction::insertBlockBefore(MachineBasicBlock &Position,
                                                  MachineBasicBlock &Block) {
  if (Position.OwnerFunction != this || !Position.IsLinked)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "block insertion position is not linked to this function");
  if (Block.OwnerFunction != this || Block.IsLinked)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "inserted block is linked or belongs to another function");

  Block.PreviousBlock = Position.PreviousBlock;
  Block.NextBlock = &Position;
  if (Position.PreviousBlock != nullptr)
    Position.PreviousBlock->NextBlock = &Block;
  else
    FirstBlock = &Block;
  Position.PreviousBlock = &Block;
  Block.IsLinked = true;
  renumberBlocks();
  return {};
}

Expected<void> MachineFunction::removeBlock(MachineBasicBlock &Block) {
  if (Block.OwnerFunction != this || !Block.IsLinked)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "removed block is not linked to this function");
  if (!Block.Predecessors.empty() || !Block.Successors.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "disconnect CFG edges before removing a basic block");
  }

  Block.clearInstructions();
  if (Block.PreviousBlock != nullptr)
    Block.PreviousBlock->NextBlock = Block.NextBlock;
  else
    FirstBlock = Block.NextBlock;
  if (Block.NextBlock != nullptr)
    Block.NextBlock->PreviousBlock = Block.PreviousBlock;
  else
    LastBlock = Block.PreviousBlock;

  Block.IsLinked = false;
  Block.PreviousBlock = nullptr;
  Block.NextBlock = nullptr;
  Block.DenseNumber.reset();
  Block.LayoutIndex.reset();
  renumberBlocks();
  return {};
}

std::size_t MachineFunction::blockCount() const noexcept {
  std::size_t Count = 0;
  for (const MachineBasicBlock *Block = FirstBlock; Block != nullptr; Block = Block->next()) {
    ++Count;
  }
  return Count;
}

bool MachineFunction::ownsMemOperand(const MachineMemOperand &MemOperand) const noexcept {
  return std::ranges::find(AllMemOperands, &MemOperand) != AllMemOperands.end();
}

MachineCFGUpdater MachineFunction::cfgUpdater() noexcept {
  return MachineCFGUpdater(*this);
}

void MachineFunction::renumberBlocks() noexcept {
  std::uint32_t Number = 0;
  for (MachineBasicBlock *Block = FirstBlock; Block != nullptr; Block = Block->NextBlock) {
    Block->DenseNumber = Number;
    Block->LayoutIndex = Number;
    ++Number;
  }
}

} // namespace rcc

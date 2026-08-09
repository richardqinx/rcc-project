// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineMCContext.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/GlobalObject.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rcc {

std::size_t MachineMCContext::FunctionIndexHash::operator()(FunctionIndexKey Key) const noexcept {
  const std::size_t FunctionHash = std::hash<const MachineFunction *>{}(Key.Function);
  const std::size_t IndexHash = std::hash<std::uint32_t>{}(Key.Index);
  return FunctionHash ^ (IndexHash + 0x9e3779b9U + (FunctionHash << 6U) + (FunctionHash >> 2U));
}

Expected<void> MachineMCContext::initialize(const CompilationUnit &Unit) {
  auto Verification = Unit.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  for (std::size_t Index = 0; Index < Unit.globalObjectCount(); ++Index) {
    const GlobalObjectDesc *Object = Unit.globalObjectAt(Index);
    if (Object == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "compilation unit contains a null global");
    auto Symbol = Context->getOrCreateSymbol(Object->Symbol, mc::MCSymbolKind::Object);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    GlobalSymbols.emplace(Object, std::move(*Symbol));
  }
  for (std::size_t Index = 0; Index < Unit.functionCount(); ++Index) {
    const MachineFunction *Function = Unit.functionAt(Index);
    if (Function == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "compilation unit contains a null function");
    auto Initialized = initializeFunction(*Function);
    if (!Initialized)
      return std::unexpected(Initialized.error());
  }
  return {};
}

Expected<void> MachineMCContext::initializeFunction(const MachineFunction &Function) {
  if (FunctionSymbols.contains(&Function))
    return {};

  auto FunctionSymbol =
      Context->getOrCreateSymbol(std::string(Function.name()), mc::MCSymbolKind::Function);
  if (!FunctionSymbol)
    return std::unexpected(FunctionSymbol.error());
  FunctionSymbols.emplace(&Function, *FunctionSymbol);

  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    const std::optional<std::size_t> DenseNumber = Block->denseNumber();
    if (!Block->isLinked() || !DenseNumber) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "MC symbol initialization requires linked and numbered blocks");
    }
    const std::string Name =
        ".LBB" + std::to_string(Function.stableID().value()) + "_" + std::to_string(*DenseNumber);
    auto Symbol = Context->getOrCreateSymbol(Name, mc::MCSymbolKind::BasicBlock);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    BlockSymbols.emplace(Block, *Symbol);
  }

  for (const MachineConstantPoolEntry &Entry : Function.constantPool().entries()) {
    if (Entry.isTombstone())
      continue;
    const std::string Name = ".LCPI" + std::to_string(Function.stableID().value()) + "_" +
                             std::to_string(Entry.index().value());
    auto Symbol = Context->getOrCreateSymbol(Name, mc::MCSymbolKind::ConstantPool);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    ConstantPoolSymbols.emplace(
        FunctionIndexKey{.Function = &Function, .Index = Entry.index().value()}, *Symbol);
  }

  for (const MachineJumpTableEntry &Entry : Function.jumpTableInfo().tables()) {
    if (Entry.isTombstone() || Entry.isPending())
      continue;
    const std::string Name = ".LJTI" + std::to_string(Function.stableID().value()) + "_" +
                             std::to_string(Entry.index().value());
    auto Symbol = Context->getOrCreateSymbol(Name, mc::MCSymbolKind::JumpTable);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    JumpTableSymbols.emplace(
        FunctionIndexKey{.Function = &Function, .Index = Entry.index().value()}, *Symbol);
  }
  return {};
}

mc::MCSymbolRef MachineMCContext::functionSymbol(const MachineFunction &Function) const noexcept {
  const auto Position = FunctionSymbols.find(&Function);
  return Position == FunctionSymbols.end() ? mc::MCSymbolRef{} : Position->second;
}

mc::MCSymbolRef MachineMCContext::globalSymbol(const GlobalObjectDesc &Object) const noexcept {
  const auto Position = GlobalSymbols.find(&Object);
  return Position == GlobalSymbols.end() ? mc::MCSymbolRef{} : Position->second;
}

mc::MCSymbolRef MachineMCContext::blockSymbol(const MachineBasicBlock &Block) const noexcept {
  const auto Position = BlockSymbols.find(&Block);
  return Position == BlockSymbols.end() ? mc::MCSymbolRef{} : Position->second;
}

mc::MCSymbolRef MachineMCContext::constantPoolSymbol(const MachineFunction &Function,
                                                     ConstantPoolIndex Index) const noexcept {
  const auto Position =
      ConstantPoolSymbols.find(FunctionIndexKey{.Function = &Function, .Index = Index.value()});
  return Position == ConstantPoolSymbols.end() ? mc::MCSymbolRef{} : Position->second;
}

mc::MCSymbolRef MachineMCContext::jumpTableSymbol(const MachineFunction &Function,
                                                  JumpTableIndex Index) const noexcept {
  const auto Position =
      JumpTableSymbols.find(FunctionIndexKey{.Function = &Function, .Index = Index.value()});
  return Position == JumpTableSymbols.end() ? mc::MCSymbolRef{} : Position->second;
}

Expected<mc::MCSymbolRef> MachineMCContext::symbol(std::string Name, mc::MCSymbolKind Kind) {
  return Context->getOrCreateSymbol(std::move(Name), Kind);
}

Expected<mc::MCSymbolRef> MachineMCContext::blockAddressSymbol(std::string_view FunctionName,
                                                               std::string_view BlockName) const {
  for (const auto &[Block, Symbol] : BlockSymbols) {
    if (Block == nullptr || Block->function().name() != FunctionName)
      continue;
    const auto IRName = Block->attributes().irBlockName();
    const auto MachineName = Block->attributes().name();
    const bool NumericMatch =
        Block->denseNumber() && std::to_string(*Block->denseNumber()) == BlockName;
    if ((IRName && *IRName == BlockName) || (MachineName && *MachineName == BlockName) ||
        NumericMatch)
      return Symbol;
  }
  return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                        "block-address operand does not resolve to an emitted block symbol");
}

} // namespace rcc

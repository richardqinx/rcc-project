// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEMCCONTEXT_H
#define RCC_CODEGEN_MACHINEMCCONTEXT_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCSymbol.h"
#include "rcc/Support/Error.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace rcc {

class CompilationUnit;
struct GlobalObjectDesc;
class MachineBasicBlock;
class MachineFunction;

class MachineMCContext final {
public:
  explicit MachineMCContext(mc::MCContext &Context) noexcept : Context(&Context) {}

  [[nodiscard]] Expected<void> initialize(const CompilationUnit &Unit);
  [[nodiscard]] Expected<void> initializeFunction(const MachineFunction &Function);

  [[nodiscard]] mc::MCSymbolRef functionSymbol(const MachineFunction &Function) const noexcept;
  [[nodiscard]] mc::MCSymbolRef globalSymbol(const GlobalObjectDesc &Object) const noexcept;
  [[nodiscard]] mc::MCSymbolRef blockSymbol(const MachineBasicBlock &Block) const noexcept;
  [[nodiscard]] mc::MCSymbolRef constantPoolSymbol(const MachineFunction &Function,
                                                   ConstantPoolIndex Index) const noexcept;
  [[nodiscard]] mc::MCSymbolRef jumpTableSymbol(const MachineFunction &Function,
                                                JumpTableIndex Index) const noexcept;

  [[nodiscard]] Expected<mc::MCSymbolRef> symbol(std::string Name,
                                                 mc::MCSymbolKind Kind = mc::MCSymbolKind::Generic);
  [[nodiscard]] Expected<mc::MCSymbolRef> blockAddressSymbol(std::string_view FunctionName,
                                                             std::string_view BlockName) const;

  [[nodiscard]] mc::MCContext &context() const noexcept {
    return *Context;
  }

private:
  struct FunctionIndexKey final {
    const MachineFunction *Function = nullptr;
    std::uint32_t Index = 0;

    friend bool operator==(FunctionIndexKey Left, FunctionIndexKey Right) noexcept = default;
  };

  struct FunctionIndexHash final {
    [[nodiscard]] std::size_t operator()(FunctionIndexKey Key) const noexcept;
  };

  mc::MCContext *Context;
  std::unordered_map<const MachineFunction *, mc::MCSymbolRef> FunctionSymbols;
  std::unordered_map<const GlobalObjectDesc *, mc::MCSymbolRef> GlobalSymbols;
  std::unordered_map<const MachineBasicBlock *, mc::MCSymbolRef> BlockSymbols;
  std::unordered_map<FunctionIndexKey, mc::MCSymbolRef, FunctionIndexHash> ConstantPoolSymbols;
  std::unordered_map<FunctionIndexKey, mc::MCSymbolRef, FunctionIndexHash> JumpTableSymbols;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEMCCONTEXT_H

// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_COMPILATIONUNIT_H
#define RCC_CODEGEN_COMPILATIONUNIT_H

#include "rcc/CodeGen/GlobalObject.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineFunction;

class CompilationUnit final {
public:
  CompilationUnit();
  CompilationUnit(const CompilationUnit &) = delete;
  CompilationUnit &operator=(const CompilationUnit &) = delete;
  CompilationUnit(CompilationUnit &&) noexcept;
  CompilationUnit &operator=(CompilationUnit &&) noexcept;
  ~CompilationUnit();

  [[nodiscard]] Expected<MachineFunction *> createMachineFunction(std::string Name);
  [[nodiscard]] Expected<GlobalObjectDesc *> addGlobalObject(GlobalObjectDesc Object);

  [[nodiscard]] std::size_t functionCount() const noexcept {
    return Functions.size();
  }

  [[nodiscard]] MachineFunction *functionAt(std::size_t Index) noexcept;
  [[nodiscard]] const MachineFunction *functionAt(std::size_t Index) const noexcept;
  [[nodiscard]] MachineFunction *function(std::string_view Name) noexcept;
  [[nodiscard]] const MachineFunction *function(std::string_view Name) const noexcept;
  [[nodiscard]] MachineFunction *function(MachineFunctionStableID ID) noexcept;
  [[nodiscard]] const MachineFunction *function(MachineFunctionStableID ID) const noexcept;

  [[nodiscard]] std::size_t globalObjectCount() const noexcept {
    return GlobalObjects.size();
  }

  [[nodiscard]] GlobalObjectDesc *globalObjectAt(std::size_t Index) noexcept;
  [[nodiscard]] const GlobalObjectDesc *globalObjectAt(std::size_t Index) const noexcept;
  [[nodiscard]] GlobalObjectDesc *globalObject(std::string_view Name) noexcept;
  [[nodiscard]] const GlobalObjectDesc *globalObject(std::string_view Name) const noexcept;

  [[nodiscard]] Expected<void> verify() const;

private:
  struct StringHash final {
    using is_transparent = void; // NOLINT(readability-identifier-naming)

    [[nodiscard]] std::size_t operator()(std::string_view Value) const noexcept {
      return std::hash<std::string_view>{}(Value);
    }
  };

  std::vector<std::unique_ptr<MachineFunction>> Functions;
  std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> FunctionNameToIndex;
  std::vector<std::unique_ptr<GlobalObjectDesc>> GlobalObjects;
  std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> GlobalNameToIndex;
  StableIDGenerator<MachineFunctionStableID> FunctionIDGenerator;
};

} // namespace rcc

#endif // RCC_CODEGEN_COMPILATIONUNIT_H

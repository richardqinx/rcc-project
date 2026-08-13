// SPDX-License-Identifier: GPL-3.0-or-later

#include "Common/CodeGenTarget.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace rcc::targetgen {

[[nodiscard]] bool CodeGenTarget::normalizeRegisterUnits() {
  if (Input.RegisterUnits.empty()) {
    fail(Input.Target.Range, "target description requires at least one register unit");
    return false;
  }
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceRegisterUnit &Unit : Input.RegisterUnits) {
    if (Unit.ID == 0 || !isIdentifier(Unit.Symbol) || Unit.Name.empty() ||
        !IDs.insert(Unit.ID).second || !Symbols.insert(Unit.Symbol).second ||
        !Names.insert(lowerASCII(Unit.Name)).second) {
      fail(Unit.Range,
           "register units require nonzero unique IDs, unique symbols, and unique names");
      return false;
    }
    UnitBySymbol.emplace(Unit.Symbol, Unit.ID);
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeRegisters(NormalizedTargetModel &Result) {
  if (Input.Registers.empty()) {
    fail(Input.Target.Range, "target description requires at least one physical register");
    return false;
  }
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceRegister &Register : Input.Registers) {
    if (Register.ID == 0 || !isIdentifier(Register.Symbol) || Register.Name.empty() ||
        !IDs.insert(Register.ID).second || !Symbols.insert(Register.Symbol).second ||
        !Names.insert(lowerASCII(Register.Name)).second || Register.Units.empty()) {
      fail(Register.Range,
           "registers require nonzero unique IDs, symbols, canonical names, and units");
      return false;
    }
    RegisterBySymbol.emplace(Register.Symbol, Register.ID);
    NormalizedRegister Normalized{
        .ID = Register.ID,
        .Symbol = Register.Symbol,
        .Name = Register.Name,
        .Encoding = Register.Encoding,
        .DwarfRegister = Register.DwarfRegister,
        .Units = {},
        .IsConstant = Register.IsConstant,
        .IsArtificial = Register.IsArtificial,
        .IsReserved = Register.IsReserved,
        .AlternativeNames = Register.AlternativeNames,
    };
    std::unordered_set<std::uint32_t> Units;
    for (const SourceRegisterUnitLane &Unit : Register.Units) {
      const auto Position = UnitBySymbol.find(Unit.UnitSymbol);
      if (Position == UnitBySymbol.end() || Unit.Lanes == 0 ||
          !Units.insert(Position->second).second) {
        fail(Unit.Range,
             "register units must name distinct declared units with nonzero lane masks");
        return false;
      }
      Normalized.Units.push_back({.UnitID = Position->second, .Lanes = Unit.Lanes});
    }
    for (const std::string &Alias : Register.AlternativeNames) {
      if (Alias.empty() || !Names.insert(lowerASCII(Alias)).second) {
        fail(Register.Range,
             "register ABI aliases must be nonempty and unique across all register names");
        return false;
      }
    }
    Result.Registers.push_back(std::move(Normalized));
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeRegisterBanks(NormalizedTargetModel &Result) {
  if (Input.RegisterBanks.empty()) {
    fail(Input.Target.Range, "target description requires at least one register bank");
    return false;
  }
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceRegisterBank &Bank : Input.RegisterBanks) {
    if (Bank.ID == 0 || !isIdentifier(Bank.Symbol) || Bank.Name.empty() ||
        Bank.MaximumSizeInBits == 0 || !IDs.insert(Bank.ID).second ||
        !Symbols.insert(Bank.Symbol).second || !Names.insert(lowerASCII(Bank.Name)).second) {
      fail(Bank.Range,
           "register banks require nonzero unique IDs, symbols, names, and maximum sizes");
      return false;
    }
    BankBySymbol.emplace(Bank.Symbol, Bank.ID);
    Result.RegisterBanks.push_back({
        .ID = Bank.ID,
        .Symbol = Bank.Symbol,
        .Name = Bank.Name,
        .MaximumSizeInBits = Bank.MaximumSizeInBits,
        .RegisterClasses = {},
    });
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeRegisterClasses(NormalizedTargetModel &Result) {
  if (Input.RegisterClasses.empty()) {
    fail(Input.Target.Range, "target description requires at least one register class");
    return false;
  }
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceRegisterClass &Class : Input.RegisterClasses) {
    const auto Bank = BankBySymbol.find(Class.BankSymbol);
    if (Class.ID == 0 || !isIdentifier(Class.Symbol) || Class.Name.empty() ||
        Bank == BankBySymbol.end() || Class.SizeInBits == 0 || Class.SpillSizeInBits == 0 ||
        Class.SpillAlignmentInBits == 0 || Class.Lanes == 0 || Class.RegisterSymbols.empty() ||
        !IDs.insert(Class.ID).second || !Symbols.insert(Class.Symbol).second ||
        !Names.insert(lowerASCII(Class.Name)).second) {
      fail(Class.Range, "register classes require unique IDs, symbols, names, a declared bank, "
                        "sizes, lanes, and members");
      return false;
    }
    ClassBySymbol.emplace(Class.Symbol, Class.ID);
    NormalizedRegisterClass Normalized{
        .ID = Class.ID,
        .Symbol = Class.Symbol,
        .Name = Class.Name,
        .BankID = Bank->second,
        .SizeInBits = Class.SizeInBits,
        .SpillSizeInBits = Class.SpillSizeInBits,
        .SpillAlignmentInBits = Class.SpillAlignmentInBits,
        .Lanes = Class.Lanes,
        .Registers = {},
        .AllocationOrder = {},
        .Allocatable = Class.Allocatable,
        .CopyCost = Class.CopyCost,
    };
    std::unordered_set<std::uint32_t> Members;
    for (const std::string &Symbol : Class.RegisterSymbols) {
      const auto Register = RegisterBySymbol.find(Symbol);
      if (Register == RegisterBySymbol.end() || !Members.insert(Register->second).second) {
        fail(Class.Range, "register class members must reference distinct declared registers");
        return false;
      }
      Normalized.Registers.push_back(Register->second);
    }
    std::unordered_set<std::uint32_t> Allocation;
    for (const std::string &Symbol : Class.AllocationOrderSymbols) {
      const auto Register = RegisterBySymbol.find(Symbol);
      if (Register == RegisterBySymbol.end() || !Members.contains(Register->second) ||
          !Allocation.insert(Register->second).second) {
        fail(Class.Range,
             "register class allocation order must contain distinct members of the same class");
        return false;
      }
      Normalized.AllocationOrder.push_back(Register->second);
    }
    if ((Normalized.Allocatable && Normalized.AllocationOrder.empty()) ||
        (!Normalized.Allocatable && !Normalized.AllocationOrder.empty())) {
      fail(Class.Range, "allocatable register classes require an allocation order and "
                        "nonallocatable classes forbid one");
      return false;
    }
    Result.RegisterClasses.push_back(std::move(Normalized));
  }
  for (NormalizedRegisterBank &Bank : Result.RegisterBanks) {
    for (const NormalizedRegisterClass &Class : Result.RegisterClasses) {
      if (Class.BankID == Bank.ID)
        Bank.RegisterClasses.push_back(Class.ID);
    }
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeRegisterMasks(NormalizedTargetModel &Result) {
  std::unordered_set<std::string> Names;
  for (const SourceRegisterMask &Mask : Input.RegisterMasks) {
    if (Mask.Name.empty() || !Names.insert(Mask.Name).second) {
      fail(Mask.Range, "register mask names must be nonempty and unique");
      return false;
    }
    NormalizedRegisterMask Normalized{.Name = Mask.Name, .PreservedRegisters = {}};
    std::unordered_set<std::uint32_t> Preserved;
    for (const std::string &Symbol : Mask.PreservedRegisterSymbols) {
      const auto Register = RegisterBySymbol.find(Symbol);
      if (Register == RegisterBySymbol.end() || !Preserved.insert(Register->second).second) {
        fail(Mask.Range, "register mask entries must name distinct declared registers");
        return false;
      }
      Normalized.PreservedRegisters.push_back(Register->second);
    }
    Result.RegisterMasks.push_back(std::move(Normalized));
  }
  return true;
}

} // namespace rcc::targetgen

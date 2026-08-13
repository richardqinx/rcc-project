// SPDX-License-Identifier: GPL-3.0-or-later

#include "Common/CodeGenTarget.h"

#include <ranges>
#include <string>
#include <unordered_set>

namespace rcc::targetgen {

[[nodiscard]] bool CodeGenTarget::normalizeSchedulingClasses(NormalizedTargetModel &Result) {
  if (Input.SchedulingClasses.empty()) {
    fail(Input.Target.Range, "target description requires at least one scheduling class");
    return false;
  }
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceSchedulingClass &Class : Input.SchedulingClasses) {
    if (Class.ID == 0 || !isIdentifier(Class.Symbol) || Class.Name.empty() || Class.MicroOps == 0 ||
        Class.Latency == 0 || !IDs.insert(Class.ID).second ||
        !Symbols.insert(Class.Symbol).second || !Names.insert(Class.Name).second) {
      fail(Class.Range,
           "scheduling classes require nonzero unique IDs, symbols, names, micro-op counts, "
           "and latencies");
      return false;
    }
    SchedulingClassBySymbol.emplace(Class.Symbol, Class.ID);
    Result.SchedulingClasses.push_back({
        .ID = Class.ID,
        .Symbol = Class.Symbol,
        .Name = Class.Name,
        .MicroOps = Class.MicroOps,
        .Latency = Class.Latency,
    });
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeEncodingCategories(NormalizedTargetModel &Result) {
  if (Input.EncodingCategories.empty()) {
    fail(Input.Target.Range, "target description requires at least one encoding category");
    return false;
  }
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceEncodingCategory &Category : Input.EncodingCategories) {
    if (Category.ID == 0 || !isIdentifier(Category.Symbol) || Category.Name.empty() ||
        Category.SizeInBytes == 0 || !IDs.insert(Category.ID).second ||
        !Symbols.insert(Category.Symbol).second || !Names.insert(Category.Name).second) {
      fail(Category.Range,
           "encoding categories require nonzero unique IDs, symbols, names, and sizes");
      return false;
    }
    EncodingCategoryBySymbol.emplace(Category.Symbol, Category.ID);
    Result.EncodingCategories.push_back({
        .ID = Category.ID,
        .Symbol = Category.Symbol,
        .Name = Category.Name,
        .SizeInBytes = Category.SizeInBytes,
    });
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeAssemblyFormats(NormalizedTargetModel &Result) {
  if (Input.AssemblyFormats.empty()) {
    fail(Input.Target.Range, "target description requires at least one assembly format");
    return false;
  }
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceAssemblyFormat &Format : Input.AssemblyFormats) {
    if (Format.ID == 0 || !isIdentifier(Format.Symbol) || Format.Name.empty() ||
        !IDs.insert(Format.ID).second || !Symbols.insert(Format.Symbol).second ||
        !Names.insert(Format.Name).second) {
      fail(Format.Range, "assembly formats require nonzero unique IDs, symbols, and names");
      return false;
    }
    AssemblyFormatBySymbol.emplace(Format.Symbol, Format.ID);
    Result.AssemblyFormats.push_back({
        .ID = Format.ID,
        .Symbol = Format.Symbol,
        .Name = Format.Name,
    });
  }
  return true;
}

} // namespace rcc::targetgen

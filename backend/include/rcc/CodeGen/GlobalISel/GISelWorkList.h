// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_GISELWORKLIST_H
#define RCC_CODEGEN_GLOBALISEL_GISELWORKLIST_H

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineInstr;

template <std::size_t InlineCapacity> class GISelWorkList final {
public:
  GISelWorkList() {
    Worklist.reserve(InlineCapacity);
    WorklistMap.reserve(InlineCapacity);
  }

  [[nodiscard]] bool empty() const noexcept {
    return WorklistMap.empty();
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return WorklistMap.size();
  }

  void deferredInsert(MachineInstr *Instruction) {
    Worklist.push_back(Instruction);
  }

  void finalize() {
    WorklistMap.reserve(Worklist.size());
    for (std::size_t Index = 0; Index < Worklist.size(); ++Index)
      WorklistMap.emplace(Worklist[Index], Index);
  }

  void insert(MachineInstr *Instruction) {
    const auto [Position, Inserted] = WorklistMap.emplace(Instruction, Worklist.size());
    if (Inserted)
      Worklist.push_back(Instruction);
  }

  void remove(const MachineInstr *Instruction) {
    const auto Position = WorklistMap.find(Instruction);
    if (Position == WorklistMap.end())
      return;
    Worklist[Position->second] = nullptr;
    WorklistMap.erase(Position);
  }

  void clear() {
    Worklist.clear();
    WorklistMap.clear();
  }

  [[nodiscard]] MachineInstr *popBack() {
    MachineInstr *Instruction = nullptr;
    do {
      Instruction = Worklist.back();
      Worklist.pop_back();
    } while (Instruction == nullptr);
    WorklistMap.erase(Instruction);
    return Instruction;
  }

private:
  std::vector<MachineInstr *> Worklist;
  std::unordered_map<const MachineInstr *, std::size_t> WorklistMap;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_GISELWORKLIST_H

// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/GISelChangeObserver.h"

#include <algorithm>

namespace rcc {

void GISelObserverWrapper::addObserver(GISelChangeObserver &Observer) {
  Observers.push_back(&Observer);
}

void GISelObserverWrapper::removeObserver(GISelChangeObserver &Observer) {
  const auto Position = std::ranges::find(Observers, &Observer);
  if (Position != Observers.end())
    Observers.erase(Position);
}

void GISelObserverWrapper::clearObservers() noexcept {
  Observers.clear();
}

void GISelObserverWrapper::erasingInstr(MachineInstr &Instruction) {
  for (GISelChangeObserver *Observer : Observers)
    Observer->erasingInstr(Instruction);
}

void GISelObserverWrapper::createdInstr(MachineInstr &Instruction) {
  for (GISelChangeObserver *Observer : Observers)
    Observer->createdInstr(Instruction);
}

void GISelObserverWrapper::changingInstr(MachineInstr &Instruction) {
  for (GISelChangeObserver *Observer : Observers)
    Observer->changingInstr(Instruction);
}

void GISelObserverWrapper::changedInstr(MachineInstr &Instruction) {
  for (GISelChangeObserver *Observer : Observers)
    Observer->changedInstr(Instruction);
}

void GISelObserverWrapper::handleInsertion(MachineInstr &Instruction) {
  createdInstr(Instruction);
}

void GISelObserverWrapper::handleRemoval(MachineInstr &Instruction) {
  erasingInstr(Instruction);
}

MachineFunctionDelegateInstaller::MachineFunctionDelegateInstaller(
    MachineFunction &Function, MachineFunction::Delegate &Delegate)
    : Function(&Function), Delegate(&Delegate) {
  Function.setDelegate(Delegate);
}

MachineFunctionDelegateInstaller::~MachineFunctionDelegateInstaller() {
  Function->resetDelegate(*Delegate);
}

} // namespace rcc

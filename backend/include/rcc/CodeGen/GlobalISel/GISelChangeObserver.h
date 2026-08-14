// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_GISELCHANGEOBSERVER_H
#define RCC_CODEGEN_GLOBALISEL_GISELCHANGEOBSERVER_H

#include "rcc/CodeGen/MachineFunction.h"

#include <span>
#include <vector>

namespace rcc {

class MachineInstr;

class GISelChangeObserver {
public:
  virtual ~GISelChangeObserver() = default;

  virtual void erasingInstr(MachineInstr &Instruction) = 0;
  virtual void createdInstr(MachineInstr &Instruction) = 0;
  virtual void changingInstr(MachineInstr &Instruction) = 0;
  virtual void changedInstr(MachineInstr &Instruction) = 0;
};

class GISelObserverWrapper final : public MachineFunction::Delegate, public GISelChangeObserver {
public:
  GISelObserverWrapper() = default;
  explicit GISelObserverWrapper(GISelChangeObserver &Observer) {
    Observers.push_back(&Observer);
  }
  explicit GISelObserverWrapper(std::span<GISelChangeObserver *const> InitialObservers)
      : Observers(InitialObservers.begin(), InitialObservers.end()) {}

  void addObserver(GISelChangeObserver &Observer);
  void removeObserver(GISelChangeObserver &Observer);
  void clearObservers() noexcept;

  void erasingInstr(MachineInstr &Instruction) override;
  void createdInstr(MachineInstr &Instruction) override;
  void changingInstr(MachineInstr &Instruction) override;
  void changedInstr(MachineInstr &Instruction) override;

  void handleInsertion(MachineInstr &Instruction) override;
  void handleRemoval(MachineInstr &Instruction) override;

private:
  std::vector<GISelChangeObserver *> Observers;
};

class MachineFunctionDelegateInstaller final {
public:
  MachineFunctionDelegateInstaller(MachineFunction &Function, MachineFunction::Delegate &Delegate);
  MachineFunctionDelegateInstaller(const MachineFunctionDelegateInstaller &) = delete;
  MachineFunctionDelegateInstaller &operator=(const MachineFunctionDelegateInstaller &) = delete;
  ~MachineFunctionDelegateInstaller();

private:
  MachineFunction *Function;
  MachineFunction::Delegate *Delegate;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_GISELCHANGEOBSERVER_H

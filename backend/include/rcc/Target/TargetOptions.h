// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_TARGETOPTIONS_H
#define RCC_TARGET_TARGETOPTIONS_H

#include "rcc/CodeGen/CommandFlags.h"

#include <string>
#include <vector>

namespace rcc {

struct TargetOption final {
  std::string Name;
  std::string Value;

  friend bool operator==(const TargetOption &, const TargetOption &) = default;
};

struct TargetMachineOptions final {
  std::string Triple;
  std::string CPU;
  std::string TuneCPU;
  std::vector<std::string> Features;
  std::string ABI;
  std::string DataLayout;
  RelocationModel Relocation = RelocationModel::Static;
  CodeModel Code = CodeModel::Small;
  OptimizationLevel Optimization = OptimizationLevel::O2;
  std::vector<TargetOption> Options;
  std::string ABILoweringVersion;
  std::string AssemblerFlavor;
  std::string AssemblerVersion;
  std::vector<TargetOption> AssemblerOptions;

  friend bool operator==(const TargetMachineOptions &, const TargetMachineOptions &) = default;
};

struct TargetPipelineOptions final {
  OptimizationLevel Optimization = OptimizationLevel::O2;
  bool UseTargetCopyInstructions = false;
  GlobalISelAbortMode GlobalISelAbort = GlobalISelAbortMode::Enable;
  bool DebugMachineScheduler = false;
  bool DebugPrologueEpilogue = false;
};

} // namespace rcc

#endif // RCC_TARGET_TARGETOPTIONS_H

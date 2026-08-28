// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/TargetRegistry.h"
#include "rcc/Target/RISCV/RISCVAssemblyPrinter.h"
#include "rcc/Target/RISCV/RISCVObjectFile.h"
#include "rcc/Target/RISCV/RISCVPipeline.h"
#include "rcc/Target/RISCV/RISCVTarget.h"
#include "rcc/Target/TargetMachine.h"

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {
namespace {

constexpr std::string_view RISCV64Triple = "riscv64-unknown-linux-gnu";
constexpr std::string_view RISCV64CPU = "generic-rv64gc";
constexpr std::string_view RISCV64DataLayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128";
constexpr std::string_view RISCV64ABILoweringVersion = "rcc-rv64gc-lp64d-v1";
constexpr std::array<std::string_view, 1> RISCV64BaselineFeatures{"+i"};

[[nodiscard]] unsigned matchRISCV64Triple(std::string_view Triple) noexcept {
  if (Triple == "riscv64" || Triple == "riscv64-linux-gnu" || Triple == RISCV64Triple)
    return 20;
  return Triple.starts_with("riscv64-") ? 10U : 0U;
}

[[nodiscard]] std::string normalizeRISCV64Triple(std::string_view Triple) {
  if (Triple == "riscv64" || Triple == "riscv64-linux-gnu")
    return std::string(RISCV64Triple);
  return std::string(Triple);
}

[[nodiscard]] std::string defaultRISCV64ABI(std::span<const std::string> Features) {
  return std::ranges::find(Features, "+d") == Features.end() ? "lp64" : "lp64d";
}

[[nodiscard]] TargetMachineOptions defaultRISCV64Options() {
  return TargetMachineOptions{
      .Triple = std::string(RISCV64Triple),
      .CPU = std::string(RISCV64CPU),
      .TuneCPU = std::string(RISCV64CPU),
      .Features = {"+i", "+m", "+a", "+f", "+d", "+c", "+zicsr", "+zifencei"},
      .ABI = "lp64d",
      .DataLayout = std::string(RISCV64DataLayout),
      .Relocation = RelocationModel::Static,
      .Code = CodeModel::Small,
      .Optimization = OptimizationLevel::O2,
      .Options = {{"function-sections", "false"},
                  {"omit-frame-pointer", "true"},
                  {"stack-realignment", "false"}},
      .ABILoweringVersion = std::string(RISCV64ABILoweringVersion),
      .AssemblerFlavor = "rcc-integrated-gnu",
      .AssemblerVersion = "rcc-mc-v1",
      .AssemblerOptions = {{"relax", "true"}},
  };
}

[[nodiscard]] bool booleanOption(std::span<const TargetOption> Options,
                                 std::string_view Name) noexcept {
  const auto Position = std::ranges::find(Options, Name, &TargetOption::Name);
  return Position != Options.end() && Position->Value == "true";
}

class RISCVTargetMachine final : public TargetMachine {
public:
  RISCVTargetMachine(const Target &RegisteredTarget, TargetMachineOptions Options,
                     TargetDescription Description, riscv::RISCVSubtargetFeatures Features,
                     RelocationModel Relocation, CodeModel Code, riscv::RISCVABI ABI)
      : TargetMachine(RegisteredTarget, std::move(Options), std::move(Description)),
        Subtarget(Features, Relocation, Code, ABI) {}

  [[nodiscard]] Expected<void>
  addCodeGenPasses(MachineFunctionPassManager &Pipeline, const MachineFunction &Function,
                   TargetPipelineOptions PipelineOptions) const override {
    return riscv::addCanonicalPipeline(Pipeline, Function, Subtarget, PipelineOptions);
  }

  [[nodiscard]] Expected<std::unique_ptr<MachineFunctionPass>>
  createMachinePass(std::string_view Name, TargetPipelineOptions PipelineOptions) const override {
    return riscv::createCanonicalPipelinePass(Name, Subtarget, PipelineOptions);
  }

  [[nodiscard]] Expected<std::vector<std::string>>
  codeGenPassNames(const MachineFunction &Function) const override {
    return riscv::canonicalPipelinePassNames(Function, Subtarget);
  }

  [[nodiscard]] Expected<std::unique_ptr<TargetMachineOutlinerInfo>>
  createOutlinerInfo() const override {
    return riscv::createMachineOutlinerInfo(
        booleanOption(options().Options, "function-sections"),
        booleanOption(options().Options, "riscv-outliner-regsave"));
  }

  [[nodiscard]] Expected<std::string> emitAssembly(const CompilationUnit &Unit) const override {
    return riscv::emitAssembly(Unit, targetDescription());
  }

  [[nodiscard]] Expected<std::vector<std::uint8_t>>
  emitObject(const CompilationUnit &Unit) const override {
    return riscv::emitObject(Unit, targetDescription());
  }

private:
  riscv::RISCVSubtarget Subtarget;
};

[[nodiscard]] Expected<std::unique_ptr<TargetMachine>>
createRISCVTargetMachine(const Target &RegisteredTarget, TargetMachineOptions Options) {
  Options.Triple = RegisteredTarget.normalizeTriple(Options.Triple);
  const TargetMachineOptions Defaults = defaultRISCV64Options();
  if (Options.Triple != Defaults.Triple || Options.CPU != Defaults.CPU ||
      Options.TuneCPU != Defaults.TuneCPU || Options.DataLayout != Defaults.DataLayout) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "backend invocation does not select a supported RV64 target");
  }
  if (Options.ABILoweringVersion != RISCV64ABILoweringVersion) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "RISC-V ABI lowering version does not match this build");
  }
  auto Features = riscv::RISCVSubtargetFeatures::fromFeatureList(Options.Features);
  if (!Features)
    return std::unexpected(Features.error());
  auto ABI = riscv::parseRISCVABI(Options.ABI);
  if (!ABI)
    return std::unexpected(ABI.error());
  if (*ABI == riscv::RISCVABI::LP64D && !Features->has(riscv::RISCVExtension::D)) {
    return makeUnexpected(ErrorCode::InvalidInvocation, "RISC-V LP64D requires the D extension");
  }
  auto Description = riscv::createTargetDescription();
  if (!Description)
    return std::unexpected(Description.error());
  const RelocationModel Relocation = Options.Relocation;
  const CodeModel Code = Options.Code;
  return std::make_unique<RISCVTargetMachine>(RegisteredTarget, std::move(Options),
                                              std::move(*Description), *Features, Relocation, Code,
                                              *ABI);
}

const Target &getRISCV64Target() {
  static const Target RegisteredTarget(
      "riscv64", "64-bit RISC-V", true, matchRISCV64Triple, normalizeRISCV64Triple,
      defaultRISCV64Options, defaultRISCV64ABI, RISCV64BaselineFeatures, createRISCVTargetMachine);
  return RegisteredTarget;
}

} // namespace

void initializeRISCVTarget() {
  TargetRegistry::registerTarget(getRISCV64Target());
}

} // namespace rcc

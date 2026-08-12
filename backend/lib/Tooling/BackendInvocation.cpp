// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/BackendInvocation.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/Support/SHA256.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace rcc::tooling {
namespace {

void appendField(std::string &Output, std::string_view Name, std::string_view Value) {
  Output.append(Name);
  Output.push_back('=');
  Output.append(std::to_string(Value.size()));
  Output.push_back(':');
  Output.append(Value);
  Output.push_back('\n');
}

template <typename IntegerT>
void appendInteger(std::string &Output, std::string_view Name, IntegerT Value) {
  appendField(Output, Name, std::to_string(Value));
}

void appendBoolean(std::string &Output, std::string_view Name, bool Value) {
  appendField(Output, Name, Value ? "true" : "false");
}

[[nodiscard]] bool isValidName(std::string_view Value) noexcept {
  return !Value.empty() && std::ranges::all_of(Value, [](unsigned char Character) {
    return std::isalnum(Character) != 0 || Character == '-' || Character == '_' || Character == '.';
  });
}

[[nodiscard]] Expected<void> canonicalizeOptions(std::vector<BackendOption> &Options,
                                                 std::string_view Category) {
  std::ranges::sort(Options, {}, &BackendOption::Name);
  for (std::size_t Index = 0; Index < Options.size(); ++Index) {
    if (!isValidName(Options[Index].Name)) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            std::string(Category) + " option has an invalid name");
    }
    if (Index != 0 && Options[Index - 1].Name == Options[Index].Name) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            std::string(Category) + " option names must be unique");
    }
  }
  return {};
}

[[nodiscard]] Expected<void> canonicalizeFeatures(std::vector<std::string> &Features) {
  for (std::string &Feature : Features) {
    if (Feature.empty())
      return makeUnexpected(ErrorCode::InvalidInvocation, "target feature cannot be empty");
    if (Feature.front() != '+' && Feature.front() != '-')
      Feature.insert(Feature.begin(), '+');
    if (!isValidName(std::string_view(Feature).substr(1))) {
      return makeUnexpected(ErrorCode::InvalidInvocation, "target feature has an invalid spelling");
    }
  }
  std::ranges::sort(Features, [](const std::string &Left, const std::string &Right) {
    return std::string_view(Left).substr(1) < std::string_view(Right).substr(1);
  });
  for (std::size_t Index = 1; Index < Features.size(); ++Index) {
    if (std::string_view(Features[Index - 1]).substr(1) ==
        std::string_view(Features[Index]).substr(1)) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "target feature names must be unique and non-conflicting");
    }
  }
  return {};
}

[[nodiscard]] bool isSHA256(std::string_view Value) noexcept {
  return Value.size() == 64 && std::ranges::all_of(Value, [](unsigned char Character) {
           return std::isxdigit(Character) != 0 &&
                  (std::isdigit(Character) != 0 || std::islower(Character) != 0);
         });
}

void appendOperandDescriptor(std::string &Output, const OperandDescriptor &Operand) {
  appendInteger(Output, "operand.kinds", Operand.Kinds.rawValue());
  appendInteger(Output, "operand.role", static_cast<unsigned>(Operand.RegisterRole));
  appendInteger(Output, "operand.reg-class", Operand.RegClass ? Operand.RegClass->value() : 0U);
  appendInteger(Output, "operand.reg-bank", Operand.RegBank ? Operand.RegBank->value() : 0U);
  appendInteger(Output, "operand.immediate-predicate",
                Operand.ImmediatePredicate ? Operand.ImmediatePredicate->value() : 0U);
  appendInteger(Output, "operand.memory-role", static_cast<unsigned>(Operand.MemoryRole));
  appendInteger(Output, "operand.tie",
                Operand.DefaultTiedOperand ? *Operand.DefaultTiedOperand + 1U : 0U);
  appendBoolean(Output, "operand.early-clobber", Operand.EarlyClobber);
  appendBoolean(Output, "operand.optional", Operand.Optional);
  appendBoolean(Output, "operand.variadic-group", Operand.VariadicGroup);
  appendBoolean(Output, "operand.allow-subreg", Operand.AllowSubReg);
  appendBoolean(Output, "operand.allow-any-target-flags", Operand.AllowAnyTargetFlags);
  appendInteger(Output, "operand.named", Operand.Name ? Operand.Name->value() : 0U);
  appendInteger(Output, "operand.target-type",
                Operand.TargetOperandType ? Operand.TargetOperandType->value() : 0U);
  appendInteger(Output, "operand.target-flags", Operand.AllowedTargetFlags);
  appendInteger(Output, "operand.type-kinds",
                Operand.TypeConstraint ? Operand.TypeConstraint->Kinds.rawValue() : 0U);
  appendInteger(Output, "operand.type-equality",
                Operand.TypeConstraint && Operand.TypeConstraint->EqualityGroup
                    ? static_cast<std::uint32_t>(*Operand.TypeConstraint->EqualityGroup) + 1U
                    : 0U);
}

void appendInstructionSet(std::string &Output, const InstructionSet &Instructions) {
  appendInteger(Output, "instructions.count", Instructions.descriptors().size());
  for (const InstructionDescriptor &Descriptor : Instructions.descriptors()) {
    appendInteger(Output, "instruction.opcode", Descriptor.InstructionOpcode.value());
    appendField(Output, "instruction.name", Descriptor.Name);
    appendInteger(Output, "instruction.explicit-defs", Descriptor.NumExplicitDefs);
    appendBoolean(Output, "instruction.variadic", Descriptor.Variadic);
    appendInteger(Output, "instruction.minimum-variadic", Descriptor.MinimumVariadicRepetitions);
    appendBoolean(Output, "instruction.extra-implicit", Descriptor.AllowExtraImplicitOperands);
    appendInteger(Output, "instruction.flags", Descriptor.Flags.rawValue());
    appendInteger(Output, "instruction.scheduling-class", Descriptor.SchedulingClass.value());
    appendInteger(Output, "instruction.encoding-category", Descriptor.EncodingCategory.value());
    appendInteger(Output, "instruction.assembly-format", Descriptor.AssemblyFormat.value());
    appendBoolean(Output, "instruction.memory-operands", Descriptor.RequiresMemoryOperands);
    appendInteger(Output, "instruction.pseudo-owner", static_cast<unsigned>(Descriptor.Owner));
    appendInteger(Output, "instruction.deadline", static_cast<unsigned>(Descriptor.Deadline));
    appendBoolean(Output, "instruction.explicit-verifier",
                  Descriptor.VerifyExplicitOperands != nullptr);
    appendBoolean(Output, "instruction.semantic-verifier",
                  Descriptor.VerifyInstructionSemantics != nullptr);
    appendInteger(Output, "instruction.operands", Descriptor.Operands.size());
    for (const OperandDescriptor &Operand : Descriptor.Operands)
      appendOperandDescriptor(Output, Operand);
    appendInteger(Output, "instruction.implicit-defs", Descriptor.ImplicitDefs.size());
    for (const ImplicitRegisterDescriptor &Implicit : Descriptor.ImplicitDefs) {
      appendInteger(Output, "implicit.register", Implicit.Reg.number());
      appendBoolean(Output, "implicit.early-clobber", Implicit.EarlyClobber);
      appendBoolean(Output, "implicit.renamable", Implicit.Renamable);
    }
    appendInteger(Output, "instruction.implicit-uses", Descriptor.ImplicitUses.size());
    for (const ImplicitRegisterDescriptor &Implicit : Descriptor.ImplicitUses) {
      appendInteger(Output, "implicit.register", Implicit.Reg.number());
      appendBoolean(Output, "implicit.early-clobber", Implicit.EarlyClobber);
      appendBoolean(Output, "implicit.renamable", Implicit.Renamable);
    }
  }
}

void appendRegisterInfo(std::string &Output, const TargetRegisterInfo &Registers,
                        const TargetMIRTextInfo &MIRText) {
  appendInteger(Output, "register-units.count", Registers.registerUnits().size());
  for (const RegisterUnitDesc &Unit : Registers.registerUnits()) {
    appendInteger(Output, "register-unit.id", Unit.ID.value());
    appendField(Output, "register-unit.name", Unit.Name);
  }
  appendInteger(Output, "registers.count", Registers.physicalRegisters().size());
  for (const PhysicalRegisterDesc &Register : Registers.physicalRegisters()) {
    appendInteger(Output, "register.id", Register.Reg.number());
    appendField(Output, "register.name", Register.Name);
    appendInteger(Output, "register.encoding", Register.Encoding);
    appendBoolean(Output, "register.constant", Register.IsConstant);
    appendBoolean(Output, "register.artificial", Register.IsArtificial);
    appendBoolean(Output, "register.reserved", Register.IsReserved);
    for (const RegisterUnitLane &Unit : Register.Units) {
      appendInteger(Output, "register.unit", Unit.Unit.value());
      appendInteger(Output, "register.lanes", Unit.Lanes.rawValue());
    }
    for (const std::string &Name : Register.AlternativeNames)
      appendField(Output, "register.alternative-name", Name);
    appendInteger(
        Output, "register.dwarf",
        MIRText.dwarfRegister(Register.Reg).value_or(std::numeric_limits<std::uint32_t>::max()));
  }
  appendInteger(Output, "subregister-indices.count", Registers.subRegIndices().size());
  for (const SubRegIndexDesc &Index : Registers.subRegIndices()) {
    appendInteger(Output, "subregister-index.id", Index.ID.value());
    appendField(Output, "subregister-index.name", Index.Name);
    appendInteger(Output, "subregister-index.lanes", Index.Lanes.rawValue());
    appendInteger(Output, "subregister-index.size", Index.SizeInBits);
    appendInteger(Output, "subregister-index.offset", Index.OffsetInBits);
  }
  appendInteger(Output, "register-classes.count", Registers.registerClasses().size());
  for (const RegisterClassDesc &Class : Registers.registerClasses()) {
    appendInteger(Output, "register-class.id", Class.ID.value());
    appendField(Output, "register-class.name", Class.Name);
    appendInteger(Output, "register-class.size", Class.SizeInBits);
    appendInteger(Output, "register-class.spill-size", Class.SpillSizeInBits);
    appendInteger(Output, "register-class.spill-alignment", Class.SpillAlignmentInBits);
    appendInteger(Output, "register-class.lanes", Class.Lanes.rawValue());
    appendInteger(Output, "register-class.bank", Class.DefaultBank.value());
    appendBoolean(Output, "register-class.allocatable", Class.Allocatable);
    appendInteger(Output, "register-class.copy-cost", Class.CopyCost);
    appendBoolean(Output, "register-class.covered-by-subregs", Class.CoveredBySubRegs);
    for (PhysicalRegister Register : Class.Registers)
      appendInteger(Output, "register-class.register", Register.number());
    for (PhysicalRegister Register : Class.AllocationOrder)
      appendInteger(Output, "register-class.allocation-order", Register.number());
    for (SubRegIndex Index : Class.SupportedSubRegs)
      appendInteger(Output, "register-class.subregister", Index.value());
    for (RegisterClassID SuperClass : Class.SuperClasses)
      appendInteger(Output, "register-class.superclass", SuperClass.value());
  }
  appendInteger(Output, "register-banks.count", Registers.registerBanks().size());
  for (const RegisterBankDesc &Bank : Registers.registerBanks()) {
    appendInteger(Output, "register-bank.id", Bank.ID.value());
    appendField(Output, "register-bank.name", Bank.Name);
    appendInteger(Output, "register-bank.maximum-size", Bank.MaximumSizeInBits);
    for (RegisterClassID Class : Bank.RegisterClasses)
      appendInteger(Output, "register-bank.class", Class.value());
  }
}

void appendMIRTextInfo(std::string &Output, const TargetMIRTextInfo &MIRText) {
  appendInteger(Output, "mir.direct-flag-mask", MIRText.directOperandFlagMask());
  for (const MIRRegisterMaskDesc &Mask : MIRText.registerMasks()) {
    appendField(Output, "mir.register-mask", Mask.Name);
    if (Mask.Mask != nullptr) {
      for (std::uint64_t Word : Mask.Mask->words())
        appendInteger(Output, "mir.register-mask-word", Word);
    }
  }
  for (const MIRTargetIndexDesc &Index : MIRText.targetIndices()) {
    appendField(Output, "mir.target-index-name", Index.Name);
    appendInteger(Output, "mir.target-index", Index.Index.value());
  }
  for (const MIROperandFlagDesc &Flag : MIRText.directOperandFlags()) {
    appendField(Output, "mir.direct-flag-name", Flag.Name);
    appendInteger(Output, "mir.direct-flag", Flag.Value);
  }
  for (const MIROperandFlagDesc &Flag : MIRText.bitmaskOperandFlags()) {
    appendField(Output, "mir.bitmask-flag-name", Flag.Name);
    appendInteger(Output, "mir.bitmask-flag", Flag.Value);
  }
  for (const MIRMachineMemOperandFlagDesc &Flag : MIRText.machineMemOperandFlags()) {
    appendField(Output, "mir.mmo-flag-name", Flag.Name);
    appendInteger(Output, "mir.mmo-flag", Flag.Value);
  }
  for (const MIRVirtualRegisterFlagDesc &Flag : MIRText.virtualRegisterFlags()) {
    appendField(Output, "mir.vreg-flag-name", Flag.Name);
    appendInteger(Output, "mir.vreg-flag", Flag.Value);
  }
  for (const MIRIntrinsicDesc &Intrinsic : MIRText.intrinsics()) {
    appendField(Output, "mir.intrinsic-name", Intrinsic.Name);
    appendInteger(Output, "mir.intrinsic", Intrinsic.ID.value());
  }
  for (StackID ID : MIRText.supportedStackIDs())
    appendInteger(Output, "mir.stack-id", ID.value());
  appendInteger(Output, "mir.function-state-type",
                MIRText.functionStateType() ? MIRText.functionStateType()->value() : 0U);
}

} // namespace

std::string_view spelling(LayoutOwnership Value) noexcept {
  switch (Value) {
  case LayoutOwnership::NativeObject:
    return "native-object";
  case LayoutOwnership::CompilerFinalLayout:
    return "compiler-final-layout";
  case LayoutOwnership::ExternalRelaxation:
    return "external-relaxation";
  }
  return "invalid";
}

Expected<LayoutOwnership> parseLayoutOwnership(std::string_view Value) {
  if (Value == "native-object")
    return LayoutOwnership::NativeObject;
  if (Value == "compiler-final-layout")
    return LayoutOwnership::CompilerFinalLayout;
  if (Value == "external-relaxation")
    return LayoutOwnership::ExternalRelaxation;
  return makeUnexpected(ErrorCode::ManifestError, "unknown layout ownership");
}

Expected<void> BackendInvocation::canonicalizeAndVerify() {
  if (SchemaVersion != BackendInvocationSchemaVersion) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "backend invocation schema version is unsupported");
  }
  if (Triple.empty() || CPU.empty() || TuneCPU.empty() || ABI.empty() || DataLayout.empty() ||
      MIRReferenceRevision.empty() || GenericOpcodeVersion.empty() ||
      TargetABILoweringVersion.empty() || BuildFingerprint.empty()) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "backend invocation is missing a required target or version fact");
  }
  if (Determinism.ThreadCount == 0) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "backend invocation thread count must be nonzero");
  }
  auto FeatureResult = canonicalizeFeatures(Features);
  if (!FeatureResult)
    return FeatureResult;
  auto TargetOptionResult = canonicalizeOptions(TargetOptions, "target");
  if (!TargetOptionResult)
    return TargetOptionResult;
  auto AssemblerOptionResult = canonicalizeOptions(AssemblerOptions, "assembler");
  if (!AssemblerOptionResult)
    return AssemblerOptionResult;
  if (PassPipeline.empty() ||
      std::ranges::any_of(PassPipeline, [](const std::string &Pass) { return Pass.empty(); })) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "backend invocation pass pipeline cannot be empty");
  }
  if (Layout == LayoutOwnership::ExternalRelaxation &&
      (AssemblerFlavor.empty() || AssemblerVersion.empty())) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "external relaxation requires explicit assembler flavor and version");
  }
  if (AssemblerFlavor.empty() != AssemblerVersion.empty()) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "assembler flavor and version must be specified together");
  }
  std::ranges::sort(EnabledActions);
  if (std::ranges::any_of(EnabledActions,
                          [](const std::string &Action) { return !isValidName(Action); }) ||
      std::ranges::adjacent_find(EnabledActions) != EnabledActions.end()) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "enabled action names must be valid and unique");
  }
  return {};
}

std::string BackendInvocation::canonicalText() const {
  std::string Output;
  appendInteger(Output, "schema", SchemaVersion);
  appendField(Output, "triple", Triple);
  appendField(Output, "cpu", CPU);
  appendField(Output, "tune-cpu", TuneCPU);
  for (const std::string &Feature : Features)
    appendField(Output, "feature", Feature);
  appendField(Output, "abi", ABI);
  appendField(Output, "data-layout", DataLayout);
  appendField(Output, "relocation-model", spelling(Relocation));
  appendField(Output, "code-model", spelling(Code));
  for (const BackendOption &Option : TargetOptions) {
    appendField(Output, "target-option-name", Option.Name);
    appendField(Output, "target-option-value", Option.Value);
  }
  appendField(Output, "layout-ownership", spelling(Layout));
  appendField(Output, "assembler-flavor", AssemblerFlavor);
  appendField(Output, "assembler-version", AssemblerVersion);
  for (const BackendOption &Option : AssemblerOptions) {
    appendField(Output, "assembler-option-name", Option.Name);
    appendField(Output, "assembler-option-value", Option.Value);
  }
  appendField(Output, "optimization-level", spelling(Optimization));
  for (const std::string &Pass : PassPipeline)
    appendField(Output, "pass", Pass);
  appendInteger(Output, "random-seed", Determinism.RandomSeed);
  appendInteger(Output, "thread-count", Determinism.ThreadCount);
  appendBoolean(Output, "stable-scheduling", Determinism.StableScheduling);
  appendField(Output, "action-fuel", ActionFuel ? std::to_string(*ActionFuel) : "unlimited");
  for (const std::string &Action : EnabledActions)
    appendField(Output, "enabled-action", Action);
  appendField(Output, "mir-reference-revision", MIRReferenceRevision);
  appendField(Output, "generic-opcode-version", GenericOpcodeVersion);
  appendField(Output, "target-abi-lowering-version", TargetABILoweringVersion);
  appendField(Output, "build-fingerprint", BuildFingerprint);
  return Output;
}

BackendInvocation defaultRV64GCInvocation() {
  BackendInvocation Result;
  Result.Triple = "riscv64-unknown-linux-gnu";
  Result.CPU = "generic-rv64gc";
  Result.TuneCPU = "generic-rv64gc";
  Result.Features = {"+i", "+m", "+a", "+f", "+d", "+c", "+zicsr", "+zifencei"};
  Result.ABI = "lp64d";
  Result.DataLayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128";
  Result.TargetOptions = {
      {"function-sections", "false"},
      {"omit-frame-pointer", "true"},
      {"stack-realignment", "false"},
  };
  Result.AssemblerFlavor = "rcc-integrated-gnu";
  Result.AssemblerVersion = "rcc-mc-v1";
  Result.AssemblerOptions = {{"relax", "true"}};
  auto Verified = Result.canonicalizeAndVerify();
  (void)Verified;
  return Result;
}

Expected<std::string> computeTargetDescriptionFingerprint(const TargetDescription &Target) {
  std::string Canonical;
  appendField(Canonical, "fingerprint-schema", "rcc-target-fingerprint-v1");
  appendInstructionSet(Canonical, Target.instructions());
  appendRegisterInfo(Canonical, Target.registers(), Target.mirText());
  appendMIRTextInfo(Canonical, Target.mirText());
  return sha256Hex(Canonical);
}

Expected<std::string> computeCacheKey(std::string_view CanonicalMIR,
                                      const BackendInvocation &Invocation,
                                      std::string_view TargetFingerprint) {
  BackendInvocation CanonicalInvocation = Invocation;
  auto Verified = CanonicalInvocation.canonicalizeAndVerify();
  if (!Verified)
    return std::unexpected(Verified.error());
  if (!isSHA256(TargetFingerprint)) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "target description fingerprint must be a lowercase SHA-256 digest");
  }
  std::string Input;
  appendField(Input, "cache-key-schema", "rcc-backend-cache-key-v1");
  appendField(Input, "input-mir-semantic-hash", sha256Hex(CanonicalMIR));
  appendField(Input, "target-description-fingerprint", TargetFingerprint);
  appendField(Input, "invocation", CanonicalInvocation.canonicalText());
  return sha256Hex(Input);
}

} // namespace rcc::tooling

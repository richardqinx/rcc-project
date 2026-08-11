// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/MIRFuzzer.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/MIR/MIRDocument.h"
#include "rcc/Target/RISCV/RISCVPipeline.h"
#include "rcc/Tooling/ArtifactStore.h"
#include "rcc/Tooling/BackendDriver.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc::tooling {
namespace {

class DeterministicRandom final {
public:
  explicit DeterministicRandom(std::uint64_t Seed) noexcept
      : State(Seed == 0 ? 0x243f6a8885a308d3ULL : Seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    State ^= State >> 12U;
    State ^= State << 25U;
    State ^= State >> 27U;
    return State * 0x2545f4914f6cdd1dULL;
  }

  [[nodiscard]] std::size_t index(std::size_t Bound) noexcept {
    return Bound == 0 ? 0 : static_cast<std::size_t>(next() % Bound);
  }

private:
  std::uint64_t State;
};

[[nodiscard]] std::vector<MIRFuzzLayer> defaultLayers() {
  return {
      MIRFuzzLayer::Parser,   MIRFuzzLayer::BuilderStructural, MIRFuzzLayer::PropertyStage,
      MIRFuzzLayer::Mutation, MIRFuzzLayer::Pipeline,          MIRFuzzLayer::RestrictedSemantics,
  };
}

[[nodiscard]] Error fuzzFailure(MIRFuzzLayer Layer, std::size_t Iteration, const Error &Failure) {
  return Error(ErrorCode::FuzzingFailure,
               std::string(spelling(Layer)) + " fuzz case " + std::to_string(Iteration) +
                   " violated an invariant: " + std::string(Failure.message()));
}

[[nodiscard]] std::string byteMutation(std::string_view Input, DeterministicRandom &Random,
                                       std::size_t MaximumSize) {
  std::string Result(Input);
  switch (Random.index(5)) {
  case 0:
    if (!Result.empty())
      Result[Random.index(Result.size())] = static_cast<char>(Random.next() & 0xffU);
    break;
  case 1:
    if (!Result.empty())
      Result.erase(Random.index(Result.size()), 1);
    break;
  case 2:
    if (Result.size() < MaximumSize)
      Result.insert(Result.begin() + static_cast<std::ptrdiff_t>(Random.index(Result.size() + 1U)),
                    static_cast<char>(Random.next() & 0xffU));
    break;
  case 3:
    if (!Result.empty())
      Result.resize(Random.index(Result.size()));
    break;
  case 4:
    if (!Result.empty() && Result.size() < MaximumSize) {
      const std::size_t Begin = Random.index(Result.size());
      const std::size_t Length = std::min(Random.index(32U) + 1U, Result.size() - Begin);
      const std::string Fragment = Result.substr(Begin, Length);
      const std::size_t Available = MaximumSize - Result.size();
      Result.append(Fragment.substr(0, Available));
    }
    break;
  default:
    break;
  }
  return Result;
}

[[nodiscard]] std::string structuralTextMutation(std::string_view Input,
                                                 DeterministicRandom &Random) {
  std::string Result(Input);
  std::vector<std::size_t> LineStarts{0};
  for (std::size_t Position = 0; Position < Result.size(); ++Position) {
    if (Result[Position] == '\n' && Position + 1U < Result.size())
      LineStarts.push_back(Position + 1U);
  }
  if (LineStarts.empty())
    return Result;
  const std::size_t LineIndex = Random.index(LineStarts.size());
  const std::size_t Begin = LineStarts[LineIndex];
  const std::size_t End = Result.find('\n', Begin);
  const std::size_t LineEnd = End == std::string::npos ? Result.size() : End + 1U;
  switch (Random.index(4)) {
  case 0:
    Result.erase(Begin, LineEnd - Begin);
    break;
  case 1:
    Result.insert(LineEnd, Result.substr(Begin, LineEnd - Begin));
    break;
  case 2: {
    const std::size_t Digit = Result.find_first_of("0123456789", Begin);
    if (Digit < LineEnd)
      Result[Digit] = static_cast<char>('0' + Random.index(10));
    break;
  }
  case 3: {
    const std::size_t Percent = Result.find('%', Begin);
    if (Percent < LineEnd && Percent + 1U < Result.size())
      Result[Percent + 1U] = static_cast<char>('0' + Random.index(10));
    break;
  }
  default:
    break;
  }
  return Result;
}

[[nodiscard]] Expected<void> verifyCanonicalRoundTrip(std::string Text,
                                                      const TargetDescription &Target,
                                                      const BackendInvocation &Invocation) {
  auto First = parseAndCanonicalizeMIR("fuzz-round-trip.mir", std::move(Text), Target, Invocation);
  if (!First)
    return std::unexpected(First.error());
  auto Second = parseAndCanonicalizeMIR("fuzz-round-trip-2.mir", First->Text, Target, Invocation);
  if (!Second)
    return std::unexpected(Second.error());
  if (First->Text != Second->Text) {
    return makeUnexpected(ErrorCode::FuzzingFailure,
                          "successful MIR round-trip is not structurally deterministic");
  }
  return {};
}

[[nodiscard]] Expected<bool> parserCase(std::string_view Canonical, DeterministicRandom &Random,
                                        const TargetDescription &Target,
                                        const BackendInvocation &Invocation,
                                        std::size_t MaximumSize) {
  std::string Mutated = byteMutation(Canonical, Random, MaximumSize);
  auto Parsed = parseAndCanonicalizeMIR("parser-fuzz.mir", Mutated, Target, Invocation);
  if (!Parsed)
    return false;
  auto RoundTrip = verifyCanonicalRoundTrip(std::move(Parsed->Text), Target, Invocation);
  if (!RoundTrip)
    return std::unexpected(RoundTrip.error());
  return true;
}

[[nodiscard]] Expected<bool> builderCase(std::string_view Canonical,
                                         const TargetDescription &Target,
                                         const BackendInvocation &Invocation) {
  auto Parsed =
      parseAndCanonicalizeMIR("builder-fuzz.mir", std::string(Canonical), Target, Invocation);
  if (!Parsed)
    return std::unexpected(Parsed.error());
  MachineFunction *Function = Parsed->Document.unit().functionAt(0);
  if (Function == nullptr || stageForFunction(*Function) != BackendStage::PreLegalizerMIR)
    return false;
  MachineBasicBlock *Block = Function->firstBlock();
  if (Block == nullptr)
    return false;
  auto Type = LLT::makeInteger(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto Register = Function->registerInfo().createVirtualRegister(*Type);
  if (!Register)
    return std::unexpected(Register.error());
  MachineIRBuilder Builder(*Function, Target);
  Expected<void> InsertPoint = Block->firstInstruction() == nullptr
                                   ? Builder.setInsertPoint(*Block)
                                   : Builder.setInsertPointBefore(*Block->firstInstruction());
  if (!InsertPoint)
    return std::unexpected(InsertPoint.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::G_IMPLICIT_DEF));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Added = Pending->addDef(*Register);
  if (!Added)
    return std::unexpected(Added.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  auto Verified = MachineVerifier::verify(*Function, Target);
  if (!Verified)
    return std::unexpected(Verified.error());

  mir::MIRDocumentPrinterOptions PrintOptions;
  PrintOptions.Mode = mir::MIPrintMode::Complete;
  PrintOptions.EmitEmbeddedIR = true;
  PrintOptions.DataLayout = Invocation.DataLayout;
  auto Printed = mir::printMIRDocument(Parsed->Document, Target, std::move(PrintOptions));
  if (!Printed)
    return std::unexpected(Printed.error());
  auto RoundTrip = verifyCanonicalRoundTrip(std::move(*Printed), Target, Invocation);
  if (!RoundTrip)
    return std::unexpected(RoundTrip.error());
  return true;
}

[[nodiscard]] BackendOutputKind outputForStage(BackendStage Stage) noexcept {
  return static_cast<BackendOutputKind>(static_cast<std::uint8_t>(Stage));
}

[[nodiscard]] Expected<bool> propertyCase(std::string_view Canonical, DeterministicRandom &Random,
                                          const TargetDescription &Target,
                                          const BackendInvocation &Invocation) {
  auto Parsed =
      parseAndCanonicalizeMIR("property-fuzz.mir", std::string(Canonical), Target, Invocation);
  if (!Parsed)
    return std::unexpected(Parsed.error());
  const auto Stage = static_cast<BackendStage>(Random.index(7U));
  const MachineFunction *Function = Parsed->Document.unit().functionAt(0);
  if (Function == nullptr ||
      static_cast<std::uint8_t>(Stage) < static_cast<std::uint8_t>(stageForFunction(*Function))) {
    return false;
  }
  auto Run = runBackend(std::move(*Parsed), Target, Invocation, outputForStage(Stage));
  if (!Run)
    return std::unexpected(Run.error());
  auto RoundTrip = verifyCanonicalRoundTrip(std::string(Run->text()), Target, Invocation);
  if (!RoundTrip)
    return std::unexpected(RoundTrip.error());
  return true;
}

[[nodiscard]] Expected<bool> mutationCase(std::string_view Canonical, DeterministicRandom &Random,
                                          const TargetDescription &Target,
                                          const BackendInvocation &Invocation) {
  std::string Mutated = structuralTextMutation(Canonical, Random);
  auto Parsed =
      parseAndCanonicalizeMIR("mutation-fuzz.mir", std::move(Mutated), Target, Invocation);
  if (!Parsed)
    return false;
  auto RoundTrip = verifyCanonicalRoundTrip(std::move(Parsed->Text), Target, Invocation);
  if (!RoundTrip)
    return std::unexpected(RoundTrip.error());
  return true;
}

[[nodiscard]] Expected<bool> pipelineCase(std::string_view Canonical, DeterministicRandom &Random,
                                          const TargetDescription &Target,
                                          BackendInvocation Invocation) {
  auto Parsed =
      parseAndCanonicalizeMIR("pipeline-fuzz.mir", std::string(Canonical), Target, Invocation);
  if (!Parsed)
    return std::unexpected(Parsed.error());
  const MachineFunction *Function = Parsed->Document.unit().functionAt(0);
  if (Function == nullptr)
    return std::unexpected(Error(ErrorCode::InternalError, "pipeline fuzz input has no function"));
  auto Names = riscv::canonicalPipelinePassNames(*Function);
  if (!Names)
    return std::unexpected(Names.error());
  PipelineControls Controls;
  if (!Names->empty())
    Controls.StopAfter = (*Names)[Random.index(Names->size())];
  Controls.VerifyEach = true;
  Controls.DetectUnreportedChanges = true;
  Invocation.Determinism.ThreadCount = std::array<std::uint32_t, 3>{1, 2, 4}[Random.index(3U)];
  const std::string Stop = Controls.StopAfter ? *Controls.StopAfter : "end";
  auto Run =
      runBackend(std::move(*Parsed), Target, Invocation, BackendOutputKind::FinalMIR, Controls);
  if (!Run)
    return std::unexpected(Run.error());
  auto RoundTrip = verifyCanonicalRoundTrip(std::string(Run->text()), Target, Invocation);
  if (!RoundTrip) {
    return std::unexpected(
        Error(RoundTrip.error().code(),
              "pipeline stop-after '" + Stop + "': " + std::string(RoundTrip.error().message())));
  }
  return true;
}

using RegisterValues = std::unordered_map<std::uint64_t, std::uint64_t>;

[[nodiscard]] Expected<Register> registerAt(const MachineInstr &Instruction, std::size_t Index) {
  const MachineOperand *Operand = Instruction.operandAt(Index);
  if (Operand == nullptr)
    return makeUnexpected(ErrorCode::Unsupported, "semantic subset requires register operands");
  const std::optional<Register> Reg = Operand->registerValue();
  if (!Reg)
    return makeUnexpected(ErrorCode::Unsupported, "semantic subset requires register operands");
  return *Reg;
}

[[nodiscard]] Expected<std::uint64_t> immediateAt(const MachineInstr &Instruction,
                                                  std::size_t Index) {
  const MachineOperand *Operand = Instruction.operandAt(Index);
  if (Operand == nullptr)
    return makeUnexpected(ErrorCode::Unsupported, "semantic subset requires an immediate operand");
  if (const auto *Immediate = Operand->getIf<std::int64_t>())
    return static_cast<std::uint64_t>(*Immediate);
  if (const auto *Immediate = Operand->getIf<IntegerImmediateOperand>()) {
    if (Immediate->Value.bitWidth() > 64 || Immediate->Value.words().empty())
      return makeUnexpected(ErrorCode::Unsupported,
                            "semantic subset only supports integer immediates up to 64 bits");
    return Immediate->Value.words().front();
  }
  return makeUnexpected(ErrorCode::Unsupported, "semantic subset requires an integer immediate");
}

[[nodiscard]] std::uint64_t readRegister(Register Reg, const RegisterValues &Values,
                                         PhysicalRegister Zero) noexcept {
  if (Reg.physicalRegister() == Zero)
    return 0;
  const auto Position = Values.find(Reg.opaqueValue());
  return Position == Values.end() ? 0 : Position->second;
}

[[nodiscard]] Expected<std::uint64_t> interpretStraightLine(const MachineFunction &Function,
                                                            const TargetDescription &Target) {
  if (Function.blockCount() != 1 || Function.firstBlock() == nullptr ||
      !Function.firstBlock()->successors().empty()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "semantic subset requires one straight-line basic block");
  }
  const PhysicalRegisterDesc *ZeroDescription = Target.registers().physicalRegister("x0");
  const PhysicalRegisterDesc *ReturnDescription = Target.registers().physicalRegister("x10");
  if (ZeroDescription == nullptr || ReturnDescription == nullptr)
    return makeUnexpected(ErrorCode::Unsupported,
                          "semantic subset requires RV64 integer registers");
  const PhysicalRegister Zero = ZeroDescription->Reg;
  const Register Return = Register::fromPhysical(ReturnDescription->Reg);
  RegisterValues Values;

  for (const MachineInstr *Instruction = Function.firstBlock()->firstInstruction();
       Instruction != nullptr; Instruction = Instruction->next()) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return makeUnexpected(ErrorCode::Unsupported,
                            "semantic subset encountered an unknown opcode");
    const std::string_view Name = Descriptor->Name;
    if (Name == "PseudoRET" || Name == "JALR_RET")
      return readRegister(Return, Values, Zero);

    if (Name == "G_CONSTANT") {
      auto Destination = registerAt(*Instruction, 0);
      auto Immediate = immediateAt(*Instruction, 1);
      if (!Destination || !Immediate)
        return makeUnexpected(ErrorCode::Unsupported, "unsupported generic constant form");
      Values[Destination->opaqueValue()] = *Immediate;
      continue;
    }
    if (Name == "COPY") {
      auto Destination = registerAt(*Instruction, 0);
      auto Source = registerAt(*Instruction, 1);
      if (!Destination || !Source)
        return makeUnexpected(ErrorCode::Unsupported, "unsupported copy form");
      Values[Destination->opaqueValue()] = readRegister(*Source, Values, Zero);
      continue;
    }
    if (Name == "LUI") {
      auto Destination = registerAt(*Instruction, 0);
      auto Immediate = immediateAt(*Instruction, 1);
      if (!Destination || !Immediate)
        return makeUnexpected(ErrorCode::Unsupported, "unsupported LUI form");
      Values[Destination->opaqueValue()] = *Immediate << 12U;
      continue;
    }

    const bool ImmediateForm = Name == "ADDI" || Name == "XORI" || Name == "ORI" ||
                               Name == "ANDI" || Name == "SLLI" || Name == "SRLI" || Name == "SRAI";
    const bool RegisterForm =
        Name == "G_ADD" || Name == "G_SUB" || Name == "G_MUL" || Name == "G_AND" ||
        Name == "G_OR" || Name == "G_XOR" || Name == "G_SHL" || Name == "G_LSHR" ||
        Name == "G_ASHR" || Name == "ADD" || Name == "SUB" || Name == "MUL" || Name == "AND" ||
        Name == "OR" || Name == "XOR" || Name == "SLL" || Name == "SRL" || Name == "SRA";
    if (!ImmediateForm && !RegisterForm && !Descriptor->Flags.has(InstructionFlag::ProducesCode)) {
      continue;
    }
    if (!ImmediateForm && !RegisterForm) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "semantic subset does not model opcode '" + std::string(Name) + "'");
    }
    auto Destination = registerAt(*Instruction, 0);
    auto LeftRegister = registerAt(*Instruction, 1);
    if (!Destination || !LeftRegister)
      return makeUnexpected(ErrorCode::Unsupported, "unsupported binary instruction form");
    const std::uint64_t Left = readRegister(*LeftRegister, Values, Zero);
    std::uint64_t Right = 0;
    if (ImmediateForm) {
      auto Immediate = immediateAt(*Instruction, 2);
      if (!Immediate)
        return makeUnexpected(ErrorCode::Unsupported, "unsupported immediate instruction form");
      Right = *Immediate;
    } else {
      auto RightRegister = registerAt(*Instruction, 2);
      if (!RightRegister)
        return makeUnexpected(ErrorCode::Unsupported, "unsupported register instruction form");
      Right = readRegister(*RightRegister, Values, Zero);
    }
    std::uint64_t Value = 0;
    if (Name == "G_ADD" || Name == "ADD" || Name == "ADDI")
      Value = Left + Right;
    else if (Name == "G_SUB" || Name == "SUB")
      Value = Left - Right;
    else if (Name == "G_MUL" || Name == "MUL")
      Value = Left * Right;
    else if (Name == "G_AND" || Name == "AND" || Name == "ANDI")
      Value = Left & Right;
    else if (Name == "G_OR" || Name == "OR" || Name == "ORI")
      Value = Left | Right;
    else if (Name == "G_XOR" || Name == "XOR" || Name == "XORI")
      Value = Left ^ Right;
    else if (Name == "G_SHL" || Name == "SLL" || Name == "SLLI")
      Value = Left << (Right & 63U);
    else if (Name == "G_LSHR" || Name == "SRL" || Name == "SRLI")
      Value = Left >> (Right & 63U);
    else
      Value = static_cast<std::uint64_t>(std::bit_cast<std::int64_t>(Left) >> (Right & 63U));
    Values[Destination->opaqueValue()] = Value;
  }
  return makeUnexpected(ErrorCode::Unsupported, "semantic subset did not encounter a return");
}

[[nodiscard]] Expected<bool> semanticCase(std::string_view Canonical,
                                          const TargetDescription &Target,
                                          const BackendInvocation &Invocation) {
  auto Original =
      parseAndCanonicalizeMIR("semantic-input.mir", std::string(Canonical), Target, Invocation);
  if (!Original)
    return std::unexpected(Original.error());
  const MachineFunction *InputFunction = Original->Document.unit().functionAt(0);
  if (InputFunction == nullptr)
    return std::unexpected(Error(ErrorCode::InternalError, "semantic fuzz input has no function"));
  auto ExpectedValue = interpretStraightLine(*InputFunction, Target);
  if (!ExpectedValue && ExpectedValue.error().code() == ErrorCode::Unsupported)
    return false;
  if (!ExpectedValue)
    return std::unexpected(ExpectedValue.error());

  auto CompiledInput =
      parseAndCanonicalizeMIR("semantic-pipeline.mir", std::string(Canonical), Target, Invocation);
  if (!CompiledInput)
    return std::unexpected(CompiledInput.error());
  auto Compiled =
      runBackend(std::move(*CompiledInput), Target, Invocation, BackendOutputKind::FinalMIR);
  if (!Compiled)
    return std::unexpected(Compiled.error());
  auto Final = parseAndCanonicalizeMIR("semantic-final.mir", std::string(Compiled->text()), Target,
                                       Invocation);
  if (!Final)
    return std::unexpected(Final.error());
  const MachineFunction *FinalFunction = Final->Document.unit().functionAt(0);
  if (FinalFunction == nullptr)
    return std::unexpected(Error(ErrorCode::InternalError, "semantic final MIR has no function"));
  auto ActualValue = interpretStraightLine(*FinalFunction, Target);
  if (!ActualValue && ActualValue.error().code() == ErrorCode::Unsupported)
    return false;
  if (!ActualValue)
    return std::unexpected(ActualValue.error());
  if (*ExpectedValue != *ActualValue) {
    return makeUnexpected(ErrorCode::FuzzingFailure,
                          "restricted semantic comparison changed the integer return value from " +
                              std::to_string(*ExpectedValue) + " to " +
                              std::to_string(*ActualValue));
  }
  return true;
}

} // namespace

std::size_t MIRFuzzResult::executed() const noexcept {
  std::size_t Result = 0;
  for (const MIRFuzzLayerResult &Layer : Layers)
    Result += Layer.Executed;
  return Result;
}

std::size_t MIRFuzzResult::accepted() const noexcept {
  std::size_t Result = 0;
  for (const MIRFuzzLayerResult &Layer : Layers)
    Result += Layer.Accepted;
  return Result;
}

std::size_t MIRFuzzResult::rejected() const noexcept {
  std::size_t Result = 0;
  for (const MIRFuzzLayerResult &Layer : Layers)
    Result += Layer.Rejected;
  return Result;
}

Expected<MIRFuzzResult> fuzzMIR(std::string SourceName, std::string SourceText,
                                BackendInvocation Invocation, const TargetDescription &Target,
                                MIRFuzzOptions Options) {
  if (Options.Iterations == 0 || Options.MaximumInputSize == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "MIR fuzzing requires nonzero iteration and input-size limits");
  }
  if (std::ranges::any_of(Options.Layers,
                          [](MIRFuzzLayer Layer) { return spelling(Layer) == "invalid"; })) {
    return makeUnexpected(ErrorCode::InvalidArgument, "MIR fuzz layer is not recognized");
  }
  auto InvocationVerification = Invocation.canonicalizeAndVerify();
  if (!InvocationVerification)
    return std::unexpected(InvocationVerification.error());
  auto Input =
      parseAndCanonicalizeMIR(std::move(SourceName), std::move(SourceText), Target, Invocation);
  if (!Input)
    return std::unexpected(Input.error());
  if (Input->Text.size() > Options.MaximumInputSize)
    return makeUnexpected(ErrorCode::InvalidArgument, "MIR fuzz seed exceeds the input-size limit");
  if (Options.Layers.empty())
    Options.Layers = defaultLayers();
  std::ranges::sort(Options.Layers);
  if (std::ranges::adjacent_find(Options.Layers) != Options.Layers.end()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "MIR fuzz layers must be unique");
  }

  MIRFuzzResult Result{.Seed = Options.Seed, .Layers = {}};
  DeterministicRandom Random(Options.Seed);
  for (MIRFuzzLayer Layer : Options.Layers) {
    MIRFuzzLayerResult LayerResult{.Layer = Layer};
    const std::size_t Iterations =
        Layer == MIRFuzzLayer::RestrictedSemantics ? 1U : Options.Iterations;
    for (std::size_t Iteration = 0; Iteration < Iterations; ++Iteration) {
      ++LayerResult.Executed;
      Expected<bool> Case = false;
      switch (Layer) {
      case MIRFuzzLayer::Parser:
        Case = parserCase(Input->Text, Random, Target, Invocation, Options.MaximumInputSize);
        break;
      case MIRFuzzLayer::BuilderStructural:
        Case = builderCase(Input->Text, Target, Invocation);
        break;
      case MIRFuzzLayer::PropertyStage:
        Case = propertyCase(Input->Text, Random, Target, Invocation);
        break;
      case MIRFuzzLayer::Mutation:
        Case = mutationCase(Input->Text, Random, Target, Invocation);
        break;
      case MIRFuzzLayer::Pipeline:
        Case = pipelineCase(Input->Text, Random, Target, Invocation);
        break;
      case MIRFuzzLayer::RestrictedSemantics:
        Case = semanticCase(Input->Text, Target, Invocation);
        break;
      }
      if (!Case)
        return std::unexpected(fuzzFailure(Layer, Iteration, Case.error()));
      if (*Case)
        ++LayerResult.Accepted;
      else
        ++LayerResult.Rejected;
    }
    Result.Layers.push_back(LayerResult);
  }
  return Result;
}

std::string_view spelling(MIRFuzzLayer Layer) noexcept {
  switch (Layer) {
  case MIRFuzzLayer::Parser:
    return "parser";
  case MIRFuzzLayer::BuilderStructural:
    return "builder";
  case MIRFuzzLayer::PropertyStage:
    return "property-stage";
  case MIRFuzzLayer::Mutation:
    return "mutation";
  case MIRFuzzLayer::Pipeline:
    return "pipeline";
  case MIRFuzzLayer::RestrictedSemantics:
    return "semantics";
  }
  return "invalid";
}

Expected<MIRFuzzLayer> parseMIRFuzzLayer(std::string_view Value) {
  if (Value == "parser")
    return MIRFuzzLayer::Parser;
  if (Value == "builder")
    return MIRFuzzLayer::BuilderStructural;
  if (Value == "property-stage")
    return MIRFuzzLayer::PropertyStage;
  if (Value == "mutation")
    return MIRFuzzLayer::Mutation;
  if (Value == "pipeline")
    return MIRFuzzLayer::Pipeline;
  if (Value == "semantics")
    return MIRFuzzLayer::RestrictedSemantics;
  return makeUnexpected(ErrorCode::InvalidInvocation, "unknown MIR fuzz layer");
}

} // namespace rcc::tooling

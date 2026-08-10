// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/MIRReducer.h"

#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Tooling/ArtifactStore.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::tooling {
namespace {

struct Line final {
  std::size_t Begin = 0;
  std::size_t End = 0;
  std::size_t ContentEnd = 0;
  std::size_t Indent = 0;
};

struct Candidate final {
  std::string MIR;
  BackendInvocation Invocation;
  std::string Description;
};

[[nodiscard]] std::vector<ReductionGranularity> defaultGranularities() {
  return {
      ReductionGranularity::MachineFunction, ReductionGranularity::BasicBlock,
      ReductionGranularity::CFGEdge,         ReductionGranularity::RegisterSubgraph,
      ReductionGranularity::Instruction,     ReductionGranularity::Operand,
      ReductionGranularity::FrameObject,     ReductionGranularity::ConstantPool,
      ReductionGranularity::JumpTable,       ReductionGranularity::TargetFeature,
      ReductionGranularity::PassPipeline,
  };
}

[[nodiscard]] std::vector<Line> lines(std::string_view Text) {
  std::vector<Line> Result;
  std::size_t Begin = 0;
  while (Begin < Text.size()) {
    const std::size_t Newline = Text.find('\n', Begin);
    const std::size_t End = Newline == std::string_view::npos ? Text.size() : Newline + 1U;
    std::size_t ContentEnd = Newline == std::string_view::npos ? Text.size() : Newline;
    if (ContentEnd > Begin && Text[ContentEnd - 1U] == '\r')
      --ContentEnd;
    std::size_t Indent = 0;
    while (Begin + Indent < ContentEnd && Text[Begin + Indent] == ' ')
      ++Indent;
    Result.push_back({Begin, End, ContentEnd, Indent});
    Begin = End;
  }
  if (Text.empty())
    Result.push_back({});
  return Result;
}

[[nodiscard]] std::string_view lineText(std::string_view Text, const Line &Value) {
  return Text.substr(Value.Begin, Value.ContentEnd - Value.Begin);
}

[[nodiscard]] std::string_view trimmedLine(std::string_view Text, const Line &Value) {
  return Text.substr(Value.Begin + Value.Indent, Value.ContentEnd - Value.Begin - Value.Indent);
}

[[nodiscard]] std::string withoutRange(std::string_view Text, std::size_t Begin, std::size_t End) {
  std::string Result;
  Result.reserve(Text.size() - (End - Begin));
  Result.append(Text.substr(0, Begin));
  Result.append(Text.substr(End));
  return Result;
}

void addCandidate(std::vector<Candidate> &Result, std::set<std::string> &Seen,
                  std::string CandidateMIR, const BackendInvocation &Invocation,
                  std::string Description, std::string_view OriginalMIR) {
  if (CandidateMIR == OriginalMIR || !Seen.insert(CandidateMIR).second)
    return;
  Result.push_back({std::move(CandidateMIR), Invocation, std::move(Description)});
}

[[nodiscard]] bool isBodyStart(std::string_view Text, const Line &Value) {
  return Value.Indent == 0 && trimmedLine(Text, Value) == "body: |";
}

[[nodiscard]] bool isBlockLabel(std::string_view Text, const Line &Value) {
  const std::string_view Trimmed = trimmedLine(Text, Value);
  return Value.Indent == 2 && Trimmed.starts_with("bb.") && Trimmed.ends_with(':');
}

[[nodiscard]] bool isInstructionLine(std::string_view Text, const Line &Value, bool InBody) {
  if (!InBody || Value.Indent < 4)
    return false;
  const std::string_view Trimmed = trimmedLine(Text, Value);
  return !Trimmed.empty() && !Trimmed.starts_with("liveins:") &&
         !Trimmed.starts_with("successors:") && !Trimmed.starts_with("align ") &&
         !Trimmed.starts_with(';');
}

[[nodiscard]] std::vector<Candidate> functionCandidates(std::string_view MIR,
                                                        const BackendInvocation &Invocation) {
  const std::vector<Line> Lines = lines(MIR);
  std::vector<std::size_t> Starts;
  for (std::size_t Index = 0; Index < Lines.size(); ++Index) {
    if (lineText(MIR, Lines[Index]) == "---")
      Starts.push_back(Index);
  }
  std::vector<Candidate> Result;
  std::set<std::string> Seen;
  if (Starts.size() <= 1)
    return Result;
  for (std::size_t Position = 0; Position < Starts.size(); ++Position) {
    const std::size_t Begin = Lines[Starts[Position]].Begin;
    const std::size_t End =
        Position + 1U < Starts.size() ? Lines[Starts[Position + 1U]].Begin : MIR.size();
    addCandidate(Result, Seen, withoutRange(MIR, Begin, End), Invocation,
                 "remove YAML document " + std::to_string(Position), MIR);
  }
  return Result;
}

[[nodiscard]] std::vector<Candidate> blockCandidates(std::string_view MIR,
                                                     const BackendInvocation &Invocation) {
  const std::vector<Line> Lines = lines(MIR);
  std::vector<Candidate> Result;
  std::set<std::string> Seen;
  bool InBody = false;
  for (std::size_t Index = 0; Index < Lines.size(); ++Index) {
    if (isBodyStart(MIR, Lines[Index])) {
      InBody = true;
      continue;
    }
    if (InBody && Lines[Index].Indent == 0) {
      InBody = isBodyStart(MIR, Lines[Index]);
      continue;
    }
    if (!InBody || !isBlockLabel(MIR, Lines[Index]))
      continue;
    std::size_t EndIndex = Index + 1U;
    while (EndIndex < Lines.size() && Lines[EndIndex].Indent != 0 &&
           !isBlockLabel(MIR, Lines[EndIndex])) {
      ++EndIndex;
    }
    const std::size_t End = EndIndex < Lines.size() ? Lines[EndIndex].Begin : MIR.size();
    addCandidate(Result, Seen, withoutRange(MIR, Lines[Index].Begin, End), Invocation,
                 "remove machine basic block", MIR);
  }
  return Result;
}

[[nodiscard]] std::vector<Candidate> instructionCandidates(std::string_view MIR,
                                                           const BackendInvocation &Invocation) {
  const std::vector<Line> Lines = lines(MIR);
  std::vector<Candidate> Result;
  std::set<std::string> Seen;
  bool InBody = false;
  for (const Line &Value : Lines) {
    if (isBodyStart(MIR, Value)) {
      InBody = true;
      continue;
    }
    if (InBody && Value.Indent == 0) {
      InBody = false;
      continue;
    }
    if (!isInstructionLine(MIR, Value, InBody))
      continue;
    addCandidate(Result, Seen, withoutRange(MIR, Value.Begin, Value.End), Invocation,
                 "remove machine instruction", MIR);
  }
  return Result;
}

[[nodiscard]] std::vector<Candidate> cfgEdgeCandidates(std::string_view MIR,
                                                       const BackendInvocation &Invocation) {
  const std::vector<Line> Lines = lines(MIR);
  std::vector<Candidate> Result;
  std::set<std::string> Seen;
  bool InBody = false;
  for (const Line &Value : Lines) {
    if (isBodyStart(MIR, Value)) {
      InBody = true;
      continue;
    }
    if (InBody && Value.Indent == 0) {
      InBody = false;
      continue;
    }
    const std::string_view Trimmed = trimmedLine(MIR, Value);
    if (!InBody || !Trimmed.starts_with("successors:"))
      continue;
    const std::size_t LastComma = Trimmed.rfind(',');
    if (LastComma == std::string_view::npos) {
      addCandidate(Result, Seen, withoutRange(MIR, Value.Begin, Value.End), Invocation,
                   "remove CFG successor list", MIR);
      continue;
    }
    const std::size_t EraseBegin = Value.Begin + Value.Indent + LastComma;
    addCandidate(Result, Seen, withoutRange(MIR, EraseBegin, Value.ContentEnd), Invocation,
                 "remove CFG successor", MIR);
  }
  return Result;
}

[[nodiscard]] std::vector<Candidate> operandCandidates(std::string_view MIR,
                                                       const BackendInvocation &Invocation) {
  const std::vector<Line> Lines = lines(MIR);
  std::vector<Candidate> Result;
  std::set<std::string> Seen;
  bool InBody = false;
  for (const Line &Value : Lines) {
    if (isBodyStart(MIR, Value)) {
      InBody = true;
      continue;
    }
    if (InBody && Value.Indent == 0) {
      InBody = false;
      continue;
    }
    if (!isInstructionLine(MIR, Value, InBody))
      continue;
    const std::string_view Text = lineText(MIR, Value);
    const std::size_t LastComma = Text.rfind(',');
    if (LastComma != std::string_view::npos) {
      addCandidate(Result, Seen, withoutRange(MIR, Value.Begin + LastComma, Value.ContentEnd),
                   Invocation, "remove trailing machine operand", MIR);
    }
    const std::size_t Implicit = Text.rfind(" implicit ");
    if (Implicit != std::string_view::npos) {
      addCandidate(Result, Seen, withoutRange(MIR, Value.Begin + Implicit, Value.ContentEnd),
                   Invocation, "remove implicit machine operand", MIR);
    }
  }
  return Result;
}

[[nodiscard]] bool containsRegisterToken(std::string_view LineText, std::string_view Token) {
  std::size_t Position = 0;
  while ((Position = LineText.find(Token, Position)) != std::string_view::npos) {
    const std::size_t End = Position + Token.size();
    const bool LeftBoundary = Position == 0 || LineText[Position - 1U] != '%';
    const bool RightBoundary =
        End == LineText.size() || std::isdigit(static_cast<unsigned char>(LineText[End])) == 0;
    if (LeftBoundary && RightBoundary)
      return true;
    Position = End;
  }
  return false;
}

[[nodiscard]] std::vector<Candidate> registerCandidates(std::string_view MIR,
                                                        const BackendInvocation &Invocation) {
  const std::vector<Line> Lines = lines(MIR);
  std::set<std::string> Registers;
  bool InBody = false;
  for (const Line &Value : Lines) {
    if (isBodyStart(MIR, Value)) {
      InBody = true;
      continue;
    }
    if (InBody && Value.Indent == 0) {
      InBody = false;
      continue;
    }
    if (!isInstructionLine(MIR, Value, InBody))
      continue;
    const std::string_view Text = lineText(MIR, Value);
    for (std::size_t Position = 0; Position < Text.size(); ++Position) {
      if (Text[Position] != '%' || Position + 1U >= Text.size() ||
          std::isdigit(static_cast<unsigned char>(Text[Position + 1U])) == 0) {
        continue;
      }
      std::size_t End = Position + 2U;
      while (End < Text.size() && std::isdigit(static_cast<unsigned char>(Text[End])) != 0) {
        ++End;
      }
      Registers.emplace(Text.substr(Position, End - Position));
      Position = End - 1U;
    }
  }

  std::vector<Candidate> Result;
  std::set<std::string> Seen;
  for (const std::string &Register : Registers) {
    std::string CandidateMIR;
    CandidateMIR.reserve(MIR.size());
    InBody = false;
    for (const Line &Value : Lines) {
      if (isBodyStart(MIR, Value))
        InBody = true;
      else if (InBody && Value.Indent == 0)
        InBody = false;
      if (isInstructionLine(MIR, Value, InBody) &&
          containsRegisterToken(lineText(MIR, Value), Register)) {
        continue;
      }
      CandidateMIR.append(MIR.substr(Value.Begin, Value.End - Value.Begin));
    }
    addCandidate(Result, Seen, std::move(CandidateMIR), Invocation,
                 "remove register subgraph " + Register, MIR);
  }
  return Result;
}

[[nodiscard]] std::vector<Candidate> sequenceItemCandidates(std::string_view MIR,
                                                            const BackendInvocation &Invocation,
                                                            std::string_view Field,
                                                            std::size_t ItemIndent,
                                                            std::string_view Description) {
  const std::vector<Line> Lines = lines(MIR);
  std::vector<Candidate> Result;
  std::set<std::string> Seen;
  bool InField = false;
  for (std::size_t Index = 0; Index < Lines.size(); ++Index) {
    const std::string_view Trimmed = trimmedLine(MIR, Lines[Index]);
    if (Lines[Index].Indent == 0 && Trimmed == std::string(Field) + ":") {
      InField = true;
      continue;
    }
    if (InField && Lines[Index].Indent == 0) {
      InField = false;
      continue;
    }
    if (!InField || Lines[Index].Indent != ItemIndent || !Trimmed.starts_with("- "))
      continue;
    std::size_t EndIndex = Index + 1U;
    while (EndIndex < Lines.size() && Lines[EndIndex].Indent > ItemIndent)
      ++EndIndex;
    const std::size_t End = EndIndex < Lines.size() ? Lines[EndIndex].Begin : MIR.size();
    addCandidate(Result, Seen, withoutRange(MIR, Lines[Index].Begin, End), Invocation,
                 std::string(Description), MIR);
  }
  return Result;
}

[[nodiscard]] std::vector<Candidate> invocationCandidates(std::string_view MIR,
                                                          const BackendInvocation &Invocation,
                                                          ReductionGranularity Granularity) {
  std::vector<Candidate> Result;
  if (Granularity == ReductionGranularity::TargetFeature) {
    for (std::size_t Index = Invocation.Features.size(); Index > 0; --Index) {
      BackendInvocation CandidateInvocation = Invocation;
      const std::string Feature = CandidateInvocation.Features[Index - 1U];
      CandidateInvocation.Features.erase(CandidateInvocation.Features.begin() +
                                         static_cast<std::ptrdiff_t>(Index - 1U));
      auto Verified = CandidateInvocation.canonicalizeAndVerify();
      if (Verified) {
        Result.push_back(
            {std::string(MIR), std::move(CandidateInvocation), "remove target feature " + Feature});
      }
    }
  } else if (Granularity == ReductionGranularity::PassPipeline &&
             (Invocation.PassPipeline.size() != 1 ||
              Invocation.PassPipeline.front() != "default")) {
    for (std::size_t Index = Invocation.PassPipeline.size(); Index > 0; --Index) {
      if (Invocation.PassPipeline.size() == 1)
        break;
      BackendInvocation CandidateInvocation = Invocation;
      const std::string Pass = CandidateInvocation.PassPipeline[Index - 1U];
      CandidateInvocation.PassPipeline.erase(CandidateInvocation.PassPipeline.begin() +
                                             static_cast<std::ptrdiff_t>(Index - 1U));
      Result.push_back(
          {std::string(MIR), std::move(CandidateInvocation), "remove pipeline pass " + Pass});
    }
  }
  return Result;
}

[[nodiscard]] std::vector<Candidate> candidatesFor(ReductionGranularity Granularity,
                                                   std::string_view MIR,
                                                   const BackendInvocation &Invocation) {
  switch (Granularity) {
  case ReductionGranularity::CompilationUnit:
  case ReductionGranularity::MachineFunction:
    return functionCandidates(MIR, Invocation);
  case ReductionGranularity::BasicBlock:
    return blockCandidates(MIR, Invocation);
  case ReductionGranularity::CFGEdge:
    return cfgEdgeCandidates(MIR, Invocation);
  case ReductionGranularity::Instruction:
    return instructionCandidates(MIR, Invocation);
  case ReductionGranularity::Operand:
    return operandCandidates(MIR, Invocation);
  case ReductionGranularity::RegisterSubgraph:
    return registerCandidates(MIR, Invocation);
  case ReductionGranularity::FrameObject: {
    std::vector<Candidate> Result =
        sequenceItemCandidates(MIR, Invocation, "fixedStack", 2, "remove fixed stack object");
    std::vector<Candidate> Stack =
        sequenceItemCandidates(MIR, Invocation, "stack", 2, "remove stack object");
    Result.insert(Result.end(), std::make_move_iterator(Stack.begin()),
                  std::make_move_iterator(Stack.end()));
    return Result;
  }
  case ReductionGranularity::ConstantPool:
    return sequenceItemCandidates(MIR, Invocation, "constants", 2, "remove constant-pool entry");
  case ReductionGranularity::JumpTable:
    return sequenceItemCandidates(MIR, Invocation, "entries", 4, "remove jump-table entry");
  case ReductionGranularity::TargetFeature:
  case ReductionGranularity::PassPipeline:
    return invocationCandidates(MIR, Invocation, Granularity);
  }
  return {};
}

} // namespace

std::string_view spelling(ReductionGranularity Granularity) noexcept {
  switch (Granularity) {
  case ReductionGranularity::CompilationUnit:
    return "compilation-unit";
  case ReductionGranularity::MachineFunction:
    return "machine-function";
  case ReductionGranularity::BasicBlock:
    return "basic-block";
  case ReductionGranularity::CFGEdge:
    return "cfg-edge";
  case ReductionGranularity::Instruction:
    return "instruction";
  case ReductionGranularity::Operand:
    return "operand";
  case ReductionGranularity::RegisterSubgraph:
    return "register-subgraph";
  case ReductionGranularity::FrameObject:
    return "frame-object";
  case ReductionGranularity::ConstantPool:
    return "constant-pool";
  case ReductionGranularity::JumpTable:
    return "jump-table";
  case ReductionGranularity::TargetFeature:
    return "target-feature";
  case ReductionGranularity::PassPipeline:
    return "pass-pipeline";
  }
  return "invalid";
}

Expected<MIRReductionResult> reduceMIR(std::string SourceName, std::string SourceText,
                                       BackendInvocation Invocation,
                                       const TargetDescription &Target,
                                       const MIRInterestingnessTest &Interesting,
                                       MIRReducerOptions Options) {
  if (Options.Mode == ReductionMode::PreserveFailure && !Interesting)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "failure-preserving MIR reduction requires an interestingness test");
  if (Options.MaximumAttempts == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "MIR reducer attempt budget must be nonzero");
  std::set<ReductionGranularity> SeenGranularities;
  for (ReductionGranularity Granularity : Options.Granularities) {
    if (spelling(Granularity) == "invalid") {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "MIR reducer granularity is not recognized");
    }
    if (!SeenGranularities.insert(Granularity).second) {
      return makeUnexpected(ErrorCode::InvalidArgument, "MIR reducer granularities must be unique");
    }
  }
  auto InvocationVerification = Invocation.canonicalizeAndVerify();
  if (!InvocationVerification)
    return std::unexpected(InvocationVerification.error());
  auto Initial =
      parseAndCanonicalizeMIR(std::move(SourceName), std::move(SourceText), Target, Invocation);
  if (!Initial)
    return std::unexpected(Initial.error());
  if (Options.Mode == ReductionMode::PreserveFailure) {
    auto InitiallyInteresting = Interesting(Initial->Text, Invocation);
    if (!InitiallyInteresting)
      return std::unexpected(InitiallyInteresting.error());
    if (!*InitiallyInteresting) {
      return makeUnexpected(ErrorCode::ReductionFailed,
                            "initial MIR input does not satisfy the interestingness test");
    }
  }
  if (Options.Granularities.empty())
    Options.Granularities = defaultGranularities();

  MIRReductionResult Result{
      .MIR = std::move(Initial->Text),
      .Invocation = std::move(Invocation),
      .Steps = {},
      .Attempts = 0,
      .Exhausted = false,
  };
  for (ReductionGranularity Granularity : Options.Granularities) {
    bool Changed = true;
    while (Changed && Result.Attempts < Options.MaximumAttempts) {
      Changed = false;
      std::vector<Candidate> Candidates = candidatesFor(Granularity, Result.MIR, Result.Invocation);
      for (Candidate &CandidateValue : Candidates) {
        if (Result.Attempts == Options.MaximumAttempts)
          break;
        ++Result.Attempts;
        auto Canonical = parseAndCanonicalizeMIR("reducer-candidate.mir", CandidateValue.MIR,
                                                 Target, CandidateValue.Invocation);
        if (!Canonical)
          continue;
        if (Canonical->Text == Result.MIR && CandidateValue.Invocation == Result.Invocation)
          continue;
        if (Options.Mode == ReductionMode::PreserveFailure) {
          auto IsInteresting = Interesting(Canonical->Text, CandidateValue.Invocation);
          if (!IsInteresting)
            return std::unexpected(IsInteresting.error());
          if (!*IsInteresting)
            continue;
        }
        const std::size_t PreviousSize = Result.MIR.size();
        Result.MIR = std::move(Canonical->Text);
        Result.Invocation = std::move(CandidateValue.Invocation);
        Result.Steps.push_back({
            .Granularity = Granularity,
            .Description = std::move(CandidateValue.Description),
            .PreviousSize = PreviousSize,
            .ReducedSize = Result.MIR.size(),
        });
        Changed = true;
        break;
      }
    }
  }
  Result.Exhausted = Result.Attempts == Options.MaximumAttempts;
  return Result;
}

} // namespace rcc::tooling

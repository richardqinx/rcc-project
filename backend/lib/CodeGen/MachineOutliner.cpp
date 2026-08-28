// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineOutliner.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionAttributes.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

struct InstructionSegment final {
  MachineFunction *Function = nullptr;
  MachineBasicBlock *Block = nullptr;
  std::vector<MachineInstr *> Instructions;
};

struct SequenceGroup final {
  std::vector<MachineInstr *> Representative;
  std::vector<MachineOutlinerCandidate> Candidates;
  std::size_t DiscoveryOrder = 0;
  std::uint32_t SequenceSize = 0;
  std::int64_t Benefit = 0;
};

[[nodiscard]] RegisterOperandFlags normalizedRegisterFlags(RegisterOperandFlags Flags) noexcept {
  Flags.clear(RegisterOperandFlag::Kill);
  Flags.clear(RegisterOperandFlag::Dead);
  Flags.clear(RegisterOperandFlag::Renamable);
  return Flags;
}

template <typename PayloadT>
[[nodiscard]] bool payloadsEqual(const MachineOperand &Left, const MachineOperand &Right) noexcept {
  const PayloadT *LeftValue = Left.getIf<PayloadT>();
  const PayloadT *RightValue = Right.getIf<PayloadT>();
  return LeftValue != nullptr && RightValue != nullptr && *LeftValue == *RightValue;
}

[[nodiscard]] bool operandsEqual(const MachineOperand &Left, const MachineOperand &Right,
                                 const MachineFunction &LeftFunction,
                                 const MachineFunction &RightFunction) noexcept {
  if (Left.kind() != Right.kind() || Left.targetFlags() != Right.targetFlags() ||
      Left.subReg() != Right.subReg() || Left.tiedOperandIndex() != Right.tiedOperandIndex()) {
    return false;
  }
  if (Left.isRegister() && normalizedRegisterFlags(Left.registerFlags()) !=
                               normalizedRegisterFlags(Right.registerFlags())) {
    return false;
  }

  switch (Left.kind()) {
  case MachineOperand::Kind::Register:
    return payloadsEqual<Register>(Left, Right);
  case MachineOperand::Kind::Immediate:
    return payloadsEqual<std::int64_t>(Left, Right);
  case MachineOperand::Kind::IntegerImmediate:
    return payloadsEqual<IntegerImmediateOperand>(Left, Right);
  case MachineOperand::Kind::FloatingImmediate:
    return payloadsEqual<FloatingImmediateOperand>(Left, Right);
  case MachineOperand::Kind::BasicBlock:
    return payloadsEqual<BasicBlockOperand>(Left, Right);
  case MachineOperand::Kind::FrameIndex:
    return payloadsEqual<FrameIndexOperand>(Left, Right);
  case MachineOperand::Kind::ConstantPoolIndex:
    return payloadsEqual<ConstantPoolOperand>(Left, Right);
  case MachineOperand::Kind::JumpTableIndex:
    return payloadsEqual<JumpTableOperand>(Left, Right);
  case MachineOperand::Kind::GlobalSymbol:
    return payloadsEqual<GlobalSymbolOperand>(Left, Right);
  case MachineOperand::Kind::ExternalSymbol:
    return payloadsEqual<ExternalSymbolOperand>(Left, Right);
  case MachineOperand::Kind::BlockAddress:
    return payloadsEqual<BlockAddressOperand>(Left, Right);
  case MachineOperand::Kind::RegisterMask:
    return payloadsEqual<RegisterMaskOperand>(Left, Right);
  case MachineOperand::Kind::LiveOutRegisterMask:
    return payloadsEqual<LiveOutRegisterMaskOperand>(Left, Right);
  case MachineOperand::Kind::Metadata:
    return payloadsEqual<MetadataOperand>(Left, Right);
  case MachineOperand::Kind::TargetIndex:
    return payloadsEqual<TargetIndexOperand>(Left, Right);
  case MachineOperand::Kind::MCSymbol:
    return payloadsEqual<MCSymbolOperand>(Left, Right);
  case MachineOperand::Kind::CFIIndex: {
    const CFIIndexOperand *LeftIndex = Left.getIf<CFIIndexOperand>();
    const CFIIndexOperand *RightIndex = Right.getIf<CFIIndexOperand>();
    if (LeftIndex == nullptr || RightIndex == nullptr)
      return false;
    const MachineCFIInstruction *LeftCFI = LeftFunction.cfiInfo().instruction(LeftIndex->Index);
    const MachineCFIInstruction *RightCFI = RightFunction.cfiInfo().instruction(RightIndex->Index);
    return LeftCFI != nullptr && RightCFI != nullptr && *LeftCFI == *RightCFI;
  }
  case MachineOperand::Kind::Intrinsic:
    return payloadsEqual<IntrinsicOperand>(Left, Right);
  case MachineOperand::Kind::Predicate:
    return payloadsEqual<PredicateOperand>(Left, Right);
  case MachineOperand::Kind::ShuffleMask:
    return payloadsEqual<ShuffleMaskOperand>(Left, Right);
  case MachineOperand::Kind::DebugInstrRef:
    return payloadsEqual<DebugInstrRefOperand>(Left, Right);
  case MachineOperand::Kind::LaneMask:
    return payloadsEqual<LaneMaskOperand>(Left, Right);
  case MachineOperand::Kind::Count:
    return false;
  }
  return false;
}

[[nodiscard]] bool instructionsEqual(const MachineInstr &Left, const MachineInstr &Right) noexcept {
  if (Left.opcode() != Right.opcode() || Left.flags() != Right.flags() ||
      Left.operandCount() != Right.operandCount() ||
      Left.memOperands().size() != Right.memOperands().size()) {
    return false;
  }
  for (std::size_t Index = 0; Index < Left.operandCount(); ++Index) {
    if (!operandsEqual(Left.operands()[Index], Right.operands()[Index], Left.function(),
                       Right.function())) {
      return false;
    }
  }
  for (std::size_t Index = 0; Index < Left.memOperands().size(); ++Index) {
    const MachineMemOperand *LeftMemory = Left.memOperands()[Index];
    const MachineMemOperand *RightMemory = Right.memOperands()[Index];
    if (LeftMemory == nullptr || RightMemory == nullptr || *LeftMemory != *RightMemory)
      return false;
  }
  return true;
}

[[nodiscard]] bool sequencesEqual(const std::vector<MachineInstr *> &Left,
                                  const std::vector<MachineInstr *> &Right) noexcept {
  return Left.size() == Right.size() &&
         std::ranges::equal(
             Left, Right,
             [](const MachineInstr *LeftInstruction, const MachineInstr *RightInstruction) {
               return LeftInstruction != nullptr && RightInstruction != nullptr &&
                      instructionsEqual(*LeftInstruction, *RightInstruction);
             });
}

[[nodiscard]] bool overlaps(PhysicalRegister Left, PhysicalRegister Right,
                            const TargetDescription &Target) noexcept {
  return Target.registers().regsOverlap(Left, Right);
}

[[nodiscard]] bool containsRegister(std::span<const PhysicalRegister> Registers,
                                    PhysicalRegister Reg,
                                    const TargetDescription &Target) noexcept {
  return std::ranges::any_of(Registers, [Reg, &Target](PhysicalRegister Existing) {
    return overlaps(Existing, Reg, Target);
  });
}

void appendUnique(std::vector<PhysicalRegister> &Registers, PhysicalRegister Reg,
                  const TargetDescription &Target) {
  if (!containsRegister(Registers, Reg, Target))
    Registers.push_back(Reg);
}

void computeCandidateRegisters(MachineOutlinerCandidate &Candidate,
                               const TargetDescription &Target) {
  std::vector<PhysicalRegister> Defined;
  for (const MachineInstr *Instruction : Candidate.Instructions) {
    for (const MachineOperand &Operand : Instruction->operands()) {
      const auto RegisterValue = Operand.registerValue();
      const auto Physical = RegisterValue ? RegisterValue->physicalRegister() : std::nullopt;
      if (!Physical || Operand.isDebugUse())
        continue;
      if (Operand.isUse() && !containsRegister(Defined, *Physical, Target))
        appendUnique(Candidate.LiveIns, *Physical, Target);
    }
    for (const MachineOperand &Operand : Instruction->operands()) {
      const auto RegisterValue = Operand.registerValue();
      const auto Physical = RegisterValue ? RegisterValue->physicalRegister() : std::nullopt;
      if (!Physical || Operand.isDebugUse())
        continue;
      if (Operand.isDef()) {
        appendUnique(Defined, *Physical, Target);
        appendUnique(Candidate.DefinedRegisters, *Physical, Target);
      }
    }
  }
  std::ranges::sort(Candidate.DefinedRegisters, {}, &PhysicalRegister::number);
}

[[nodiscard]] const MachineOutlinerFunctionProperties &
propertiesFor(const MachineOutlinerOptions &Options, const MachineFunction &Function) noexcept {
  static const MachineOutlinerFunctionProperties Empty;
  const auto Position = Options.FunctionProperties.find(std::string(Function.name()));
  return Position == Options.FunctionProperties.end() ? Empty : Position->second;
}

[[nodiscard]] std::vector<InstructionSegment>
collectSegments(CompilationUnit &Unit, const TargetMachineOutlinerInfo &TargetInfo,
                const MachineOutlinerOptions &Options, const TargetDescription &Target) {
  std::vector<InstructionSegment> Segments;
  for (std::size_t FunctionIndex = 0; FunctionIndex < Unit.functionCount(); ++FunctionIndex) {
    MachineFunction *Function = Unit.functionAt(FunctionIndex);
    if (Function == nullptr || Function->attributes().has(MachineFunctionAttribute::IsOutlined) ||
        !TargetInfo.isFunctionSafeToOutlineFrom(*Function, propertiesFor(Options, *Function))) {
      continue;
    }
    for (MachineBasicBlock *Block = Function->firstBlock(); Block != nullptr;
         Block = Block->next()) {
      InstructionSegment Current{
          .Function = Function,
          .Block = Block,
          .Instructions = {},
      };
      const auto Flush = [&]() {
        if (Current.Instructions.size() >= 2)
          Segments.push_back(Current);
        Current.Instructions.clear();
      };
      for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
           Instruction = Instruction->next()) {
        const MachineOutlinerInstrType Type =
            TargetInfo.getOutliningType(*Instruction, propertiesFor(Options, *Function), Target);
        if (Type == MachineOutlinerInstrType::Illegal ||
            Type == MachineOutlinerInstrType::Invisible) {
          Flush();
          continue;
        }
        Current.Instructions.push_back(Instruction);
        if (Type == MachineOutlinerInstrType::LegalTerminator)
          Flush();
      }
      Flush();
    }
  }
  return Segments;
}

[[nodiscard]] Expected<std::vector<SequenceGroup>>
buildSequenceGroups(std::vector<InstructionSegment> &Segments,
                    const TargetMachineOutlinerInfo &TargetInfo, const TargetDescription &Target) {
  std::vector<SequenceGroup> Groups;
  std::size_t DiscoveryOrder = 0;
  for (InstructionSegment &Segment : Segments) {
    for (std::size_t Begin = 0; Begin + 1 < Segment.Instructions.size(); ++Begin) {
      for (std::size_t Length = 2; Begin + Length <= Segment.Instructions.size(); ++Length) {
        std::vector<MachineInstr *> Sequence(
            Segment.Instructions.begin() + static_cast<std::ptrdiff_t>(Begin),
            Segment.Instructions.begin() + static_cast<std::ptrdiff_t>(Begin + Length));
        auto Position = std::ranges::find_if(Groups, [&Sequence](const SequenceGroup &Group) {
          return sequencesEqual(Group.Representative, Sequence);
        });
        if (Position == Groups.end()) {
          Groups.push_back({
              .Representative = Sequence,
              .Candidates = {},
              .DiscoveryOrder = DiscoveryOrder++,
              .SequenceSize = 0,
              .Benefit = 0,
          });
          Position = std::prev(Groups.end());
        }
        MachineOutlinerCandidate Candidate{
            .Function = Segment.Function,
            .Block = Segment.Block,
            .Instructions = std::move(Sequence),
            .LiveIns = {},
            .DefinedRegisters = {},
            .CallConstructionID = 0,
            .CallOverhead = 0,
            .SaveRegister = {},
        };
        computeCandidateRegisters(Candidate, Target);
        Position->Candidates.push_back(std::move(Candidate));
      }
    }
  }

  for (SequenceGroup &Group : Groups) {
    std::vector<MachineOutlinerCandidate> Viable;
    for (MachineOutlinerCandidate &Candidate : Group.Candidates) {
      auto Accepted = TargetInfo.analyzeCandidate(Candidate, Target);
      if (!Accepted)
        return std::unexpected(Accepted.error());
      if (*Accepted)
        Viable.push_back(std::move(Candidate));
    }
    Group.Candidates = std::move(Viable);
  }
  std::erase_if(Groups, [](const SequenceGroup &Group) { return Group.Candidates.size() < 2; });

  std::unordered_map<const MachineInstr *, std::size_t> LongestAtStart;
  for (const SequenceGroup &Group : Groups) {
    for (const MachineOutlinerCandidate &Candidate : Group.Candidates) {
      const MachineInstr *Start = Candidate.Instructions.front();
      LongestAtStart[Start] = std::max(LongestAtStart[Start], Candidate.Instructions.size());
    }
  }
  for (SequenceGroup &Group : Groups) {
    std::erase_if(Group.Candidates, [&LongestAtStart](const MachineOutlinerCandidate &Candidate) {
      return Candidate.Instructions.size() < LongestAtStart[Candidate.Instructions.front()];
    });
    std::unordered_set<const MachineInstr *> Claimed;
    std::vector<MachineOutlinerCandidate> NonOverlapping;
    for (MachineOutlinerCandidate &Candidate : Group.Candidates) {
      const bool HasOverlap = std::ranges::any_of(
          Candidate.Instructions, [&Claimed](const auto *I) { return Claimed.contains(I); });
      if (HasOverlap)
        continue;
      Claimed.insert(Candidate.Instructions.begin(), Candidate.Instructions.end());
      NonOverlapping.push_back(std::move(Candidate));
    }
    Group.Candidates = std::move(NonOverlapping);
  }
  std::erase_if(Groups, [](const SequenceGroup &Group) { return Group.Candidates.size() < 2; });
  return Groups;
}

[[nodiscard]] Expected<void> calculateBenefit(SequenceGroup &Group,
                                              const TargetMachineOutlinerInfo &TargetInfo,
                                              const TargetDescription &Target) {
  std::uint64_t SequenceSize = 0;
  for (const MachineInstr *Instruction : Group.Representative) {
    auto Size = TargetInfo.instructionSize(*Instruction, Instruction->function(), Target);
    if (!Size)
      return std::unexpected(Size.error());
    SequenceSize += *Size;
  }
  if (SequenceSize > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "machine outliner sequence size exceeds the supported range");
  }
  Group.SequenceSize = static_cast<std::uint32_t>(SequenceSize);
  std::uint64_t Cost = SequenceSize + TargetInfo.frameOverhead(Group.Candidates.front());
  for (const MachineOutlinerCandidate &Candidate : Group.Candidates)
    Cost += Candidate.CallOverhead;
  const std::uint64_t Original = SequenceSize * Group.Candidates.size();
  Group.Benefit = static_cast<std::int64_t>(Original) - static_cast<std::int64_t>(Cost);
  return {};
}

[[nodiscard]] Expected<MachineOperand> cloneOperand(const MachineOperand &Operand,
                                                    const MachineFunction &Source,
                                                    MachineFunction &Destination) {
  if (Operand.kind() == MachineOperand::Kind::CFIIndex) {
    const CFIIndexOperand *Index = Operand.getIf<CFIIndexOperand>();
    const MachineCFIInstruction *Instruction =
        Index == nullptr ? nullptr : Source.cfiInfo().instruction(Index->Index);
    if (Instruction == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine outliner encountered an invalid CFI reference");
    }
    auto NewIndex = Destination.cfiInfo().addInstruction(*Instruction);
    if (!NewIndex)
      return std::unexpected(NewIndex.error());
    return MachineOperand::cfiIndex(*NewIndex, Operand.targetFlags());
  }
  return Operand.cloneDetached();
}

[[nodiscard]] Expected<MachineInstr *> cloneInstruction(const MachineInstr &Source,
                                                        MachineFunction &Destination,
                                                        MachineBasicBlock &DestinationBlock) {
  auto Created = Destination.createInstruction(Source.opcode());
  if (!Created)
    return std::unexpected(Created.error());
  MachineInstr *Clone = *Created;
  Clone->reserveOperands(Source.operandCount());
  for (const MachineOperand &Operand : Source.operands()) {
    auto ClonedOperand = cloneOperand(Operand, Source.function(), Destination);
    if (!ClonedOperand)
      return std::unexpected(ClonedOperand.error());
    auto Added = Clone->addOperand(std::move(*ClonedOperand));
    if (!Added)
      return std::unexpected(Added.error());
  }
  for (std::size_t Index = 0; Index < Source.operandCount(); ++Index) {
    const std::optional<std::size_t> Tied = Source.operands()[Index].tiedOperandIndex();
    if (!Tied || Index > *Tied)
      continue;
    const std::size_t DefIndex = Source.operands()[Index].isDef() ? Index : *Tied;
    const std::size_t UseIndex = Source.operands()[Index].isDef() ? *Tied : Index;
    auto TiedResult = Clone->tieOperands(DefIndex, UseIndex);
    if (!TiedResult)
      return std::unexpected(TiedResult.error());
  }
  for (const MachineMemOperand *Memory : Source.memOperands()) {
    if (Memory == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine outliner encountered a null memory operand");
    }
    auto ClonedMemory = Destination.createMemOperand(Memory->description());
    if (!ClonedMemory)
      return std::unexpected(ClonedMemory.error());
    auto Added = Clone->addMemOperand(**ClonedMemory);
    if (!Added)
      return std::unexpected(Added.error());
  }
  Clone->setFlags(Source.flags());
  auto Appended = DestinationBlock.appendInstruction(*Clone);
  if (!Appended)
    return std::unexpected(Appended.error());
  return Clone;
}

[[nodiscard]] Expected<std::string> nextOutlinedFunctionName(const CompilationUnit &Unit) {
  for (std::uint64_t Index = 0; Index < std::numeric_limits<std::uint32_t>::max(); ++Index) {
    std::string Name = "OUTLINED_FUNCTION_" + std::to_string(Index);
    if (Unit.function(Name) == nullptr && Unit.globalObject(Name) == nullptr)
      return Name;
  }
  return makeUnexpected(ErrorCode::Unsupported,
                        "machine outliner exhausted outlined function names");
}

[[nodiscard]] Expected<void> outlineGroup(SequenceGroup &Group, CompilationUnit &Unit,
                                          const TargetMachineOutlinerInfo &TargetInfo,
                                          const TargetDescription &Target) {
  auto Name = nextOutlinedFunctionName(Unit);
  if (!Name)
    return std::unexpected(Name.error());
  auto Created = Unit.createMachineFunction(*Name);
  if (!Created)
    return std::unexpected(Created.error());
  MachineFunction &Outlined = **Created;
  Outlined.properties() = Group.Candidates.front().Function->properties();
  Outlined.attributes().set(MachineFunctionAttribute::IsOutlined);

  auto CreatedBlock = Outlined.createBasicBlock();
  if (!CreatedBlock)
    return std::unexpected(CreatedBlock.error());
  MachineBasicBlock &OutlinedBlock = **CreatedBlock;
  auto Appended = Outlined.appendBlock(OutlinedBlock);
  if (!Appended)
    return std::unexpected(Appended.error());
  std::vector<PhysicalRegister> OutlinedLiveIns = Group.Candidates.front().LiveIns;
  std::ranges::sort(OutlinedLiveIns, std::greater{}, &PhysicalRegister::number);
  for (const PhysicalRegister LiveIn : OutlinedLiveIns) {
    auto Added = OutlinedBlock.addLiveIn(LiveIn);
    if (!Added)
      return std::unexpected(Added.error());
  }
  for (const MachineInstr *Instruction : Group.Representative) {
    auto Cloned = cloneInstruction(*Instruction, Outlined, OutlinedBlock);
    if (!Cloned)
      return std::unexpected(Cloned.error());
  }
  auto Frame =
      TargetInfo.buildOutlinedFrame(Outlined, OutlinedBlock, Group.Candidates.front(), Target);
  if (!Frame)
    return std::unexpected(Frame.error());

  for (MachineOutlinerCandidate &Candidate : Group.Candidates) {
    auto Inserted = TargetInfo.insertOutlinedCall(Candidate, *Name, Target);
    if (!Inserted)
      return std::unexpected(Inserted.error());
    for (MachineInstr *Instruction : Candidate.Instructions) {
      auto Removed = Instruction->eraseFromParent();
      if (!Removed)
        return std::unexpected(Removed.error());
    }
  }
  return {};
}

class MachineOutlinerPass final : public CompilationUnitPass {
public:
  MachineOutlinerPass(std::unique_ptr<TargetMachineOutlinerInfo> TargetInfo,
                      MachineOutlinerOptions Options)
      : TargetInfo(std::move(TargetInfo)), Options(std::move(Options)) {}

  [[nodiscard]] const CompilationUnitPassContract &contract() const noexcept override {
    static const CompilationUnitPassContract Contract{
        .Name = "machine-outliner",
        .RequiredAnalyses = {},
        .Parameters = {},
        .DeclaredChanges = MachinePassChanges::all(),
    };
    return Contract;
  }

  [[nodiscard]] Expected<CompilationUnitPassResult> run(CompilationUnit &Unit,
                                                        const TargetDescription &Target,
                                                        CompilationUnitAnalysisManager &,
                                                        MachineFunctionAnalysisManager &) override {
    if (TargetInfo == nullptr) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "machine outliner requires target-specific hooks");
    }
    bool Changed = false;
    while (true) {
      std::vector<InstructionSegment> Segments =
          collectSegments(Unit, *TargetInfo, Options, Target);
      auto Groups = buildSequenceGroups(Segments, *TargetInfo, Target);
      if (!Groups)
        return std::unexpected(Groups.error());
      for (SequenceGroup &Group : *Groups) {
        auto Calculated = calculateBenefit(Group, *TargetInfo, Target);
        if (!Calculated)
          return std::unexpected(Calculated.error());
      }
      auto Best = std::ranges::max_element(
          *Groups, [](const SequenceGroup &Left, const SequenceGroup &Right) {
            if (Left.Benefit != Right.Benefit)
              return Left.Benefit < Right.Benefit;
            if (Left.Representative.size() != Right.Representative.size())
              return Left.Representative.size() < Right.Representative.size();
            return Left.DiscoveryOrder > Right.DiscoveryOrder;
          });
      if (Best == Groups->end() || Best->Benefit <= 0)
        break;
      auto Outlined = outlineGroup(*Best, Unit, *TargetInfo, Target);
      if (!Outlined)
        return std::unexpected(Outlined.error());
      Changed = true;
    }
    return CompilationUnitPassResult{
        .Changed = Changed,
        .Changes = Changed ? MachinePassChanges::all() : MachinePassChanges{},
        .PreservedUnitAnalyses = PreservedAnalyses::none(),
        .PreservedFunctionAnalyses = PreservedAnalyses::none(),
    };
  }

private:
  std::unique_ptr<TargetMachineOutlinerInfo> TargetInfo;
  MachineOutlinerOptions Options;
};

} // namespace

MachineInstr &MachineOutlinerCandidate::front() const noexcept {
  return *Instructions.front();
}

MachineInstr &MachineOutlinerCandidate::back() const noexcept {
  return *Instructions.back();
}

bool MachineOutlinerCandidate::containsRegister(PhysicalRegister Reg,
                                                const TargetDescription &Target) const noexcept {
  for (const MachineInstr *Instruction : Instructions) {
    for (const MachineOperand &Operand : Instruction->operands()) {
      const auto RegisterValue = Operand.registerValue();
      const auto Physical = RegisterValue ? RegisterValue->physicalRegister() : std::nullopt;
      if (Physical && !Operand.isDebugUse() && overlaps(*Physical, Reg, Target))
        return true;
    }
  }
  return false;
}

bool MachineOutlinerCandidate::isRegisterLiveAfter(PhysicalRegister Reg,
                                                   const TargetDescription &Target) const noexcept {
  bool Live = false;
  for (const MachineSuccessor &Successor : Block->successors()) {
    if (Successor.Block == nullptr)
      continue;
    for (const MachineBasicBlockLiveIn &LiveIn : Successor.Block->liveIns()) {
      if (overlaps(LiveIn.PhysReg, Reg, Target))
        Live = true;
    }
  }
  for (const MachineInstr *Instruction = Block->lastInstruction();
       Instruction != nullptr && Instruction != &back(); Instruction = Instruction->previous()) {
    for (const MachineOperand &Operand : Instruction->operands()) {
      const auto RegisterValue = Operand.registerValue();
      const auto Physical = RegisterValue ? RegisterValue->physicalRegister() : std::nullopt;
      if (Physical && Operand.isDef() && overlaps(*Physical, Reg, Target))
        Live = false;
    }
    for (const MachineOperand &Operand : Instruction->operands()) {
      const auto RegisterValue = Operand.registerValue();
      const auto Physical = RegisterValue ? RegisterValue->physicalRegister() : std::nullopt;
      if (Physical && Operand.isUse() && !Operand.isDebugUse() && overlaps(*Physical, Reg, Target))
        Live = true;
    }
  }
  return Live;
}

bool MachineOutlinerCandidate::isRegisterAvailable(PhysicalRegister Reg,
                                                   const TargetDescription &Target) const noexcept {
  return !containsRegister(Reg, Target) && !isRegisterLiveAfter(Reg, Target) &&
         !Function->registerInfo().isReserved(Reg) && !Target.registers().isReservedRegister(Reg);
}

TargetMachineOutlinerInfo::~TargetMachineOutlinerInfo() = default;

std::unique_ptr<CompilationUnitPass>
createMachineOutlinerPass(std::unique_ptr<TargetMachineOutlinerInfo> TargetInfo,
                          MachineOutlinerOptions Options) {
  return std::make_unique<MachineOutlinerPass>(std::move(TargetInfo), std::move(Options));
}

} // namespace rcc

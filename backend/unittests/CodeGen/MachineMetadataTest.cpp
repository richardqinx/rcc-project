// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineMetadata.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t NopOpcode = 1;

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++FailureCount;
  }

  [[nodiscard]] int result() const noexcept {
    return FailureCount == 0 ? 0 : 1;
  }

private:
  int FailureCount = 0;
};

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  rcc::InstructionDescriptor Nop;
  Nop.InstructionOpcode = rcc::Opcode::fromValue(NopOpcode);
  Nop.Name = "TEST_NOP";
  Nop.Flags = {rcc::InstructionFlag::ProducesCode};

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Nop));
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction>
createFunction(TestContext &Context, std::uint64_t StableID, std::string Name) {
  auto Function = rcc::MachineFunction::create(std::move(Name),
                                               rcc::MachineFunctionStableID::fromValue(StableID));
  Context.expect(Function.has_value(), "machine function creation must succeed");
  if (!Function)
    return nullptr;
  return std::move(*Function);
}

[[nodiscard]] rcc::MachineMetadataElement
referenceElement(TestContext &Context, rcc::MachineMetadataReference Reference) {
  auto Element = rcc::MachineMetadataElement::reference(Reference);
  Context.expect(Element.has_value(), "valid machine metadata references must form elements");
  return Element ? std::move(*Element) : rcc::MachineMetadataElement{};
}

void testReferenceIdentity(TestContext &Context) {
  const rcc::MachineMetadataReference Invalid;
  const rcc::MachineMetadataReference EmbeddedZero = rcc::MachineMetadataReference::embeddedIR(0);
  const rcc::MachineMetadataReference FunctionZero =
      rcc::MachineMetadataReference::machineFunction(0);

  Context.expect(!Invalid.isValid() && EmbeddedZero.isValid() && FunctionZero.isValid(),
                 "metadata slot zero must be valid in both explicit reference scopes");
  Context.expect(EmbeddedZero != FunctionZero &&
                     EmbeddedZero.scope() == rcc::MachineMetadataScope::EmbeddedIR &&
                     FunctionZero.scope() == rcc::MachineMetadataScope::MachineFunction &&
                     EmbeddedZero.slot() == 0 && FunctionZero.slot() == 0,
                 "embedded and function-local metadata slots must have distinct identities");
  Context.expect(!rcc::MachineMetadataElement::reference(Invalid),
                 "an invalid metadata reference must not form an element");
}

void testOrderedGraphValidation(TestContext &Context) {
  rcc::MachineMetadataInfo Metadata;
  rcc::MachineMetadataNode First{
      .Slot = 0,
      .IsDistinct = true,
      .Elements =
          {
              referenceElement(Context, rcc::MachineMetadataReference::embeddedIR(0)),
              rcc::MachineMetadataElement::string(""),
              referenceElement(Context, rcc::MachineMetadataReference::machineFunction(1)),
          },
  };
  Context.expect(Metadata.addNode(std::move(First)).has_value() && !Metadata.verify(),
                 "forward references must be accepted before graph closure is verified");

  rcc::MachineMetadataNode Second{
      .Slot = 1,
      .IsDistinct = false,
      .Elements =
          {
              referenceElement(Context, rcc::MachineMetadataReference::machineFunction(0)),
              rcc::MachineMetadataElement::string("payload"),
          },
  };
  Context.expect(Metadata.addNode(std::move(Second)).has_value() && Metadata.verify().has_value(),
                 "closed cyclic metadata tuple graphs must be valid");
  Context.expect(Metadata.nodes().size() == 2 && Metadata.nodes()[0].Slot == 0 &&
                     Metadata.nodes()[0].IsDistinct && Metadata.nodes()[1].Slot == 1 &&
                     Metadata.node(0) == &Metadata.nodes()[0] &&
                     Metadata.node(1) == &Metadata.nodes()[1] && Metadata.node(2) == nullptr,
                 "metadata definitions and lookup must preserve deterministic source order");
  Context.expect(Metadata.nodes()[0].Elements[1].string() != nullptr &&
                     Metadata.nodes()[0].Elements[1].string()->empty(),
                 "empty metadata strings must remain valid tuple elements");

  const std::size_t OriginalSize = Metadata.nodes().size();
  Context.expect(!Metadata.addNode({.Slot = 0, .IsDistinct = false, .Elements = {}}) &&
                     Metadata.nodes().size() == OriginalSize,
                 "duplicate metadata definitions must be rejected without changing the graph");

  auto Clone = Metadata.clone();
  Context.expect(Clone.has_value() && *Clone == Metadata &&
                     Clone->nodes().data() != Metadata.nodes().data(),
                 "metadata graphs must support independent deterministic deep cloning");

  rcc::MachineMetadataInfo InvalidMetadata;
  rcc::MachineMetadataNode InvalidNode;
  InvalidNode.Elements.emplace_back();
  Context.expect(!InvalidMetadata.addNode(std::move(InvalidNode)) &&
                     InvalidMetadata.nodes().empty(),
                 "invalid tuple elements must be rejected before publication");
}

void testFunctionVerifierAndClone(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, 1, "metadata");
  if (!Target || Function == nullptr)
    return;

  Context.expect(
      Function->metadata()
              .addNode({
                  .Slot = 7,
                  .IsDistinct = false,
                  .Elements =
                      {
                          referenceElement(Context, rcc::MachineMetadataReference::embeddedIR(3)),
                          rcc::MachineMetadataElement::string("root"),
                      },
              })
              .has_value() &&
          Function->metadata()
              .addNode({
                  .Slot = 2,
                  .IsDistinct = true,
                  .Elements =
                      {
                          referenceElement(Context,
                                           rcc::MachineMetadataReference::machineFunction(7)),
                      },
              })
              .has_value() &&
          rcc::MachineVerifier::verify(*Function, *Target).has_value(),
      "well-formed machine metadata must pass unified function verification");

  auto Clone =
      rcc::cloneMachineFunction(*Function, *Target,
                                {
                                    .NewStableID = rcc::MachineFunctionStableID::fromValue(2),
                                    .NewName = std::string("metadata.clone"),
                                });
  Context.expect(Clone.has_value() && Clone->ClonedFunction->metadata() == Function->metadata() &&
                     Clone->ClonedFunction->metadata().nodes().data() !=
                         Function->metadata().nodes().data(),
                 "function cloning must preserve an independent metadata graph");

  auto InvalidFunction = createFunction(Context, 3, "invalid-metadata");
  if (InvalidFunction == nullptr)
    return;
  Context.expect(
      InvalidFunction->metadata()
              .addNode({
                  .Slot = 0,
                  .Elements =
                      {
                          referenceElement(Context,
                                           rcc::MachineMetadataReference::machineFunction(99)),
                      },
              })
              .has_value() &&
          !rcc::MachineVerifier::verify(*InvalidFunction, *Target),
      "the unified verifier must reject undefined function-local metadata references");
}

} // namespace

int main() {
  TestContext Context;
  testReferenceIdentity(Context);
  testOrderedGraphValidation(Context);
  testFunctionVerifierAndClone(Context);
  return Context.result();
}

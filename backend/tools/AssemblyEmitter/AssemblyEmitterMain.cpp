// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/MIR/MIRDocument.h"
#include "rcc/Support/TextDiagnostic.h"
#include "rcc/Target/RISCV/RISCVAssemblyPrinter.h"
#include "rcc/Target/RISCV/RISCVObjectFile.h"
#include "rcc/Target/RISCV/RISCVPipeline.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class OutputKind : std::uint8_t {
  Assembly,
  Object,
};

struct CommandLine final {
  std::string InputPath;
  std::string OutputPath = "-";
  OutputKind Kind = OutputKind::Assembly;
};

void printUsage() {
  std::cerr << "usage: rcc-backend-asm --input <mir-file|-> "
               "[--filetype asm|obj] [--output <file|->]\n";
}

[[nodiscard]] bool parseCommandLine(int ArgumentCount, char *Arguments[], CommandLine &Result) {
  for (int Index = 1; Index < ArgumentCount; ++Index) {
    const std::string_view Argument(Arguments[Index]);
    if (Argument == "--input" && Index + 1 < ArgumentCount) {
      Result.InputPath = Arguments[++Index];
      continue;
    }
    if ((Argument == "--output" || Argument == "-o") && Index + 1 < ArgumentCount) {
      Result.OutputPath = Arguments[++Index];
      continue;
    }
    if (Argument == "--filetype" && Index + 1 < ArgumentCount) {
      const std::string_view Value(Arguments[++Index]);
      if (Value == "asm" || Value == "assembly") {
        Result.Kind = OutputKind::Assembly;
      } else if (Value == "obj" || Value == "object") {
        Result.Kind = OutputKind::Object;
      } else {
        printUsage();
        return false;
      }
      continue;
    }
    {
      printUsage();
      return false;
    }
  }
  if (Result.InputPath.empty()) {
    printUsage();
    return false;
  }
  return true;
}

[[nodiscard]] bool writeOutput(const CommandLine &Command, std::span<const std::uint8_t> Contents) {
  if (Command.OutputPath == "-") {
    std::cout.write(reinterpret_cast<const char *>(Contents.data()),
                    static_cast<std::streamsize>(Contents.size()));
    return static_cast<bool>(std::cout);
  }
  std::ofstream Output(Command.OutputPath, std::ios::binary);
  if (!Output) {
    std::cerr << "rcc-backend-asm: cannot open output '" << Command.OutputPath << "'\n";
    return false;
  }
  Output.write(reinterpret_cast<const char *>(Contents.data()),
               static_cast<std::streamsize>(Contents.size()));
  if (!Output) {
    std::cerr << "rcc-backend-asm: cannot write output '" << Command.OutputPath << "'\n";
    return false;
  }
  return true;
}

[[nodiscard]] bool writeOutput(const CommandLine &Command, std::string_view Contents) {
  const auto *Data = reinterpret_cast<const std::uint8_t *>(Contents.data());
  return writeOutput(Command, std::span<const std::uint8_t>(Data, Contents.size()));
}

[[nodiscard]] bool readInput(const CommandLine &Command, std::string &Contents) {
  if (Command.InputPath == "-") {
    Contents.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    return true;
  }
  std::ifstream Input(Command.InputPath, std::ios::binary);
  if (!Input) {
    std::cerr << "rcc-backend-asm: cannot open input '" << Command.InputPath << "'\n";
    return false;
  }
  Contents.assign(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
  return true;
}

} // namespace

int main(int ArgumentCount, char *Arguments[]) {
  CommandLine Command;
  if (!parseCommandLine(ArgumentCount, Arguments, Command))
    return 1;

  std::string SourceText;
  if (!readInput(Command, SourceText))
    return 1;

  auto Target = rcc::riscv::createTargetDescription();
  if (!Target) {
    std::cerr << "rcc-backend-asm: cannot create RV64GC target: " << Target.error().message()
              << '\n';
    return 1;
  }
  rcc::mir::MIRDocumentParserOptions Options;
  Options.DataLayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128";
  auto Parsed =
      rcc::mir::parseMIRDocument(*Target, Command.InputPath, std::move(SourceText), Options);
  if (!Parsed.succeeded()) {
    for (const rcc::TextDiagnostic &Diagnostic : Parsed.diagnostics())
      std::cerr << rcc::formatTextDiagnostic(Diagnostic, Parsed.source()) << '\n';
    return 1;
  }

  rcc::CompilationUnit &Unit = Parsed.document()->unit();
  for (std::size_t Index = 0; Index < Unit.functionCount(); ++Index) {
    rcc::MachineFunction *Function = Unit.functionAt(Index);
    if (Function == nullptr) {
      std::cerr << "rcc-backend-asm: parsed document contains a null function\n";
      return 1;
    }
    auto Run = rcc::riscv::runCanonicalPipeline(*Function, *Target);
    if (!Run) {
      std::cerr << "rcc-backend-asm: backend pipeline failed for '" << Function->name()
                << "': " << Run.error().message() << '\n';
      return 1;
    }
  }

  if (Command.Kind == OutputKind::Object) {
    auto Object = rcc::riscv::emitObject(Unit, *Target);
    if (!Object) {
      std::cerr << "rcc-backend-asm: object emission failed: " << Object.error().message() << '\n';
      return 1;
    }
    return writeOutput(Command, *Object) ? 0 : 1;
  }
  auto Assembly = rcc::riscv::emitAssembly(Unit, *Target);
  if (!Assembly) {
    std::cerr << "rcc-backend-asm: assembly emission failed: " << Assembly.error().message()
              << '\n';
    return 1;
  }
  return writeOutput(Command, *Assembly) ? 0 : 1;
}

// SPDX-License-Identifier: GPL-3.0-or-later

#include "TargetDescriptionGenerator.h"
#include "rcc/Support/TextDiagnostic.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

struct CommandLine final {
  std::string InputPath;
  std::string RegisterHeaderPath;
  std::string InstructionHeaderPath;
  std::string TablesPath;
};

[[nodiscard]] bool parseCommandLine(int ArgumentCount, char *Arguments[], CommandLine &Result) {
  for (int Index = 1; Index < ArgumentCount; ++Index) {
    const std::string_view Argument(Arguments[Index]);
    if (Argument == "--input" || Argument == "--register-header" ||
        Argument == "--instruction-header" || Argument == "--tables") {
      if (Index + 1 >= ArgumentCount) {
        std::cerr << "rcc-backend-descgen: missing value for " << Argument << '\n';
        return false;
      }
      std::string *Destination = nullptr;
      if (Argument == "--input")
        Destination = &Result.InputPath;
      else if (Argument == "--register-header")
        Destination = &Result.RegisterHeaderPath;
      else if (Argument == "--instruction-header")
        Destination = &Result.InstructionHeaderPath;
      else
        Destination = &Result.TablesPath;
      *Destination = Arguments[++Index];
      continue;
    }
    std::cerr << "rcc-backend-descgen: unknown argument '" << Argument << "'\n";
    return false;
  }
  if (!Result.InputPath.empty() && !Result.RegisterHeaderPath.empty() &&
      !Result.InstructionHeaderPath.empty() && !Result.TablesPath.empty()) {
    return true;
  }
  std::cerr << "usage: rcc-backend-descgen --input <file> --register-header <file> "
               "--instruction-header <file> --tables <file>\n";
  return false;
}

[[nodiscard]] bool writeFile(const std::filesystem::path &Path, std::string_view Contents) {
  std::error_code Error;
  std::filesystem::create_directories(Path.parent_path(), Error);
  if (Error) {
    std::cerr << "rcc-backend-descgen: cannot create output directory for '" << Path.string()
              << "': " << Error.message() << '\n';
    return false;
  }
  std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
  if (!Output) {
    std::cerr << "rcc-backend-descgen: cannot open output '" << Path.string() << "'\n";
    return false;
  }
  Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
  if (Output) {
    Output.close();
    return true;
  }
  std::cerr << "rcc-backend-descgen: cannot write output '" << Path.string() << "'\n";
  return false;
}

} // namespace

int main(int ArgumentCount, char *Arguments[]) {
  CommandLine Command;
  if (!parseCommandLine(ArgumentCount, Arguments, Command))
    return 1;

  std::ifstream Input(Command.InputPath, std::ios::binary);
  if (!Input) {
    std::cerr << "rcc-backend-descgen: cannot open input '" << Command.InputPath << "'\n";
    return 1;
  }
  const std::string SourceText{std::istreambuf_iterator<char>(Input),
                               std::istreambuf_iterator<char>()};
  const rcc::targetgen::DescriptionGenerationResult Result =
      rcc::targetgen::generateDescription(Command.InputPath, SourceText);
  if (!Result.succeeded()) {
    const rcc::TextSource Source(Command.InputPath, SourceText);
    for (const rcc::TextDiagnostic &Diagnostic : Result.Diagnostics)
      std::cerr << rcc::formatTextDiagnostic(Diagnostic, Source) << '\n';
    return 1;
  }

  return writeFile(Command.RegisterHeaderPath, Result.Output->RegisterHeader) &&
                 writeFile(Command.InstructionHeaderPath, Result.Output->InstructionHeader) &&
                 writeFile(Command.TablesPath, Result.Output->TargetTables)
             ? 0
             : 1;
}

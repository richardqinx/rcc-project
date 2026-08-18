// SPDX-License-Identifier: GPL-3.0-or-later

#include "TableGenBackends.h"
#include "rcc/Support/TextDiagnostic.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

struct CommandLine final {
  std::vector<std::string> InputPaths;
  std::string RegisterHeaderPath;
  std::string InstructionHeaderPath;
  std::string TablesPath;
  std::string GlobalISelPath;
  std::string GlobalISelCombinerPath;
};

[[nodiscard]] bool parseCommandLine(int ArgumentCount, char *Arguments[], CommandLine &Result) {
  for (int Index = 1; Index < ArgumentCount; ++Index) {
    const std::string_view Argument(Arguments[Index]);
    if (Argument == "--input") {
      if (Index + 1 >= ArgumentCount) {
        std::cerr << "rcc-tblgen: missing value for " << Argument << '\n';
        return false;
      }
      Result.InputPaths.emplace_back(Arguments[++Index]);
      continue;
    }
    if (Argument == "--register-header" || Argument == "--instruction-header" ||
        Argument == "--tables" || Argument == "--global-isel" ||
        Argument == "--global-isel-combiner") {
      if (Index + 1 >= ArgumentCount) {
        std::cerr << "rcc-tblgen: missing value for " << Argument << '\n';
        return false;
      }
      std::string *Destination = nullptr;
      if (Argument == "--register-header")
        Destination = &Result.RegisterHeaderPath;
      else if (Argument == "--instruction-header")
        Destination = &Result.InstructionHeaderPath;
      else if (Argument == "--global-isel")
        Destination = &Result.GlobalISelPath;
      else if (Argument == "--global-isel-combiner")
        Destination = &Result.GlobalISelCombinerPath;
      else
        Destination = &Result.TablesPath;
      *Destination = Arguments[++Index];
      continue;
    }
    std::cerr << "rcc-tblgen: unknown argument '" << Argument << "'\n";
    return false;
  }
  if (!Result.InputPaths.empty() && !Result.RegisterHeaderPath.empty() &&
      !Result.InstructionHeaderPath.empty() && !Result.TablesPath.empty() &&
      !Result.GlobalISelPath.empty() && !Result.GlobalISelCombinerPath.empty()) {
    return true;
  }
  std::cerr << "usage: rcc-tblgen --input <file> [--input <fragment>]... --register-header <file> "
               "--instruction-header <file> --tables <file> --global-isel <file> "
               "--global-isel-combiner <file>\n";
  return false;
}

[[nodiscard]] bool writeFile(const std::filesystem::path &Path, std::string_view Contents) {
  std::error_code Error;
  std::filesystem::create_directories(Path.parent_path(), Error);
  if (Error) {
    std::cerr << "rcc-tblgen: cannot create output directory for '" << Path.string()
              << "': " << Error.message() << '\n';
    return false;
  }
  std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
  if (!Output) {
    std::cerr << "rcc-tblgen: cannot open output '" << Path.string() << "'\n";
    return false;
  }
  Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
  if (Output) {
    Output.close();
    return true;
  }
  std::cerr << "rcc-tblgen: cannot write output '" << Path.string() << "'\n";
  return false;
}

struct SourceSection final {
  std::string Name;
  std::string Contents;
  std::size_t HeaderSize = 0;
};

[[nodiscard]] bool isIdentifierCharacter(char Character) noexcept {
  return std::isalnum(static_cast<unsigned char>(Character)) != 0 || Character == '_';
}

[[nodiscard]] std::optional<std::string_view> topLevelKey(std::string_view Line) noexcept {
  if (Line.empty() || Line.front() == '#' ||
      (!std::isalpha(static_cast<unsigned char>(Line.front())) && Line.front() != '_')) {
    return std::nullopt;
  }
  std::size_t End = 1;
  while (End < Line.size() && isIdentifierCharacter(Line[End]))
    ++End;
  if (End == Line.size() || Line[End] != ':')
    return std::nullopt;
  return Line.substr(0, End);
}

[[nodiscard]] std::vector<SourceSection> splitSections(std::string_view SourceText) {
  std::vector<SourceSection> Result;
  std::size_t SectionStart = 0;
  std::size_t HeaderEnd = 0;
  std::optional<std::string> SectionName;
  std::size_t LineStart = 0;
  while (LineStart < SourceText.size()) {
    const std::size_t LineEnd = SourceText.find('\n', LineStart);
    const std::size_t ContentEnd = LineEnd == std::string_view::npos ? SourceText.size() : LineEnd;
    const std::string_view Line = SourceText.substr(LineStart, ContentEnd - LineStart);
    if (const auto Key = topLevelKey(Line)) {
      if (SectionName) {
        Result.push_back({
            .Name = std::move(*SectionName),
            .Contents = std::string(SourceText.substr(SectionStart, LineStart - SectionStart)),
            .HeaderSize = HeaderEnd - SectionStart,
        });
      }
      SectionStart = LineStart;
      HeaderEnd = LineEnd == std::string_view::npos ? SourceText.size() : LineEnd + 1U;
      SectionName = std::string(*Key);
    }
    if (LineEnd == std::string_view::npos)
      break;
    LineStart = LineEnd + 1U;
  }
  if (SectionName) {
    Result.push_back({
        .Name = std::move(*SectionName),
        .Contents = std::string(SourceText.substr(SectionStart)),
        .HeaderSize = HeaderEnd - SectionStart,
    });
  }
  return Result;
}

[[nodiscard]] std::optional<std::string>
combineDescriptionSources(const std::vector<std::string> &Paths,
                          const std::vector<std::string> &Sources) {
  std::vector<SourceSection> Result;
  std::unordered_map<std::string, std::size_t> SectionIndex;
  for (std::size_t Index = 0; Index < Sources.size(); ++Index) {
    const std::vector<SourceSection> Sections = splitSections(Sources[Index]);
    if (Sections.empty()) {
      std::cerr << "rcc-tblgen: input '" << Paths[Index]
                << "' does not contain a top-level description section\n";
      return std::nullopt;
    }
    for (const SourceSection &Section : Sections) {
      const auto Position = SectionIndex.find(Section.Name);
      if (Position == SectionIndex.end()) {
        SectionIndex.emplace(Section.Name, Result.size());
        Result.push_back(Section);
        continue;
      }
      if (Section.Name != "instructions" && Section.Name != "selection_rules" &&
          Section.Name != "combiner_rules") {
        std::cerr << "rcc-tblgen: duplicate description section '" << Section.Name << "' in '"
                  << Paths[Index] << "'\n";
        return std::nullopt;
      }
      SourceSection &Existing = Result[Position->second];
      const std::string_view Body(Section.Contents.data() + Section.HeaderSize,
                                  Section.Contents.size() - Section.HeaderSize);
      if (!Existing.Contents.empty() && Existing.Contents.back() != '\n')
        Existing.Contents.push_back('\n');
      Existing.Contents.append(Body);
    }
  }

  std::string Combined;
  for (const SourceSection &Section : Result) {
    if (!Combined.empty() && Combined.back() != '\n')
      Combined.push_back('\n');
    Combined.append(Section.Contents);
  }
  return Combined;
}

} // namespace

int main(int ArgumentCount, char *Arguments[]) {
  CommandLine Command;
  if (!parseCommandLine(ArgumentCount, Arguments, Command))
    return 1;

  std::vector<std::string> Sources;
  Sources.reserve(Command.InputPaths.size());
  for (const std::string &InputPath : Command.InputPaths) {
    std::ifstream Input(InputPath, std::ios::binary);
    if (!Input) {
      std::cerr << "rcc-tblgen: cannot open input '" << InputPath << "'\n";
      return 1;
    }
    Sources.emplace_back(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
  }
  const auto SourceText = combineDescriptionSources(Command.InputPaths, Sources);
  if (!SourceText)
    return 1;
  const rcc::targetgen::DescriptionGenerationResult Result =
      rcc::targetgen::generateDescription(Command.InputPaths.front(), *SourceText);
  if (!Result.succeeded()) {
    const rcc::TextSource Source(Command.InputPaths.front(), *SourceText);
    for (const rcc::TextDiagnostic &Diagnostic : Result.Diagnostics)
      std::cerr << rcc::formatTextDiagnostic(Diagnostic, Source) << '\n';
    return 1;
  }

  if (!Result.Output) {
    std::cerr << "rcc-tblgen: successful generation produced no output\n";
    return 1;
  }
  const rcc::targetgen::GeneratedDescription &Output = Result.Output.value();
  return writeFile(Command.RegisterHeaderPath, Output.RegisterHeader) &&
                 writeFile(Command.InstructionHeaderPath, Output.InstructionHeader) &&
                 writeFile(Command.TablesPath, Output.TargetTables) &&
                 writeFile(Command.GlobalISelPath, Output.GlobalISel) &&
                 writeFile(Command.GlobalISelCombinerPath, Output.GlobalISelCombiner)
             ? 0
             : 1;
}

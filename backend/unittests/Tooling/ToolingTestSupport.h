// SPDX-License-Identifier: GPL-3.0-or-later

// The private header has no stable include-root-relative guard name.
// NOLINTNEXTLINE(llvm-header-guard)
#ifndef RCC_UNITTESTS_TOOLING_TOOLINGTESTSUPPORT_H
#define RCC_UNITTESTS_TOOLING_TOOLINGTESTSUPPORT_H

#include "rcc/Support/Error.h"
#include "rcc/Tooling/ArtifactStore.h"
#include "rcc/Tooling/BackendInvocation.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef RCC_BACKEND_TOOLING_TEST_INPUT_DIR
#error "RCC_BACKEND_TOOLING_TEST_INPUT_DIR must be defined"
#endif

namespace tooling_test {

inline int fail(std::string_view Message) {
  std::cerr << Message << '\n';
  return 1;
}

[[nodiscard]] inline rcc::Expected<std::string> readText(const std::filesystem::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  if (!Input) {
    return rcc::makeUnexpected(rcc::ErrorCode::IoError,
                               "cannot open test input '" + Path.string() + "'");
  }
  std::string Text((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
  if (Input.bad()) {
    return rcc::makeUnexpected(rcc::ErrorCode::IoError,
                               "cannot read test input '" + Path.string() + "'");
  }
  return Text;
}

[[nodiscard]] inline rcc::Expected<void> writeText(const std::filesystem::path &Path,
                                                   std::string_view Text) {
  std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
  if (!Output)
    return rcc::makeUnexpected(rcc::ErrorCode::IoError, "cannot create test file");
  Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
  Output.close();
  if (!Output)
    return rcc::makeUnexpected(rcc::ErrorCode::IoError, "cannot write test file");
  return {};
}

[[nodiscard]] inline rcc::Expected<void> writeBytes(const std::filesystem::path &Path,
                                                    std::span<const std::uint8_t> Bytes) {
  if (Bytes.empty())
    return writeText(Path, {});
  const auto *Data = reinterpret_cast<const char *>(Bytes.data());
  return writeText(Path, std::string_view(Data, Bytes.size()));
}

[[nodiscard]] inline rcc::Expected<std::string> readInput(std::string_view Name) {
  return readText(std::filesystem::path(RCC_BACKEND_TOOLING_TEST_INPUT_DIR) / Name);
}

[[nodiscard]] inline rcc::Expected<rcc::tooling::CanonicalMIR>
loadCanonicalInput(std::string_view Name, const rcc::TargetDescription &Target,
                   const rcc::tooling::BackendInvocation &Invocation) {
  auto Source = readInput(Name);
  if (!Source)
    return std::unexpected(Source.error());
  return rcc::tooling::parseAndCanonicalizeMIR(std::string(Name), std::move(*Source), Target,
                                               Invocation);
}

class TemporaryDirectory final {
public:
  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  TemporaryDirectory(TemporaryDirectory &&Other) noexcept : Path(std::move(Other.Path)) {
    Other.Path.clear();
  }
  TemporaryDirectory &operator=(TemporaryDirectory &&) = delete;

  ~TemporaryDirectory() {
    if (Path.empty())
      return;
    std::error_code Error;
    std::filesystem::remove_all(Path, Error);
  }

  [[nodiscard]] static rcc::Expected<TemporaryDirectory> create(std::string_view Prefix) {
    std::error_code Error;
    const std::filesystem::path Root = std::filesystem::temp_directory_path(Error);
    if (Error)
      return rcc::makeUnexpected(rcc::ErrorCode::IoError, "cannot locate test temporary directory");
    const auto Stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (std::size_t Attempt = 0; Attempt < 1024; ++Attempt) {
      const std::filesystem::path Candidate =
          Root /
          (std::string(Prefix) + "-" + std::to_string(Stamp) + "-" + std::to_string(Attempt));
      Error.clear();
      if (std::filesystem::create_directory(Candidate, Error))
        return TemporaryDirectory(Candidate);
    }
    return rcc::makeUnexpected(rcc::ErrorCode::IoError,
                               "cannot create a unique test temporary directory");
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return Path;
  }

private:
  explicit TemporaryDirectory(std::filesystem::path Path) : Path(std::move(Path)) {}

  std::filesystem::path Path;
};

} // namespace tooling_test

#endif // RCC_UNITTESTS_TOOLING_TOOLINGTESTSUPPORT_H

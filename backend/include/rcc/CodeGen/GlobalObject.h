// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALOBJECT_H
#define RCC_CODEGEN_GLOBALOBJECT_H

#include "rcc/CodeGen/GenericConstant.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/TargetConstant.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rcc {

class GlobalInitializer;
using GlobalInitializerRef = std::shared_ptr<const GlobalInitializer>;

struct GlobalZeroFill final {
  std::uint64_t Size = 0;
};

struct GlobalRepeatedInitializer final {
  std::uint64_t Count = 0;
  GlobalInitializerRef Value;
};

struct GlobalAggregateInitializer final {
  std::vector<GlobalInitializerRef> Elements;
};

struct GlobalSymbolAddress final {
  std::string Symbol;
  std::int64_t Addend = 0;
  std::uint8_t Size = 8;
  std::uint32_t TargetFlags = 0;
};

struct GlobalSymbolDifference final {
  std::string Minuend;
  std::string Subtrahend;
  std::int64_t Addend = 0;
  std::uint8_t Size = 8;
};

struct GlobalAlignmentPadding final {
  std::uint64_t Size = 0;
  std::uint8_t Fill = 0;
};

struct GlobalUndefinedStorage final {
  std::uint64_t Size = 0;
};

class GlobalInitializer final {
public:
  enum class Kind : std::uint8_t {
    Invalid,
    Integer,
    Floating,
    Bytes,
    ZeroFill,
    Repeated,
    Aggregate,
    SymbolAddress,
    SymbolDifference,
    Target,
    AlignmentPadding,
    UndefinedStorage,
  };

  GlobalInitializer() = default;
  GlobalInitializer(const GlobalInitializer &) = delete;
  GlobalInitializer &operator=(const GlobalInitializer &) = delete;
  GlobalInitializer(GlobalInitializer &&) noexcept = default;
  GlobalInitializer &operator=(GlobalInitializer &&) noexcept = default;
  ~GlobalInitializer() = default;

  [[nodiscard]] static GlobalInitializer integer(IntegerImmediate Value);
  [[nodiscard]] static GlobalInitializer floating(FloatingPointBits Value);
  [[nodiscard]] static Expected<GlobalInitializer> bytes(std::vector<std::byte> Value);
  [[nodiscard]] static Expected<GlobalInitializer> zeroFill(std::uint64_t Size);
  [[nodiscard]] static Expected<GlobalInitializer> repeated(std::uint64_t Count,
                                                            GlobalInitializer Value);
  [[nodiscard]] static Expected<GlobalInitializer>
  aggregate(std::vector<GlobalInitializer> Elements);
  [[nodiscard]] static Expected<GlobalInitializer> symbolAddress(GlobalSymbolAddress Value);
  [[nodiscard]] static Expected<GlobalInitializer> symbolDifference(GlobalSymbolDifference Value);
  [[nodiscard]] static Expected<GlobalInitializer> target(TargetConstant Value);
  [[nodiscard]] static Expected<GlobalInitializer> alignmentPadding(std::uint64_t Size,
                                                                    std::uint8_t Fill = 0);
  [[nodiscard]] static Expected<GlobalInitializer> undefinedStorage(std::uint64_t Size);

  [[nodiscard]] Kind kind() const noexcept;

  template <typename ValueT> [[nodiscard]] const ValueT *getIf() const noexcept {
    return std::get_if<ValueT>(&Storage);
  }

  [[nodiscard]] Expected<void> verify(std::size_t MaximumDepth = 1024) const;
  [[nodiscard]] Expected<std::uint64_t> storageSize(std::size_t MaximumDepth = 1024) const;

private:
  using StorageT =
      std::variant<std::monostate, IntegerImmediate, FloatingPointBits, std::vector<std::byte>,
                   GlobalZeroFill, GlobalRepeatedInitializer, GlobalAggregateInitializer,
                   GlobalSymbolAddress, GlobalSymbolDifference, TargetConstant,
                   GlobalAlignmentPadding, GlobalUndefinedStorage>;

  explicit GlobalInitializer(StorageT Storage) : Storage(std::move(Storage)) {}

  StorageT Storage;
};

enum class GlobalLinkage : std::uint8_t {
  External,
  AvailableExternally,
  LinkOnceAny,
  LinkOnceODR,
  WeakAny,
  WeakODR,
  Appending,
  Internal,
  Private,
  ExternalWeak,
  Common,
  Count,
};

enum class GlobalVisibility : std::uint8_t {
  Default,
  Hidden,
  Protected,
  Count,
};

enum class GlobalTLSModel : std::uint8_t {
  None,
  GeneralDynamic,
  LocalDynamic,
  InitialExec,
  LocalExec,
  Count,
};

enum class GlobalUnnamedAddress : std::uint8_t {
  None,
  Local,
  Global,
  Count,
};

struct GlobalObjectDesc final {
  std::string Symbol;
  GenericConstantType ObjectType;
  GlobalLinkage Linkage = GlobalLinkage::External;
  GlobalVisibility Visibility = GlobalVisibility::Default;
  bool DSOLocal = false;
  std::optional<std::string> Section;
  Alignment Align;
  std::uint64_t Size = 0;
  std::uint32_t AddressSpace = 0;
  GlobalTLSModel TLSModel = GlobalTLSModel::None;
  bool ExternallyInitialized = false;
  std::optional<std::string> COMDAT;
  bool IsConstant = false;
  GlobalUnnamedAddress UnnamedAddress = GlobalUnnamedAddress::None;
  std::optional<GlobalInitializer> Initializer;
  std::vector<std::string> DebugExpressions;
  std::optional<OriginID> StableOrigin;
  std::uint64_t TargetFlags = 0;
  std::vector<std::pair<std::string, std::string>> TargetAttributes;

  GlobalObjectDesc() = default;
  GlobalObjectDesc(const GlobalObjectDesc &) = delete;
  GlobalObjectDesc &operator=(const GlobalObjectDesc &) = delete;
  GlobalObjectDesc(GlobalObjectDesc &&) noexcept = default;
  GlobalObjectDesc &operator=(GlobalObjectDesc &&) noexcept = default;
  ~GlobalObjectDesc() = default;

  [[nodiscard]] bool isDeclaration() const noexcept {
    return !Initializer.has_value() && Linkage != GlobalLinkage::Common;
  }

  [[nodiscard]] Expected<void> verify() const;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALOBJECT_H

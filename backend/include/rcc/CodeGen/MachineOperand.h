// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEOPERAND_H
#define RCC_CODEGEN_MACHINEOPERAND_H

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineDebugInfo.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/Support/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineInstr;
class MachineRegisterInfo;
class RegOperandIterator;

class FloatingPointBits final {
public:
  [[nodiscard]] static Expected<FloatingPointBits> create(FloatSemantics Semantics,
                                                          std::span<const std::uint64_t> Words);

  [[nodiscard]] FloatSemantics semantics() const noexcept {
    return Semantics;
  }

  [[nodiscard]] std::uint32_t bitWidth() const noexcept {
    return getFloatSemanticsBitWidth(Semantics).value_or(0);
  }

  [[nodiscard]] std::span<const std::uint64_t> words() const noexcept {
    return std::span(Words).first(WordCount);
  }

  friend bool operator==(const FloatingPointBits &Left, const FloatingPointBits &Right) = default;

private:
  FloatingPointBits(FloatSemantics Semantics, std::array<std::uint64_t, 2> Words,
                    std::uint8_t WordCount) noexcept
      : Semantics(Semantics), Words(Words), WordCount(WordCount) {}

  FloatSemantics Semantics;
  std::array<std::uint64_t, 2> Words;
  std::uint8_t WordCount;
};

class IntegerImmediate final {
public:
  static constexpr std::uint32_t MaxBitWidth = std::uint32_t{1} << 23U;

  [[nodiscard]] static Expected<IntegerImmediate> create(std::uint32_t BitWidth,
                                                         std::span<const std::uint64_t> Words);
  [[nodiscard]] static Expected<IntegerImmediate> fromUnsigned(std::uint32_t BitWidth,
                                                               std::uint64_t Value);
  [[nodiscard]] static Expected<IntegerImmediate> fromSigned(std::uint32_t BitWidth,
                                                             std::int64_t Value);

  [[nodiscard]] std::uint32_t bitWidth() const noexcept {
    return BitWidth;
  }

  [[nodiscard]] std::span<const std::uint64_t> words() const noexcept {
    return Words;
  }

  [[nodiscard]] bool isZero() const noexcept;
  [[nodiscard]] bool isNegative() const noexcept;
  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const IntegerImmediate &Left, const IntegerImmediate &Right) = default;

private:
  IntegerImmediate(std::uint32_t BitWidth, std::vector<std::uint64_t> Words)
      : BitWidth(BitWidth), Words(std::move(Words)) {}

  std::uint32_t BitWidth;
  std::vector<std::uint64_t> Words;
};

enum class RegisterOperandFlag : std::uint8_t {
  Define,
  Use,
  Implicit,
  Kill,
  Dead,
  Undef,
  EarlyClobber,
  InternalRead,
  Renamable,
  Debug,
  Count,
};

class RegisterOperandFlags final {
public:
  constexpr RegisterOperandFlags() noexcept = default;

  RegisterOperandFlags(std::initializer_list<RegisterOperandFlag> Flags) noexcept;

  [[nodiscard]] constexpr bool has(RegisterOperandFlag Flag) const noexcept {
    const auto Index = static_cast<std::uint16_t>(Flag);
    if (Index >= static_cast<std::uint16_t>(RegisterOperandFlag::Count))
      return false;
    return (Mask & (std::uint16_t{1} << Index)) != 0;
  }

  constexpr void set(RegisterOperandFlag Flag) noexcept {
    const auto Index = static_cast<std::uint16_t>(Flag);
    if (Index < static_cast<std::uint16_t>(RegisterOperandFlag::Count))
      Mask |= std::uint16_t{1} << Index;
  }

  constexpr void clear(RegisterOperandFlag Flag) noexcept {
    const auto Index = static_cast<std::uint16_t>(Flag);
    if (Index < static_cast<std::uint16_t>(RegisterOperandFlag::Count))
      Mask &= static_cast<std::uint16_t>(~(std::uint16_t{1} << Index));
  }

  [[nodiscard]] constexpr std::uint16_t rawValue() const noexcept {
    return Mask;
  }

  friend constexpr bool operator==(RegisterOperandFlags Left,
                                   RegisterOperandFlags Right) noexcept = default;

private:
  std::uint16_t Mask = 0;
};

struct FloatingImmediateOperand final {
  FloatingPointBits Bits;

  friend bool operator==(const FloatingImmediateOperand &Left,
                         const FloatingImmediateOperand &Right) = default;
};

struct IntegerImmediateOperand final {
  IntegerImmediate Value;

  friend bool operator==(const IntegerImmediateOperand &Left,
                         const IntegerImmediateOperand &Right) = default;
};

struct BasicBlockOperand final {
  MachineBasicBlock *Block = nullptr;

  friend bool operator==(BasicBlockOperand Left, BasicBlockOperand Right) noexcept = default;
};

struct FrameIndexOperand final {
  FrameIndex Index;
  std::int64_t Offset = 0;

  friend constexpr bool operator==(FrameIndexOperand Left,
                                   FrameIndexOperand Right) noexcept = default;
};

struct ConstantPoolOperand final {
  ConstantPoolIndex Index;
  std::int64_t Offset = 0;

  friend constexpr bool operator==(ConstantPoolOperand Left,
                                   ConstantPoolOperand Right) noexcept = default;
};

struct JumpTableOperand final {
  JumpTableIndex Index;

  friend constexpr bool operator==(JumpTableOperand Left,
                                   JumpTableOperand Right) noexcept = default;
};

struct GlobalSymbolOperand final {
  std::string Name;
  std::int64_t Offset = 0;

  friend bool operator==(const GlobalSymbolOperand &Left,
                         const GlobalSymbolOperand &Right) = default;
};

struct ExternalSymbolOperand final {
  std::string Name;
  std::int64_t Offset = 0;

  friend bool operator==(const ExternalSymbolOperand &Left,
                         const ExternalSymbolOperand &Right) = default;
};

struct BlockAddressOperand final {
  std::string FunctionName;
  std::string BlockName;
  std::int64_t Offset = 0;

  friend bool operator==(const BlockAddressOperand &Left,
                         const BlockAddressOperand &Right) = default;
};

struct RegisterMaskOperand final {
  std::shared_ptr<const RegisterMask> Mask;

  friend bool operator==(const RegisterMaskOperand &Left,
                         const RegisterMaskOperand &Right) = default;
};

struct LiveOutRegisterMaskOperand final {
  std::shared_ptr<const RegisterMask> Mask;

  friend bool operator==(const LiveOutRegisterMaskOperand &Left,
                         const LiveOutRegisterMaskOperand &Right) = default;
};

struct MetadataOperand final {
  MachineMetadataValue Value;

  friend bool operator==(const MetadataOperand &Left, const MetadataOperand &Right) = default;
};

struct TargetIndexOperand final {
  TargetIndex Index;
  std::int64_t Offset = 0;

  friend constexpr bool operator==(TargetIndexOperand Left,
                                   TargetIndexOperand Right) noexcept = default;
};

struct MCSymbolOperand final {
  std::string Name;
  std::int64_t Offset = 0;

  friend bool operator==(const MCSymbolOperand &Left, const MCSymbolOperand &Right) = default;
};

struct CFIIndexOperand final {
  CFIInstructionIndex Index;

  friend constexpr bool operator==(CFIIndexOperand Left, CFIIndexOperand Right) noexcept = default;
};

struct IntrinsicOperand final {
  IntrinsicID ID;

  friend constexpr bool operator==(IntrinsicOperand Left,
                                   IntrinsicOperand Right) noexcept = default;
};

enum class IntegerPredicate : std::uint8_t {
  Equal,
  NotEqual,
  SignedGreaterThan,
  SignedGreaterEqual,
  SignedLessThan,
  SignedLessEqual,
  UnsignedGreaterThan,
  UnsignedGreaterEqual,
  UnsignedLessThan,
  UnsignedLessEqual,
  Count,
};

enum class FloatingPredicate : std::uint8_t {
  AlwaysFalse,
  OrderedEqual,
  OrderedGreaterThan,
  OrderedGreaterEqual,
  OrderedLessThan,
  OrderedLessEqual,
  OrderedNotEqual,
  Ordered,
  Unordered,
  UnorderedEqual,
  UnorderedGreaterThan,
  UnorderedGreaterEqual,
  UnorderedLessThan,
  UnorderedLessEqual,
  UnorderedNotEqual,
  AlwaysTrue,
  Count,
};

class PredicateOperand final {
public:
  explicit PredicateOperand(IntegerPredicate Predicate) : Value(Predicate) {}
  explicit PredicateOperand(FloatingPredicate Predicate) : Value(Predicate) {}

  [[nodiscard]] bool isInteger() const noexcept {
    return std::holds_alternative<IntegerPredicate>(Value);
  }

  [[nodiscard]] const IntegerPredicate *integerPredicate() const noexcept {
    return std::get_if<IntegerPredicate>(&Value);
  }

  [[nodiscard]] const FloatingPredicate *floatingPredicate() const noexcept {
    return std::get_if<FloatingPredicate>(&Value);
  }

  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const PredicateOperand &Left, const PredicateOperand &Right) = default;

private:
  std::variant<IntegerPredicate, FloatingPredicate> Value;
};

struct ShuffleMaskOperand final {
  static constexpr std::int32_t Undef = -1;

  std::vector<std::int32_t> Elements;

  friend bool operator==(const ShuffleMaskOperand &Left, const ShuffleMaskOperand &Right) = default;
};

struct DebugInstrRefOperand final {
  std::uint32_t InstructionNumber = 0;
  std::uint32_t OperandNumber = 0;

  friend constexpr bool operator==(DebugInstrRefOperand Left,
                                   DebugInstrRefOperand Right) noexcept = default;
};

struct LaneMaskOperand final {
  LaneMask Mask;

  friend constexpr bool operator==(LaneMaskOperand Left, LaneMaskOperand Right) noexcept = default;
};

class MachineOperand final {
public:
  enum class Kind : std::uint8_t {
    Register,
    Immediate,
    IntegerImmediate,
    FloatingImmediate,
    BasicBlock,
    FrameIndex,
    ConstantPoolIndex,
    JumpTableIndex,
    GlobalSymbol,
    ExternalSymbol,
    BlockAddress,
    RegisterMask,
    LiveOutRegisterMask,
    Metadata,
    TargetIndex,
    MCSymbol,
    CFIIndex,
    Intrinsic,
    Predicate,
    ShuffleMask,
    DebugInstrRef,
    LaneMask,
    Count,
  };

  MachineOperand(const MachineOperand &) = delete;
  MachineOperand &operator=(const MachineOperand &) = delete;
  MachineOperand(MachineOperand &&Other) noexcept;
  MachineOperand &operator=(MachineOperand &&Other) noexcept;
  ~MachineOperand();

  [[nodiscard]] static Expected<MachineOperand> registerOperand(Register Reg,
                                                                RegisterOperandFlags Flags,
                                                                SubRegIndex SubReg = {},
                                                                std::uint32_t TargetFlags = 0);
  [[nodiscard]] static MachineOperand immediate(std::int64_t Value, std::uint32_t TargetFlags = 0);
  [[nodiscard]] static MachineOperand integerImmediate(IntegerImmediate Value,
                                                       std::uint32_t TargetFlags = 0);
  [[nodiscard]] static MachineOperand floatingImmediate(FloatingPointBits Bits,
                                                        std::uint32_t TargetFlags = 0);
  [[nodiscard]] static MachineOperand basicBlock(MachineBasicBlock &Block,
                                                 std::uint32_t TargetFlags = 0);
  [[nodiscard]] static MachineOperand frameIndex(FrameIndex Index, std::int64_t Offset = 0,
                                                 std::uint32_t TargetFlags = 0);
  [[nodiscard]] static MachineOperand constantPool(ConstantPoolIndex Index, std::int64_t Offset = 0,
                                                   std::uint32_t TargetFlags = 0);
  [[nodiscard]] static MachineOperand jumpTable(JumpTableIndex Index,
                                                std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand>
  globalSymbol(std::string Name, std::int64_t Offset = 0, std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand>
  externalSymbol(std::string Name, std::int64_t Offset = 0, std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand> blockAddress(std::string FunctionName,
                                                             std::string BlockName,
                                                             std::int64_t Offset = 0,
                                                             std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand>
  registerMask(std::shared_ptr<const RegisterMask> Mask, std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand>
  liveOutRegisterMask(std::shared_ptr<const RegisterMask> Mask, std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand> metadata(MachineMetadataValue Value,
                                                         std::uint32_t TargetFlags = 0);
  [[nodiscard]] static MachineOperand targetIndex(TargetIndex Index, std::int64_t Offset = 0,
                                                  std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand> mcSymbol(std::string Name, std::int64_t Offset = 0,
                                                         std::uint32_t TargetFlags = 0);
  [[nodiscard]] static MachineOperand cfiIndex(CFIInstructionIndex Index,
                                               std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand> intrinsic(IntrinsicID ID,
                                                          std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand> integerPredicate(IntegerPredicate Predicate,
                                                                 std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand> floatingPredicate(FloatingPredicate Predicate,
                                                                  std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand> shuffleMask(std::vector<std::int32_t> Elements,
                                                            std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineOperand> debugInstrRef(std::uint64_t InstructionNumber,
                                                              std::uint64_t OperandNumber,
                                                              std::uint32_t TargetFlags = 0);
  [[nodiscard]] static MachineOperand laneMask(LaneMask Mask, std::uint32_t TargetFlags = 0);

  [[nodiscard]] Kind kind() const noexcept {
    return OperandKind;
  }

  [[nodiscard]] bool isRegister() const noexcept {
    return OperandKind == Kind::Register;
  }

  [[nodiscard]] std::optional<Register> registerValue() const noexcept;
  [[nodiscard]] RegisterOperandFlags registerFlags() const noexcept {
    return RegFlags;
  }
  [[nodiscard]] SubRegIndex subReg() const noexcept {
    return RegSubReg;
  }
  [[nodiscard]] bool isDef() const noexcept;
  [[nodiscard]] bool isUse() const noexcept;
  [[nodiscard]] bool isDebugUse() const noexcept;
  [[nodiscard]] bool isImplicit() const noexcept;

  [[nodiscard]] Expected<void> setRegister(Register Reg);
  [[nodiscard]] Expected<void> setRegisterFlags(RegisterOperandFlags Flags);
  [[nodiscard]] Expected<void> setSubReg(SubRegIndex SubReg);

  [[nodiscard]] std::optional<std::size_t> tiedOperandIndex() const noexcept {
    return TiedOperandIndex;
  }

  [[nodiscard]] std::uint32_t targetFlags() const noexcept {
    return TargetFlags;
  }

  void setTargetFlags(std::uint32_t NewTargetFlags) noexcept {
    TargetFlags = NewTargetFlags;
  }

  [[nodiscard]] MachineInstr *parent() noexcept {
    return Parent;
  }

  [[nodiscard]] const MachineInstr *parent() const noexcept {
    return Parent;
  }

  template <typename PayloadT> [[nodiscard]] const PayloadT *getIf() const noexcept {
    return std::get_if<PayloadT>(&Storage);
  }

  [[nodiscard]] MachineOperand cloneDetached() const;

private:
  using OperandStorage =
      std::variant<Register, std::int64_t, IntegerImmediateOperand, FloatingImmediateOperand,
                   BasicBlockOperand, FrameIndexOperand, ConstantPoolOperand, JumpTableOperand,
                   GlobalSymbolOperand, ExternalSymbolOperand, BlockAddressOperand,
                   RegisterMaskOperand, LiveOutRegisterMaskOperand, MetadataOperand,
                   TargetIndexOperand, MCSymbolOperand, CFIIndexOperand, IntrinsicOperand,
                   PredicateOperand, ShuffleMaskOperand, DebugInstrRefOperand, LaneMaskOperand>;

  MachineOperand(Kind OperandKind, OperandStorage Storage, std::uint32_t TargetFlags = 0)
      : OperandKind(OperandKind), Storage(std::move(Storage)), TargetFlags(TargetFlags) {}

  [[nodiscard]] static Expected<void> verifyRegisterFlags(RegisterOperandFlags Flags);
  void attachTo(MachineInstr &Instruction, MachineRegisterInfo *RegInfo);
  void detachFromInstruction() noexcept;
  void linkToRegInfo(MachineRegisterInfo &RegInfo);
  void unlinkFromRegInfo() noexcept;
  void takeRegLinkFrom(MachineOperand &Other) noexcept;
  void setTiedOperandIndex(std::optional<std::size_t> Index) noexcept {
    TiedOperandIndex = Index;
  }

  Kind OperandKind;
  OperandStorage Storage;
  RegisterOperandFlags RegFlags;
  SubRegIndex RegSubReg;
  std::optional<std::size_t> TiedOperandIndex;
  std::uint32_t TargetFlags = 0;
  MachineInstr *Parent = nullptr;
  MachineRegisterInfo *LinkedRegInfo = nullptr;
  MachineOperand **PreviousRegLink = nullptr;
  MachineOperand *NextRegOperand = nullptr;

  friend class MachineInstr;
  friend class MachineRegisterInfo;
  friend class RegOperandIterator;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEOPERAND_H

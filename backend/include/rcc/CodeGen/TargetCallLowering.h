// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETCALLLOWERING_H
#define RCC_CODEGEN_TARGETCALLLOWERING_H

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/Support/Error.h"

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

class MachineIRBuilder;

enum class ABIExtensionKind : std::uint8_t {
  None,
  AnyExtend,
  SignExtend,
  ZeroExtend,
  Count,
};

enum class ABIValueCategory : std::uint8_t {
  Invalid,
  Integer,
  FloatingPoint,
  Pointer,
  Count,
};

enum class ABIValueFlag : std::uint8_t {
  ByVal,
  StructReturn,
  Indirect,
  InRegister,
  Count,
};

class ABIValueFlags final {
public:
  constexpr ABIValueFlags() noexcept = default;

  ABIValueFlags(std::initializer_list<ABIValueFlag> Flags) noexcept;

  [[nodiscard]] constexpr bool has(ABIValueFlag Flag) const noexcept {
    const auto Index = static_cast<std::uint32_t>(Flag);
    return Index < static_cast<std::uint32_t>(ABIValueFlag::Count) &&
           (Bits & (std::uint8_t{1} << Index)) != 0;
  }

  constexpr void set(ABIValueFlag Flag) noexcept {
    const auto Index = static_cast<std::uint32_t>(Flag);
    if (Index < static_cast<std::uint32_t>(ABIValueFlag::Count))
      Bits |= std::uint8_t{1} << Index;
  }

  [[nodiscard]] constexpr std::uint8_t rawValue() const noexcept {
    return Bits;
  }

  friend constexpr bool operator==(ABIValueFlags Left, ABIValueFlags Right) noexcept = default;

private:
  std::uint8_t Bits = 0;
};

struct ABIValue final {
  Register Value;
  LowLevelType Type;
  ABIValueCategory Category = ABIValueCategory::Invalid;
  std::uint32_t OriginalValue = 0;
  std::uint16_t FragmentIndex = 0;
  std::uint16_t FragmentCount = 1;
  std::uint32_t FragmentOffsetBytes = 0;
  Alignment OriginalAlignment;
  ABIExtensionKind Extension = ABIExtensionKind::None;
  ABIValueFlags Flags;
  std::uint64_t ByValSizeBytes = 0;
  Alignment ByValAlignment;

  friend bool operator==(const ABIValue &Left, const ABIValue &Right) = default;
};

enum class MachineCallTargetKind : std::uint8_t {
  Invalid,
  GlobalSymbol,
  ExternalSymbol,
  IndirectRegister,
};

class MachineCallTarget final {
public:
  constexpr MachineCallTarget() noexcept = default;

  [[nodiscard]] static Expected<MachineCallTarget> globalSymbol(std::string Name,
                                                                std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineCallTarget> externalSymbol(std::string Name,
                                                                  std::uint32_t TargetFlags = 0);
  [[nodiscard]] static Expected<MachineCallTarget> indirect(Register Callee,
                                                            std::uint32_t TargetFlags = 0);

  [[nodiscard]] MachineCallTargetKind kind() const noexcept {
    return Kind;
  }

  [[nodiscard]] std::string_view symbolName() const noexcept;
  [[nodiscard]] Register indirectRegister() const noexcept;
  [[nodiscard]] std::uint32_t targetFlags() const noexcept {
    return TargetFlags;
  }

  [[nodiscard]] bool isValid() const noexcept {
    return Kind != MachineCallTargetKind::Invalid;
  }

  friend bool operator==(const MachineCallTarget &Left, const MachineCallTarget &Right) = default;

private:
  using Storage = std::variant<std::monostate, std::string, Register>;

  MachineCallTarget(MachineCallTargetKind Kind, Storage Value, std::uint32_t TargetFlags) noexcept
      : Kind(Kind), Value(std::move(Value)), TargetFlags(TargetFlags) {}

  MachineCallTargetKind Kind = MachineCallTargetKind::Invalid;
  Storage Value;
  std::uint32_t TargetFlags = 0;
};

enum class TailCallKind : std::uint8_t {
  NotTail,
  Tail,
  MustTail,
};

struct TailCallContext final {
  CallingConventionID CallerConvention;
  std::vector<ABIValue> CallerReturns;
  std::uint64_t ReusableIncomingArgumentBytes = 0;
  std::size_t CallerFixedArgumentCount = 0;
  bool CallerIsVariadic = false;
  bool CallerHasDynamicStackAllocation = false;
  bool CallerUsesStructReturn = false;
};

struct FormalArgumentRequest final {
  CallingConventionID Convention;
  std::vector<ABIValue> Arguments;
  std::size_t FixedArgumentCount = 0;
  bool IsVariadic = false;
};

struct CallLoweringRequest final {
  CallingConventionID Convention;
  MachineCallTarget Callee;
  std::vector<ABIValue> Arguments;
  std::vector<ABIValue> Results;
  std::size_t FixedArgumentCount = 0;
  bool IsVariadic = false;
  TailCallKind TailKind = TailCallKind::NotTail;
  std::optional<TailCallContext> TailContext;
};

struct ReturnLoweringRequest final {
  CallingConventionID Convention;
  std::vector<ABIValue> Values;
  bool FunctionIsVariadic = false;
};

enum class CCLocationKind : std::uint8_t {
  Register,
  Stack,
  Indirect,
  Custom,
};

enum class CCAssignmentFlag : std::uint8_t {
  Variadic,
  ByVal,
  StructReturn,
  Indirect,
  InRegister,
  Bitcast,
  Count,
};

class CCAssignmentFlags final {
public:
  constexpr CCAssignmentFlags() noexcept = default;

  CCAssignmentFlags(std::initializer_list<CCAssignmentFlag> Flags) noexcept;

  [[nodiscard]] constexpr bool has(CCAssignmentFlag Flag) const noexcept {
    const auto Index = static_cast<std::uint32_t>(Flag);
    return Index < static_cast<std::uint32_t>(CCAssignmentFlag::Count) &&
           (Bits & (std::uint8_t{1} << Index)) != 0;
  }

  constexpr void set(CCAssignmentFlag Flag) noexcept {
    const auto Index = static_cast<std::uint32_t>(Flag);
    if (Index < static_cast<std::uint32_t>(CCAssignmentFlag::Count))
      Bits |= std::uint8_t{1} << Index;
  }

  [[nodiscard]] constexpr std::uint8_t rawValue() const noexcept {
    return Bits;
  }

  friend constexpr bool operator==(CCAssignmentFlags Left,
                                   CCAssignmentFlags Right) noexcept = default;

private:
  std::uint8_t Bits = 0;
};

struct CCAssignment final {
  std::size_t ValueIndex = 0;
  Register OriginalValue;
  std::uint32_t OriginalValueIndex = 0;
  std::uint16_t FragmentIndex = 0;
  std::uint16_t FragmentCount = 1;
  LowLevelType ValueType;
  LowLevelType LocationType;
  CCLocationKind LocationKind = CCLocationKind::Register;
  std::optional<PhysicalRegister> LocationRegister;
  std::optional<std::uint64_t> StackOffsetBytes;
  std::uint64_t LocationSizeBytes = 0;
  std::uint64_t StackSlotSizeBytes = 0;
  Alignment LocationAlignment;
  ABIExtensionKind Extension = ABIExtensionKind::None;
  CCAssignmentFlags CustomFlags;

  friend bool operator==(const CCAssignment &Left, const CCAssignment &Right) = default;
};

struct StackArgumentArea final {
  std::uint64_t SizeBytes = 0;
  std::uint64_t ShadowSpaceBytes = 0;
  Alignment Align;

  friend bool operator==(StackArgumentArea Left, StackArgumentArea Right) noexcept = default;
};

struct CallFrameRequirements final {
  std::uint64_t SizeBytes = 0;
  Alignment Align;
  bool EmitSetup = false;
  bool EmitDestroy = false;
  bool ReusesIncomingArea = false;

  friend bool operator==(CallFrameRequirements Left,
                         CallFrameRequirements Right) noexcept = default;
};

struct VarArgRegisterSave final {
  PhysicalRegister Reg;
  std::uint64_t OffsetBytes = 0;

  friend bool operator==(VarArgRegisterSave Left, VarArgRegisterSave Right) noexcept = default;
};

struct VarArgLoweringState final {
  bool IsVariadic = false;
  std::size_t FixedArgumentCount = 0;
  std::size_t FirstUnallocatedIntegerRegister = 0;
  std::uint64_t OverflowAreaOffsetBytes = 0;
  std::int64_t RegisterSaveAreaOffsetBytes = 0;
  std::uint64_t RegisterSaveAreaSizeBytes = 0;
  std::uint64_t RegisterSavePaddingBytes = 0;
  std::vector<VarArgRegisterSave> RegistersToSave;

  friend bool operator==(const VarArgLoweringState &Left,
                         const VarArgLoweringState &Right) = default;
};

struct ByValCopyAssignment final {
  std::size_t ArgumentAssignmentIndex = 0;
  Register Source;
  LowLevelType PointerType;
  std::uint64_t StackOffsetBytes = 0;
  std::uint64_t SizeBytes = 0;
  Alignment Align;

  friend bool operator==(ByValCopyAssignment Left, ByValCopyAssignment Right) noexcept = default;
};

enum class CallLoweringPlanKind : std::uint8_t {
  FormalArguments,
  Call,
  Return,
};

struct CallLoweringPlanDesc final {
  CallLoweringPlanKind Kind = CallLoweringPlanKind::FormalArguments;
  std::string Target;
  CallingConventionID Convention;
  std::vector<CCAssignment> ArgumentAssignments;
  std::vector<CCAssignment> ReturnAssignments;
  StackArgumentArea StackArguments;
  CallFrameRequirements CallFrame;
  VarArgLoweringState VarArgs;
  std::vector<ByValCopyAssignment> ByValCopies;
  std::shared_ptr<const RegisterMask> PreservedMask;
  std::optional<MachineCallTarget> Callee;
  TailCallKind TailDecision = TailCallKind::NotTail;
  std::string TailRejectionReason;
};

class CallLoweringPlan final {
public:
  [[nodiscard]] static Expected<CallLoweringPlan> create(CallLoweringPlanDesc Description);

  [[nodiscard]] CallLoweringPlanKind kind() const noexcept {
    return Description.Kind;
  }

  [[nodiscard]] std::string_view target() const noexcept {
    return Description.Target;
  }

  [[nodiscard]] CallingConventionID convention() const noexcept {
    return Description.Convention;
  }

  [[nodiscard]] std::span<const CCAssignment> argumentAssignments() const noexcept {
    return Description.ArgumentAssignments;
  }

  [[nodiscard]] std::span<const CCAssignment> returnAssignments() const noexcept {
    return Description.ReturnAssignments;
  }

  [[nodiscard]] const StackArgumentArea &stackArguments() const noexcept {
    return Description.StackArguments;
  }

  [[nodiscard]] const CallFrameRequirements &callFrame() const noexcept {
    return Description.CallFrame;
  }

  [[nodiscard]] const VarArgLoweringState &varArgs() const noexcept {
    return Description.VarArgs;
  }

  [[nodiscard]] std::span<const ByValCopyAssignment> byValCopies() const noexcept {
    return Description.ByValCopies;
  }

  [[nodiscard]] const std::shared_ptr<const RegisterMask> &preservedMask() const noexcept {
    return Description.PreservedMask;
  }

  [[nodiscard]] const MachineCallTarget *callee() const noexcept {
    return Description.Callee ? &*Description.Callee : nullptr;
  }

  [[nodiscard]] TailCallKind tailDecision() const noexcept {
    return Description.TailDecision;
  }

  [[nodiscard]] std::string_view tailRejectionReason() const noexcept {
    return Description.TailRejectionReason;
  }

private:
  explicit CallLoweringPlan(CallLoweringPlanDesc Description)
      : Description(std::move(Description)) {}

  CallLoweringPlanDesc Description;
};

class TargetCallLowering {
public:
  virtual ~TargetCallLowering() = default;

  [[nodiscard]] Expected<void> lowerFormalArguments(MachineIRBuilder &Builder,
                                                    const FormalArgumentRequest &Request) const;
  [[nodiscard]] Expected<void> lowerCall(MachineIRBuilder &Builder,
                                         const CallLoweringRequest &Request) const;
  [[nodiscard]] Expected<void> lowerReturn(MachineIRBuilder &Builder,
                                           const ReturnLoweringRequest &Request) const;

  [[nodiscard]] virtual Expected<CallLoweringPlan>
  planFormalArguments(const FormalArgumentRequest &Request) const = 0;
  [[nodiscard]] virtual Expected<CallLoweringPlan>
  planCall(const CallLoweringRequest &Request) const = 0;
  [[nodiscard]] virtual Expected<CallLoweringPlan>
  planReturn(const ReturnLoweringRequest &Request) const = 0;
  [[nodiscard]] virtual Expected<void> emitPlan(MachineIRBuilder &Builder,
                                                const CallLoweringPlan &Plan) const = 0;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETCALLLOWERING_H

// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETCONSTANT_H
#define RCC_CODEGEN_TARGETCONSTANT_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/Support/Error.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {

struct TargetConstantFixup final {
  std::uint64_t Offset = 0;
  TargetRelocationKindID Kind;
  std::string Symbol;
  std::int64_t Addend = 0;

  friend bool operator==(const TargetConstantFixup &Left,
                         const TargetConstantFixup &Right) = default;
};

struct TargetConstantEmission final {
  std::vector<std::byte> Bytes;
  std::vector<TargetConstantFixup> Fixups;

  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const TargetConstantEmission &Left,
                         const TargetConstantEmission &Right) = default;
};

struct EncodedTargetConstant final {
  TargetConstantTypeID TypeID;
  std::uint32_t Version = 0;
  std::vector<std::byte> Payload;

  friend bool operator==(const EncodedTargetConstant &Left,
                         const EncodedTargetConstant &Right) = default;
};

template <typename ValueT>
concept TargetConstantValue =
    std::move_constructible<ValueT> &&
    requires(const ValueT &Value, const ValueT &Other, std::uint32_t Version,
             std::span<const std::byte> Payload, std::string_view Text) {
      { ValueT::stableTypeID() } -> std::same_as<TargetConstantTypeID>;
      { ValueT::schemaVersion() } -> std::same_as<std::uint32_t>;
      { Value.verify() } -> std::same_as<Expected<void>>;
      { Value.clone() } -> std::same_as<Expected<ValueT>>;
      { Value.stableHash() } -> std::same_as<std::uint64_t>;
      { Value.print() } -> std::same_as<std::string>;
      { Value.encode() } -> std::same_as<Expected<std::vector<std::byte>>>;
      { Value.emit() } -> std::same_as<Expected<TargetConstantEmission>>;
      { Value.requiredAlignment() } -> std::same_as<Alignment>;
      { Value.relocationKind() } -> std::same_as<std::optional<TargetRelocationKindID>>;
      { Value == Other } -> std::convertible_to<bool>;
      { ValueT::parse(Text) } -> std::same_as<Expected<ValueT>>;
      { ValueT::decode(Version, Payload) } -> std::same_as<Expected<ValueT>>;
    };

class TargetConstant final {
public:
  TargetConstant() = default;
  TargetConstant(const TargetConstant &) = delete;
  TargetConstant &operator=(const TargetConstant &) = delete;
  TargetConstant(TargetConstant &&) noexcept;
  TargetConstant &operator=(TargetConstant &&) noexcept;
  ~TargetConstant();

  template <TargetConstantValue ValueT>
  [[nodiscard]] static Expected<TargetConstant> create(ValueT Value) {
    if (ValueT::stableTypeID().value() == 0)
      return makeUnexpected(ErrorCode::InvalidArgument, "target constant TypeID must be nonzero");
    if (ValueT::schemaVersion() == 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "target constant schema version must be nonzero");
    }
    auto Verification = Value.verify();
    if (!Verification)
      return std::unexpected(Verification.error());

    std::unique_ptr<Concept> NewStorage = std::make_unique<Model<ValueT>>(std::move(Value));
    return TargetConstant(std::move(NewStorage));
  }

  template <TargetConstantValue ValueT>
  [[nodiscard]] static Expected<TargetConstant> parse(std::string_view Text) {
    auto Value = ValueT::parse(Text);
    if (!Value)
      return std::unexpected(Value.error());
    return create<ValueT>(std::move(*Value));
  }

  template <TargetConstantValue ValueT>
  [[nodiscard]] static Expected<TargetConstant> decode(const EncodedTargetConstant &Encoded) {
    if (Encoded.TypeID != ValueT::stableTypeID()) {
      return makeUnexpected(ErrorCode::ParseError,
                            "encoded target constant TypeID does not match its target schema");
    }
    if (Encoded.Version == 0)
      return makeUnexpected(ErrorCode::ParseError, "target constant version must be nonzero");
    auto Value = ValueT::decode(Encoded.Version, Encoded.Payload);
    if (!Value)
      return std::unexpected(Value.error());
    return create<ValueT>(std::move(*Value));
  }

  [[nodiscard]] bool hasValue() const noexcept {
    return Storage != nullptr;
  }

  [[nodiscard]] std::optional<TargetConstantTypeID> typeID() const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> schemaVersion() const noexcept;
  [[nodiscard]] Expected<void> verify() const;
  [[nodiscard]] Expected<TargetConstant> clone() const;
  [[nodiscard]] Expected<std::uint64_t> stableHash() const;
  [[nodiscard]] Expected<std::string> print() const;
  [[nodiscard]] Expected<EncodedTargetConstant> encode() const;
  [[nodiscard]] Expected<TargetConstantEmission> emit() const;
  [[nodiscard]] Expected<Alignment> requiredAlignment() const;
  [[nodiscard]] Expected<std::optional<TargetRelocationKindID>> relocationKind() const;

  [[nodiscard]] bool equals(const TargetConstant &Other) const noexcept;

  template <TargetConstantValue ValueT> [[nodiscard]] ValueT *getIf() noexcept {
    if (!hasType<ValueT>())
      return nullptr;
    return &static_cast<Model<ValueT> *>(Storage.get())->value();
  }

  template <TargetConstantValue ValueT> [[nodiscard]] const ValueT *getIf() const noexcept {
    if (!hasType<ValueT>())
      return nullptr;
    return &static_cast<const Model<ValueT> *>(Storage.get())->value();
  }

private:
  class Concept {
  public:
    Concept() = default;
    Concept(const Concept &) = delete;
    Concept &operator=(const Concept &) = delete;
    Concept(Concept &&) = delete;
    Concept &operator=(Concept &&) = delete;
    virtual ~Concept() = default;

    [[nodiscard]] virtual TargetConstantTypeID typeID() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t schemaVersion() const noexcept = 0;
    [[nodiscard]] virtual const void *localTypeToken() const noexcept = 0;
    [[nodiscard]] virtual Expected<void> verify() const = 0;
    [[nodiscard]] virtual Expected<std::unique_ptr<Concept>> cloneStorage() const = 0;
    [[nodiscard]] virtual std::uint64_t stableHash() const noexcept = 0;
    [[nodiscard]] virtual std::string print() const = 0;
    [[nodiscard]] virtual Expected<std::vector<std::byte>> encodePayload() const = 0;
    [[nodiscard]] virtual Expected<TargetConstantEmission> emit() const = 0;
    [[nodiscard]] virtual Alignment requiredAlignment() const noexcept = 0;
    [[nodiscard]] virtual std::optional<TargetRelocationKindID> relocationKind() const noexcept = 0;
    [[nodiscard]] virtual bool equals(const Concept &Other) const noexcept = 0;
  };

  template <typename ValueT> [[nodiscard]] static const void *typeToken() noexcept {
    static const std::byte Token{};
    return &Token;
  }

  template <TargetConstantValue ValueT> class Model final : public Concept {
  public:
    explicit Model(ValueT Value) : Value(std::move(Value)) {}

    [[nodiscard]] TargetConstantTypeID typeID() const noexcept override {
      return ValueT::stableTypeID();
    }

    [[nodiscard]] std::uint32_t schemaVersion() const noexcept override {
      return ValueT::schemaVersion();
    }

    [[nodiscard]] const void *localTypeToken() const noexcept override {
      return TargetConstant::typeToken<ValueT>();
    }

    [[nodiscard]] Expected<void> verify() const override {
      return Value.verify();
    }

    [[nodiscard]] Expected<std::unique_ptr<Concept>> cloneStorage() const override {
      auto Cloned = Value.clone();
      if (!Cloned)
        return std::unexpected(Cloned.error());
      std::unique_ptr<Concept> Result = std::make_unique<Model<ValueT>>(std::move(*Cloned));
      return Result;
    }

    [[nodiscard]] std::uint64_t stableHash() const noexcept override {
      return Value.stableHash();
    }

    [[nodiscard]] std::string print() const override {
      return Value.print();
    }

    [[nodiscard]] Expected<std::vector<std::byte>> encodePayload() const override {
      return Value.encode();
    }

    [[nodiscard]] Expected<TargetConstantEmission> emit() const override {
      return Value.emit();
    }

    [[nodiscard]] Alignment requiredAlignment() const noexcept override {
      return Value.requiredAlignment();
    }

    [[nodiscard]] std::optional<TargetRelocationKindID> relocationKind() const noexcept override {
      return Value.relocationKind();
    }

    [[nodiscard]] bool equals(const Concept &Other) const noexcept override {
      if (Other.localTypeToken() != TargetConstant::typeToken<ValueT>())
        return false;
      return Value == static_cast<const Model<ValueT> &>(Other).Value;
    }

    [[nodiscard]] ValueT &value() noexcept {
      return Value;
    }

    [[nodiscard]] const ValueT &value() const noexcept {
      return Value;
    }

  private:
    ValueT Value;
  };

  explicit TargetConstant(std::unique_ptr<Concept> Storage) : Storage(std::move(Storage)) {}

  template <TargetConstantValue ValueT> [[nodiscard]] bool hasType() const noexcept {
    return Storage != nullptr && Storage->typeID() == ValueT::stableTypeID() &&
           Storage->localTypeToken() == typeToken<ValueT>();
  }

  std::unique_ptr<Concept> Storage;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETCONSTANT_H

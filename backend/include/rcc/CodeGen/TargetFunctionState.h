// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETFUNCTIONSTATE_H
#define RCC_CODEGEN_TARGETFUNCTIONSTATE_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/Support/Error.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rcc {

class MachineFunctionCloneContext;

struct EncodedTargetFunctionState final {
  TargetFunctionStateTypeID TypeID;
  std::uint32_t Version = 0;
  std::vector<std::byte> Payload;

  friend bool operator==(const EncodedTargetFunctionState &Left,
                         const EncodedTargetFunctionState &Right) = default;
};

template <typename ValueT>
concept TargetFunctionStateValue =
    std::move_constructible<ValueT> &&
    requires(const ValueT &Value, const ValueT &Other,
             const MachineFunctionCloneContext &CloneContext, std::uint32_t Version,
             std::span<const std::byte> Payload) {
      { ValueT::stableTypeID() } -> std::same_as<TargetFunctionStateTypeID>;
      { ValueT::schemaVersion() } -> std::same_as<std::uint32_t>;
      { Value.verify() } -> std::same_as<Expected<void>>;
      { Value.clone(CloneContext) } -> std::same_as<Expected<ValueT>>;
      { Value.print() } -> std::same_as<std::string>;
      { Value.encode() } -> std::same_as<Expected<std::vector<std::byte>>>;
      { Value == Other } -> std::convertible_to<bool>;
      { ValueT::decode(Version, Payload) } -> std::same_as<Expected<ValueT>>;
    };

class TargetFunctionState final {
public:
  TargetFunctionState() = default;
  TargetFunctionState(const TargetFunctionState &) = delete;
  TargetFunctionState &operator=(const TargetFunctionState &) = delete;
  TargetFunctionState(TargetFunctionState &&) noexcept;
  TargetFunctionState &operator=(TargetFunctionState &&) noexcept;
  ~TargetFunctionState();

  template <TargetFunctionStateValue ValueT>
  [[nodiscard]] static Expected<TargetFunctionState> create(ValueT Value) {
    if (ValueT::stableTypeID().value() == 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "target function state TypeID must be nonzero");
    }
    if (ValueT::schemaVersion() == 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "target function state schema version must be nonzero");
    }
    auto Verification = Value.verify();
    if (!Verification)
      return std::unexpected(Verification.error());

    std::unique_ptr<Concept> NewStorage = std::make_unique<Model<ValueT>>(std::move(Value));
    return TargetFunctionState(std::move(NewStorage));
  }

  template <TargetFunctionStateValue ValueT>
  [[nodiscard]] static Expected<TargetFunctionState>
  decode(const EncodedTargetFunctionState &Encoded) {
    if (Encoded.TypeID != ValueT::stableTypeID()) {
      return makeUnexpected(
          ErrorCode::ParseError,
          "encoded target function state TypeID does not match its target schema");
    }
    if (Encoded.Version == 0) {
      return makeUnexpected(ErrorCode::ParseError, "target function state version must be nonzero");
    }
    auto Value = ValueT::decode(Encoded.Version, Encoded.Payload);
    if (!Value)
      return std::unexpected(Value.error());
    return create<ValueT>(std::move(*Value));
  }

  [[nodiscard]] bool hasValue() const noexcept {
    return Storage != nullptr;
  }

  [[nodiscard]] std::optional<TargetFunctionStateTypeID> typeID() const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> schemaVersion() const noexcept;
  [[nodiscard]] Expected<void> verify() const;
  [[nodiscard]] Expected<TargetFunctionState>
  clone(const MachineFunctionCloneContext &CloneContext) const;
  [[nodiscard]] Expected<std::string> print() const;
  [[nodiscard]] Expected<EncodedTargetFunctionState> encode() const;
  [[nodiscard]] bool equals(const TargetFunctionState &Other) const noexcept;

  template <TargetFunctionStateValue ValueT> [[nodiscard]] ValueT *getIf() noexcept {
    if (!hasType<ValueT>())
      return nullptr;
    return &static_cast<Model<ValueT> *>(Storage.get())->value();
  }

  template <TargetFunctionStateValue ValueT> [[nodiscard]] const ValueT *getIf() const noexcept {
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

    [[nodiscard]] virtual TargetFunctionStateTypeID typeID() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t schemaVersion() const noexcept = 0;
    [[nodiscard]] virtual const void *localTypeToken() const noexcept = 0;
    [[nodiscard]] virtual Expected<void> verify() const = 0;
    [[nodiscard]] virtual Expected<std::unique_ptr<Concept>>
    cloneStorage(const MachineFunctionCloneContext &CloneContext) const = 0;
    [[nodiscard]] virtual std::string print() const = 0;
    [[nodiscard]] virtual Expected<std::vector<std::byte>> encodePayload() const = 0;
    [[nodiscard]] virtual bool equals(const Concept &Other) const noexcept = 0;
  };

  template <typename ValueT> [[nodiscard]] static const void *typeToken() noexcept {
    static const std::byte Token{};
    return &Token;
  }

  template <TargetFunctionStateValue ValueT> class Model final : public Concept {
  public:
    explicit Model(ValueT Value) : Value(std::move(Value)) {}

    [[nodiscard]] TargetFunctionStateTypeID typeID() const noexcept override {
      return ValueT::stableTypeID();
    }

    [[nodiscard]] std::uint32_t schemaVersion() const noexcept override {
      return ValueT::schemaVersion();
    }

    [[nodiscard]] const void *localTypeToken() const noexcept override {
      return TargetFunctionState::typeToken<ValueT>();
    }

    [[nodiscard]] Expected<void> verify() const override {
      return Value.verify();
    }

    [[nodiscard]] Expected<std::unique_ptr<Concept>>
    cloneStorage(const MachineFunctionCloneContext &CloneContext) const override {
      auto Cloned = Value.clone(CloneContext);
      if (!Cloned)
        return std::unexpected(Cloned.error());
      std::unique_ptr<Concept> Result = std::make_unique<Model<ValueT>>(std::move(*Cloned));
      return Result;
    }

    [[nodiscard]] std::string print() const override {
      return Value.print();
    }

    [[nodiscard]] Expected<std::vector<std::byte>> encodePayload() const override {
      return Value.encode();
    }

    [[nodiscard]] bool equals(const Concept &Other) const noexcept override {
      if (Other.localTypeToken() != TargetFunctionState::typeToken<ValueT>())
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

  explicit TargetFunctionState(std::unique_ptr<Concept> Storage) : Storage(std::move(Storage)) {}

  template <TargetFunctionStateValue ValueT> [[nodiscard]] bool hasType() const noexcept {
    return Storage != nullptr && Storage->typeID() == ValueT::stableTypeID() &&
           Storage->localTypeToken() == typeToken<ValueT>();
  }

  std::unique_ptr<Concept> Storage;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETFUNCTIONSTATE_H

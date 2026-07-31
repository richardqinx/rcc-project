// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_STABLEID_H
#define RCC_CODEGEN_STABLEID_H

#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>

namespace rcc {

template <typename TagT> class StableID final {
public:
  constexpr StableID() noexcept = default;

  [[nodiscard]] static constexpr StableID fromValue(std::uint64_t Value) noexcept {
    return StableID(Value);
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != 0;
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return Value;
  }

  friend constexpr bool operator==(StableID Left, StableID Right) noexcept = default;

private:
  explicit constexpr StableID(std::uint64_t Value) noexcept : Value(Value) {}

  std::uint64_t Value = 0;
};

namespace detail {

struct MachineFunctionStableIDTag;
struct MachineBlockStableIDTag;
struct MachineInstrStableIDTag;

} // namespace detail

using MachineFunctionStableID = StableID<detail::MachineFunctionStableIDTag>;
using MachineBlockStableID = StableID<detail::MachineBlockStableIDTag>;
using MachineInstrStableID = StableID<detail::MachineInstrStableIDTag>;

template <typename StableIDT> class StableIDGenerator final {
public:
  StableIDGenerator() = default;
  StableIDGenerator(const StableIDGenerator &) = delete;
  StableIDGenerator &operator=(const StableIDGenerator &) = delete;
  StableIDGenerator(StableIDGenerator &&) noexcept = default;
  StableIDGenerator &operator=(StableIDGenerator &&) noexcept = default;

  [[nodiscard]] Expected<StableIDT> allocate() {
    if (NextValue == 0) {
      return makeUnexpected(ErrorCode::InternalError, "stable ID space is exhausted");
    }

    const StableIDT Result = StableIDT::fromValue(NextValue);
    if (NextValue == std::numeric_limits<std::uint64_t>::max())
      NextValue = 0;
    else
      ++NextValue;
    return Result;
  }

private:
  std::uint64_t NextValue = 1;
};

static_assert(sizeof(MachineFunctionStableID) == sizeof(std::uint64_t));
static_assert(sizeof(MachineBlockStableID) == sizeof(std::uint64_t));
static_assert(sizeof(MachineInstrStableID) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<MachineFunctionStableID>);
static_assert(std::is_trivially_copyable_v<MachineBlockStableID>);
static_assert(std::is_trivially_copyable_v<MachineInstrStableID>);

} // namespace rcc

template <typename TagT> struct std::hash<rcc::StableID<TagT>> {
  [[nodiscard]] std::size_t operator()(rcc::StableID<TagT> ID) const noexcept {
    return std::hash<std::uint64_t>{}(ID.value());
  }
};

#endif // RCC_CODEGEN_STABLEID_H

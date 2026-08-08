// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_PBQP_MATH_H
#define RCC_CODEGEN_PBQP_MATH_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace rcc::pbqp {

using Cost = std::int64_t;

inline constexpr Cost InfiniteCost = std::numeric_limits<Cost>::max() / 4;
inline constexpr Cost NegativeInfiniteCost = -InfiniteCost;

[[nodiscard]] constexpr bool isInfinite(Cost Value) noexcept {
  return Value >= InfiniteCost;
}

[[nodiscard]] constexpr Cost addCosts(Cost Left, Cost Right) noexcept {
  if (isInfinite(Left) || isInfinite(Right))
    return InfiniteCost;
  if (Left <= NegativeInfiniteCost || Right <= NegativeInfiniteCost)
    return NegativeInfiniteCost;
  if (Right > 0 && Left > InfiniteCost - Right)
    return InfiniteCost;
  if (Right < 0 && Left < NegativeInfiniteCost - Right)
    return NegativeInfiniteCost;
  return Left + Right;
}

class CostVector final {
public:
  explicit CostVector(std::size_t Size, Cost Initial = 0) : Values(Size, Initial) {}
  explicit CostVector(std::vector<Cost> Values) : Values(std::move(Values)) {}

  [[nodiscard]] std::size_t size() const noexcept {
    return Values.size();
  }
  [[nodiscard]] bool empty() const noexcept {
    return Values.empty();
  }
  [[nodiscard]] Cost &operator[](std::size_t Index) noexcept {
    return Values[Index];
  }
  [[nodiscard]] Cost operator[](std::size_t Index) const noexcept {
    return Values[Index];
  }
  [[nodiscard]] std::span<const Cost> values() const noexcept {
    return Values;
  }

  [[nodiscard]] std::size_t minimumIndex() const noexcept {
    return static_cast<std::size_t>(std::ranges::min_element(Values) - Values.begin());
  }

  void add(const CostVector &Other) noexcept {
    for (std::size_t Index = 0; Index < Values.size(); ++Index)
      Values[Index] = addCosts(Values[Index], Other.Values[Index]);
  }

private:
  std::vector<Cost> Values;
};

class CostMatrix final {
public:
  CostMatrix(std::size_t Rows, std::size_t Columns, Cost Initial = 0)
      : RowCount(Rows), ColumnCount(Columns), Values(Rows * Columns, Initial) {}

  [[nodiscard]] std::size_t rows() const noexcept {
    return RowCount;
  }
  [[nodiscard]] std::size_t columns() const noexcept {
    return ColumnCount;
  }
  [[nodiscard]] bool empty() const noexcept {
    return RowCount == 0 || ColumnCount == 0;
  }

  [[nodiscard]] Cost &at(std::size_t Row, std::size_t Column) noexcept {
    return Values[Row * ColumnCount + Column];
  }
  [[nodiscard]] Cost at(std::size_t Row, std::size_t Column) const noexcept {
    return Values[Row * ColumnCount + Column];
  }

  [[nodiscard]] CostMatrix transpose() const {
    CostMatrix Result(ColumnCount, RowCount);
    for (std::size_t Row = 0; Row < RowCount; ++Row)
      for (std::size_t Column = 0; Column < ColumnCount; ++Column)
        Result.at(Column, Row) = at(Row, Column);
    return Result;
  }

  void add(const CostMatrix &Other) noexcept {
    for (std::size_t Index = 0; Index < Values.size(); ++Index)
      Values[Index] = addCosts(Values[Index], Other.Values[Index]);
  }

private:
  std::size_t RowCount;
  std::size_t ColumnCount;
  std::vector<Cost> Values;
};

} // namespace rcc::pbqp

#endif // RCC_CODEGEN_PBQP_MATH_H

/*
 RCC-Project

 Copyright (C) 2026 Richard Qin

 This file is part of RCC-Project.

 RCC-Project is free software; you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free
 Software Foundation; either version 3 of the License, or (at your option) any
 later version.

 RCC-Project is distributed in the hope that it will be useful, but WITHOUT ANY
 WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License along with
 RCC-Project; see the file COPYING.  If not see
 <http://www.gnu.org/licenses/>.
*/

//===-- MathExtras.cpp - Implement the MathExtras header --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the MathExtras.h header
//
//===----------------------------------------------------------------------===//

#include "rcc/Support/MathExtras.h"

#ifdef _MSC_VER
#include <limits>
#else
#include <cmath>
#endif

namespace rcc
{

#if defined(_MSC_VER)
// Visual Studio defines the HUGE_VAL class of macros using purposeful
// constant arithmetic overflow, which it then warns on when encountered.
const float huge_valf = std::numeric_limits<float>::infinity ();
#else
const float huge_valf = HUGE_VALF;
#endif

/// Returns the number of digits in the given integer.
int
NumDigitsBase10 (uint64_t X)
{
  static constexpr struct ConstexprData
  {
    uint8_t AtLeast[65] = {};
    uint64_t Boundaries[20] = {};
    static constexpr int
    NumDigitsConstexpr (uint64_t N)
    {
      int res = 1;
      while (N >= 10)
      {
        res++;
        N /= 10;
      }
      return res;
    }
    constexpr ConstexprData ()
    {
      uint64_t Val = ~0ull;
      for (uint64_t i = 0; i <= 64; i++)
      {
        uint64_t Digits = NumDigitsConstexpr (Val) - 1;
        AtLeast[i] = Digits;
        Val >>= 1;
      }
      // Special case because X=0 should return 1 and not 0
      Boundaries[0] = 0;
      Val = 10;
      for (uint64_t i = 1; i < 20; i++)
      {
        Boundaries[i] = Val;
        Val *= 10;
      }
    }
  } Data;

  uint64_t Base2 = X ? std::countl_zero (X) : 64;
  uint64_t Digits = Data.AtLeast[Base2];
  return Digits + (X >= Data.Boundaries[Digits]);
}

} // namespace rcc

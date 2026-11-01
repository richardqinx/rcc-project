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

//===- MemAlloc.cpp - Memory allocation functions -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "core/memory/MemAlloc.h"
#include <new>

// These are out of line to have __cpp_aligned_new not affect ABI.

LIBBASE_ATTRIBUTE_RETURNS_NONNULL LIBBASE_ATTRIBUTE_RETURNS_NOALIAS void *
rcc::allocate_buffer (size_t Size, size_t Alignment)
{
  void *Result = ::operator new (Size,
#ifdef __cpp_aligned_new
                                 std::align_val_t (Alignment),
#endif
                                 std::nothrow);
  if (Result == nullptr)
  {
    report_bad_alloc_error ("Buffer allocation failed");
  }
  return Result;
}

void
rcc::deallocate_buffer (void *Ptr, size_t Size, size_t Alignment)
{
  ::operator delete (Ptr
#ifdef __cpp_sized_deallocation
                     ,
                     Size
#endif
#ifdef __cpp_aligned_new
                     ,
                     std::align_val_t (Alignment)
#endif
  );
}

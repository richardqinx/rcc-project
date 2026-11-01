/*
 RCC-Project

 Copyright (C) 2026 Richard Qin

 This file is part of RCC-Project.

 RCC-Project is free software; you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free
 Software Foundation; either version 3 of the License, or (at your option) any
 later version.

 RCC-Project is distributed in the hope that it will be useful, but WITHOUT ANY
 WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 PARTICULAR PURPOSE. See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License along with
 RCC-Project; see the file COPYING. If not see
 <http://www.gnu.org/licenses/>.
 */

//===- AllocatorBase.h - Simple memory allocation abstraction ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file defines MallocAllocator. MallocAllocator conforms to the LLVM
/// "Allocator" concept which consists of an Allocate method accepting a size
/// and alignment, and a Deallocate accepting a pointer and size. Further, the
/// LLVM "Allocator" concept has overloads of Allocate and Deallocate for
/// setting size and alignment based on the final type. These overloads are
/// typically provided by a base class template \c AllocatorBase.
///
//===----------------------------------------------------------------------===//

#ifndef LIBBASE_SUPPORT_ALLOCATORBASE_H
#define LIBBASE_SUPPORT_ALLOCATORBASE_H

#ifdef _MSC_VER
#define LIBBASE_ALLOCATORHOLDER_EMPTYBASE __declspec(empty_bases)
#else
#define LIBBASE_ALLOCATORHOLDER_EMPTYBASE
#endif // _MSC_VER

#include "core/compiler/Compiler.h"
#include "core/memory/MemAlloc.h"
#include <type_traits>
#include <utility>

namespace rcc {

/// CRTP base class providing obvious overloads for the core \c
/// Allocate() methods of LLVM-style allocators.
///
/// This base class both documents the full public interface exposed by all
/// LLVM-style allocators, and redirects all of the overloads to a single core
/// set of methods which the derived class must define.
template <typename DerivedT> class AllocatorBase {
public:
  /// Allocate \a Size bytes of \a Alignment aligned memory. This method
  /// must be implemented by \c DerivedT.
  void *Allocate(size_t Size, size_t Alignment) {
#ifdef __clang__
    static_assert(static_cast<void *(AllocatorBase::*)(size_t, size_t)>(
                      &AllocatorBase::Allocate) !=
                      static_cast<void *(DerivedT::*)(size_t, size_t)>(
                          &DerivedT::Allocate),
                  "Class derives from AllocatorBase without implementing the "
                  "core Allocate(size_t, size_t) overload!");
#endif
    return static_cast<DerivedT *>(this)->Allocate(Size, Alignment);
  }

  /// Deallocate \a Ptr to \a Size bytes of memory allocated by this
  /// allocator.
  void Deallocate(const void *Ptr, size_t Size, size_t Alignment) {
#ifdef __clang__
    static_assert(
        static_cast<void (AllocatorBase::*)(const void *, size_t, size_t)>(
            &AllocatorBase::Deallocate) !=
            static_cast<void (DerivedT::*)(const void *, size_t, size_t)>(
                &DerivedT::Deallocate),
        "Class derives from AllocatorBase without implementing the "
        "core Deallocate(void *) overload!");
#endif
    return static_cast<DerivedT *>(this)->Deallocate(Ptr, Size, Alignment);
  }

  // The rest of these methods are helpers that redirect to one of the above
  // core methods.

  /// Allocate space for a sequence of objects without constructing them.
  template <typename T> T *Allocate(size_t Num = 1) {
    return static_cast<T *>(Allocate(Num * sizeof(T), alignof(T)));
  }

  /// Deallocate space for a sequence of objects without constructing them.
  template <typename T>
  std::enable_if_t<!std::is_same_v<std::remove_cv_t<T>, void>, void>
  Deallocate(T *Ptr, size_t Num = 1) {
    Deallocate(static_cast<const void *>(Ptr), Num * sizeof(T), alignof(T));
  }
};

class MallocAllocator : public AllocatorBase<MallocAllocator> {
public:
  void Reset() {}

  LIBBASE_ATTRIBUTE_RETURNS_NONNULL void *Allocate(size_t Size, size_t Alignment) {
    return allocate_buffer(Size, Alignment);
  }

  // Pull in base class overloads.
  using AllocatorBase<MallocAllocator>::Allocate;

  void Deallocate(const void *Ptr, size_t Size, size_t Alignment) {
    deallocate_buffer(const_cast<void *>(Ptr), Size, Alignment);
  }

  // Pull in base class overloads.
  using AllocatorBase<MallocAllocator>::Deallocate;

  void PrintStats() const {}
};

namespace detail {

template <typename Alloc> class AllocatorHolder : Alloc {
public:
  AllocatorHolder() = default;
  AllocatorHolder(const Alloc &A) : Alloc(A) {}
  AllocatorHolder(Alloc &&A) : Alloc(std::move(A)) {}
  Alloc &getAllocator() { return *this; }
  const Alloc &getAllocator() const { return *this; }
};

template <typename Alloc> class AllocatorHolder<Alloc &> {
  Alloc &A;

public:
  AllocatorHolder(Alloc &A) : A(A) {}
  Alloc &getAllocator() { return A; }
  const Alloc &getAllocator() const { return A; }
};

} // namespace detail

} // namespace rcc

#endif // LIBBASE_SUPPORT_ALLOCATORBASE_H

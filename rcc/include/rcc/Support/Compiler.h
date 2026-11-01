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

//===-- rcc/Support/Compiler.h - RCC ABI support ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef RCC_SUPPORT_COMPILER_H
#define RCC_SUPPORT_COMPILER_H

#include "core/compiler/Compiler.h"
#include "rcc/Config/rcc-config.h"

/// RCC_EXTERNAL_VISIBILITY - classes, functions, and variables marked with
/// this attribute will be made public and visible outside of a shared library.
#if defined(RCC_BUILD_RCC_DYLIB) || defined(RCC_BUILD_SHARED_LIBS)
#define RCC_EXTERNAL_VISIBILITY LIBBASE_ATTRIBUTE_VISIBILITY_DEFAULT
#else
#define RCC_EXTERNAL_VISIBILITY
#endif

/// RCC_LIBRARY_VISIBILITY - Mark symbols private to the RCC shared library.
#if (!(defined(_WIN32) || defined(__CYGWIN__)) ||                            \
     ((defined(__MINGW32__) || defined(__CYGWIN__)) && defined(__clang__)))
#define RCC_LIBRARY_VISIBILITY LIBBASE_ATTRIBUTE_VISIBILITY_HIDDEN
#if defined(__clang__) && __clang_major__ < 15
#define RCC_LIBRARY_VISIBILITY_NAMESPACE [[gnu::visibility("hidden")]]
#else
#define RCC_LIBRARY_VISIBILITY_NAMESPACE LIBBASE_ATTRIBUTE_VISIBILITY_HIDDEN
#endif
#define RCC_ALWAYS_EXPORT LIBBASE_ATTRIBUTE_VISIBILITY_DEFAULT
#elif defined(_WIN32)
#define RCC_ALWAYS_EXPORT __declspec(dllexport)
#define RCC_LIBRARY_VISIBILITY
#define RCC_LIBRARY_VISIBILITY_NAMESPACE
#else
#define RCC_LIBRARY_VISIBILITY
#define RCC_ALWAYS_EXPORT
#define RCC_LIBRARY_VISIBILITY_NAMESPACE
#endif

/// RCC_ABI is the main export macro for the RCC public interface.
/// RCC_ABI_EXPORT always exports a symbol from the defining library.
/// RCC_EXPORT_TEMPLATE marks explicit template instantiations.
/// RCC_TEMPLATE_ABI marks extern template declarations.
#ifndef RCC_ABI_GENERATING_ANNOTATIONS
#define RCC_ABI_NOT_EXPORTED
#if defined(RCC_ENABLE_RCC_EXPORT_ANNOTATIONS) && !defined(RCC_BUILD_STATIC)
#if defined(_WIN32) && !defined(__MINGW32__)
#if defined(RCC_EXPORTS)
#define RCC_ABI __declspec(dllexport)
#define RCC_TEMPLATE_ABI
#define RCC_EXPORT_TEMPLATE __declspec(dllexport)
#else
#define RCC_ABI __declspec(dllimport)
#define RCC_TEMPLATE_ABI __declspec(dllimport)
#define RCC_EXPORT_TEMPLATE
#endif
#define RCC_ABI_EXPORT __declspec(dllexport)
#elif __has_attribute(visibility)
#if defined(__ELF__) || defined(__MINGW32__) || defined(_AIX) ||               \
    defined(__MVS__) || defined(__CYGWIN__)
#define RCC_ABI LIBBASE_ATTRIBUTE_VISIBILITY_DEFAULT
#define RCC_TEMPLATE_ABI RCC_ABI
#define RCC_EXPORT_TEMPLATE
#define RCC_ABI_EXPORT RCC_ABI
#elif defined(__MACH__) || defined(__wasm__) || defined(__EMSCRIPTEN__)
#define RCC_ABI LIBBASE_ATTRIBUTE_VISIBILITY_DEFAULT
#define RCC_TEMPLATE_ABI
#define RCC_EXPORT_TEMPLATE
#define RCC_ABI_EXPORT RCC_ABI
#endif
#endif
#endif
#if !defined(RCC_ABI)
#define RCC_ABI
#define RCC_TEMPLATE_ABI
#define RCC_EXPORT_TEMPLATE
#define RCC_ABI_EXPORT
#endif
#define RCC_ABI_FOR_TEST RCC_ABI
#endif

#endif

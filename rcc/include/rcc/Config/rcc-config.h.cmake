/*===------- rcc/Config/rcc-config.h - RCC configuration -------*- C -*-===*/
/*                                                                            */
/* Part of the LLVM Project, under the Apache License v2.0 with LLVM          */
/* Exceptions.                                                                */
/* See https://llvm.org/LICENSE.txt for license information.                  */
/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception                    */
/*                                                                            */
/*===----------------------------------------------------------------------===*/

/* This file enumerates variables from the RCC configuration so that they
   can be in exported headers and won't override package specific directives.
   This is a C header that can be included in the RCC C headers. */

#ifndef RCC_CONFIG_H
#define RCC_CONFIG_H

/* Define if RCC_ENABLE_DUMP is enabled */
#cmakedefine RCC_ENABLE_DUMP

/* Target triple RCC will generate code for by default */
/* Doesn't use `cmakedefine` because it is allowed to be empty. */
#define RCC_DEFAULT_TARGET_TRIPLE "${RCC_DEFAULT_TARGET_TRIPLE}"

/* Define if threads enabled */
#cmakedefine01 RCC_ENABLE_THREADS

/* Has gcc/MSVC atomic intrinsics */
#cmakedefine01 RCC_HAS_ATOMICS

/* Host triple RCC will be executed on */
#cmakedefine RCC_HOST_TRIPLE "${RCC_HOST_TRIPLE}"

/* RCC architecture name for the native architecture, if available */
#cmakedefine RCC_NATIVE_ARCH ${RCC_NATIVE_ARCH}

/* RCC name for the native AsmParser init function, if available */
#cmakedefine RCC_NATIVE_ASMPARSER RCCInitialize${RCC_NATIVE_ARCH}AsmParser

/* RCC name for the native AsmPrinter init function, if available */
#cmakedefine RCC_NATIVE_ASMPRINTER RCCInitialize${RCC_NATIVE_ARCH}AsmPrinter

/* RCC name for the native Disassembler init function, if available */
#cmakedefine RCC_NATIVE_DISASSEMBLER RCCInitialize${RCC_NATIVE_ARCH}Disassembler

/* RCC name for the native Target init function, if available */
#cmakedefine RCC_NATIVE_TARGET RCCInitialize${RCC_NATIVE_ARCH}Target

/* RCC name for the native TargetInfo init function, if available */
#cmakedefine RCC_NATIVE_TARGETINFO RCCInitialize${RCC_NATIVE_ARCH}TargetInfo

/* RCC name for the native target MC init function, if available */
#cmakedefine RCC_NATIVE_TARGETMC RCCInitialize${RCC_NATIVE_ARCH}TargetMC

/* RCC name for the native target MCA init function, if available */
#cmakedefine RCC_NATIVE_TARGETMCA RCCInitialize${RCC_NATIVE_ARCH}TargetMCA

/* Define if this is Unixish platform */
#cmakedefine RCC_ON_UNIX ${RCC_ON_UNIX}

/* Define if we have the Intel JIT API runtime support library */
#cmakedefine01 RCC_USE_INTEL_JITEVENTS

/* Define if we have the oprofile JIT-support library */
#cmakedefine01 RCC_USE_OPROFILE

/* Define if we have the perf JIT-support library */
#cmakedefine01 RCC_USE_PERF

/* Major version of the RCC API */
#define RCC_VERSION_MAJOR ${RCC_VERSION_MAJOR}

/* Minor version of the RCC API */
#define RCC_VERSION_MINOR ${RCC_VERSION_MINOR}

/* Patch version of the RCC API */
#define RCC_VERSION_PATCH ${RCC_VERSION_PATCH}

/* RCC version string */
#define RCC_VERSION_STRING "${PACKAGE_VERSION}"

/* Whether RCC records statistics for use with GetStatistics(),
 * PrintStatistics() or PrintStatisticsJSON()
 */
#cmakedefine01 RCC_FORCE_ENABLE_STATS

/* Define if we have z3 and want to build it */
#cmakedefine RCC_WITH_Z3 ${RCC_WITH_Z3}

/* Define if we have curl and want to use it */
#cmakedefine RCC_ENABLE_CURL ${RCC_ENABLE_CURL}

/* Define if we have cpp-httplib and want to use it */
#cmakedefine RCC_ENABLE_HTTPLIB ${RCC_ENABLE_HTTPLIB}

/* Define if zlib compression is available */
#cmakedefine01 RCC_ENABLE_ZLIB

/* Define if zstd compression is available */
#cmakedefine01 RCC_ENABLE_ZSTD

/* Define if xz (lzma) decompression is available */
#cmakedefine01 RCC_ENABLE_LZMA

/* Define if RCC is using tflite */
#cmakedefine RCC_HAVE_TFLITE

/* Define if we want to check profile consistency in lit tests */
#cmakedefine RCC_ENABLE_PROFCHECK

/* Define to 1 if you have the <sysexits.h> header file. */
#cmakedefine HAVE_SYSEXITS_H ${HAVE_SYSEXITS_H}

/* Define if building libRCC shared library */
#cmakedefine RCC_BUILD_RCC_DYLIB

/* Define if building RCC with BUILD_SHARED_LIBS */
#cmakedefine RCC_BUILD_SHARED_LIBS

/* Define if exporting RCC public interface for shared library */
#cmakedefine RCC_ENABLE_RCC_EXPORT_ANNOTATIONS

/* Define if exporting RCC-C public interface for shared library */
#cmakedefine RCC_ENABLE_RCC_C_EXPORT_ANNOTATIONS

/* Define if building RCC with RCC_FORCE_USE_OLD_TOOLCHAIN_LIBS */
#cmakedefine RCC_FORCE_USE_OLD_TOOLCHAIN ${RCC_FORCE_USE_OLD_TOOLCHAIN}

/* Define if rcc_unreachable should be optimized with undefined behavior
 * in non assert builds */
#cmakedefine01 RCC_UNREACHABLE_OPTIMIZE

/* Define if building RCC with RCC_ENABLE_IO_SANDBOX */
#cmakedefine01 RCC_ENABLE_IO_SANDBOX

/* Define to 1 if you have the DIA SDK installed, and to 0 if you don't. */
#cmakedefine01 RCC_ENABLE_DIA_SDK

/* Define if plugins enabled */
#cmakedefine RCC_ENABLE_PLUGINS

/* Define if logf128 is available */
#cmakedefine RCC_HAS_LOGF128

/* Define if building RCC with RCC_ENABLE_TELEMETRY */
#cmakedefine01 RCC_ENABLE_TELEMETRY

/* Define to 1 to enable expensive checks for debug location coverage checking,
   and to 0 otherwise. */
#cmakedefine01 RCC_ENABLE_DEBUGLOC_TRACKING_COVERAGE

/* Define to 1 to enable expensive tracking of the origin of debug location
   coverage bugs, and to 0 otherwise. */
#cmakedefine01 RCC_ENABLE_DEBUGLOC_TRACKING_ORIGIN

/* Define to 1 to enable RCC OnDisk Content Addressable Storage */
#cmakedefine01 RCC_ENABLE_ONDISK_CAS

#endif

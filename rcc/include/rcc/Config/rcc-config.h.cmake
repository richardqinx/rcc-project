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

/* Target triple RCC will generate code for by default */
/* Doesn't use `cmakedefine` because it is allowed to be empty. */
#define RCC_DEFAULT_TARGET_TRIPLE "${RCC_DEFAULT_TARGET_TRIPLE}"

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

/* Define if RCC is using tflite */
#cmakedefine RCC_HAVE_TFLITE

/* Define if we want to check profile consistency in lit tests */
#cmakedefine RCC_ENABLE_PROFCHECK

/* Define if building libRCC shared library */
#cmakedefine RCC_BUILD_RCC_DYLIB

/* Define if exporting RCC-C public interface for shared library */
#cmakedefine RCC_ENABLE_RCC_C_EXPORT_ANNOTATIONS

/* Define to 1 if you have the DIA SDK installed, and to 0 if you don't. */
#cmakedefine01 RCC_ENABLE_DIA_SDK

/* Define if plugins enabled */
#cmakedefine RCC_ENABLE_PLUGINS

/* Define if building RCC with RCC_ENABLE_TELEMETRY */
#cmakedefine01 RCC_ENABLE_TELEMETRY

/* Define to 1 to enable expensive checks for debug location coverage checking,
   and to 0 otherwise. */
#cmakedefine01 RCC_ENABLE_DEBUGLOC_TRACKING_COVERAGE

/* Define to 1 to enable expensive tracking of the origin of debug location
   coverage bugs, and to 0 otherwise. */
#cmakedefine01 RCC_ENABLE_DEBUGLOC_TRACKING_ORIGIN

#endif

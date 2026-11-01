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

/* This file enumerates variables from the libbase configuration so that they
   can be used by exported headers without depending on RCC configuration. */

#ifndef LIBBASE_CONFIG_H
#define LIBBASE_CONFIG_H

/* Define if libbase dump support is enabled. */
#cmakedefine LIBBASE_ENABLE_DUMP

/* Define if threads are enabled in libbase. */
#cmakedefine01 LIBBASE_ENABLE_THREADS

/* Has gcc/MSVC atomic intrinsics. */
#cmakedefine01 LIBBASE_HAS_ATOMICS

/* Define if this is a Unixish platform. */
#cmakedefine LIBBASE_ON_UNIX ${LIBBASE_ON_UNIX}

/* Whether libbase records statistics. */
#cmakedefine01 LIBBASE_FORCE_ENABLE_STATS

/* Define if libbase has z3 support. */
#cmakedefine LIBBASE_WITH_Z3 ${LIBBASE_WITH_Z3}

/* Define if libbase has curl support. */
#cmakedefine LIBBASE_ENABLE_CURL ${LIBBASE_ENABLE_CURL}

/* Define if libbase has cpp-httplib support. */
#cmakedefine LIBBASE_ENABLE_HTTPLIB ${LIBBASE_ENABLE_HTTPLIB}

/* Define if zlib compression is available to libbase. */
#cmakedefine01 LIBBASE_ENABLE_ZLIB

/* Define if zstd compression is available to libbase. */
#cmakedefine01 LIBBASE_ENABLE_ZSTD

/* Define if xz (lzma) decompression is available to libbase. */
#cmakedefine01 LIBBASE_ENABLE_LZMA

/* Define to 1 if libbase has the <sysexits.h> header file. */
#cmakedefine LIBBASE_HAVE_SYSEXITS_H ${LIBBASE_HAVE_SYSEXITS_H}

/* Define if libbase is built with BUILD_SHARED_LIBS. */
#cmakedefine LIBBASE_BUILD_SHARED_LIBS

/* Define if libbase is built as a static library. */
#cmakedefine LIBBASE_BUILD_STATIC

/* Define if exporting the libbase public interface for a shared library. */
#cmakedefine LIBBASE_ENABLE_EXPORT_ANNOTATIONS

/* Define if libbase uses the old toolchain behavior. */
#cmakedefine LIBBASE_FORCE_USE_OLD_TOOLCHAIN ${LIBBASE_FORCE_USE_OLD_TOOLCHAIN}

/* Define if libbase_unreachable should be optimized with undefined behavior
 * in non-assert builds. */
#cmakedefine01 LIBBASE_UNREACHABLE_OPTIMIZE

/* Define if libbase provides I/O sandbox support. */
#cmakedefine01 LIBBASE_ENABLE_IO_SANDBOX

/* Define if logf128 is available to libbase. */
#cmakedefine LIBBASE_HAS_LOGF128

/* Define if libbase provides on-disk content-addressable storage. */
#cmakedefine01 LIBBASE_ENABLE_ONDISK_CAS

#endif

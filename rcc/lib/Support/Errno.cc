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

// This implementation is based on llvm/lib/Support/Errno.cpp.

#include "rcc/Support/Errno.h"
#include "rcc/Config/config.h"

#include <cstring>
#include <errno.h>

namespace rcc {
namespace sys {

std::string
StrError ()
{
  return StrError (errno);
}

std::string
StrError (int errnum)
{
  std::string str;
  if (errnum == 0)
  {
    return str;
  }

#if defined(HAVE_STRERROR_R) || HAVE_DECL_STRERROR_S
  const int MaxErrStrLen = 2000;
  char buffer[MaxErrStrLen];
  buffer[0] = '\0';
#endif

#ifdef HAVE_STRERROR_R
  // strerror_r is thread-safe.
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
  // glibc defines its own incompatible version of strerror_r which may not
  // use the buffer supplied.
  str = strerror_r (errnum, buffer, MaxErrStrLen - 1);
#else
  strerror_r (errnum, buffer, MaxErrStrLen - 1);
  str = buffer;
#endif
#elif HAVE_DECL_STRERROR_S
  // Windows Secure API.
  strerror_s (buffer, MaxErrStrLen - 1, errnum);
  str = buffer;
#else
  // Copy the thread-unsafe result of strerror into the buffer as fast as
  // possible to minimize the impact of collisions in multiple threads.
  str = strerror (errnum);
#endif
  return str;
}

} // namespace sys
} // namespace rcc

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

// This implementation is based on llvm/include/llvm/Support/Errno.h.

#ifndef RCC_SUPPORT_ERRNO_H
#define RCC_SUPPORT_ERRNO_H

#include "CoreLib/Compiler/Compiler.h"

#include <cerrno>
#include <string>

namespace rcc
{
namespace sys
{

/// Returns a string representation of the errno value, using whatever
/// thread-safe variant of strerror() is available. Be sure to call this
/// immediately after the function that set errno, or errno may have been
/// overwritten by an intervening call.
RCC_ABI std::string
StrError ();

/// Like the no-argument version above, but uses \p errnum instead of errno.
RCC_ABI std::string
StrError (int errnum);

template <typename FailT, typename Fun, typename... Args>
inline decltype (auto)
RetryAfterSignal (const FailT &Fail, const Fun &F, const Args &...As)
{
  decltype (F (As...)) Res;
  do
  {
    errno = 0;
    Res = F (As...);
  } while (Res == Fail && errno == EINTR);
  return Res;
}

} // namespace sys
} // namespace rcc

#endif // RCC_SUPPORT_ERRNO_H

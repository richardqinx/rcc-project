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

#include "bios.h"

extern u8 __bss_start[];
extern u8 __bss_end[];

static void
zero_bytes (volatile u8 *start, volatile u8 *end)
{
  volatile u8 *cursor;

  for (cursor = start; cursor < end; ++cursor)
    {
      *cursor = 0;
    }
}

void
memory_init (void)
{
  zero_bytes ((volatile u8 *)MUBIOS_IVT_BASE,
              (volatile u8 *)(MUBIOS_IVT_BASE + MUBIOS_IVT_SIZE));

  zero_bytes ((volatile u8 *)MUBIOS_BDA_BASE,
              (volatile u8 *)(MUBIOS_BDA_BASE + MUBIOS_BDA_SIZE));

  zero_bytes (__bss_start, __bss_end);
}

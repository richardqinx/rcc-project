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
#include "regs.h"

void
bios_service_dispatch (u8 vector, struct bios_regs *regs)
{
  switch (vector)
    {
    case 0x10u:
      int10_service (regs);
      break;

    case 0x14u:
      int14_service (regs);
      break;

    case 0x13u:
      int13_service (regs);
      break;

    case 0x15u:
      int15_service (regs);
      break;

    case 0x16u:
      int16_service (regs);
      break;

    default:
      regs->eax = 0x0100u;
      regs->flags |= 0x0001u;
      break;
    }
}

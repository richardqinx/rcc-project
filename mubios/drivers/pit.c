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
#include "io.h"

#define PIT_COUNTER0 0x0040u
#define PIT_COMMAND 0x0043u

#define PIT_SELECT_CHANNEL0 0x00u
#define PIT_ACCESS_WORD 0x30u
#define PIT_MODE_2 0x04u
#define PIT_BINARY 0x00u

#define PIT_COMMAND_CHANNEL0_MODE2                                            \
  (PIT_SELECT_CHANNEL0 | PIT_ACCESS_WORD | PIT_MODE_2 | PIT_BINARY)

void
pit_init (void)
{
  io_out8 (PIT_COMMAND, PIT_COMMAND_CHANNEL0_MODE2);

  /*
   * A divisor of zero represents 65536.
   */
  io_out8 (PIT_COUNTER0, 0x00u);
  io_out8 (PIT_COUNTER0, 0x00u);
}

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

#define PIC1_COMMAND 0x0020u
#define PIC1_DATA 0x0021u
#define PIC2_COMMAND 0x00A0u
#define PIC2_DATA 0x00A1u

#define PIC_ICW1_INIT 0x11u
#define PIC_ICW4_8086 0x01u

#define PIC1_VECTOR_BASE 0x08u
#define PIC2_VECTOR_BASE 0x70u

#define PIC1_CASCADE_ID 0x04u
#define PIC2_CASCADE_ID 0x02u

#define PIC1_MASK_DEFAULT 0xF8u
#define PIC2_MASK_DEFAULT 0xFFu

void
pic_init (void)
{
  io_out8 (PIC1_COMMAND, PIC_ICW1_INIT);
  io_out8 (PIC2_COMMAND, PIC_ICW1_INIT);

  io_out8 (PIC1_DATA, PIC1_VECTOR_BASE);
  io_out8 (PIC2_DATA, PIC2_VECTOR_BASE);

  io_out8 (PIC1_DATA, PIC1_CASCADE_ID);
  io_out8 (PIC2_DATA, PIC2_CASCADE_ID);

  io_out8 (PIC1_DATA, PIC_ICW4_8086);
  io_out8 (PIC2_DATA, PIC_ICW4_8086);

  /*
   * Keep the timer, keyboard, and cascade lines enabled.
   * All other master IRQ lines remain masked.
   */
  io_out8 (PIC1_DATA, PIC1_MASK_DEFAULT);
  io_out8 (PIC2_DATA, PIC2_MASK_DEFAULT);
}

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

extern u8 __ebda_start[];

static void
store_u16 (volatile u8 *address, u16 value)
{
  address[0] = (u8)(value & 0x00FFu);
  address[1] = (u8)(value >> 8);
}

void
bda_init (void)
{
  volatile u8 *bda;
  u32 ebda_base;

  bda = (volatile u8 *)MUBIOS_BDA_BASE;
  ebda_base = (u32)__ebda_start;

  store_u16 (bda + BDA_EBDA_SEGMENT_OFFSET, (u16)(ebda_base >> 4));

  store_u16 (bda + BDA_EQUIPMENT_OFFSET, (u16)(ebda_base >> 4));

  store_u16 (bda + BDA_CONVENTIONAL_MEMORY_OFFSET, (u16)(ebda_base >> 10));
}

struct ivt_entry
{
  u16 offset;
  u16 segment;
};

extern u8 __rom_start[];
extern void irq0_entry (void);
extern void irq1_entry (void);

extern void int1a_entry (void);
extern void int10_entry (void);
extern void int13_entry (void);

static void
install_ivt_entry (u8 vector, void (*handler) (void))
{
  volatile struct ivt_entry *ivt;
  u32 rom_base;
  u32 handler_address;

  ivt = (volatile struct ivt_entry *)MUBIOS_IVT_BASE;
  rom_base = (u32)__rom_start;
  handler_address = (u32)handler;

  ivt[vector].offset = (u16)(handler_address - rom_base);
  ivt[vector].segment = (u16)(rom_base >> 4);
}

void
ivt_init (void)
{
  install_ivt_entry (0x08u, irq0_entry);
  install_ivt_entry (0x09u, irq1_entry);
  install_ivt_entry (0x1Au, int1a_entry);
  install_ivt_entry (0x10u, int10_entry);
  install_ivt_entry (0x13u, int13_entry);
}

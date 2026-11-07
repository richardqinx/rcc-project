/*
 RCC-Project

 Copyright (C) 2026 Richard Qin

 This file is part of RCC-Project.

 RCC-Project is free software; you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free
 Software Foundation; either version 3 of the License, or (at your option)
 any later version.

 RCC-Project is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 more details.
 */

#include "bios.h"
#include "regs.h"

#define BIOS_CF 0x0001u
#define E820_SIGNATURE 0x534D4150u
#define E820_FUNCTION 0x0000E820u
#define E820_ENTRY_SIZE 20u

struct e820_entry
{
  u32 base_low;
  u32 base_high;
  u32 length_low;
  u32 length_high;
  u32 type;
} __attribute__ ((packed));

static const struct e820_entry e820_map[]
    = { { 0x00000000u, 0x00000000u, 0x0009C000u, 0x00000000u, 1u },
        { 0x0009C000u, 0x00000000u, 0x00004000u, 0x00000000u, 2u },
        { 0x000A0000u, 0x00000000u, 0x00060000u, 0x00000000u, 2u },
        { 0x00100000u, 0x00000000u, 0x07F00000u, 0x00000000u, 1u } };

#define E820_ENTRY_COUNT ((u32)(sizeof (e820_map) / sizeof (e820_map[0])))

static void
int15_fail (struct bios_regs *regs)
{
  regs->eax = 0x8600u;
  regs->flags |= BIOS_CF;
}

void
int15_service (struct bios_regs *regs)
{
  struct e820_entry *output;
  u32 index;
  u32 address;

  if (regs->eax != E820_FUNCTION || regs->edx != E820_SIGNATURE
      || regs->ecx < E820_ENTRY_SIZE)
    {
      int15_fail (regs);
      return;
    }

  index = regs->ebx;

  if (index >= E820_ENTRY_COUNT)
    {
      int15_fail (regs);
      return;
    }

  address = ((u32)regs->es << 4) + (u16)regs->edi;
  output = (struct e820_entry *)address;

  *output = e820_map[index];

  regs->eax = E820_SIGNATURE;
  regs->ecx = E820_ENTRY_SIZE;

  index++;
  regs->ebx = index < E820_ENTRY_COUNT ? index : 0u;
  regs->flags &= (u16)~BIOS_CF;
}

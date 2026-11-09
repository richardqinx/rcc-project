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
        { 0x00100000u, 0x00000000u, 0x00000000u, 0x00000000u, 1u } };

#define E820_ENTRY_COUNT ((u32)(sizeof (e820_map) / sizeof (e820_map[0])))
#define E820_MEMORY_BASE 0x00100000u
#define E820_FALLBACK_END 0x08000000u
#define CMOS_EXTENDED_MEMORY_LOW_16M 0x30u
#define CMOS_EXTENDED_MEMORY_HIGH_16M 0x31u
#define CMOS_EXTENDED_MEMORY_LOW 0x34u
#define CMOS_EXTENDED_MEMORY_HIGH 0x35u

static u32
get_memory_end (void)
{
  u16 units;
  u16 extended_kib;
  u32 memory_end;

  units = (u16)cmos_read8 (CMOS_EXTENDED_MEMORY_LOW)
          | (u16)((u16)cmos_read8 (CMOS_EXTENDED_MEMORY_HIGH) << 8);
  if (units != 0u)
    {
      memory_end = 0x01000000u + ((u32)units << 16);
    }
  else
    {
      extended_kib = (u16)cmos_read8 (CMOS_EXTENDED_MEMORY_LOW_16M)
                     | (u16)((u16)cmos_read8 (CMOS_EXTENDED_MEMORY_HIGH_16M)
                             << 8);
      if (extended_kib != 0u)
        memory_end = E820_MEMORY_BASE + ((u32)extended_kib << 10);
      else
        memory_end = E820_FALLBACK_END;
    }

  return memory_end;
}

static void
int15_fail (struct bios_regs *regs)
{
  regs->eax = 0x8600u;
  regs->flags |= BIOS_CF;
}

void
int15_service (struct bios_regs *regs)
{
  struct e820_entry entry;
  struct e820_entry *output;
  u32 index;
  u32 address;
  u32 memory_end;

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

  entry = e820_map[index];
  if (index == 3u)
    {
      memory_end = get_memory_end ();
      entry.length_low = memory_end - E820_MEMORY_BASE;
    }

  *output = entry;

  regs->eax = E820_SIGNATURE;
  regs->ecx = E820_ENTRY_SIZE;

  index++;
  regs->ebx = index < E820_ENTRY_COUNT ? index : 0u;
  regs->flags &= (u16)~BIOS_CF;
}

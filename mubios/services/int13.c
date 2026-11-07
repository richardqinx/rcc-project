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

#define BIOS_CF 0x0001u

struct edd_packet
{
  u8 size;
  u8 reserved;
  u16 count;
  u16 buffer_offset;
  u16 buffer_segment;
  u32 lba_low;
  u32 lba_high;
} __attribute__ ((packed));

static void
int13_fail (struct bios_regs *regs)
{
  regs->ax = 0x0100u;
  regs->flags |= BIOS_CF;
}

static struct edd_packet *
get_edd_packet (struct bios_regs *regs)
{
  u32 address;

  address = ((u32)regs->ds << 4) + regs->si;
  return (struct edd_packet *)address;
}

void
int13_service (struct bios_regs *regs)
{
  u8 function;
  struct edd_packet *packet;
  u32 buffer_address;

  function = (u8)(regs->ax >> 8);

  if (function == 0x41u)
    {
      if (regs->bx != 0x55AAu)
        {
          int13_fail (regs);
          return;
        }

      regs->ax = 0x3000u;
      regs->bx = 0xAA55u;
      regs->cx |= 0x0001u;
      regs->flags &= (u16)~BIOS_CF;
      return;
    }

  if (function == 0x42u)
    {
      packet = get_edd_packet (regs);

      if (packet->size != 0x10u || packet->count != 1u
          || packet->lba_high != 0u || packet->lba_low > 0x0FFFFFFFu)
        {
          int13_fail (regs);
          return;
        }

      buffer_address
          = ((u32)packet->buffer_segment << 4) + packet->buffer_offset;

      if (ata_read_sector28 (packet->lba_low, (u16 *)buffer_address) != 0)
        {
          int13_fail (regs);
          return;
        }

      regs->ax = 0;
      regs->flags &= (u16)~BIOS_CF;
      return;
    }

  int13_fail (regs);
}

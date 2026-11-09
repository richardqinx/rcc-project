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
#define BIOS_HARD_DISK 0x0080u
#define CHS_MAX_CYLINDER 0x03FFu

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

struct edd_parameters
{
  u16 size;
  u16 flags;
  u32 cylinders_low;
  u32 cylinders_high;
  u32 heads;
  u32 sectors_per_track;
  u32 total_sectors_low;
  u32 total_sectors_high;
  u16 bytes_per_sector;
} __attribute__ ((packed));

struct ata_geometry
{
  u16 cylinders;
  u16 heads;
  u16 sectors_per_track;
};

static void
int13_fail (struct bios_regs *regs)
{
  regs->eax = 0x0100u;
  regs->flags |= BIOS_CF;
}

static struct edd_packet *
get_edd_packet (struct bios_regs *regs)
{
  u32 address;

  address = ((u32)regs->ds << 4) + (u16)regs->esi;
  return (struct edd_packet *)address;
}

static struct edd_parameters *
get_edd_parameters (struct bios_regs *regs)
{
  u32 address;

  address = ((u32)regs->ds << 4) + (u16)regs->esi;
  return (struct edd_parameters *)address;
}

static u8
get_ata_geometry (struct ata_geometry *geometry)
{
  u32 total_sectors_low;
  u32 total_sectors_high;

  return ata_identify (&geometry->cylinders, &geometry->heads,
                       &geometry->sectors_per_track, &total_sectors_low,
                       &total_sectors_high);
}

void
int13_service (struct bios_regs *regs)
{
  u8 function;
  struct edd_packet *packet;
  u32 buffer_address;

  function = (u8)(regs->eax >> 8);

  if (function == 0x00u)
    {
      if ((regs->edx & 0x00FFu) != BIOS_HARD_DISK)
        {
          int13_fail (regs);
          return;
        }

      if (ata_reset () != 0)
        {
          int13_fail (regs);
          return;
        }

      regs->eax &= 0xFFFF0000u;
      regs->flags &= (u16)~BIOS_CF;
      return;
    }

  if (function == 0x02u)
    {
      struct ata_geometry geometry;
      u8 count;
      u8 sector;
      u8 head;
      u16 cylinder;
      u32 lba;

      count = (u8)regs->eax;
      sector = (u8)regs->ecx & 0x3Fu;
      cylinder = (u16)(regs->ecx >> 8)
                 | (u16)(((u8)(regs->ecx >> 14) & 0x03u) << 8);
      head = (u8)(regs->edx >> 8);

      if ((regs->edx & 0x00FFu) != BIOS_HARD_DISK || count == 0u
          || get_ata_geometry (&geometry) != 0 || sector == 0u
          || sector > geometry.sectors_per_track || head >= geometry.heads
          || cylinder >= geometry.cylinders)
        {
          int13_fail (regs);
          return;
        }

      if (cylinder > CHS_MAX_CYLINDER)
        {
          int13_fail (regs);
          return;
        }

      lba = (((u32)cylinder * geometry.heads) + head)
                * geometry.sectors_per_track
            + (u32)sector - 1u;
      buffer_address = ((u32)regs->es << 4) + (u16)regs->ebx;

      if (ata_read_sectors28 (lba, (u16 *)buffer_address, count) != 0)
        {
          int13_fail (regs);
          return;
        }

      regs->eax = (regs->eax & 0xFFFF0000u) | (u32)count;
      regs->flags &= (u16)~BIOS_CF;
      return;
    }

  if (function == 0x08u)
    {
      struct ata_geometry geometry;
      u16 max_cylinder;
      u8 max_head;
      u8 sectors_per_track;

      if ((regs->edx & 0x00FFu) != BIOS_HARD_DISK
          || get_ata_geometry (&geometry) != 0)
        {
          int13_fail (regs);
          return;
        }

      max_cylinder = geometry.cylinders - 1u;
      if (max_cylinder > CHS_MAX_CYLINDER)
        max_cylinder = CHS_MAX_CYLINDER;

      max_head = (u8)(geometry.heads - 1u);
      sectors_per_track = (u8)geometry.sectors_per_track;
      regs->eax &= 0xFFFF0000u;
      regs->ecx = (regs->ecx & 0xFFFF0000u) | (u32)(sectors_per_track & 0x3Fu)
                  | ((u32)(max_cylinder & 0x00FFu) << 8)
                  | ((u32)(max_cylinder >> 8) << 14);
      regs->edx = (regs->edx & 0xFFFF0000u) | ((u32)max_head << 8) | 1u;
      regs->flags &= (u16)~BIOS_CF;
      return;
    }

  if (function == 0x15u)
    {
      if ((regs->edx & 0x00FFu) != BIOS_HARD_DISK)
        {
          int13_fail (regs);
          return;
        }

      regs->eax = (regs->eax & 0xFFFF0000u) | 0x0300u;
      regs->flags &= (u16)~BIOS_CF;
      return;
    }

  if (function == 0x41u)
    {
      if (regs->ebx != 0x55AAu)
        {
          int13_fail (regs);
          return;
        }

      regs->eax = 0x3000u;
      regs->ebx = 0xAA55u;
      regs->ecx |= 0x0001u;
      regs->flags &= (u16)~BIOS_CF;
      return;
    }

  if (function == 0x42u)
    {
      packet = get_edd_packet (regs);

      if (packet->size != 0x10u || packet->count == 0u || packet->count > 127u
          || packet->lba_high != 0u
          || packet->lba_low > 0x10000000u - (u32)packet->count)
        {
          int13_fail (regs);
          return;
        }

      buffer_address
          = ((u32)packet->buffer_segment << 4) + packet->buffer_offset;

      if (ata_read_sectors28 (packet->lba_low, (u16 *)buffer_address,
                              packet->count)
          != 0)
        {
          int13_fail (regs);
          return;
        }

      regs->eax = 0;
      regs->flags &= (u16)~BIOS_CF;
      return;
    }

  if (function == 0x48u)
    {
      struct edd_parameters *parameters;
      u16 cylinders;
      u16 heads;
      u16 sectors_per_track;
      u32 total_sectors_low;
      u32 total_sectors_high;

      parameters = get_edd_parameters (regs);

      if (parameters->size < 0x001Eu
          || ata_identify (&cylinders, &heads, &sectors_per_track,
                           &total_sectors_low, &total_sectors_high)
                 != 0)
        {
          int13_fail (regs);
          return;
        }

      parameters->size = 0x001Eu;
      parameters->flags = 0;
      parameters->cylinders_low = cylinders;
      parameters->cylinders_high = 0;
      parameters->heads = heads;
      parameters->sectors_per_track = sectors_per_track;
      parameters->total_sectors_low = total_sectors_low;
      parameters->total_sectors_high = total_sectors_high;
      parameters->bytes_per_sector = 512;

      regs->eax = 0;
      regs->flags &= (u16)~BIOS_CF;
      return;
    }

  int13_fail (regs);
}

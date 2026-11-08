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

#define ATA_DATA 0x1F0u
#define ATA_SECTOR_COUNT 0x1F2u
#define ATA_LBA_LOW 0x1F3u
#define ATA_LBA_MID 0x1F4u
#define ATA_LBA_HIGH 0x1F5u
#define ATA_DRIVE 0x1F6u
#define ATA_STATUS 0x1F7u
#define ATA_COMMAND 0x1F7u

#define ATA_STATUS_ERR 0x01u
#define ATA_STATUS_DRQ 0x08u
#define ATA_STATUS_BSY 0x80u

#define ATA_CMD_READ_SECTORS 0x20u
#define ATA_CMD_IDENTIFY 0xECu

static u8
ata_wait_not_busy (void)
{
  u8 status;

  do
    {
      status = io_in8 (ATA_STATUS);
    }
  while ((status & ATA_STATUS_BSY) != 0);

  return status;
}

static void
ata_delay_400ns (void)
{
  io_in8 (ATA_STATUS);
  io_in8 (ATA_STATUS);
  io_in8 (ATA_STATUS);
  io_in8 (ATA_STATUS);
}

static u8
ata_read_one_sector28 (u32 lba, u16 *buffer)
{
  u8 status;
  u16 index;

  status = ata_wait_not_busy ();
  if ((status & ATA_STATUS_ERR) != 0)
    return 1;

  io_out8 (ATA_DRIVE, (u8)(0xE0u | ((lba >> 24) & 0x0Fu)));
  ata_delay_400ns ();

  io_out8 (ATA_SECTOR_COUNT, 1);
  io_out8 (ATA_LBA_LOW, (u8)(lba & 0xFFu));
  io_out8 (ATA_LBA_MID, (u8)((lba >> 8) & 0xFFu));
  io_out8 (ATA_LBA_HIGH, (u8)((lba >> 16) & 0xFFu));
  io_out8 (ATA_COMMAND, ATA_CMD_READ_SECTORS);

  status = ata_wait_not_busy ();
  if ((status & ATA_STATUS_ERR) != 0)
    return 1;

  if ((status & ATA_STATUS_DRQ) == 0)
    return 1;

  for (index = 0; index < 256; ++index)
    buffer[index] = io_in16 (ATA_DATA);

  return 0;
}

u8
ata_read_sectors28 (u32 lba, u16 *buffer, u16 count)
{
  u16 sector;

  if (count == 0u)
    return 1;

  for (sector = 0; sector < count; ++sector)
    {
      if (ata_read_one_sector28 (lba, buffer) != 0)
        return 1;

      lba++;
      buffer += 256;
    }

  return 0;
}

u8
ata_identify (u16 *cylinders, u16 *heads, u16 *sectors_per_track,
              u32 *total_sectors_low, u32 *total_sectors_high)
{
  u16 identify_data[256];
  u8 status;
  u16 index;

  status = ata_wait_not_busy ();
  if ((status & ATA_STATUS_ERR) != 0)
    return 1;

  io_out8 (ATA_DRIVE, 0xE0u);
  ata_delay_400ns ();

  io_out8 (ATA_SECTOR_COUNT, 0);
  io_out8 (ATA_LBA_LOW, 0);
  io_out8 (ATA_LBA_MID, 0);
  io_out8 (ATA_LBA_HIGH, 0);
  io_out8 (ATA_COMMAND, ATA_CMD_IDENTIFY);

  status = ata_wait_not_busy ();
  if ((status & ATA_STATUS_ERR) != 0 || (status & ATA_STATUS_DRQ) == 0)
    return 1;

  for (index = 0; index < 256; ++index)
    identify_data[index] = io_in16 (ATA_DATA);

  *cylinders = identify_data[1];
  *heads = identify_data[3];
  *sectors_per_track = identify_data[6];
  *total_sectors_low
      = (u32)identify_data[60] | ((u32)identify_data[61] << 16);
  *total_sectors_high
      = (u32)identify_data[100] | ((u32)identify_data[101] << 16);

  return 0;
}

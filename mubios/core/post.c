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

void
post_main (void)
{
  serial_init ();
  serial_write ("mubios: POST start\r\n");

  memory_init ();
  bda_init ();
  vga_text_mode_init ();

  ivt_init ();

  kbd_init ();
  pic_init ();
  pit_init ();
  ioapic_init ();
  pci_irq_init ();
  ata_init ();

  volatile u8 *mbr;
  u8 ata_status;

  mbr = (volatile u8 *)0x00007C00u;
  ata_status = ata_read_sectors28 (0, (u16 *)mbr, 1);

  if (ata_status == 0 && mbr[0x01FE] == 0x55 && mbr[0x01FF] == 0xAA)
    serial_write ("mubios: MBR loaded\r\n");
  else
    serial_write ("mubios: MBR read failed\r\n");
}

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

#ifndef MUBIOS_BIOS_H
#define MUBIOS_BIOS_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define MUBIOS_IVT_BASE 0x00000000u
#define MUBIOS_IVT_SIZE 0x00000400u

#define MUBIOS_BDA_BASE 0x00000400u
#define MUBIOS_BDA_SIZE 0x00000100u

#define BDA_EBDA_SEGMENT_OFFSET 0x000Eu
#define BDA_EQUIPMENT_OFFSET 0x0010u
#define BDA_CONVENTIONAL_MEMORY_OFFSET 0x0013u

void post_main (void);
void memory_init (void);
void bda_init (void);

void serial_init (void);
void serial_putc (u8 value);
void serial_write (const char *text);

u8 cmos_read8 (u8 index);

void kbd_init (void);
void pic_init (void);
void pit_init (void);
void ioapic_init (void);
void vga_text_mode_init (void);

void ivt_init (void);

u8 ata_read_sectors28 (u32 lba, u16 *buffer, u16 count);
u8 ata_reset (void);
u8 ata_identify (u16 *cylinders, u16 *heads, u16 *sectors_per_track,
                 u32 *total_sectors_low, u32 *total_sectors_high);

struct bios_regs;

void bios_service_dispatch (u8 vector, struct bios_regs *regs);
void int10_service (struct bios_regs *regs);
void int14_service (struct bios_regs *regs);
void int13_service (struct bios_regs *regs);
void int15_service (struct bios_regs *regs);
void int16_service (struct bios_regs *regs);

#endif // MUBIOS_BIOS_H

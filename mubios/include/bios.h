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

#endif // MUBIOS_BIOS_H

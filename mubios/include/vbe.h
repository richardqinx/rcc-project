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

#ifndef MUBIOS_VBE_H
#define MUBIOS_VBE_H

#include "bios.h"

#define VBE_MODE_COUNT 15u
#define VBE_MODE_LINEAR 0x4000u
#define VBE_MODE_PRESERVE 0x8000u
#define VBE_OK 0x004Fu
#define VBE_FAILED 0x014Fu
#define VBE_UNSUPPORTED 0x024Fu

#define DISPI_XRES 1u
#define DISPI_YRES 2u
#define DISPI_BPP 3u
#define DISPI_ENABLE 4u
#define DISPI_BANK 5u
#define DISPI_VIRT_WIDTH 6u
#define DISPI_VIRT_HEIGHT 7u
#define DISPI_X_OFFSET 8u
#define DISPI_Y_OFFSET 9u

struct vbe_mode
{
  u16 number;
  u16 width;
  u16 height;
  u16 bpp;
};

struct vbe_state
{
  u32 framebuffer;
  u32 memory_size;
  u16 max_width;
  u16 max_height;
  u16 max_bpp;
  u16 current_mode;
  u16 mode_list[VBE_MODE_COUNT + 1u];
  u8 present;
};

extern struct vbe_state vbe;

void vbe_init (void);
void vbe_leave_graphics (void);
u16 vbe_read (u16 index);
void vbe_write (u16 index, u16 value);
const struct vbe_mode *vbe_find_mode (u16 number);
u16 vbe_set_mode (u16 request);
u16 vbe_get_mode (u16 *number);
void vbe_service (struct bios_regs *regs);

#endif

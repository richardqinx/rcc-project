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

#ifndef MUBIOS_VGA_H
#define MUBIOS_VGA_H

#include "bios.h"

#define VGA_TEXT_BASE 0x000B8000u
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u
#define VGA_TEXT_CELLS (VGA_COLUMNS * VGA_ROWS)
#define VGA_PAGE_SIZE 0x1000u
#define VGA_PAGE_COUNT 8u
#define VGA_FONT_HEIGHT 16u

/* All BDA constants below are offsets relative to MUBIOS_BDA_BASE. */
#define BDA_VIDEO_MODE 0x49u
#define BDA_VIDEO_COLUMNS 0x4Au
#define BDA_VIDEO_PAGE_SIZE 0x4Cu
#define BDA_VIDEO_PAGE_START 0x4Eu
#define BDA_CURSOR_BASE 0x50u
#define BDA_CURSOR_SHAPE 0x60u
#define BDA_VIDEO_PAGE 0x62u
#define BDA_VIDEO_CRTC 0x63u
#define BDA_VIDEO_MSR 0x65u
#define BDA_VIDEO_PALETTE 0x66u
#define BDA_VIDEO_ROWS 0x84u
#define BDA_VIDEO_HEIGHT 0x85u
#define BDA_VIDEO_CTL 0x87u
#define BDA_VIDEO_SWITCHES 0x88u
#define BDA_VIDEO_MODESET_CTL 0x89u
#define BDA_VIDEO_DCC 0x8Au

extern const u8 vga_font_8x16[256 * VGA_FONT_HEIGHT];

void vga_text_mode_set (u8 preserve);
void vga_set_cursor_shape (u16 shape);
void vga_set_cursor_position (u8 page, u16 position);
void vga_set_active_page (u8 page);

#endif

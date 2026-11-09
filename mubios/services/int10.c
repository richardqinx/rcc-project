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

#include "regs.h"
#include "vbe.h"
#include "vga.h"

static u16
int10_cursor_for_page (u8 page)
{
  return *(volatile u16 *)(MUBIOS_BDA_BASE + BDA_CURSOR_BASE + page * 2u);
}

static volatile u16 *
int10_video_for_page (u8 page)
{
  return (volatile u16 *)(VGA_TEXT_BASE + (u32)page * VGA_PAGE_SIZE);
}

static void
int10_teletype (u8 character)
{
  volatile u8 *bda = (volatile u8 *)MUBIOS_BDA_BASE;
  u8 page = bda[BDA_VIDEO_PAGE];
  volatile u16 *video = int10_video_for_page (page);
  u16 cursor = int10_cursor_for_page (page);
  u16 row = cursor >> 8;
  u16 column = cursor & 0xFFu;
  u16 position;
  u16 cell;
  u16 blank;

  /* AH=0Eh uses the active page, as in SeaVGABIOS. BEL has no text effect. */
  if (character == 0x07u)
    return;
  if (character == 0x0Du)
    column = 0u;
  else if (character == 0x0Au)
    ++row;
  else if (character == 0x08u)
    {
      /* A text-mode backspace moves left without erasing or crossing a row. */
      if (column != 0u)
        --column;
    }
  else
    {
      position = (u16)(row * VGA_COLUMNS + column);
      /* Mode 03h has 2000 visible cells per page. */
      if (position >= VGA_TEXT_CELLS)
        return;
      video[position] = (u16)((video[position] & 0xFF00u) | character);
      ++column;
      if (column >= VGA_COLUMNS)
        {
          column = 0u;
          ++row;
        }
    }

  /* Passing the last row scrolls the active page, retaining text attributes.
   */
  if (row >= VGA_ROWS)
    {
      blank = (u16)((video[(VGA_ROWS - 1u) * VGA_COLUMNS] & 0xFF00u) | 0x20u);
      for (cell = 0; cell < (VGA_ROWS - 1u) * VGA_COLUMNS; ++cell)
        video[cell] = video[cell + VGA_COLUMNS];
      for (; cell < VGA_TEXT_CELLS; ++cell)
        video[cell] = blank;
      row = VGA_ROWS - 1u;
    }
  vga_set_cursor_position (page, (u16)((row << 8) | column));
}

void
int10_service (struct bios_regs *regs)
{
  volatile u8 *bda = (volatile u8 *)MUBIOS_BDA_BASE;
  volatile u16 *video;
  u8 function = (u8)(regs->eax >> 8);
  u8 page = (u8)(regs->ebx >> 8);
  u16 cursor;
  u16 position;
  u16 count;
  u32 font_address;

  if (function == 0x4Fu)
    {
      vbe_service (regs);
      return;
    }

  if (function == 0x00u)
    {
      /* Bit 7 preserves video memory; the remaining bits select mode 03h. */
      if (((u8)regs->eax & 0x7Fu) == 0x03u)
        vga_text_mode_set ((u8)regs->eax & 0x80u);
      return;
    }

  if (function == 0x01u)
    {
      vga_set_cursor_shape ((u16)regs->ecx);
      return;
    }

  if (function == 0x05u)
    {
      vga_set_active_page ((u8)regs->eax);
      return;
    }

  if (function == 0x0Fu)
    {
      regs->eax = (regs->eax & 0xFFFF0000u)
                  | ((u32)bda[BDA_VIDEO_COLUMNS] << 8) | bda[BDA_VIDEO_MODE]
                  | (bda[BDA_VIDEO_CTL] & 0x80u);
      regs->ebx = (regs->ebx & 0xFFFF00FFu) | ((u32)bda[BDA_VIDEO_PAGE] << 8);
      return;
    }

  /* AH=11h/AL=30h reports the current font (BH=1) or ROM 8x16 font (BH=6). */
  if (function == 0x11u && (u8)regs->eax == 0x30u)
    {
      if (page == 0x01u || page == 0x06u)
        {
          font_address = (u32)vga_font_8x16;
          regs->es = (u16)(font_address >> 4);
          regs->ebp = (regs->ebp & 0xFFFF0000u) | (font_address & 0x0Fu);
        }
      regs->ecx = (regs->ecx & 0xFFFF0000u)
                  | *(volatile u16 *)(bda + BDA_VIDEO_HEIGHT);
      regs->edx = (regs->edx & 0xFFFFFF00u) | bda[BDA_VIDEO_ROWS];
      return;
    }

  if (function == 0x0Eu)
    {
      int10_teletype ((u8)regs->eax);
      return;
    }

  /* The mode 03h INT 10h interface provides page numbers 0 through 7. */
  if (page >= VGA_PAGE_COUNT)
    return;

  if (function == 0x02u)
    {
      vga_set_cursor_position (page, (u16)regs->edx);
      return;
    }

  if (function == 0x03u)
    {
      regs->edx = (regs->edx & 0xFFFF0000u) | int10_cursor_for_page (page);
      regs->ecx = (regs->ecx & 0xFFFF0000u)
                  | *(volatile u16 *)(bda + BDA_CURSOR_SHAPE);
      return;
    }

  if (function != 0x09u)
    return;

  cursor = int10_cursor_for_page (page);
  position = (u16)((cursor >> 8) * VGA_COLUMNS + (cursor & 0xFFu));
  video = int10_video_for_page (page);
  count = (u16)regs->ecx;
  /* Repeated character/attribute output stays within the visible page and
     does not change either the BDA or hardware cursor. */
  while (count != 0u && position < VGA_TEXT_CELLS)
    {
      video[position] = (u16)((u8)regs->eax | ((u16)(u8)regs->ebx << 8));
      ++position;
      --count;
    }
}

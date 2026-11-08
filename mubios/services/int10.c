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

#define VGA_TEXT_BASE 0x000B8000u
#define BDA_CURSOR 0x00000450u
#define BDA_CURSOR_SHAPE 0x00000460u
#define VGA_PAGE_SIZE 0x00001000u

static volatile u16 *
int10_cursor_for_page (u8 page)
{
  return (volatile u16 *)(BDA_CURSOR + ((u32)page * 2u));
}

static volatile u16 *
int10_video_for_page (u8 page)
{
  return (volatile u16 *)(VGA_TEXT_BASE + ((u32)page * VGA_PAGE_SIZE));
}

static void
int10_newline (volatile u16 *cursor)
{
  u16 row;

  row = (u16)(*cursor / 80u);
  *cursor = (u16)((row + 1u) * 80u);
}

void
int10_service (struct bios_regs *regs)
{
  volatile u16 *cursor;
  volatile u16 *video;
  u8 function;
  u8 page;
  u8 character;
  u8 attribute;
  u16 position;
  u16 count;

  function = (u8)(regs->eax >> 8);
  page = (u8)(regs->ebx >> 8);

  if (function == 0x02u)
    {
      cursor = int10_cursor_for_page (page);
      position = (u16)(((u8)(regs->edx >> 8) * 80u) + (u8)regs->edx);
      *cursor = position;
      return;
    }

  if (function == 0x03u)
    {
      cursor = int10_cursor_for_page (page);
      position = *cursor;
      regs->edx = (regs->edx & 0xFFFF0000u) | ((u32)(position / 80u) << 8)
                  | (u32)(position % 80u);
      regs->ecx = (regs->ecx & 0xFFFF0000u) | 0x0607u;
      return;
    }

  if (function != 0x09u && function != 0x0Eu)
    return;

  cursor = int10_cursor_for_page (page);
  video = int10_video_for_page (page);

  character = (u8)regs->eax;
  attribute = (u8)regs->ebx;
  position = *cursor;

  if (function == 0x09u)
    {
      count = (u16)regs->ecx;

      while (count != 0u)
        {
          if (position >= 2000u)
            position = 0;

          video[position] = (u16)character | ((u16)attribute << 8);
          position++;
          count--;
        }

      return;
    }

  if (character == 0x0Du)
    {
      *cursor = (u16)((position / 80u) * 80u);
      return;
    }

  if (character == 0x0Au)
    {
      int10_newline (cursor);
      return;
    }

  if (character == 0x08u)
    {
      if (position != 0u)
        {
          position--;
          video[position] = (u16)(0x20u | ((u16)attribute << 8));
          *cursor = position;
        }

      return;
    }

  if (position >= 2000u)
    position = 0;

  video[position] = (u16)character | ((u16)attribute << 8);
  *cursor = (u16)(position + 1u);
}

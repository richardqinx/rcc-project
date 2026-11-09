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

#include "vga.h"
#include "io.h"
#include "vbe.h"

#define VGA_MISC_OUTPUT_WRITE 0x03C2u
#define VGA_SEQ_INDEX 0x03C4u
#define VGA_CRTC_INDEX 0x03D4u
#define VGA_GC_INDEX 0x03CEu
#define VGA_INPUT_STATUS 0x03DAu
#define VGA_ATTR_INDEX_DATA 0x03C0u
#define VGA_PEL_MASK 0x03C6u
#define VGA_DAC_WRITE_INDEX 0x03C8u
#define VGA_DAC_DATA 0x03C9u
#define VGA_FONT_BASE 0x000A0000u

static void
vga_write_register (u16 port, u8 index, u8 value)
{
  io_out8 (port, index);
  io_out8 ((u16)(port + 1u), value);
}

static void
vga_write_registers (u16 port, const u8 *values, u8 count)
{
  u8 index;

  for (index = 0; index < count; ++index)
    vga_write_register (port, index, values[index]);
}

static void
vga_load_font (void)
{
  volatile u8 *plane = (volatile u8 *)VGA_FONT_BASE;
  u16 character;
  u8 line;

  /* Select plane 2 and disable odd/even addressing while writing glyphs. */
  vga_write_register (VGA_SEQ_INDEX, 0x00u, 0x01u);
  vga_write_register (VGA_SEQ_INDEX, 0x02u, 0x04u);
  vga_write_register (VGA_SEQ_INDEX, 0x04u, 0x07u);
  vga_write_register (VGA_SEQ_INDEX, 0x00u, 0x03u);
  vga_write_register (VGA_GC_INDEX, 0x04u, 0x02u);
  vga_write_register (VGA_GC_INDEX, 0x05u, 0x00u);
  vga_write_register (VGA_GC_INDEX, 0x06u, 0x04u);

  for (character = 0; character < 256u; ++character)
    {
      for (line = 0; line < VGA_FONT_HEIGHT; ++line)
        plane[character * 32u + line]
            = vga_font_8x16[character * VGA_FONT_HEIGHT + line];
      for (; line < 32u; ++line)
        plane[character * 32u + line] = 0u;
    }

  /* Restore the B8000 text aperture and character/attribute plane mapping. */
  vga_write_register (VGA_SEQ_INDEX, 0x00u, 0x01u);
  vga_write_register (VGA_SEQ_INDEX, 0x02u, 0x03u);
  vga_write_register (VGA_SEQ_INDEX, 0x03u, 0x00u);
  vga_write_register (VGA_SEQ_INDEX, 0x04u, 0x03u);
  vga_write_register (VGA_SEQ_INDEX, 0x00u, 0x03u);
  vga_write_register (VGA_GC_INDEX, 0x06u, 0x0Eu);
  vga_write_register (VGA_GC_INDEX, 0x04u, 0x00u);
  vga_write_register (VGA_GC_INDEX, 0x05u, 0x10u);
}

static void
vga_load_palette (void)
{
  u16 index;

  io_out8 (VGA_PEL_MASK, 0xFFu);
  io_out8 (VGA_DAC_WRITE_INDEX, 0u);
  /* Mode 03h uses the 64 EGA colors, each DAC component is six bits. */
  for (index = 0; index < 64u; ++index)
    {
      io_out8 (VGA_DAC_DATA, (u8)(((index >> 2) & 1u) * 0x2Au
                                  + ((index >> 5) & 1u) * 0x15u));
      io_out8 (VGA_DAC_DATA, (u8)(((index >> 1) & 1u) * 0x2Au
                                  + ((index >> 4) & 1u) * 0x15u));
      io_out8 (VGA_DAC_DATA,
               (u8)((index & 1u) * 0x2Au + ((index >> 3) & 1u) * 0x15u));
    }
  for (; index < 256u; ++index)
    {
      io_out8 (VGA_DAC_DATA, 0u);
      io_out8 (VGA_DAC_DATA, 0u);
      io_out8 (VGA_DAC_DATA, 0u);
    }
}

void
vga_set_cursor_shape (u16 shape)
{
  volatile u8 *bda = (volatile u8 *)MUBIOS_BDA_BASE;
  u8 start = (u8)((shape >> 8) & 0x3Fu);
  u8 end = (u8)(shape & 0x1Fu);
  u16 height = *(volatile u16 *)(bda + BDA_VIDEO_HEIGHT);

  *(volatile u16 *)(bda + BDA_CURSOR_SHAPE) = shape;
  /* VGA BIOS scales an eight-scanline cursor unless emulation is disabled.
     Start bit 5 disables the cursor and must not be scaled. */
  if ((bda[BDA_VIDEO_CTL] & 1u) == 0u && height > 8u && end < 8u
      && start < 0x20u)
    {
      if (end != start + 1u)
        start = (u8)(((start + 1u) * height / 8u) - 1u);
      else
        start = (u8)(((end + 1u) * height / 8u) - 2u);
      end = (u8)(((end + 1u) * height / 8u) - 1u);
    }
  vga_write_register (VGA_CRTC_INDEX, 0x0Au, start);
  vga_write_register (VGA_CRTC_INDEX, 0x0Bu, end);
}

void
vga_set_cursor_position (u8 page, u16 position)
{
  volatile u8 *bda = (volatile u8 *)MUBIOS_BDA_BASE;
  u16 address;

  /* INT 10h exposes eight text pages. */
  if (page >= VGA_PAGE_COUNT)
    return;

  *(volatile u16 *)(bda + BDA_CURSOR_BASE + page * 2u) = position;
  if (page != bda[BDA_VIDEO_PAGE])
    return;

  address = (u16)(page * (VGA_PAGE_SIZE / 2u) + (position >> 8) * VGA_COLUMNS
                  + (position & 0xFFu));
  vga_write_register (VGA_CRTC_INDEX, 0x0Eu, (u8)(address >> 8));
  vga_write_register (VGA_CRTC_INDEX, 0x0Fu, (u8)address);
}

void
vga_set_active_page (u8 page)
{
  volatile u8 *bda = (volatile u8 *)MUBIOS_BDA_BASE;
  u16 offset;

  /* Each page occupies 4 KiB within the 32 KiB text aperture. */
  if (page >= VGA_PAGE_COUNT)
    return;

  offset = (u16)(page * VGA_PAGE_SIZE);
  bda[BDA_VIDEO_PAGE] = page;
  *(volatile u16 *)(bda + BDA_VIDEO_PAGE_START) = offset;
  vga_write_register (VGA_CRTC_INDEX, 0x0Cu, (u8)((offset / 2u) >> 8));
  vga_write_register (VGA_CRTC_INDEX, 0x0Du, (u8)(offset / 2u));
  vga_set_cursor_position (
      page, *(volatile u16 *)(bda + BDA_CURSOR_BASE + page * 2u));
}

void
vga_text_mode_set (u8 preserve)
{
  /* Standard VGA 80x25, 720x400 mode 03h values, as used by SeaVGABIOS. */
  static const u8 sequencer[] = { 0x01u, 0x00u, 0x03u, 0x00u, 0x02u };
  static const u8 crtc[]
      = { 0x5Fu, 0x4Fu, 0x50u, 0x82u, 0x55u, 0x81u, 0xBFu, 0x1Fu, 0x00u,
          0x4Fu, 0x0Du, 0x0Eu, 0x00u, 0x00u, 0x00u, 0x00u, 0x9Cu, 0x8Eu,
          0x8Fu, 0x28u, 0x1Fu, 0x96u, 0xB9u, 0xA3u, 0xFFu };
  static const u8 graphics[]
      = { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u, 0x0Eu, 0x0Fu, 0xFFu };
  static const u8 attributes[]
      = { 0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x14u,
          0x07u, 0x38u, 0x39u, 0x3Au, 0x3Bu, 0x3Cu, 0x3Du,
          0x3Eu, 0x3Fu, 0x0Cu, 0x00u, 0x0Fu, 0x08u, 0x00u };
  volatile u16 *video = (volatile u16 *)VGA_TEXT_BASE;
  volatile u8 *bda = (volatile u8 *)MUBIOS_BDA_BASE;
  u32 font_address = (u32)vga_font_8x16;
  volatile u16 *font_vector = (volatile u16 *)(0x43u * 4u);
  u16 cell;
  u8 index;

  vbe_leave_graphics ();

  /* Keep attribute-controller output blank until registers and RAM are ready.
   */
  io_in8 (VGA_INPUT_STATUS);
  io_out8 (VGA_ATTR_INDEX_DATA, 0x00u);
  vga_write_register (VGA_SEQ_INDEX, 0x00u, 0x01u);
  io_out8 (VGA_MISC_OUTPUT_WRITE, 0x67u);
  vga_write_registers (VGA_SEQ_INDEX, sequencer, 5u);
  vga_write_register (VGA_SEQ_INDEX, 0x00u, 0x03u);

  /* Clear CRTC write protection before programming timing registers. */
  vga_write_register (VGA_CRTC_INDEX, 0x11u, 0x00u);
  vga_write_registers (VGA_CRTC_INDEX, crtc, 25u);
  vga_write_registers (VGA_GC_INDEX, graphics, 9u);
  for (index = 0; index < 21u; ++index)
    {
      io_in8 (VGA_INPUT_STATUS);
      io_out8 (VGA_ATTR_INDEX_DATA, index);
      io_out8 (VGA_ATTR_INDEX_DATA, attributes[index]);
    }

  vga_load_palette ();
  vga_load_font ();

  /* AL bit 7 requests a mode change without clearing text video memory. */
  if (preserve == 0u)
    for (cell = 0; cell < VGA_PAGE_COUNT * VGA_PAGE_SIZE / 2u; ++cell)
      video[cell] = 0x0720u;

  bda[BDA_VIDEO_MODE] = 0x03u;
  *(volatile u16 *)(bda + BDA_VIDEO_COLUMNS) = VGA_COLUMNS;
  *(volatile u16 *)(bda + BDA_VIDEO_PAGE_SIZE) = VGA_PAGE_SIZE;
  for (index = 0; index < VGA_PAGE_COUNT; ++index)
    *(volatile u16 *)(bda + BDA_CURSOR_BASE + index * 2u) = 0u;
  *(volatile u16 *)(bda + BDA_VIDEO_CRTC) = VGA_CRTC_INDEX;
  bda[BDA_VIDEO_MSR] = 0x29u;
  bda[BDA_VIDEO_PALETTE] = 0u;
  bda[BDA_VIDEO_ROWS] = VGA_ROWS - 1u;
  *(volatile u16 *)(bda + BDA_VIDEO_HEIGHT) = VGA_FONT_HEIGHT;
  bda[BDA_VIDEO_CTL] = (u8)(0x60u | (preserve & 0x80u));
  vbe.current_mode = (u16)(0x03u | ((u16)(preserve & 0x80u) << 8));
  bda[BDA_VIDEO_SWITCHES] = 0xF9u;
  bda[BDA_VIDEO_MODESET_CTL] &= 0x7Fu;
  bda[BDA_VIDEO_DCC] = 0x08u;

  /* INT 43h identifies the ROM font through a real-mode segment:offset. */
  font_vector[0] = (u16)(font_address & 0x0Fu);
  font_vector[1] = (u16)(font_address >> 4);
  vga_set_cursor_shape (0x0607u);
  vga_set_active_page (0u);

  io_in8 (VGA_INPUT_STATUS);
  io_out8 (VGA_ATTR_INDEX_DATA, 0x20u);
}

void
vga_text_mode_init (void)
{
  volatile u8 *bda = (volatile u8 *)MUBIOS_BDA_BASE;

  /* VGA is present, with a color display and the default 400-line timing. */
  bda[BDA_VIDEO_MODESET_CTL] = 0x51u;
  vbe_init ();
  vga_text_mode_set (0u);
}

/*
 RCC-Project

 Copyright (C) 2026 Richard Qin

 This file is part of RCC-Project.

 RCC-Project is free software; you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free Software
 Foundation; either version 3 of the License, or (at your option) any later
 version.
 */

#include "bios.h"
#include "io.h"

#define VGA_TEXT_BASE 0x000B8000u
#define VGA_TEXT_CELLS 2000u

#define BDA_VIDEO_MODE 0x0049u
#define BDA_VIDEO_COLUMNS 0x004Au
#define BDA_VIDEO_PAGE_SIZE 0x004Cu
#define BDA_VIDEO_PAGE 0x0062u
#define BDA_CURSOR_BASE 0x0450u
#define BDA_CURSOR_SHAPE 0x0460u

#define VGA_MISC_OUTPUT_WRITE 0x03C2u
#define VGA_SEQ_INDEX 0x03C4u
#define VGA_SEQ_DATA 0x03C5u
#define VGA_CRTC_INDEX 0x03D4u
#define VGA_CRTC_DATA 0x03D5u
#define VGA_GC_INDEX 0x03CEu
#define VGA_GC_DATA 0x03CFu
#define VGA_INPUT_STATUS 0x03DAu
#define VGA_ATTR_INDEX_DATA 0x03C0u

static void
vga_write_registers (u16 index_port, u16 data_port, const u8 *values, u8 count)
{
  u8 index;

  for (index = 0; index < count; ++index)
    {
      io_out8 (index_port, index);
      io_out8 (data_port, values[index]);
    }
}

void
vga_text_mode_init (void)
{
  static const u8 sequencer[] = { 0x00u, 0x00u, 0x03u, 0x00u, 0x02u };
  static const u8 crtc[]
      = { 0x5Fu, 0x4Fu, 0x50u, 0x82u, 0x55u, 0x81u, 0xBFu, 0x1Fu, 0x00u,
          0x4Fu, 0x0Du, 0x0Eu, 0x00u, 0x00u, 0x00u, 0x00u, 0x9Cu, 0x8Eu,
          0x8Fu, 0x28u, 0x1Fu, 0x96u, 0xB9u, 0xA3u, 0xFFu };
  static const u8 graphics[]
      = { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u, 0x0Fu, 0x00u, 0xFFu };
  static const u8 attributes[]
      = { 0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u,
          0x07u, 0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du,
          0x0Eu, 0x0Fu, 0x0Cu, 0x00u, 0x0Fu, 0x08u, 0x00u };
  volatile u16 *video;
  volatile u8 *bda;
  u16 cell;
  u8 index;

  io_out8 (VGA_MISC_OUTPUT_WRITE, 0x67u);

  io_out8 (VGA_SEQ_INDEX, 0x00u);
  io_out8 (VGA_SEQ_DATA, 0x01u);
  for (index = 1; index < 5u; ++index)
    {
      io_out8 (VGA_SEQ_INDEX, index);
      io_out8 (VGA_SEQ_DATA, sequencer[index]);
    }
  io_out8 (VGA_SEQ_INDEX, 0x00u);
  io_out8 (VGA_SEQ_DATA, 0x03u);

  io_out8 (VGA_CRTC_INDEX, 0x11u);
  io_out8 (VGA_CRTC_DATA, 0x00u);
  vga_write_registers (VGA_CRTC_INDEX, VGA_CRTC_DATA, crtc, 25);

  vga_write_registers (VGA_GC_INDEX, VGA_GC_DATA, graphics, 9);

  for (index = 0; index < 21u; ++index)
    {
      io_in8 (VGA_INPUT_STATUS);
      io_out8 (VGA_ATTR_INDEX_DATA, index);
      io_out8 (VGA_ATTR_INDEX_DATA, attributes[index]);
    }

  io_in8 (VGA_INPUT_STATUS);
  io_out8 (VGA_ATTR_INDEX_DATA, 0x20u);

  video = (volatile u16 *)VGA_TEXT_BASE;
  for (cell = 0; cell < VGA_TEXT_CELLS; ++cell)
    video[cell] = 0x0720u;

  bda = (volatile u8 *)MUBIOS_BDA_BASE;
  bda[BDA_VIDEO_MODE] = 0x03u;
  bda[BDA_VIDEO_PAGE] = 0x00u;
  bda[BDA_VIDEO_COLUMNS] = 80u;
  bda[BDA_VIDEO_COLUMNS + 1u] = 0u;
  bda[BDA_VIDEO_PAGE_SIZE] = 0x00u;
  bda[BDA_VIDEO_PAGE_SIZE + 1u] = 0x10u;
  *(volatile u16 *)(bda + BDA_CURSOR_BASE) = 0u;
  *(volatile u16 *)(bda + BDA_CURSOR_SHAPE) = 0x0607u;
}

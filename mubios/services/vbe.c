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

#include "vbe.h"
#include "regs.h"

static const char oem_name[] = "mubios QEMU standard VGA";
static const char vendor_name[] = "mubios";
static const char product_name[] = "QEMU standard VGA VBE";
static const char product_revision[] = "2.0";

static void
put16 (volatile u8 *buffer, u16 offset, u16 value)
{
  buffer[offset] = (u8)value;
  buffer[offset + 1u] = (u8)(value >> 8);
}

static void
put32 (volatile u8 *buffer, u16 offset, u32 value)
{
  put16 (buffer, offset, (u16)value);
  put16 (buffer, (u16)(offset + 2u), (u16)(value >> 16));
}

static u32
far_pointer (const void *pointer)
{
  u32 address = (u32)pointer;

  return ((address >> 4) << 16) | (address & 0x0Fu);
}

static void
clear_block (volatile u8 *buffer, u16 size)
{
  u16 index;

  for (index = 0; index < size; ++index)
    buffer[index] = 0u;
}

static u16
controller_info (volatile u8 *buffer)
{
  /* VBE 1.x callers provide 256 bytes; "VBE2" requests 512 bytes. */
  u8 extended = buffer[0] == 'V' && buffer[1] == 'B' && buffer[2] == 'E'
                && buffer[3] == '2';

  clear_block (buffer, extended ? 512u : 256u);
  buffer[0] = 'V';
  buffer[1] = 'E';
  buffer[2] = 'S';
  buffer[3] = 'A';
  put16 (buffer, 0x04u, 0x0200u);
  put32 (buffer, 0x06u, far_pointer (oem_name));
  /* Capabilities=0: VGA-compatible, fixed six-bit DAC, no stereo. */
  put32 (buffer, 0x0Eu, far_pointer (vbe.mode_list));
  put16 (buffer, 0x12u, (u16)(vbe.memory_size >> 16));
  if (extended)
    {
      put16 (buffer, 0x14u, 0x0100u);
      put32 (buffer, 0x16u, far_pointer (vendor_name));
      put32 (buffer, 0x1Au, far_pointer (product_name));
      put32 (buffer, 0x1Eu, far_pointer (product_revision));
    }
  return VBE_OK;
}

static u16
mode_info (volatile u8 *buffer, u16 number)
{
  const struct vbe_mode *mode = vbe_find_mode (number & 0x3FFFu);
  u32 frame_size;
  u32 pages;

  if (mode == (const struct vbe_mode *)0)
    return VBE_FAILED;
  clear_block (buffer, 256u);
  /* Supported, extended info, color graphics, LFB available, no bank window.
     BIOS character output in these graphics modes is not advertised. */
  put16 (buffer, 0x00u, 0x00DBu);
  put16 (buffer, 0x10u, (u16)(mode->width * (mode->bpp / 8u)));
  put16 (buffer, 0x12u, mode->width);
  put16 (buffer, 0x14u, mode->height);
  buffer[0x16u] = 8u;
  buffer[0x17u] = 16u;
  buffer[0x18u] = 1u;
  buffer[0x19u] = (u8)mode->bpp;
  buffer[0x1Bu] = 6u; /* Direct color, packed BGR/RGB565 pixels. */
  frame_size = (u32)mode->width * mode->height * (mode->bpp / 8u);
  pages = vbe.memory_size / frame_size - 1u;
  /* DISPI display-start Y is limited independently of the VRAM capacity. */
  if (pages > vbe.max_height / mode->height)
    pages = vbe.max_height / mode->height;
  buffer[0x1Du] = (u8)(pages > 255u ? 255u : pages);
  if (mode->bpp == 16u)
    {
      buffer[0x1Fu] = 5u;
      buffer[0x20u] = 11u;
      buffer[0x21u] = 6u;
      buffer[0x22u] = 5u;
      buffer[0x23u] = 5u;
    }
  else
    {
      buffer[0x1Fu] = 8u;
      buffer[0x20u] = 16u;
      buffer[0x21u] = 8u;
      buffer[0x22u] = 8u;
      buffer[0x23u] = 8u;
      if (mode->bpp == 32u)
        {
          buffer[0x25u] = 8u;
          buffer[0x26u] = 24u;
        }
    }
  put32 (buffer, 0x28u, vbe.framebuffer);
  return VBE_OK;
}

static int
graphics_active (void)
{
  return (vbe_read (DISPI_ENABLE) & 1u) != 0u;
}

static u16
scanline_length (struct bios_regs *regs)
{
  u8 subfunction = (u8)regs->ebx;
  u16 width = vbe_read (DISPI_XRES);
  u16 height = vbe_read (DISPI_YRES);
  u16 bytes_per_pixel = (u16)(vbe_read (DISPI_BPP) / 8u);
  u16 old_width;
  u16 old_x;
  u16 old_y;
  u32 pixels;
  u32 pitch;
  u32 lines;

  /* Logical scanline operations require an active direct-color mode. */
  if (!graphics_active ()
      || (bytes_per_pixel != 2u && bytes_per_pixel != 3u
          && bytes_per_pixel != 4u))
    return VBE_FAILED;
  if (subfunction == 0u || subfunction == 2u)
    {
      pixels = (u16)regs->ecx;
      if (subfunction == 2u)
        pixels = (pixels + bytes_per_pixel - 1u) / bytes_per_pixel;
      /* DISPI virtual widths are multiples of eight pixels. */
      pixels = (pixels + 7u) & ~7u;
      pitch = pixels * bytes_per_pixel;
      if (pixels < width || pixels > vbe.max_width || pitch > 0xFFFFu
          || pitch * height > vbe.memory_size)
        return VBE_FAILED;
      old_width = vbe_read (DISPI_VIRT_WIDTH);
      old_x = vbe_read (DISPI_X_OFFSET);
      old_y = vbe_read (DISPI_Y_OFFSET);
      vbe_write (DISPI_X_OFFSET, 0u);
      vbe_write (DISPI_Y_OFFSET, 0u);
      vbe_write (DISPI_VIRT_WIDTH, (u16)pixels);
      if (vbe_read (DISPI_VIRT_WIDTH) != pixels)
        {
          vbe_write (DISPI_VIRT_WIDTH, old_width);
          vbe_write (DISPI_X_OFFSET, old_x);
          vbe_write (DISPI_Y_OFFSET, old_y);
          return VBE_FAILED;
        }
    }
  else if (subfunction == 3u)
    {
      /* Maximum width still has to hold one complete visible image. */
      pixels = vbe.memory_size / ((u32)bytes_per_pixel * height);
      if (pixels > vbe.max_width)
        pixels = vbe.max_width;
      if (pixels > 0xFFFFu / bytes_per_pixel)
        pixels = 0xFFFFu / bytes_per_pixel;
      pixels &= ~7u;
    }
  else if (subfunction != 1u)
    return VBE_UNSUPPORTED;

  if (subfunction != 3u)
    pixels = vbe_read (DISPI_VIRT_WIDTH);
  pitch = pixels * bytes_per_pixel;
  lines = vbe.memory_size / pitch;
  if (lines > (u32)vbe.max_height + height)
    lines = (u32)vbe.max_height + height;
  regs->ebx = (regs->ebx & 0xFFFF0000u) | (u16)pitch;
  regs->ecx = (regs->ecx & 0xFFFF0000u) | (u16)pixels;
  regs->edx
      = (regs->edx & 0xFFFF0000u) | (u16)(lines > 0xFFFFu ? 0xFFFFu : lines);
  return VBE_OK;
}

static u16
display_start (struct bios_regs *regs)
{
  u8 subfunction = (u8)regs->ebx;
  u32 x = (u16)regs->ecx;
  u32 y = (u16)regs->edx;
  u32 width = vbe_read (DISPI_XRES);
  u32 height = vbe_read (DISPI_YRES);
  u32 virtual_width = vbe_read (DISPI_VIRT_WIDTH);
  u32 bytes_per_pixel = vbe_read (DISPI_BPP) / 8u;
  u32 pitch;
  u16 old_x;
  u16 old_y;

  if (!graphics_active ()
      || (bytes_per_pixel != 2u && bytes_per_pixel != 3u
          && bytes_per_pixel != 4u))
    return VBE_FAILED;
  if (subfunction == 1u)
    {
      regs->ebx &= 0xFFFF00FFu;
      regs->ecx = (regs->ecx & 0xFFFF0000u) | vbe_read (DISPI_X_OFFSET);
      regs->edx = (regs->edx & 0xFFFF0000u) | vbe_read (DISPI_Y_OFFSET);
      return VBE_OK;
    }
  if (subfunction != 0u && subfunction != 0x80u)
    return VBE_UNSUPPORTED;

  pitch = virtual_width * bytes_per_pixel;
  /* The entire displayed rectangle must lie in VRAM. QEMU expresses its
     start address in dwords, so the byte offset must be dword-aligned. */
  if (x + width > virtual_width || y > vbe.max_height
      || (y + height) > vbe.memory_size / pitch
      || x * bytes_per_pixel > vbe.memory_size - (y + height) * pitch
      || ((x * bytes_per_pixel + y * pitch) & 3u) != 0u)
    return VBE_FAILED;
  old_x = vbe_read (DISPI_X_OFFSET);
  old_y = vbe_read (DISPI_Y_OFFSET);
  /* QEMU applies display changes at its display refresh. No guest-side
     retrace busy wait is needed for BL=80h. */
  vbe_write (DISPI_Y_OFFSET, 0u);
  vbe_write (DISPI_X_OFFSET, (u16)x);
  vbe_write (DISPI_Y_OFFSET, (u16)y);
  if (vbe_read (DISPI_X_OFFSET) != x || vbe_read (DISPI_Y_OFFSET) != y)
    {
      vbe_write (DISPI_Y_OFFSET, 0u);
      vbe_write (DISPI_X_OFFSET, old_x);
      vbe_write (DISPI_Y_OFFSET, old_y);
      return VBE_FAILED;
    }
  return VBE_OK;
}

void
vbe_service (struct bios_regs *regs)
{
  u16 status = VBE_FAILED;
  u16 mode;
  u8 function = (u8)regs->eax;
  volatile u8 *buffer = (volatile u8 *)(((u32)regs->es << 4) + (u16)regs->edi);

  /* VBE uses AX status codes, including for unavailable hardware/functions. */
  if (!vbe.present)
    goto done;
  switch (function)
    {
    case 0x00u:
      status = controller_info (buffer);
      break;
    case 0x01u:
      status = mode_info (buffer, (u16)regs->ecx);
      break;
    case 0x02u:
      status = vbe_set_mode ((u16)regs->ebx);
      break;
    case 0x03u:
      status = vbe_get_mode (&mode);
      if (status == VBE_OK)
        regs->ebx = (regs->ebx & 0xFFFF0000u) | mode;
      break;
    case 0x06u:
      status = scanline_length (regs);
      break;
    case 0x07u:
      status = display_start (regs);
      break;
    case 0x08u:
      /* Capabilities advertise a fixed six-bit DAC; accept only that format.
       */
      if ((u8)regs->ebx == 1u
          || ((u8)regs->ebx == 0u && (u8)(regs->ebx >> 8) == 6u))
        {
          regs->ebx = (regs->ebx & 0xFFFF00FFu) | 0x0600u;
          status = VBE_OK;
        }
      break;
    default:
      /* No banking, VBE PM interface, custom timings, DDC or stereo is
       * claimed. */
      status = VBE_FAILED;
      break;
    }
done:
  regs->eax = (regs->eax & 0xFFFF0000u) | status;
}

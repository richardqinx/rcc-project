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
#include "io.h"
#include "vga.h"

#define PCI_CONFIG_ADDRESS 0x0CF8u
#define PCI_CONFIG_DATA 0x0CFCu
#define PCI_COMMAND 0x04u
#define PCI_BAR0 0x10u
#define PCI_BAR2 0x18u
#define PCI_MEMORY_MASK 0xFFFFFFF0u
#define QEMU_VGA_ID 0x11111234u
/* Allocate below the platform's high MMIO/firmware area, never in RAM.
   This is an allocation ceiling, not a framebuffer physical address. */
#define PCI_MMIO_CEILING 0xF0000000u
#define DISPI_INDEX 0x01CEu
#define DISPI_DATA 0x01CFu
#define DISPI_ID 0u
#define DISPI_MEMORY_64K 0x0Au
#define DISPI_ENABLED 0x01u
#define DISPI_GETCAPS 0x02u
#define DISPI_LINEAR 0x40u
#define DISPI_PRESERVE 0x80u

static const struct vbe_mode modes[VBE_MODE_COUNT]
    = { { 0x111u, 640u, 480u, 16u },   { 0x112u, 640u, 480u, 24u },
        { 0x142u, 640u, 480u, 32u },   { 0x114u, 800u, 600u, 16u },
        { 0x115u, 800u, 600u, 24u },   { 0x143u, 800u, 600u, 32u },
        { 0x117u, 1024u, 768u, 16u },  { 0x118u, 1024u, 768u, 24u },
        { 0x144u, 1024u, 768u, 32u },  { 0x11Au, 1280u, 1024u, 16u },
        { 0x11Bu, 1280u, 1024u, 24u }, { 0x145u, 1280u, 1024u, 32u },
        { 0x11Eu, 1600u, 1200u, 16u }, { 0x11Fu, 1600u, 1200u, 24u },
        { 0x147u, 1600u, 1200u, 32u } };

struct vbe_state vbe;

static u32
pci_read (u16 devfn, u8 offset)
{
  io_out32 (PCI_CONFIG_ADDRESS,
            0x80000000u | ((u32)devfn << 8) | (offset & 0xFCu));
  return io_in32 (PCI_CONFIG_DATA);
}

static void
pci_write (u16 devfn, u8 offset, u32 value)
{
  io_out32 (PCI_CONFIG_ADDRESS,
            0x80000000u | ((u32)devfn << 8) | (offset & 0xFCu));
  io_out32 (PCI_CONFIG_DATA, value);
}

static void
pci_write_command (u16 devfn, u16 command)
{
  io_out32 (PCI_CONFIG_ADDRESS, 0x80000000u | ((u32)devfn << 8) | PCI_COMMAND);
  /* Do not write back the write-one-to-clear PCI status word. */
  io_out16 (PCI_CONFIG_DATA, command);
}

u16
vbe_read (u16 index)
{
  io_out16 (DISPI_INDEX, index);
  return io_in16 (DISPI_DATA);
}

void
vbe_write (u16 index, u16 value)
{
  io_out16 (DISPI_INDEX, index);
  io_out16 (DISPI_DATA, value);
}

static u32
pci_ram_end (void)
{
  u16 units = (u16)cmos_read8 (0x34u) | ((u16)cmos_read8 (0x35u) << 8);
  u16 kib;

  /* QEMU CMOS 34h/35h describes RAM above 16 MiB in 64 KiB units.
     For smaller guests, 30h/31h describes RAM above 1 MiB in KiB. */
  if (units != 0u)
    return 0x01000000u + ((u32)units << 16);
  kib = (u16)cmos_read8 (0x30u) | ((u16)cmos_read8 (0x31u) << 8);
  return 0x00100000u + ((u32)kib << 10);
}

static u32
pci_allocation_limit (u16 vga_devfn, u32 ram_end)
{
  u32 limit = PCI_MMIO_CEILING;
  u32 value;
  u32 base;
  u16 devfn;
  u8 header;
  u8 offset;

  /* The default QEMU pc VGA is on root bus 0. Allocate below all existing
     root-bus memory resources, leaving their full ranges untouched. */
  for (devfn = 0; devfn < 256u; ++devfn)
    {
      if (devfn == vga_devfn || (u16)pci_read (devfn, 0u) == 0xFFFFu)
        continue;
      header = (u8)(pci_read (devfn, 0x0Cu) >> 16) & 0x7Fu;
      for (offset = 0x10u; offset < (header == 0u ? 0x28u : 0x18u);
           offset += 4u)
        {
          value = pci_read (devfn, offset);
          /* Bit 0 identifies I/O BARs; a 64-bit BAR has type bits 10b. */
          if ((value & 1u) != 0u)
            continue;
          base = value & PCI_MEMORY_MASK;
          if ((value & 6u) == 4u)
            {
              offset += 4u;
              if (pci_read (devfn, offset) != 0u)
                continue;
            }
          if (base >= ram_end && base < limit)
            limit = base;
        }
      /* Bridge windows and PCI expansion ROMs also occupy address space. */
      if (header == 1u)
        {
          base = (pci_read (devfn, 0x20u) & 0xFFF0u) << 16;
          if (base >= ram_end && base < limit)
            limit = base;
          base = (pci_read (devfn, 0x24u) & 0xFFF0u) << 16;
          if (pci_read (devfn, 0x28u) == 0u && base >= ram_end && base < limit)
            limit = base;
        }
      value = pci_read (devfn, header == 1u ? 0x38u : 0x30u);
      base = value & 0xFFFFF800u;
      if (base >= ram_end && base < limit)
        limit = base;
    }
  return limit;
}

static u32
pci_bar_size (u16 devfn, u8 offset)
{
  u32 saved = pci_read (devfn, offset);
  u32 mask;

  /* PCI sizing is done with memory decoding disabled by the caller. */
  pci_write (devfn, offset, 0xFFFFFFFFu);
  mask = pci_read (devfn, offset);
  pci_write (devfn, offset, saved);
  /* This driver uses QEMU standard VGA's 32-bit memory BARs only. */
  if ((mask & 7u) != 0u || (mask & PCI_MEMORY_MASK) == 0u)
    return 0u;
  return ~(mask & PCI_MEMORY_MASK) + 1u;
}

static int
pci_configure_vga (u16 devfn)
{
  u16 command = (u16)pci_read (devfn, PCI_COMMAND);
  u32 old_framebuffer = pci_read (devfn, PCI_BAR0);
  u32 old_registers = pci_read (devfn, PCI_BAR2);
  u32 ram_end = pci_ram_end ();
  u32 limit = pci_allocation_limit (devfn, ram_end);
  u32 framebuffer_size;
  u32 registers_size;
  u32 framebuffer;
  u32 registers = 0u;

  pci_write_command (devfn, (u16)(command & ~3u));
  framebuffer_size = pci_bar_size (devfn, PCI_BAR0);
  registers_size = pci_bar_size (devfn, PCI_BAR2);
  /* PCI memory BARs must fit, aligned to their probed power-of-two size,
     entirely above guest RAM and below the platform MMIO ceiling. */
  if (framebuffer_size == 0u || framebuffer_size > limit)
    goto fail;
  framebuffer = (limit - framebuffer_size) & ~(framebuffer_size - 1u);
  if (framebuffer < ram_end)
    goto fail;
  if (registers_size != 0u)
    {
      if (registers_size > framebuffer)
        goto fail;
      registers = (framebuffer - registers_size) & ~(registers_size - 1u);
      if (registers < ram_end)
        goto fail;
    }

  pci_write (devfn, PCI_BAR0, framebuffer | (old_framebuffer & 0xFu));
  if (registers_size != 0u)
    pci_write (devfn, PCI_BAR2, registers | (old_registers & 0xFu));
  /* A rejected BAR assignment must not be exposed as a usable framebuffer. */
  if ((pci_read (devfn, PCI_BAR0) & PCI_MEMORY_MASK) != framebuffer
      || (registers_size != 0u
          && (pci_read (devfn, PCI_BAR2) & PCI_MEMORY_MASK) != registers))
    goto fail;

  pci_write_command (devfn, (u16)(command | 3u));
  vbe.framebuffer = pci_read (devfn, PCI_BAR0) & PCI_MEMORY_MASK;
  vbe.memory_size = (u32)vbe_read (DISPI_MEMORY_64K) << 16;
  /* The CPU mapping must cover all advertised video memory. */
  if (vbe.memory_size == 0u || vbe.memory_size > framebuffer_size)
    goto fail;
  return 1;

fail:
  pci_write_command (devfn, (u16)(command & ~3u));
  pci_write (devfn, PCI_BAR0, old_framebuffer);
  pci_write (devfn, PCI_BAR2, old_registers);
  pci_write_command (devfn, command);
  vbe.framebuffer = 0u;
  vbe.memory_size = 0u;
  return 0;
}

static int
mode_supported (const struct vbe_mode *mode)
{
  /* Advertise only modes within DISPI capabilities and installed VRAM. */
  return vbe.present && mode->width <= vbe.max_width
         && mode->height <= vbe.max_height && mode->bpp <= vbe.max_bpp
         && (u32)mode->width * mode->height * (mode->bpp / 8u)
                <= vbe.memory_size;
}

const struct vbe_mode *
vbe_find_mode (u16 number)
{
  u8 index;

  for (index = 0; index < VBE_MODE_COUNT; ++index)
    if (modes[index].number == number && mode_supported (&modes[index]))
      return &modes[index];
  return (const struct vbe_mode *)0;
}

void
vbe_init (void)
{
  u16 devfn;
  u16 enable;
  u8 index;
  u8 count = 0u;

  vbe.current_mode = 0x03u;
  for (devfn = 0; devfn < 256u; ++devfn)
    if (pci_read (devfn, 0u) == QEMU_VGA_ID
        && (pci_read (devfn, 0x08u) >> 16) == 0x0300u)
      break;
  /* Absence of the supported PCI device must leave text VGA available. */
  if (devfn == 256u)
    return;
  vbe_write (DISPI_ID, 0xB0C5u);
  if (vbe_read (DISPI_ID) != 0xB0C5u || !pci_configure_vga (devfn))
    return;

  enable = vbe_read (DISPI_ENABLE);
  vbe_write (DISPI_ENABLE, (u16)(enable | DISPI_GETCAPS));
  vbe.max_width = vbe_read (DISPI_XRES);
  vbe.max_height = vbe_read (DISPI_YRES);
  vbe.max_bpp = vbe_read (DISPI_BPP);
  vbe_write (DISPI_ENABLE, enable);
  vbe.present = 1u;
  for (index = 0; index < VBE_MODE_COUNT; ++index)
    if (mode_supported (&modes[index]))
      vbe.mode_list[count++] = modes[index].number;
  vbe.mode_list[count] = 0xFFFFu;
}

void
vbe_leave_graphics (void)
{
  if (vbe.present)
    vbe_write (DISPI_ENABLE, 0u);
  vbe.current_mode = 0x03u;
}

static void
graphics_registers (void)
{
  /* DISPI sets graphics timing; enable VGA planes and attribute output too. */
  io_out8 (0x03C4u, 0x01u);
  io_out8 (0x03C5u, 0x01u);
  io_out8 (0x03C4u, 0x02u);
  io_out8 (0x03C5u, 0x0Fu);
  io_out8 (0x03C4u, 0x04u);
  io_out8 (0x03C5u, 0x0Eu);
  io_out8 (0x03CEu, 0x05u);
  io_out8 (0x03CFu, 0x40u);
  io_out8 (0x03CEu, 0x06u);
  io_out8 (0x03CFu, 0x05u);
  io_in8 (0x03DAu);
  io_out8 (0x03C0u, 0x10u);
  io_out8 (0x03C0u, 0x41u);
  io_in8 (0x03DAu);
  io_out8 (0x03C0u, 0x20u);
}

u16
vbe_set_mode (u16 request)
{
  const struct vbe_mode *mode;
  volatile u8 *bda = (volatile u8 *)MUBIOS_BDA_BASE;
  u16 saved[10];
  u16 index;
  u16 enable;
  u16 number = request & 0x3FFFu;
  u8 old_video_ctl = bda[BDA_VIDEO_CTL];
  volatile u32 *pixel;
  u32 words;

  if (number == 0x03u)
    {
      vga_text_mode_set ((u8)((request >> 8) & 0x80u));
      return VBE_OK;
    }
  mode = vbe_find_mode (number);
  /* Only LFB access is implemented. Bit 11 requests a VBE 3.0 CRTC block;
     banked modes and custom timings are not supported or advertised. */
  if (mode == (const struct vbe_mode *)0 || (request & VBE_MODE_LINEAR) == 0u)
    return VBE_FAILED;

  for (index = 0; index < 10u; ++index)
    saved[index] = vbe_read (index);
  vbe_write (DISPI_ENABLE, 0u);
  vbe_write (DISPI_BPP, mode->bpp);
  vbe_write (DISPI_XRES, mode->width);
  vbe_write (DISPI_YRES, mode->height);
  vbe_write (DISPI_BANK, 0u);
  /* Delay clearing until the actual programmed mode has been verified. */
  enable = DISPI_ENABLED | DISPI_LINEAR | DISPI_PRESERVE;
  vbe_write (DISPI_ENABLE, enable);
  if (vbe_read (DISPI_XRES) != mode->width
      || vbe_read (DISPI_YRES) != mode->height
      || vbe_read (DISPI_BPP) != mode->bpp
      || vbe_read (DISPI_VIRT_WIDTH) != mode->width
      || (vbe_read (DISPI_ENABLE) & (DISPI_ENABLED | DISPI_LINEAR))
             != (DISPI_ENABLED | DISPI_LINEAR))
    {
      /* Restore the prior hardware state on a rejected mode request. */
      vbe_write (DISPI_ENABLE, 0u);
      for (index = 1u; index <= DISPI_BPP; ++index)
        vbe_write (index, saved[index]);
      vbe_write (DISPI_ENABLE, saved[DISPI_ENABLE] | DISPI_PRESERVE);
      vbe_write (DISPI_BANK, saved[DISPI_BANK]);
      vbe_write (DISPI_VIRT_WIDTH, saved[DISPI_VIRT_WIDTH]);
      vbe_write (DISPI_X_OFFSET, saved[DISPI_X_OFFSET]);
      vbe_write (DISPI_Y_OFFSET, saved[DISPI_Y_OFFSET]);
      vbe_write (DISPI_ENABLE, saved[DISPI_ENABLE]);
      /* Disabling DISPI alone does not restore text CRTC/graphics registers.
       */
      if ((saved[DISPI_ENABLE] & DISPI_ENABLED) == 0u)
        {
          vga_text_mode_set (0x80u);
          bda[BDA_VIDEO_CTL] = old_video_ctl;
          vbe.current_mode
              = (u16)(0x03u | ((u16)(old_video_ctl & 0x80u) << 8));
        }
      return VBE_FAILED;
    }

  graphics_registers ();
  if ((request & VBE_MODE_PRESERVE) == 0u)
    {
      pixel = (volatile u32 *)vbe.framebuffer;
      words = vbe.memory_size / sizeof (*pixel);
      while (words-- != 0u)
        *pixel++ = 0u;
      enable &= (u16)~DISPI_PRESERVE;
      vbe_write (DISPI_ENABLE, enable);
    }
  vbe.current_mode = request;
  /* FFh denotes a non-legacy video mode; no mode-03h geometry is claimed. */
  bda[BDA_VIDEO_MODE] = 0xFFu;
  *(volatile u16 *)(bda + BDA_VIDEO_COLUMNS) = mode->width / 8u;
  *(volatile u16 *)(bda + BDA_VIDEO_PAGE_SIZE) = 0u;
  *(volatile u16 *)(bda + BDA_VIDEO_PAGE_START) = 0u;
  bda[BDA_VIDEO_PAGE] = 0u;
  for (index = 0u; index < VGA_PAGE_COUNT; ++index)
    *(volatile u16 *)(bda + BDA_CURSOR_BASE + index * 2u) = 0u;
  *(volatile u16 *)(bda + BDA_CURSOR_SHAPE) = 0u;
  bda[BDA_VIDEO_ROWS] = (u8)(mode->height / VGA_FONT_HEIGHT - 1u);
  *(volatile u16 *)(bda + BDA_VIDEO_HEIGHT) = VGA_FONT_HEIGHT;
  bda[BDA_VIDEO_CTL] = (u8)(0x60u | ((request >> 8) & 0x80u));
  return VBE_OK;
}

u16
vbe_get_mode (u16 *number)
{
  u8 index;
  u16 enable = vbe_read (DISPI_ENABLE);

  /* GRUB's native Bochs driver may program DISPI without calling INT 10h.
     Query hardware instead of returning a stale BIOS mode number. */
  if ((enable & DISPI_ENABLED) == 0u)
    {
      *number = (u16)(0x03u | (vbe.current_mode & VBE_MODE_PRESERVE));
      return VBE_OK;
    }
  for (index = 0; index < VBE_MODE_COUNT; ++index)
    if (mode_supported (&modes[index])
        && modes[index].width == vbe_read (DISPI_XRES)
        && modes[index].height == vbe_read (DISPI_YRES)
        && modes[index].bpp == vbe_read (DISPI_BPP))
      {
        *number = modes[index].number
                  | ((enable & DISPI_LINEAR) != 0u ? VBE_MODE_LINEAR : 0u)
                  | ((enable & DISPI_PRESERVE) != 0u ? VBE_MODE_PRESERVE : 0u);
        return VBE_OK;
      }
  return VBE_FAILED;
}

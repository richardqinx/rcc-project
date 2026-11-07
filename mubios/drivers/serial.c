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
#include "io.h"

#define COM1_BASE 0x03F8u

#define COM_DATA_OFFSET 0x00u
#define COM_IER_OFFSET 0x01u
#define COM_FCR_OFFSET 0x02u
#define COM_LCR_OFFSET 0x03u
#define COM_MCR_OFFSET 0x04u
#define COM_LSR_OFFSET 0x05u

#define COM_LCR_DLAB 0x80u
#define COM_LCR_8N1 0x03u

#define COM_FCR_ENABLE_FIFO 0x01u
#define COM_FCR_CLEAR_RX 0x02u
#define COM_FCR_CLEAR_TX 0x04u
#define COM_FCR_TRIGGER_14 0xC0u

#define COM_MCR_DTR 0x01u
#define COM_MCR_RTS 0x02u
#define COM_MCR_OUT2 0x08u

#define COM_LSR_THRE 0x20u

static u16
com1_port (u16 offset)
{
  return (u16)(COM1_BASE + offset);
}

void
serial_init (void)
{
  // clang-format off
    io_out8(com1_port(COM_IER_OFFSET), 0x00u);

    io_out8(com1_port(COM_LCR_OFFSET), COM_LCR_DLAB);
    io_out8(com1_port(COM_DATA_OFFSET), 0x01u);
    io_out8(com1_port(COM_IER_OFFSET), 0x00u);

    io_out8(com1_port(COM_LCR_OFFSET), COM_LCR_8N1);
    io_out8(
        com1_port(COM_FCR_OFFSET),
        COM_FCR_ENABLE_FIFO |
        COM_FCR_CLEAR_RX |
        COM_FCR_CLEAR_TX |
        COM_FCR_TRIGGER_14
    );
    io_out8(
        com1_port(COM_MCR_OFFSET),
        COM_MCR_DTR |
        COM_MCR_RTS |
        COM_MCR_OUT2
    );
  // clang-format on
}

void
serial_putc (u8 value)
{
  while ((io_in8 (com1_port (COM_LSR_OFFSET)) & COM_LSR_THRE) == 0u)
    {
    }

  io_out8 (com1_port (COM_DATA_OFFSET), value);
}

void
serial_write (const char *text)
{
  while (*text != '\0')
    {
      serial_putc ((u8)*text);
      ++text;
    }
}

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
#define COM_MSR_OFFSET 0x06u

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
#define COM_LSR_IDLE 0x40u
#define COM_LSR_DATA_READY 0x01u
#define BIOS_SERIAL_TIMEOUT 0x80u

#define PIT_COUNTER0 0x0040u
#define PIT_COMMAND 0x0043u

static u16
com1_port (u16 offset)
{
  return (u16)(COM1_BASE + offset);
}

/* Latching channel 0 works while the protected-mode service has IRQs off. */
u16
serial_timer_counter (void)
{
  u16 counter;

  io_out8 (PIT_COMMAND, 0x00u);
  counter = io_in8 (PIT_COUNTER0);
  counter |= (u16)((u16)io_in8 (PIT_COUNTER0) << 8);
  return counter;
}

static int
serial_wait (u16 base, u8 mask, u8 timeout, u8 *status)
{
  u16 previous;
  u16 current;
  u32 elapsed;
  u32 duration;

  previous = serial_timer_counter ();
  elapsed = 0;
  /* One BDA timeout unit is one 65536-clock BIOS timer tick. */
  duration = (u32)timeout << 16;

  for (;;)
    {
      *status = io_in8 ((u16)(base + COM_LSR_OFFSET));
      if ((*status & mask) == mask)
        return 1;

      if (elapsed >= duration)
        {
          *status |= BIOS_SERIAL_TIMEOUT;
          return 0;
        }

      current = serial_timer_counter ();
      elapsed += (u16)(previous - current);
      previous = current;
    }
}

u16
serial_status (u16 base)
{
  u8 line;
  u8 modem;

  line = io_in8 ((u16)(base + COM_LSR_OFFSET));
  modem = io_in8 ((u16)(base + COM_MSR_OFFSET));
  return (u16)(((u16)line << 8) | modem);
}

u16
serial_configure (u16 base, u8 parameters)
{
  static const u16 divisors[]
      = { 1047u, 768u, 384u, 192u, 96u, 48u, 24u, 12u };
  u16 divisor;

  /* INT 14h AL: bits 7:5 select baud; bits 4:0 are the UART format. */
  divisor = divisors[parameters >> 5];
  io_out8 ((u16)(base + COM_LCR_OFFSET), COM_LCR_DLAB);
  io_out8 ((u16)(base + COM_DATA_OFFSET), (u8)divisor);
  io_out8 ((u16)(base + COM_IER_OFFSET), (u8)(divisor >> 8));
  io_out8 ((u16)(base + COM_LCR_OFFSET), parameters & 0x1Fu);
  /* Do not reset the FIFO here: pending terminal input must survive. */
  return serial_status (base);
}

u8
serial_transmit (u16 base, u8 value, u8 timeout)
{
  u8 status;

  if (serial_wait (base, COM_LSR_THRE | COM_LSR_IDLE, timeout, &status))
    io_out8 ((u16)(base + COM_DATA_OFFSET), value);

  return status;
}

u8
serial_receive (u16 base, u8 *value, u8 timeout)
{
  u8 status;

  if (serial_wait (base, COM_LSR_DATA_READY, timeout, &status))
    *value = io_in8 ((u16)(base + COM_DATA_OFFSET));

  return status;
}

int
serial_try_receive (u16 base, u8 *value)
{
  u8 status;

  status = io_in8 ((u16)(base + COM_LSR_OFFSET));
  if ((status & COM_LSR_DATA_READY) == 0u)
    return 0;

  *value = io_in8 ((u16)(base + COM_DATA_OFFSET));
  return 1;
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

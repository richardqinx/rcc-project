/*
 RCC-Project

 Copyright (C) 2026 Richard Qin

 This file is part of RCC-Project.
 */

#include "bios.h"
#include "regs.h"

#define BIOS_CF 0x0001u
#define BIOS_INVALID_FUNCTION 0x86u
#define BDA_COM_PORTS 0x0400u
#define BDA_COM_TIMEOUTS 0x047Cu
#define BIOS_COM_PORT_COUNT 4u

void
int14_service (struct bios_regs *regs)
{
  u8 function;
  u8 status;
  u8 value;
  u8 timeout;
  u16 port;
  u16 base;
  u16 result;
  volatile u16 *ports;
  volatile u8 *timeouts;

  function = (u8)(regs->eax >> 8);
  port = (u16)regs->edx;
  ports = (volatile u16 *)BDA_COM_PORTS;
  timeouts = (volatile u8 *)BDA_COM_TIMEOUTS;

  /* BIOS numbers COM ports through DX and advertises their bases in BDA. */
  if (function > 0x03u || port >= BIOS_COM_PORT_COUNT || ports[port] == 0u)
    {
      regs->eax
          = (regs->eax & 0xFFFF00FFu) | ((u32)BIOS_INVALID_FUNCTION << 8);
      regs->flags |= BIOS_CF;
      return;
    }

  base = ports[port];
  timeout = timeouts[port];

  switch (function)
    {
    case 0x00u:
      result = serial_configure (base, (u8)regs->eax);
      regs->eax = (regs->eax & 0xFFFF0000u) | result;
      break;

    case 0x01u:
      status = serial_transmit (base, (u8)regs->eax, timeout);
      regs->eax = (regs->eax & 0xFFFF00FFu) | ((u32)status << 8);
      break;

    case 0x02u:
      value = (u8)regs->eax;
      status = serial_receive (base, &value, timeout);
      regs->eax = (regs->eax & 0xFFFF0000u) | ((u32)status << 8) | value;
      break;

    case 0x03u:
      result = serial_status (base);
      regs->eax = (regs->eax & 0xFFFF0000u) | result;
      break;
    }

  /* As in SeaBIOS, timeouts use AH bit 7; CF denotes an invalid call. */
  regs->flags &= (u16)~BIOS_CF;
}

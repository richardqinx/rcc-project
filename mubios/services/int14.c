/*
 RCC-Project

 Copyright (C) 2026 Richard Qin

 This file is part of RCC-Project.
 */

#include "bios.h"
#include "io.h"
#include "regs.h"

#define COM1_BASE 0x03F8u
#define COM_DATA 0x00u
#define COM_LSR 0x05u
#define COM_LSR_THRE 0x20u
#define COM_LSR_IDLE 0x40u

static void
int14_putc (u8 value)
{
  while ((io_in8 ((u16)(COM1_BASE + COM_LSR)) & COM_LSR_THRE) == 0u)
    {
    }

  io_out8 ((u16)(COM1_BASE + COM_DATA), value);
}

void
int14_service (struct bios_regs *regs)
{
  u8 function;
  u8 status;

  function = (u8)(regs->eax >> 8);

  if (function == 0x00u)
    {
      serial_init ();
      regs->eax &= 0xFFFF0000u;
      return;
    }

  if (function == 0x01u)
    {
      int14_putc ((u8)regs->eax);
      status = (u8)(io_in8 ((u16)(COM1_BASE + COM_LSR))
                    | COM_LSR_THRE | COM_LSR_IDLE);
      regs->eax = (regs->eax & 0xFFFF00FFu) | ((u32)status << 8);
      return;
    }

  if (function == 0x03u)
    {
      status = io_in8 ((u16)(COM1_BASE + COM_LSR));
      regs->eax = (regs->eax & 0xFFFF00FFu) | ((u32)status << 8);
      return;
    }

  regs->eax = 0x0100u;
}

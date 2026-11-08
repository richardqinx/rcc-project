/*
 RCC-Project

 Copyright (C) 2026 Richard Qin

 This file is part of RCC-Project.

 RCC-Project is free software; you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free
 Software Foundation; either version 3 of the License, or (at your option)
 any later version.

 RCC-Project is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 more details.
 */

#include "bios.h"
#include "regs.h"

#define BIOS_ZF 0x0040u
#define BDA_KBD_HEAD 0x041Au
#define BDA_KBD_TAIL 0x041Cu
#define BDA_KBD_BUFFER 0x041Eu
#define BDA_KBD_BUFFER_START 0x001Eu
#define BDA_KBD_BUFFER_END 0x003Eu

static u8
normalize_scancode (u8 scancode)
{
  switch (scancode)
    {
    case 0x5Au:
      return 0x1Cu;
    default:
      return scancode;
    }
}

static u8
scancode_to_ascii (u8 scancode)
{
  switch (scancode)
    {
    case 0x1Eu:
      return 'a';
    case 0x30u:
      return 'b';
    case 0x2Eu:
      return 'c';
    case 0x20u:
      return 'd';
    case 0x12u:
      return 'e';
    case 0x21u:
      return 'f';
    case 0x22u:
      return 'g';
    case 0x23u:
      return 'h';
    case 0x17u:
      return 'i';
    case 0x24u:
      return 'j';
    case 0x25u:
      return 'k';
    case 0x26u:
      return 'l';
    case 0x32u:
      return 'm';
    case 0x31u:
      return 'n';
    case 0x18u:
      return 'o';
    case 0x19u:
      return 'p';
    case 0x10u:
      return 'q';
    case 0x13u:
      return 'r';
    case 0x1Fu:
      return 's';
    case 0x14u:
      return 't';
    case 0x16u:
      return 'u';
    case 0x2Fu:
      return 'v';
    case 0x11u:
      return 'w';
    case 0x2Du:
      return 'x';
    case 0x15u:
      return 'y';
    case 0x2Cu:
      return 'z';
    case 0x39u:
      return ' ';
    case 0x1Cu:
      return '\r';
    default:
      return 0;
    }
}

void
int16_service (struct bios_regs *regs)
{
  volatile u16 *head;
  volatile u16 *tail;
  volatile u16 *buffer;
  u8 function;
  u16 position;
  u16 value;
  u8 scancode;
  u8 ascii;

  function = (u8)(regs->eax >> 8);

  if (function != 0x00u && function != 0x01u)
    {
      regs->eax = 0;
      regs->flags |= BIOS_ZF;
      return;
    }

  head = (volatile u16 *)BDA_KBD_HEAD;
  tail = (volatile u16 *)BDA_KBD_TAIL;
  buffer = (volatile u16 *)BDA_KBD_BUFFER;

  if (*head == *tail)
    {
      regs->eax = 0;
      regs->flags |= BIOS_ZF;
      return;
    }

  position = *head;
  value = buffer[(position - BDA_KBD_BUFFER_START) >> 1];
  scancode = normalize_scancode ((u8)(value >> 8));
  ascii = scancode_to_ascii (scancode);

  regs->eax = ((u32)scancode << 8) | ascii;
  regs->flags &= (u16)~BIOS_ZF;

  if (function == 0x00u)
    {
      position += 2;

      if (position >= BDA_KBD_BUFFER_END)
        position = BDA_KBD_BUFFER_START;

      *head = position;
    }
}

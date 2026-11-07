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

#ifndef MUBIOS_IO_H
#define MUBIOS_IO_H

#include "bios.h"

static inline void
io_out8 (u16 port, u8 value)
{
  __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8
io_in8 (u16 port)
{
  u8 value;

  __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));

  return value;
}

#endif // MUBIOS_IO_H

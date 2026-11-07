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

#ifndef MUBIOS_REGS_H
#define MUBIOS_REGS_H

#include "bios.h"

struct bios_regs
{
  u16 ax;
  u16 bx;
  u16 cx;
  u16 dx;
  u16 si;
  u16 di;
  u16 bp;
  u16 ds;
  u16 es;
  u16 flags;
};

#endif // MUBIOS_REGS_H

/*
 RCC-Project

 Copyright (C) 2026 Richard Qin

 This file is part of RCC-Project.
 */

#include "bios.h"
#include "io.h"

#define CMOS_INDEX_PORT 0x0070u
#define CMOS_DATA_PORT 0x0071u

u8
cmos_read8 (u8 index)
{
  io_out8 (CMOS_INDEX_PORT, index);
  return io_in8 (CMOS_DATA_PORT);
}

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

#define IOAPIC_INDEX 0xFEC00000u
#define IOAPIC_DATA 0xFEC00010u

#define IOAPIC_IRQ0_LOW 0x10u
#define IOAPIC_IRQ0_HIGH 0x11u

#define IOAPIC_IRQ0_EXTINT 0x00000700u

#define IA32_APIC_BASE_MSR 0x0000001Bu
#define APIC_BASE 0xFEE00000u
#define APIC_SVR_OFFSET 0x000000F0u
#define APIC_LVT0_OFFSET 0x00000350u

static volatile u32 *const ioapic_index = (volatile u32 *)IOAPIC_INDEX;

static volatile u32 *const ioapic_data = (volatile u32 *)IOAPIC_DATA;

static inline void
rdmsr (u32 msr, u32 *low, u32 *high)
{
  __asm__ volatile ("rdmsr" : "=a"(*low), "=d"(*high) : "c"(msr));
}

static inline void
wrmsr (u32 msr, u32 low, u32 high)
{
  __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

static void
ioapic_write (u8 index, u32 value)
{
  *ioapic_index = index;
  *ioapic_data = value;
}

void
ioapic_init (void)
{
  u32 apic_base_low;
  u32 apic_base_high;

  rdmsr (IA32_APIC_BASE_MSR, &apic_base_low, &apic_base_high);

  apic_base_low |= 0x00000800u;
  wrmsr (IA32_APIC_BASE_MSR, apic_base_low, apic_base_high);

  /*
   * Enable the local APIC.
   */
  *(volatile u32 *)(APIC_BASE + APIC_SVR_OFFSET) = 0x000001FFu;

  /*
   * Route legacy PIC interrupts through LVT0.
   */
  *(volatile u32 *)(APIC_BASE + APIC_LVT0_OFFSET) = 0x00000700u;

  /*
   * Route IOAPIC pin 0 to the legacy PIC ExtINT path.
   */
  ioapic_write (IOAPIC_IRQ0_LOW, IOAPIC_IRQ0_EXTINT);
  ioapic_write (IOAPIC_IRQ0_HIGH, 0x00000000u);
}

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

#define PCI_CONFIG_ADDRESS 0x0CF8u
#define PCI_CONFIG_DATA 0x0CFCu
#define PCI_VENDOR_DEVICE 0x00u
#define PCI_INTERRUPT_LINE 0x3Cu
#define PCI_INTERRUPT_PIN 0x3Du
#define PCI_VENDOR_ABSENT 0xFFFFu
#define PIIX3_ISA_ID 0x70008086u
#define PIIX4_PM_ID 0x71138086u
#define PIIX_PIRQ_ROUTE 0x60u
#define PIIX_SCI_IRQ 9u
#define ELCR_MASTER 0x04D0u
#define ELCR_SLAVE 0x04D1u

/* Match SeaBIOS's PIRQ A-D assignment for the QEMU pc machine. */
static const u8 pirq_irqs[4] = { 10u, 10u, 11u, 11u };

static u32
pci_read (u16 devfn, u8 offset)
{
  io_out32 (PCI_CONFIG_ADDRESS,
            0x80000000u | ((u32)devfn << 8) | (offset & 0xFCu));
  return io_in32 (PCI_CONFIG_DATA);
}

static void
pci_write8 (u16 devfn, u8 offset, u8 value)
{
  io_out32 (PCI_CONFIG_ADDRESS,
            0x80000000u | ((u32)devfn << 8) | (offset & 0xFCu));
  io_out8 ((u16)(PCI_CONFIG_DATA + (offset & 3u)), value);
}

void
pci_irq_init (void)
{
  u16 bridge;
  u16 devfn;
  u16 level_irqs = 0;
  u32 id;
  u8 pin;
  u8 pirq;
  u8 irq;

  /* PIRQ routing registers below belong to the PIIX3 ISA bridge. */
  for (bridge = 0; bridge < 256u; ++bridge)
    if (pci_read (bridge, PCI_VENDOR_DEVICE) == PIIX3_ISA_ID)
      break;

  if (bridge == 256u)
    return;

  for (pirq = 0; pirq < 4u; ++pirq)
    {
      irq = pirq_irqs[pirq];
      pci_write8 (bridge, (u8)(PIIX_PIRQ_ROUTE + pirq), irq);
      level_irqs |= (u16)(1u << irq);
    }

  for (devfn = 0; devfn < 256u; ++devfn)
    {
      id = pci_read (devfn, PCI_VENDOR_DEVICE);
      /* PCI returns an all-ones vendor ID for an absent function. */
      if ((u16)id == PCI_VENDOR_ABSENT)
        continue;

      pin = (u8)(pci_read (devfn, PCI_INTERRUPT_PIN) >> 8);
      /* Interrupt Pin zero denotes a function without a PCI INTx pin. */
      if (pin == 0u)
        continue;

      /* The PIIX PM SCI is wired directly to IRQ9, not through PIRQ. */
      if (id == PIIX4_PM_ID)
        {
          irq = PIIX_SCI_IRQ;
          level_irqs |= (u16)(1u << irq);
        }
      else
        {
          /* QEMU pc: PIRQ = (slot - 1 + zero-based INTx pin) & 3. */
          pirq = (u8)(((devfn >> 3) - 1u + pin - 1u) & 3u);
          irq = pirq_irqs[pirq];
        }

      pci_write8 (devfn, PCI_INTERRUPT_LINE, irq);
    }

  /* PCI INTx and SCI are level-triggered; preserve other IRQ settings. */
  io_out8 (ELCR_MASTER, (u8)(io_in8 (ELCR_MASTER) | (u8)level_irqs));
  io_out8 (ELCR_SLAVE, (u8)(io_in8 (ELCR_SLAVE) | (level_irqs >> 8)));
}

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
#include "io.h"

#define KBD_STATUS_PORT 0x0064u
#define KBD_DATA_PORT 0x0060u

#define KBD_STATUS_OBF 0x01u
#define KBD_STATUS_IBF 0x02u
#define KBD_STATUS_AUX 0x20u

#define KBD_CMD_READ_CONFIG 0x20u
#define KBD_CMD_WRITE_CONFIG 0x60u
#define KBD_CMD_DISABLE_KBD 0xADu
#define KBD_CMD_ENABLE_KBD 0xAEu

/*
 * 8042 command byte bits.
 * Bit 0 enables the keyboard IRQ, bit 1 the auxiliary port IRQ, bit 4 and bit
 * 5 hold the keyboard and auxiliary clock, and bit 6 selects set 1 scan codes
 * on the keyboard interface.
 */
#define KBD_CONFIG_IRQ1 0x01u
#define KBD_CONFIG_IRQ12 0x02u
#define KBD_CONFIG_DISABLE_KBD_CLOCK 0x10u
#define KBD_CONFIG_DISABLE_AUX_CLOCK 0x20u
#define KBD_CONFIG_TRANSLATE 0x40u

#define KBD_DEVICE_ENABLE_SCAN 0xF4u

#define KBD_WAIT_LIMIT 0x10000u

static int
wait_input_clear (void)
{
  u32 count;

  for (count = 0; count < KBD_WAIT_LIMIT; count++)
    {
      if ((io_in8 (KBD_STATUS_PORT) & KBD_STATUS_IBF) == 0u)
        return 0;
    }

  return -1;
}

static int
wait_output_full (void)
{
  u32 count;

  for (count = 0; count < KBD_WAIT_LIMIT; count++)
    {
      if ((io_in8 (KBD_STATUS_PORT) & KBD_STATUS_OBF) != 0u)
        return 0;
    }

  return -1;
}

static int
kbd_write_command (u8 command)
{
  if (wait_input_clear () != 0)
    return -1;

  io_out8 (KBD_STATUS_PORT, command);
  return 0;
}

static int
kbd_write_data (u8 value)
{
  if (wait_input_clear () != 0)
    return -1;

  io_out8 (KBD_DATA_PORT, value);
  return 0;
}

void
kbd_init (void)
{
  u8 config;

  /* Disable both ports before the controller is reconfigured. */
  kbd_write_command (KBD_CMD_DISABLE_KBD);

  /* Drain the self-test and any pending scan code left in the output buffer.
   */
  while ((io_in8 (KBD_STATUS_PORT) & KBD_STATUS_OBF) != 0u)
    (void)io_in8 (KBD_DATA_PORT);

  if (kbd_write_command (KBD_CMD_READ_CONFIG) != 0)
    return;

  if (wait_output_full () != 0)
    return;

  config = io_in8 (KBD_DATA_PORT);

  config
      &= (u8) ~(KBD_CONFIG_DISABLE_KBD_CLOCK | KBD_CONFIG_DISABLE_AUX_CLOCK);
  config |= KBD_CONFIG_IRQ1 | KBD_CONFIG_IRQ12 | KBD_CONFIG_TRANSLATE;

  if (kbd_write_command (KBD_CMD_WRITE_CONFIG) != 0)
    return;

  kbd_write_data (config);

  kbd_write_command (KBD_CMD_ENABLE_KBD);

  /* Ask the keyboard to start scanning and consume its acknowledge byte. */
  kbd_write_data (KBD_DEVICE_ENABLE_SCAN);

  if (wait_output_full () == 0)
    (void)io_in8 (KBD_DATA_PORT);
}

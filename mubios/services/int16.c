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
#include "regs.h"

#define BIOS_ZF 0x0040u
#define BDA_KBD_HEAD 0x041Au
#define BDA_KBD_TAIL 0x041Cu
#define BDA_KBD_BUFFER_START 0x001Eu
#define BDA_KBD_BUFFER_END 0x003Eu
#define BDA_KBD_FLAGS 0x0417u
#define BDA_KBD_EXTENDED_FLAGS 0x0496u
#define BDA_COM1 0x0400u
#define KBD_STATUS_PORT 0x0064u
#define KBD_DATA_PORT 0x0060u
#define KBD_STATUS_OBF 0x01u
#define KBD_STATUS_AUX 0x20u
#define KBD_RELEASE 0x80u
#define KBD_IGNORED 0xFFFFu
#define TERMINAL_ESCAPE 0x1Bu
#define TERMINAL_ESCAPE_CLOCKS 119319u /* 100 ms at the PIT's 1193182 Hz. */

struct ascii_key
{
  u8 normal;
  u8 shifted;
};

/* Set 1 positions, also used to give terminal ASCII its BIOS scan code. */
static const struct ascii_key ascii_keys[] = {
  [0x01] = { 0x1B, 0x1B }, [0x02] = { '1', '!' },   [0x03] = { '2', '@' },
  [0x04] = { '3', '#' },   [0x05] = { '4', '$' },   [0x06] = { '5', '%' },
  [0x07] = { '6', '^' },   [0x08] = { '7', '&' },   [0x09] = { '8', '*' },
  [0x0A] = { '9', '(' },   [0x0B] = { '0', ')' },   [0x0C] = { '-', '_' },
  [0x0D] = { '=', '+' },   [0x0E] = { '\b', '\b' }, [0x0F] = { '\t', '\t' },
  [0x10] = { 'q', 'Q' },   [0x11] = { 'w', 'W' },   [0x12] = { 'e', 'E' },
  [0x13] = { 'r', 'R' },   [0x14] = { 't', 'T' },   [0x15] = { 'y', 'Y' },
  [0x16] = { 'u', 'U' },   [0x17] = { 'i', 'I' },   [0x18] = { 'o', 'O' },
  [0x19] = { 'p', 'P' },   [0x1A] = { '[', '{' },   [0x1B] = { ']', '}' },
  [0x1C] = { '\r', '\r' }, [0x1E] = { 'a', 'A' },   [0x1F] = { 's', 'S' },
  [0x20] = { 'd', 'D' },   [0x21] = { 'f', 'F' },   [0x22] = { 'g', 'G' },
  [0x23] = { 'h', 'H' },   [0x24] = { 'j', 'J' },   [0x25] = { 'k', 'K' },
  [0x26] = { 'l', 'L' },   [0x27] = { ';', ':' },   [0x28] = { '\'', '"' },
  [0x29] = { '`', '~' },   [0x2B] = { '\\', '|' },  [0x2C] = { 'z', 'Z' },
  [0x2D] = { 'x', 'X' },   [0x2E] = { 'c', 'C' },   [0x2F] = { 'v', 'V' },
  [0x30] = { 'b', 'B' },   [0x31] = { 'n', 'N' },   [0x32] = { 'm', 'M' },
  [0x33] = { ',', '<' },   [0x34] = { '.', '>' },   [0x35] = { '/', '?' },
  [0x37] = { '*', '*' },   [0x39] = { ' ', ' ' },   [0x4A] = { '-', '-' },
  [0x4E] = { '+', '+' },   [0x56] = { '\\', '|' }
};

struct terminal_sequence
{
  char text[5];
  u16 key;
};

/* CSI and SS3 sequences used by terminals in normal/application mode. */
static const struct terminal_sequence terminal_sequences[]
    = { { "[A", 0x48E0u },   { "[B", 0x50E0u },   { "[C", 0x4DE0u },
        { "[D", 0x4BE0u },   { "OA", 0x48E0u },   { "OB", 0x50E0u },
        { "OC", 0x4DE0u },   { "OD", 0x4BE0u },   { "[H", 0x47E0u },
        { "[F", 0x4FE0u },   { "OH", 0x47E0u },   { "OF", 0x4FE0u },
        { "[1~", 0x47E0u },  { "[4~", 0x4FE0u },  { "[7~", 0x47E0u },
        { "[8~", 0x4FE0u },  { "[2~", 0x52E0u },  { "[3~", 0x53E0u },
        { "[5~", 0x49E0u },  { "[6~", 0x51E0u },  { "[Z", 0x0F00u },
        { "OP", 0x3B00u },   { "OQ", 0x3C00u },   { "OR", 0x3D00u },
        { "OS", 0x3E00u },   { "[11~", 0x3B00u }, { "[12~", 0x3C00u },
        { "[13~", 0x3D00u }, { "[14~", 0x3E00u }, { "[15~", 0x3F00u },
        { "[17~", 0x4000u }, { "[18~", 0x4100u }, { "[19~", 0x4200u },
        { "[20~", 0x4300u }, { "[21~", 0x4400u }, { "[23~", 0x8500u },
        { "[24~", 0x8600u } };

static u8 terminal_buffer[16];
static u8 terminal_count;
static u8 terminal_escape_pending;
static u16 terminal_counter;
static u32 terminal_elapsed;
static u16 decoded_tail;

static u16
next_position (u16 position)
{
  position += 2u;
  if (position >= BDA_KBD_BUFFER_END)
    position = BDA_KBD_BUFFER_START;
  return position;
}

static int
keyboard_full (void)
{
  volatile u16 *head = (volatile u16 *)BDA_KBD_HEAD;
  volatile u16 *tail = (volatile u16 *)BDA_KBD_TAIL;

  return next_position (*tail) == *head;
}

static void
enqueue_key (u16 key)
{
  volatile u16 *tail = (volatile u16 *)BDA_KBD_TAIL;
  volatile u16 *slot = (volatile u16 *)(MUBIOS_BDA_BASE + *tail);

  *slot = key;
  *tail = next_position (*tail);
  /* This producer stores a complete BIOS key, not an IRQ1 raw scan code. */
  decoded_tail = *tail;
}

static u16
scancode_to_key (u8 scancode)
{
  if (scancode < sizeof (ascii_keys) / sizeof (ascii_keys[0])
      && ascii_keys[scancode].normal != 0u)
    return (u16)(((u16)scancode << 8) | ascii_keys[scancode].normal);

  if (scancode >= 0x3Bu && scancode <= 0x44u)
    return (u16)((u16)scancode << 8);
  if (scancode >= 0x47u && scancode <= 0x53u)
    return (u16)(((u16)scancode << 8) | 0xE0u);
  if (scancode == 0x57u || scancode == 0x58u)
    return (u16)((u16)(scancode + 0x2Eu) << 8);

  /* Modifier make codes and controller responses are not character keys. */
  return KBD_IGNORED;
}

static void
decode_irq_keys (void)
{
  volatile u16 *tail = (volatile u16 *)BDA_KBD_TAIL;
  volatile u16 *slot;
  u16 read_position;
  u16 write_position;
  u16 end;
  u16 key;

  if (decoded_tail == 0u)
    decoded_tail = BDA_KBD_BUFFER_START;

  /* IRQ1 currently queues raw set 1 codes; decode only its new entries. */
  read_position = decoded_tail;
  write_position = decoded_tail;
  end = *tail;
  while (read_position != end)
    {
      slot = (volatile u16 *)(MUBIOS_BDA_BASE + read_position);
      key = scancode_to_key ((u8)(*slot >> 8));
      if (key != KBD_IGNORED)
        {
          slot = (volatile u16 *)(MUBIOS_BDA_BASE + write_position);
          *slot = key;
          write_position = next_position (write_position);
        }
      read_position = next_position (read_position);
    }
  /* Compact away modifier codes before publishing the decoded tail. */
  *tail = write_position;
  decoded_tail = write_position;
}

static void
poll_keyboard_controller (void)
{
  u8 status;
  u8 scancode;
  u16 key;

  /* INT 16h C services run with IRQs off, so a blocking read must poll. */
  while (!keyboard_full ())
    {
      status = io_in8 (KBD_STATUS_PORT);
      if ((status & KBD_STATUS_OBF) == 0u)
        return;

      /* The auxiliary device shares the data port, not the keyboard IRQ. */
      if ((status & KBD_STATUS_AUX) != 0u)
        return;

      scancode = io_in8 (KBD_DATA_PORT);
      if ((scancode & KBD_RELEASE) != 0u)
        continue;

      key = scancode_to_key (scancode);
      if (key != KBD_IGNORED)
        enqueue_key (key);
    }
}

static u16
ascii_to_key (u8 character)
{
  u16 scan;

  /* Terminals commonly send DEL for the Backspace key. */
  if (character == 0x7Fu)
    return 0x0E08u;
  if (character == '\n')
    return 0x1C0Au;
  if (character == 0u)
    return 0x0300u;

  for (scan = 1; scan < sizeof (ascii_keys) / sizeof (ascii_keys[0]); ++scan)
    {
      if (ascii_keys[scan].normal == character
          || ascii_keys[scan].shifted == character)
        return (u16)((scan << 8) | character);
    }

  /* A terminal encodes Ctrl+A through Ctrl+Z as ASCII 1 through 26. */
  if (character >= 1u && character <= 26u)
    {
      for (scan = 1; scan < sizeof (ascii_keys) / sizeof (ascii_keys[0]);
           ++scan)
        {
          if (ascii_keys[scan].normal == (u8)('a' + character - 1u))
            return (u16)((scan << 8) | character);
        }
    }

  switch (character)
    {
    case 0x1Cu:
      return 0x2B1Cu;
    case 0x1Du:
      return 0x1B1Du;
    case 0x1Eu:
      return 0x071Eu;
    case 0x1Fu:
      return 0x0C1Fu;
    default:
      /* Byte-oriented BIOS input has no scan code for other byte values. */
      return character;
    }
}

static void
consume_terminal (u8 length)
{
  u8 index;

  terminal_count -= length;
  for (index = 0; index < terminal_count; ++index)
    terminal_buffer[index] = terminal_buffer[index + length];
  terminal_escape_pending = 0;
}

static int
escape_expired (void)
{
  u16 current;

  current = serial_timer_counter ();
  if (!terminal_escape_pending)
    {
      terminal_counter = current;
      terminal_elapsed = 0;
      terminal_escape_pending = 1;
      return 0;
    }

  terminal_elapsed += (u16)(terminal_counter - current);
  terminal_counter = current;
  return terminal_elapsed >= TERMINAL_ESCAPE_CLOCKS;
}

static void
poll_terminal (void)
{
  volatile u16 *com1 = (volatile u16 *)BDA_COM1;
  u16 base;
  u16 key;
  u16 sequence;
  u8 index;
  u8 length;
  u8 prefix;
  u8 matched;

  base = *com1;
  /* No console UART is advertised when the BDA COM1 entry is zero. */
  if (base == 0u)
    return;

  while (terminal_count < sizeof (terminal_buffer)
         && serial_try_receive (base, &terminal_buffer[terminal_count]))
    ++terminal_count;

  while (terminal_count != 0u && !keyboard_full ())
    {
      matched = 0;
      prefix = 0;
      if (terminal_buffer[0] == TERMINAL_ESCAPE)
        {
          for (sequence = 0; sequence < sizeof (terminal_sequences)
                                            / sizeof (terminal_sequences[0]);
               ++sequence)
            {
              length = 0;
              while (terminal_sequences[sequence].text[length] != '\0')
                ++length;

              for (index = 0; index < length && index + 1u < terminal_count;
                   ++index)
                {
                  if (terminal_buffer[index + 1u]
                      != (u8)terminal_sequences[sequence].text[index])
                    break;
                }

              if (index == length)
                {
                  key = terminal_sequences[sequence].key;
                  consume_terminal ((u8)(length + 1u));
                  matched = 1;
                  break;
                }
              if (index + 1u == terminal_count)
                prefix = 1;
            }

          /* Keep fragmented sequences; a lone Escape must eventually return.
           */
          if (!matched && prefix && !escape_expired ())
            return;
        }

      if (!matched)
        {
          key = ascii_to_key (terminal_buffer[0]);
          consume_terminal (1);
        }
      enqueue_key (key);
    }
}

void
int16_service (struct bios_regs *regs)
{
  volatile u16 *head;
  volatile u16 *tail;
  volatile u16 *slot;
  volatile u8 *flags;
  u8 function;
  u16 position;
  u16 value;
  u8 consume;
  u8 extended;

  function = (u8)(regs->eax >> 8);

  head = (volatile u16 *)BDA_KBD_HEAD;
  tail = (volatile u16 *)BDA_KBD_TAIL;
  flags = (volatile u8 *)BDA_KBD_FLAGS;
  decode_irq_keys ();

  switch (function)
    {
    case 0x02u:
      regs->eax = (regs->eax & 0xFFFFFF00u) | flags[0];
      return;
    case 0x05u:
      /* AH=05h returns AL=1 when the BDA ring cannot accept another key. */
      value = (u16)keyboard_full ();
      if (value == 0u)
        enqueue_key ((u16)regs->ecx);
      regs->eax = (regs->eax & 0xFFFFFF00u) | value;
      return;
    case 0x09u:
      /* Advertise enhanced reads/status, not unimplemented keyboard-ID calls.
       */
      regs->eax = (regs->eax & 0xFFFFFF00u) | 0x20u;
      return;
    case 0x12u:
      regs->eax = (regs->eax & 0xFFFF0000u)
                  | ((u32)((flags[1] & 0xF3u)
                           | (*(volatile u8 *)BDA_KBD_EXTENDED_FLAGS & 0x0Cu))
                     << 8)
                  | flags[0];
      return;
    case 0x00u:
    case 0x01u:
    case 0x10u:
    case 0x11u:
      break;
    default:
      /* SeaBIOS leaves unsupported INT 16h function registers unchanged. */
      return;
    }

  consume = (u8)((function & 1u) == 0u);
  extended = function >= 0x10u;

  for (;;)
    {
      poll_terminal ();
      poll_keyboard_controller ();

      if (*head != *tail)
        {
          position = *head;
          slot = (volatile u16 *)(MUBIOS_BDA_BASE + position);
          value = *slot;
          if (!extended)
            {
              /* Legacy reads expose navigation keys with AL=0, not E0h. */
              if ((u8)value == 0xE0u && (value & 0xFF00u) != 0u)
                value &= 0xFF00u;
              else if (value == 0xE00Du || value == 0xE00Au)
                value = (u16)(0x1C00u | (u8)value);
              else if (value == 0xE02Fu)
                value = 0x352Fu;
            }
          if ((u8)value == 0xF0u && (value & 0xFF00u) != 0u)
            value &= 0xFF00u;

          regs->eax = (regs->eax & 0xFFFF0000u) | value;
          if (consume)
            *head = next_position (position);
          else
            regs->flags &= (u16)~BIOS_ZF;
          return;
        }

      if (!consume)
        {
          regs->flags |= BIOS_ZF;
          return;
        }
      /* IRQs stay disabled here; both input sources are polled on every pass.
       */
    }
}

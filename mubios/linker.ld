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

OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(reset_entry)

MEMORY
{
    ROM  (rx) : ORIGIN = 0xF0000, LENGTH = 0x10000
    EBDA (rw) : ORIGIN = 0x9C000, LENGTH = 0x4000
}

SECTIONS
{
    . = ORIGIN(ROM);
    __rom_start = .;

    . = ALIGN(16);
    .text16 :
    {
        KEEP(*(.text16 .text16.*))
    } > ROM

    . = ALIGN(16);
    .text32 :
    {
        *(.text32 .text32.*)
    } > ROM

    . = ALIGN(4);
    .rodata :
    {
        *(.rodata .rodata.*)
    } > ROM

    . = ALIGN(4);
    .data :
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } > EBDA AT> ROM

    __data_load_start = LOADADDR(.data);

    .reset 0xFFFF0 :
    {
        KEEP(*(.reset))
    } > ROM

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.* COMMON)
        __bss_end = .;
    } > EBDA

    __ebda_start = ORIGIN(EBDA);
    __ebda_end = ORIGIN(EBDA) + LENGTH(EBDA);
    __rom_end = ORIGIN(ROM) + LENGTH(ROM);

    ASSERT(SIZEOF(.reset) <= 16, "reset section exceeds 16 bytes")
    ASSERT(__bss_end <= __ebda_end, "EBDA overflow")
    ASSERT(
        LOADADDR(.reset) + SIZEOF(.reset) <= ORIGIN(ROM) + LENGTH(ROM),
        "ROM overflow"
    )
}

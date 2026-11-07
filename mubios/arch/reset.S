# RCC-Project
#
# Copyright (C) 2026 Richard Qin
#
# This file is part of RCC-Project.
#
# RCC-Project is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 3 of the License, or (at your option) any
# later version.
#
# RCC-Project is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# RCC-Project; see the file COPYING.  If not see
# <http://www.gnu.org/licenses/>.
	
	.intel_syntax noprefix
	.code16

	.extern enter_protected_mode
	
	.section .text16.entry, "ax"

	.global reset_entry
	.type reset_entry, @function

reset_entry:
	cli
	cld

	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x8000

	call enter_protected_mode

reset_halt:
	sti

reset_halt_loop:
	hlt
	jmp reset_halt_loop

	.section .reset, "ax"
	
	.global reset_vector

reset_vector:
	cli

	/*
        * The linker places .text16 at physical address 0xF0000.
        * F000:0000 therefore reaches the first 16-bit entry.
        */
	.byte 0xEA
	.word 0x0000
	.word 0xF000

	.fill 16 - (. - reset_vector), 1, 0x90
	
	
	


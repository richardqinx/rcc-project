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

	.section .text16, "ax"

	.extern post_main
	
	.global enter_protected_mode
	.type enter_protected_mode, @function

enter_protected_mode:
	cli

	# Enable A20 through the fast A20 gate.

	in al, 0x92
	or al, 0x02
	and al, 0xFE
	out 0x92, al

	# Make data references address the ROM code segment.

	push cs
	pop ds

	call 1f
1:
	pop bx
	add bx, gdt_descriptor - 1b
	lgdt [bx]

	# Enable protected mode
	mov eax, cr0
	or eax, 0x00000001
	mov cr0, eax

	# Reload CS with the 32-bit code selector.
	.byte 0x66
	.byte 0xEA
	.long protected_mode_entry
	.word 0x0008

	.align 8

gdt_start:
	.quad 0x0000000000000000

gdt_code:
	.word 0xFFFF
	.word 0x0000
	.byte 0x00
	.byte 0x9A
	.byte 0xCF
	.byte 0x00

gdt_data:
	.word 0xFFFF
	.word 0x0000
	.byte 0x00
	.byte 0x92
	.byte 0xCF
	.byte 0x00

gdt_end:

gdt_descriptor:
	.word gdt_end - gdt_start - 1
	.long gdt_start

	.code32

	.section .text32, "ax"

	.global protected_mode_entry
	.type protected_mode_entry, @function

protected_mode_entry:
	mov ax, 0x0010
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax

	mov esp, 0x00008000

	call post_main

protected_mode_halt:
	hlt
	jmp protected_mode_halt
	
	

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

	.section .text16.mode, "ax"

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

gdt_code16:
	.word 0xFFFF
	.word 0x0000
	.byte 0x0F
	.byte 0x9A
	.byte 0x0F
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

	mov esp, 0x00007000

	call post_main

	call leave_protected_mode

protected_mode_halt:
	hlt
	jmp protected_mode_halt

	.global leave_protected_mode
	.type leave_protected_mode, @function

	.extern __rom_start
	.extern protected16_return
	.extern real_mode_return

leave_protected_mode:
	# Keep the original 16-bit return address at 0x7FFE intact.
	sub esp, 12

	# Stack[0]: protected-mode 16-bit code offset
	# Stack[2]: protected-mode 16-bit code selector
	mov eax, OFFSET FLAT:protected16_return
	mov ebx, OFFSET FLAT:__rom_start
	sub eax, ebx
	mov word ptr [esp], ax
	mov word ptr [esp + 2], 0x0018

	# Stack[4]: real-mode offset
	# Stack[6]: real-mode segment
	mov eax, OFFSET FLAT:real_mode_return
	mov ebx, OFFSET FLAT:__rom_start
	sub eax, ebx
	mov word ptr [esp + 4], ax

	shr ebx, 4
	mov word ptr [esp + 6], bx

	# Enter the 16-bit protected-mode code segment.
	.byte 0x66
	.byte 0xFF, 0x2C, 0x24

	.code16

	.section .text16.mode, "ax"

	.global protected16_return
	.type protected16_return, @function

protected16_return:
	mov bx, sp
	add bx, 4

	# Leave protected mode while already using a 16-bit code segment.
	mov eax, cr0
	and eax, 0xFFFFFFFE
	mov cr0, eax

	# Jump to the prepared real-mode CS:IP pointer.
	.byte 0xFF, 0x2F

	.global real_mode_return
	.type real_mode_return, @function

real_mode_return:
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax
	mov sp, 0x7FFE

	ret
	
	

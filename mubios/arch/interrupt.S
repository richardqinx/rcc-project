# RCC-Project
#
# Copyright (C) 2026 Richard Qin
#
# This file is part of RCC-Project.
#
# RCC-Project is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 3 of the License, or (at your option)
# any later version.
#
# RCC-Project is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.

	.intel_syntax noprefix
	.code16

	.section .text16.irq, "ax"

	.extern enter_protected_service

	.global int1a_entry
	.type int1a_entry, @function

int1a_entry:
	cmp ah, 0x00
	jne int1a_return

	push ds
	push bx

	xor bx, bx
	mov ds, bx

	mov bx, 0x046C
	mov cx, word ptr [bx + 2]
	mov dx, word ptr [bx]

	mov al, byte ptr [0x0470]
	mov byte ptr [0x0470], 0
	mov ah, 0x00

	pop bx
	pop ds

int1a_return:
	iret

	.macro BIOS_SERVICE_ENTRY name, vector
	.global \name
	.type \name, @function

\name:
		# Build struct bios_regs from the pushad and segment frames.
		push ds
		push es
		.byte 0x66
		pushad

		# ESI = linear address of struct bios_regs.
		movzx esi, sp
		xor eax, eax
		mov ax, ss
		shl eax, 4
		add esi, eax

		# EBP = original SS:SP return point, packed as SS:SP.
		movzx ebp, sp
		sub ebp, 2
		xor eax, eax
		mov ax, ss
		shl eax, 16
		or ebp, eax

		mov edi, \vector
		call enter_protected_service

		# Restore the context modified by the C service.
		.byte 0x66
		popad
		pop es
		pop ds
		iret
	.endm

	BIOS_SERVICE_ENTRY int10_entry, 0x10
	BIOS_SERVICE_ENTRY int13_entry, 0x13
	BIOS_SERVICE_ENTRY int15_entry, 0x15
	BIOS_SERVICE_ENTRY int16_entry, 0x16

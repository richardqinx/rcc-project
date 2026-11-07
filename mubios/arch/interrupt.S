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

	.section .text16.irq, "ax"

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

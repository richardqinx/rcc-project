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

	.equ BDA_TIMER_COUNTER, 0x046C
	.equ BDA_TIMER_ROLLOVER, 0x0470
	.equ BDA_BASE, 0x0400
	.equ BDA_KBD_BUFFER_START, 0x001E
	.equ BDA_KBD_BUFFER_END, 0x003E

	.equ TIMER_TICKS_PER_DAY_LOW, 0x00B0
	.equ TIMER_TICKS_PER_DAY_HIGH, 0x0018

	.section .text16.irq, "ax"

	.global irq0_entry
	.type irq0_entry, @function

irq0_entry:
	push ax
	push bx
	push ds

	xor bx, bx
	mov ds, bx

	mov bx, BDA_TIMER_COUNTER
	inc word ptr [bx]

	jnz irq0_send_eoi

	inc word ptr [bx + 2]

	cmp word ptr [bx + 2], TIMER_TICKS_PER_DAY_HIGH
	jne irq0_send_eoi

	cmp word ptr [bx], TIMER_TICKS_PER_DAY_LOW
	jne irq0_send_eoi

	mov word ptr [bx], 0
	mov word ptr [bx + 2], 0
	mov byte ptr [BDA_TIMER_ROLLOVER], 1

irq0_send_eoi:
	# Acknowledge IRQ0 on the master PIC.
	mov al, 0x20
	out 0x20, al

	pop ds
	pop bx
	pop ax
	iret

	.global irq1_entry
	.type irq1_entry, @function

irq1_entry:
	push ax
	push bx
	push dx
	push ds
	push si

	# Read the PS/2 scan code before sending EOI.
	in al, 0x60
	mov dl, al

	test dl, 0x80
	jnz irq1_send_eoi

	xor ax, ax
	mov ds, ax

	mov bx, word ptr [0x041C]
	mov ax, bx
	add ax, 2
	cmp ax, BDA_KBD_BUFFER_END
	jb irq1_next_tail_ready
	mov ax, BDA_KBD_BUFFER_START

irq1_next_tail_ready:
	cmp ax, word ptr [0x041A]
	je irq1_send_eoi

	mov si, bx
	add si, BDA_BASE
	mov byte ptr [si], 0
	mov byte ptr [si + 1], dl
	mov word ptr [0x041C], ax

irq1_send_eoi:
	mov al, 0x20
	out 0x20, al

	pop si
	pop ds
	pop dx
	pop bx
	pop ax
	iret

	.global irq7_entry
	.type irq7_entry, @function

irq7_entry:
	push ax

	# Acknowledge IRQ7 on the master PIC.
	mov al, 0x20
	out 0x20, al

	pop ax
	iret

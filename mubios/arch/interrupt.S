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

	.equ ATA_DATA,         0x1F0
	.equ ATA_SECTOR_COUNT, 0x1F2
	.equ ATA_LBA_LOW,      0x1F3
	.equ ATA_LBA_MID,      0x1F4
	.equ ATA_LBA_HIGH,     0x1F5
	.equ ATA_DRIVE,        0x1F6
	.equ ATA_STATUS,       0x1F7
	.equ ATA_COMMAND,      0x1F7

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

	.global int10_entry
	.type int10_entry, @function

int10_entry:
	cmp ah, 0x0E
	jne int10_return

	push bp
	mov bp, sp

	push ax
	push bx
	push dx
	push si
	push di
	push ds
	push es

	xor ax, ax
	mov ds, ax

	mov ax, 0xB800
	mov es, ax

	mov si, 0x0050
	mov dx, word ptr [si]

	mov al, byte ptr [bp - 2]
	mov bl, byte ptr [bp - 4]

	cmp al, 0x0D
	je int10_carriage_return

	cmp al, 0x0A
	je int10_line_feed

	cmp al, 0x08
	je int10_backspace

	mov di, dx
	shl di, 1
	mov ah, bl
	mov word ptr es:[di], ax
	inc dx
	jmp int10_store_cursor

	
int10_carriage_return:
	mov ax, dx
	mov si, 80
	xor dx, dx
	div si
	mul si
	mov dx, ax
	jmp int10_store_cursor

int10_line_feed:
	add dx, 80
	jmp int10_store_cursor

int10_backspace:
	cmp dx, 0
	je int10_store_cursor

	dec dx
	mov di, dx
	shl di, 1
	mov al, 0x20
	mov ah, bl
	mov word ptr es:[di], ax

int10_store_cursor:
	cmp dx, 2000
	jb int10_save_cursor

	xor dx, dx

int10_save_cursor:
	mov word ptr [0x0050], dx

	pop es
	pop ds
	pop di
	pop si
	pop dx
	pop bx
	pop ax
	pop bp

int10_return:
	iret

	.global int13_entry
	.type int13_entry, @function

int13_entry:
	cmp ah, 0x42
	je int13_extended_read
	
	cmp ah, 0x41
	jne int13_return

	push bp
	mov bp, sp

	cmp bx, 0x55AA
	jne int13_fail

	mov bx, 0xAA55
	mov cx, 0x0001
	mov ax, 0x3000

	# Clear CF in the FLAGS saved by INT.
	and word ptr [bp + 6], 0xFFFE

	pop bp
	iret

int13_fail:
	mov ah, 0x01

	# Set CF in the FLAGS saved by INT.
	or word ptr [bp + 6], 0x0001

	pop bp
	iret

int13_return:
	push bp
	mov bp, sp

	mov ah, 0x01

	# Set CF in the FLAGS saved by INT.
	or word ptr [bp + 6], 0x0001

	pop bp
	iret
int13_extended_read:
	push bp
	mov bp, sp

	push bx
	push cx
	push dx
	push si
	push di
	push ds
	push es

	# DAP size must be 0x10.
	cmp byte ptr [si], 0x10
	jne int13_extended_fail

	# This step supports exactly one sector.
	cmp word ptr [si + 2], 1
	jne int13_extended_fail

	# Reject LBA values above 28 bits.
	mov ax, word ptr [si + 12]
	or ax, word ptr [si + 14]
	jnz int13_extended_fail

	mov ax, word ptr [si + 10]
	test ax, 0xF000
	jnz int13_extended_fail

	# Load DAP buffer ES:DI.
	mov di, word ptr [si + 4]
	mov ax, word ptr [si + 6]
	mov es, ax

	cld

	# Select ATA drive from DL.
	mov al, 0xE0
	test dl, 1
	jz int13_drive_selected

	or al, 0x10

int13_drive_selected:
	mov dx, ATA_DRIVE
	out dx, al

	# 400 ns delay.
	mov dx, ATA_STATUS
	in al, dx
	in al, dx
	in al, dx
	in al, dx

	# Sector count = 1.
	mov dx, ATA_SECTOR_COUNT
	mov al, 1
	out dx, al

	# LBA byte 0.
	mov ax, word ptr [si + 8]
	mov dx, ATA_LBA_LOW
	out dx, al

	# LBA byte 1.
	mov al, ah
	mov dx, ATA_LBA_MID
	out dx, al

	# LBA byte 2.
	mov ax, word ptr [si + 10]
	mov dx, ATA_LBA_HIGH
	out dx, al

	# READ SECTORS command.
	mov dx, ATA_COMMAND
	mov al, 0x20
	out dx, al

int13_wait_data:
	mov dx, ATA_STATUS
	in al, dx

	test al, 0x80
	jnz int13_wait_data

	test al, 0x01
	jnz int13_extended_fail

	test al, 0x08
	jz int13_wait_data

	# Read one 512-byte sector.
	mov dx, ATA_DATA
	mov cx, 256
	rep insw

	# Clear CF in the saved FLAGS.
	and word ptr [bp + 6], 0xFFFE

	pop es
	pop ds
	pop di
	pop si
	pop dx
	pop cx
	pop bx
	pop bp

	xor ax, ax
	iret

int13_extended_fail:
	# Set CF in the saved FLAGS.
	or word ptr [bp + 6], 0x0001

	pop es
	pop ds
	pop di
	pop si
	pop dx
	pop cx
	pop bx
	pop bp

	mov ax, 0x0100
	iret

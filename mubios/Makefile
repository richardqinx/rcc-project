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

PROJECT := mubios
BUILD_DIR := build
ELF := $(BUILD_DIR)/$(PROJECT).elf
ROM := $(BUILD_DIR)/$(PROJECT).rom

CC := gcc
AS := as
LD := ld
OBJCOPY := objcopy

CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector \
	  -fno-builtin -nostdinc -Iinclude -Wall -Wextra    \
	  -fno-unwind-tables -fno-asynchronous-unwind-tables
ASFLAGS := --32
LDFLAGS := -m elf_i386 -nostdlib -T linker.ld

C_SRCS := $(wildcard core/*.c drivers/*.c services/*.c)
S_SRCS := $(wildcard arch/*.S)
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS)) \
        $(patsubst %.S,$(BUILD_DIR)/%.o,$(S_SRCS))

.PHONY: all elf rom

all: $(ROM)

elf: $(ELF)

rom: $(ROM)

$(ELF): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(ROM): $(ELF)
	@mkdir -p $(dir $@)
	$(OBJCOPY) -O binary $< $@
	@test "$$(stat -c %s $@)" -le 65536
	@truncate -s 65536 $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# =============================================================================
# PumpkinOS - build system
# -----------------------------------------------------------------------------
# Produces a bootable 1.44 MB floppy image (pumpkinos.img) containing:
#   sector 0        -> the 512-byte boot sector (boot/boot.asm)
#   sectors 1..N    -> the flat kernel binary (kernel/*)
#
# Usage:
#   make            build the floppy image
#   make run        build, then boot it in QEMU (graphical)
#   make run-serial build, then boot headless with serial on stdout
#   make clean      remove all build artifacts
# =============================================================================

# ---- toolchain --------------------------------------------------------------
CC      := gcc
LD      := ld
NASM    := nasm
OBJCOPY := objcopy
QEMU    := qemu-system-i386

# ---- flags ------------------------------------------------------------------
# GCC ships freestanding-safe headers (stdint.h, stddef.h, ...) in its own
# include dir. -nostdinc drops ALL default paths, so we add just that one back
# with -isystem -- this keeps host libc headers out while still giving us the
# fixed-width integer types.
GCC_INC := $(shell $(CC) -print-file-name=include)

# The kernel source is split across subsystem directories. Every one is added
# to the header search path, so `#include "foo.h"` keeps working regardless of
# which directory a file lives in, and VPATH lets the pattern rules find the
# matching .c/.asm.
SRC_DIRS := kernel cpu mm drivers sched lib shell user fs net
VPATH    := $(SRC_DIRS)
INCLUDES := $(addprefix -I,$(SRC_DIRS))

# 32-bit, target the plain i386 (real "old machines"), no host runtime at all.
CFLAGS := -m32 -march=i386 -ffreestanding -fno-pie -fno-pic \
          -fno-stack-protector -fno-builtin -nostdlib -nostdinc \
          -isystem $(GCC_INC) $(INCLUDES) \
          -Wall -Wextra -O2 -c

LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

# ---- layout -----------------------------------------------------------------
BUILD  := build
IMG    := pumpkinos.img

BOOT_SRC   := boot/boot.asm
BOOT_BIN   := $(BUILD)/boot.bin

# Objects are flattened into build/ by basename (names are unique across dirs).
# entry.o must link first (so _start lands at 0x1000), so it is pulled out of
# the wildcard list and prepended explicitly.
ENTRY_OBJ  := $(BUILD)/entry.o
ASM_SRCS   := $(filter-out %/entry.asm,$(wildcard $(addsuffix /*.asm,$(SRC_DIRS))))
ASM_OBJS   := $(patsubst %.asm,$(BUILD)/%.o,$(notdir $(ASM_SRCS)))
C_SRCS     := $(wildcard $(addsuffix /*.c,$(SRC_DIRS)))
C_OBJS     := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(C_SRCS)))
KERNEL_OBJS := $(ENTRY_OBJ) $(ASM_OBJS) $(C_OBJS)
HEADERS    := $(wildcard $(addsuffix /*.h,$(SRC_DIRS)))

KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin

# A standalone ring-3 program, built to its own ELF (NOT part of the kernel)
# and copied onto the floppy so the ELF loader can run it: `run HELLO.ELF`.
UPROG_SRC  := userprog/hello.c
UPROG_LD   := userprog/user.ld
UPROG_OBJ  := $(BUILD)/hello.uo
UPROG_ELF  := $(BUILD)/hello.elf

# The FAT12 filesystem is organised into logical directories. fsroot/ mirrors
# the on-disk tree (fsroot/docs -> /docs, fsroot/desktop -> /desktop, ...); the
# kernel (KERNEL.BIN) stays in the root because the boot sector loads it from
# there, and the ring-3 program is placed in /apps.
FSROOT     := fsroot
FS_FILES   := $(shell find $(FSROOT) -type f)

# KERNEL.BIN is loaded at 0x10000 by the boot sector and grows upward; it must
# stay below the floppy DMA buffer at 0x80000 (and the stack at 0x90000). That
# gives a 0x70000 = 448 KiB window; 512 sectors (256 KiB) leaves a wide margin.
MAX_KERNEL_SECTORS := 512

.PHONY: all run run-serial clean

all: $(IMG)

# ---- boot sector: assemble the FAT12 loader (carries its own BPB) -----------
$(BOOT_BIN): $(BOOT_SRC) | $(BUILD)
	$(NASM) -f bin $< -o $@

# ---- kernel: assemble any .asm (found via VPATH across SRC_DIRS) ------------
$(BUILD)/%.o: %.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

# ---- kernel: compile any .c (found via VPATH across SRC_DIRS) ---------------
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# Rebuild every C object if any header changes (simple + correct for a project
# this size; avoids stale builds while hacking on the kernel).
$(C_OBJS): $(HEADERS)

# ---- ring-3 program: compile and link into its own ELF ----------------------
$(UPROG_OBJ): $(UPROG_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(UPROG_ELF): $(UPROG_OBJ) $(UPROG_LD)
	$(LD) -m elf_i386 -T $(UPROG_LD) -o $@ $(UPROG_OBJ)

# ---- kernel: link (entry.o FIRST so _start lands at 0x1000) -----------------
$(KERNEL_ELF): $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

# ---- kernel: strip ELF wrapper down to a flat binary ------------------------
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@ksize=$$(( ($$(wc -c < $@) + 511) / 512 )); \
	if [ $$ksize -gt $(MAX_KERNEL_SECTORS) ]; then \
	    echo "ERROR: KERNEL.BIN is $$ksize sectors but must fit in $(MAX_KERNEL_SECTORS) (it loads at 0x10000, below the DMA buffer at 0x80000)."; \
	    exit 1; \
	fi; \
	echo "KERNEL.BIN occupies $$ksize / $(MAX_KERNEL_SECTORS) sectors."

# ---- assemble the floppy image ----------------------------------------------
# The whole floppy is one FAT12 volume. We format it, copy the kernel in as the
# file KERNEL.BIN (so 'ls' shows it) along with the fsroot/ files, then overlay
# our own boot sector (its BPB matches the standard 1.44 MB layout mkfs.fat
# used, so the volume stays consistent).
$(IMG): $(BOOT_BIN) $(KERNEL_BIN) $(UPROG_ELF) $(FS_FILES)
	dd if=/dev/zero of=$(IMG) bs=512 count=2880 status=none
	mkfs.fat -F 12 -n PUMPKIN $(IMG) >/dev/null
	MTOOLS_SKIP_CHECK=1 mcopy -i $(IMG) $(KERNEL_BIN) ::KERNEL.BIN
	MTOOLS_SKIP_CHECK=1 mmd   -i $(IMG) ::apps
	MTOOLS_SKIP_CHECK=1 mcopy -i $(IMG) $(UPROG_ELF) ::apps/HELLO.ELF
	@for d in $(FSROOT)/*/; do dir=$${d%/}; \
	    MTOOLS_SKIP_CHECK=1 mcopy -s -i $(IMG) "$$dir" :: ; \
	done
	dd if=$(BOOT_BIN) of=$(IMG) conv=notrunc status=none
	@echo "Built $(IMG) (1.44 MB FAT12 floppy: /KERNEL.BIN, /apps, /docs, /desktop)."

$(BUILD):
	mkdir -p $(BUILD)

# ---- run in QEMU ------------------------------------------------------------
run: $(IMG)
	$(QEMU) -fda $(IMG) -boot a

# Headless: mirror the kernel's serial output to this terminal.
run-serial: $(IMG)
	$(QEMU) -fda $(IMG) -boot a -serial stdio -display none -no-reboot

clean:
	rm -rf $(BUILD) $(IMG)

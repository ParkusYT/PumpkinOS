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
SRC_DIRS := kernel cpu mm drivers sched lib shell user
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

# The kernel loads at 0x1000 and grows upward; it must end below the boot
# sector at 0x7C00. 48 sectors ends at 0x7000, leaving headroom for the stack.
MAX_KERNEL_SECTORS := 48

.PHONY: all run run-serial clean

all: $(IMG)

# ---- boot sector ------------------------------------------------------------
# Assembled AFTER the kernel so we can tell it the exact sector count to load
# (over-reading would run the disk load straight over the boot code at 0x7C00).
$(BOOT_BIN): $(BOOT_SRC) $(KERNEL_BIN) | $(BUILD)
	@ksize=$$(( ($$(wc -c < $(KERNEL_BIN)) + 511) / 512 )); \
	if [ $$ksize -gt $(MAX_KERNEL_SECTORS) ]; then \
	    echo "ERROR: kernel is $$ksize sectors but the load budget is $(MAX_KERNEL_SECTORS) (would collide with the boot sector at 0x7C00)."; \
	    echo "       Shrink the kernel, or move its load address in boot/boot.asm."; \
	    exit 1; \
	fi; \
	echo "Kernel occupies $$ksize / $(MAX_KERNEL_SECTORS) sectors; assembling boot sector to load $$ksize."; \
	$(NASM) -f bin -DKSECTORS=$$ksize $< -o $@

# ---- kernel: assemble any .asm (found via VPATH across SRC_DIRS) ------------
$(BUILD)/%.o: %.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

# ---- kernel: compile any .c (found via VPATH across SRC_DIRS) ---------------
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# Rebuild every C object if any header changes (simple + correct for a project
# this size; avoids stale builds while hacking on the kernel).
$(C_OBJS): $(HEADERS)

# ---- kernel: link (entry.o FIRST so _start lands at 0x1000) -----------------
$(KERNEL_ELF): $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

# ---- kernel: strip ELF wrapper down to a flat binary ------------------------
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

# ---- assemble the floppy image ----------------------------------------------
$(IMG): $(BOOT_BIN) $(KERNEL_BIN)
	# Start with a blank 1.44 MB floppy (2880 x 512-byte sectors).
	dd if=/dev/zero of=$(IMG) bs=512 count=2880 status=none
	# Boot sector goes in sector 0.
	dd if=$(BOOT_BIN) of=$(IMG) conv=notrunc status=none
	# Kernel goes right after it, starting at sector 1 (LBA 1).
	dd if=$(KERNEL_BIN) of=$(IMG) seek=1 conv=notrunc status=none
	@echo "Built $(IMG) (1.44 MB floppy)."

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

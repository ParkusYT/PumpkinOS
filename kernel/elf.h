/* ===========================================================================
 * PumpkinOS - ELF32 program loader
 * ---------------------------------------------------------------------------
 * Loads a statically-linked 32-bit ELF executable off the filesystem and runs
 * it as a ring-3 task. Each PT_LOAD segment is mapped as user pages at its link
 * address, so programs must be linked into the user region (>= 0x40000000, i.e.
 * above the kernel's identity map). They reach the kernel only via int 0x80.
 * ========================================================================= */
#ifndef PUMPKIN_ELF_H
#define PUMPKIN_ELF_H

/* Load and launch the ELF file 'path' from the current directory. Returns 0 if
 * the program was accepted and spawned, -1 on any error (not found, too big,
 * not a valid 32-bit x86 executable). The program itself runs asynchronously
 * as a scheduled ring-3 task. */
int elf_exec(const char *path);

#endif /* PUMPKIN_ELF_H */

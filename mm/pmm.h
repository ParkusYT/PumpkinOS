/* ===========================================================================
 * PumpkinOS - physical memory manager (4 KiB frame allocator)
 * ========================================================================= */
#ifndef PUMPKIN_PMM_H
#define PUMPKIN_PMM_H

#include <stdint.h>

#define PMM_FRAME_SIZE 4096u

/* Parse the BIOS E820 map (left in low memory by the bootloader) and build the
 * frame bitmap. Safe to call once, early in boot. */
void pmm_init(void);

/* Allocate one 4 KiB physical frame; returns its physical address, or 0 when
 * out of memory (frame 0 is always reserved, so 0 doubles as "null"). */
uint32_t pmm_alloc_frame(void);

/* Return a frame previously handed out by pmm_alloc_frame(). */
void pmm_free_frame(uint32_t phys_addr);

/* Permanently reserve the frames covering [phys, phys+bytes) so the allocator
 * never hands them out - e.g. a fixed DMA buffer in high memory. Call right
 * after pmm_init(), before anything else allocates. */
void pmm_reserve(uint32_t phys, uint32_t bytes);

/* Statistics, in frames. */
uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_free_frames(void);

/* Print the E820 map and allocator stats (the shell's 'meminfo' command). */
void pmm_report(void);

#endif /* PUMPKIN_PMM_H */

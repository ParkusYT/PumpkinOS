/* ===========================================================================
 * PumpkinOS - paging (32-bit, 4 KiB pages, no PAE/PSE so it runs on a 386)
 * ========================================================================= */
#ifndef PUMPKIN_PAGING_H
#define PUMPKIN_PAGING_H

#include <stdint.h>
#include "isr.h"

#define PAGE_PRESENT 0x1
#define PAGE_WRITE   0x2
#define PAGE_USER    0x4

/* Build an identity map over usable RAM, load CR3, and turn on paging. */
void paging_init(void);

/* Map one 4 KiB virtual page to a physical frame. Allocates a page table from
 * the PMM if needed. Returns 0 on success, -1 if out of memory. */
int paging_map(uint32_t virt, uint32_t phys, uint32_t flags);

/* Remove a mapping (marks the page not-present). */
void paging_unmap(uint32_t virt);

/* Translate a virtual address, or return 0xFFFFFFFF if it is not mapped. */
uint32_t paging_get_phys(uint32_t virt);

/* Non-zero once paging is on. */
int paging_is_enabled(void);

/* Page-fault (vector 14) reporter, called from the interrupt dispatcher. */
void paging_fault(struct registers *r);

#endif /* PUMPKIN_PAGING_H */

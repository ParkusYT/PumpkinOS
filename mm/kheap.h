/* ===========================================================================
 * PumpkinOS - kernel heap (kmalloc / kfree)
 * ========================================================================= */
#ifndef PUMPKIN_KHEAP_H
#define PUMPKIN_KHEAP_H

#include <stddef.h>

/* Set up the heap's virtual region. Must run after paging is enabled. */
void kheap_init(void);

/* Allocate/free kernel memory. kmalloc returns 8-byte-aligned storage, or
 * NULL if it cannot grow the heap. kfree(NULL) is a no-op. */
void *kmalloc(size_t size);
void  kfree(void *ptr);

/* Print heap statistics (the shell's 'heap' command). */
void kheap_report(void);

#endif /* PUMPKIN_KHEAP_H */

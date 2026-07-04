/* ===========================================================================
 * PumpkinOS - kernel heap
 * ---------------------------------------------------------------------------
 * A first-fit free-list allocator over a dedicated virtual region at 3 GiB.
 * That region sits above the identity map, so every heap page is genuinely
 * backed by a physical frame from the PMM that we map in on demand - real
 * virtual memory, not just a window onto identity-mapped RAM.
 *
 * Layout: the heap is a contiguous chain of blocks in address order, each with
 * a 16-byte header (so payloads are 8-byte aligned). kmalloc splits a big free
 * block; kfree marks a block free and coalesces neighbours.
 * ========================================================================= */
#include "kheap.h"
#include "paging.h"
#include "pmm.h"
#include "console.h"
#include <stdint.h>

#define KHEAP_BASE     0xC0000000u    /* 3 GiB: above the identity map */
#define KHEAP_INITIAL  0x4000u        /* pre-map 16 KiB to start with   */
#define PAGE_SIZE      4096u

typedef struct block {
    uint32_t      size;               /* payload size in bytes           */
    struct block *next;               /* next block in address order     */
    uint32_t      used;               /* 0 = free, 1 = allocated         */
    uint32_t      _pad;               /* pad header to 16 bytes          */
} block_t;

#define HEADER_SIZE ((uint32_t)sizeof(block_t))   /* 16 */
#define MIN_SPLIT   8u                             /* smallest payload to split off */

static uint32_t heap_base;
static uint32_t heap_end;             /* one past the last mapped byte    */
static block_t *head;

/* Merge any adjacent free blocks (single linear pass, address ordered). */
static void coalesce(void) {
    block_t *b = head;
    while (b && b->next) {
        if (!b->used && !b->next->used) {
            b->size += HEADER_SIZE + b->next->size;
            b->next = b->next->next;      /* absorb next; retry from b */
        } else {
            b = b->next;
        }
    }
}

/* Map more frames onto the end of the heap and add them as one free block.
 * Returns 0 on success, -1 if no physical memory is available. */
static int grow_heap(uint32_t min_bytes) {
    uint32_t bytes = (min_bytes + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
    uint32_t start = heap_end;
    uint32_t mapped = 0;

    while (mapped < bytes) {
        uint32_t phys = pmm_alloc_frame();
        if (phys == 0)
            break;
        if (paging_map(heap_end, phys, PAGE_PRESENT | PAGE_WRITE) != 0) {
            pmm_free_frame(phys);
            break;
        }
        heap_end += PAGE_SIZE;
        mapped   += PAGE_SIZE;
    }
    if (mapped == 0)
        return -1;

    block_t *b = (block_t *)start;
    b->size = mapped - HEADER_SIZE;
    b->used = 0;
    b->next = 0;

    if (head == 0) {
        head = b;
    } else {
        block_t *last = head;
        while (last->next)
            last = last->next;
        last->next = b;
    }
    coalesce();
    return 0;
}

void kheap_init(void) {
    heap_base = KHEAP_BASE;
    heap_end  = KHEAP_BASE;
    head      = 0;
    grow_heap(KHEAP_INITIAL);
}

/* If block b is much bigger than 'size', carve the tail into a new free block. */
static void split(block_t *b, uint32_t size) {
    if (b->size >= size + HEADER_SIZE + MIN_SPLIT) {
        block_t *n = (block_t *)((uint8_t *)b + HEADER_SIZE + size);
        n->size = b->size - size - HEADER_SIZE;
        n->used = 0;
        n->next = b->next;
        b->next = n;
        b->size = size;
    }
}

void *kmalloc(size_t size) {
    if (size == 0)
        return 0;
    uint32_t need = ((uint32_t)size + 7u) & ~7u;      /* 8-byte align */

    for (int attempt = 0; attempt < 2; attempt++) {
        for (block_t *b = head; b; b = b->next) {
            if (!b->used && b->size >= need) {
                split(b, need);
                b->used = 1;
                return (void *)((uint8_t *)b + HEADER_SIZE);
            }
        }
        if (grow_heap(need + HEADER_SIZE) != 0)        /* out of memory */
            return 0;
    }
    return 0;
}

void kfree(void *ptr) {
    if (ptr == 0)
        return;
    block_t *b = (block_t *)((uint8_t *)ptr - HEADER_SIZE);
    b->used = 0;
    coalesce();
}

void kheap_report(void) {
    uint32_t used = 0, freeb = 0, nblocks = 0, nfree = 0;
    for (block_t *b = head; b; b = b->next) {
        nblocks++;
        if (b->used)
            used += b->size;
        else {
            freeb += b->size;
            nfree++;
        }
    }

    console_write("  heap base ... ");
    console_write_hex(heap_base);
    console_write("\n  mapped ...... ");
    console_write_dec((heap_end - heap_base) >> 10);
    console_write(" KB\n  blocks ...... ");
    console_write_dec(nblocks);
    console_write(" (");
    console_write_dec(nfree);
    console_write(" free)\n  used ........ ");
    console_write_dec(used);
    console_write(" bytes\n  free ........ ");
    console_write_dec(freeb);
    console_write(" bytes\n");
}

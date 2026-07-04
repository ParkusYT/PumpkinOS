/* ===========================================================================
 * PumpkinOS - paging
 * ---------------------------------------------------------------------------
 * Classic 32-bit two-level paging: a page directory of 1024 entries, each
 * pointing at a page table of 1024 entries, each mapping a 4 KiB frame. We
 * build an identity map (virtual == physical) over usable RAM so that the
 * running kernel, its stack, the PMM bitmap, the VGA buffer, and the page
 * structures themselves all keep working the instant CR0.PG flips on.
 *
 * We deliberately use only 4 KiB pages and flush the TLB by reloading CR3
 * (not invlpg), so this works on a plain i386 with no PSE.
 * ========================================================================= */
#include "paging.h"
#include "pmm.h"
#include "console.h"
#include <stdint.h>

#define ENTRIES    1024
#define PAGE_SIZE  4096u
#define ADDR_MASK  0xFFFFF000u

/* The identity map is capped so the page-table cost stays bounded on machines
 * with a lot of RAM (1 GiB => 256 page tables => 1 MiB of tables). */
#define IDENTITY_CAP 0x40000000ULL   /* 1 GiB */

static uint32_t *page_directory;     /* accessed at its identity (== phys) addr */
static int       enabled = 0;

static inline void load_cr3(uint32_t pd_phys) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

static inline void set_paging_bit(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;              /* CR0.PG */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static void zero_table(uint32_t *table) {
    for (int i = 0; i < ENTRIES; i++)
        table[i] = 0;
}

int paging_map(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pdi = virt >> 22;
    uint32_t pti = (virt >> 12) & 0x3FF;

    uint32_t *pt;
    if (!(page_directory[pdi] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (pt_phys == 0)
            return -1;
        pt = (uint32_t *)pt_phys;                 /* identity-mapped */
        zero_table(pt);
        page_directory[pdi] = pt_phys | PAGE_PRESENT | PAGE_WRITE |
                              (flags & PAGE_USER);
    } else {
        pt = (uint32_t *)(page_directory[pdi] & ADDR_MASK);
    }

    pt[pti] = (phys & ADDR_MASK) | (flags & 0xFFF) | PAGE_PRESENT;

    if (enabled)
        load_cr3((uint32_t)page_directory);       /* flush the TLB (386-safe) */
    return 0;
}

void paging_unmap(uint32_t virt) {
    uint32_t pdi = virt >> 22;
    uint32_t pti = (virt >> 12) & 0x3FF;

    if (!(page_directory[pdi] & PAGE_PRESENT))
        return;
    uint32_t *pt = (uint32_t *)(page_directory[pdi] & ADDR_MASK);
    pt[pti] = 0;

    if (enabled)
        load_cr3((uint32_t)page_directory);
}

uint32_t paging_get_phys(uint32_t virt) {
    uint32_t pdi = virt >> 22;
    uint32_t pti = (virt >> 12) & 0x3FF;

    if (!(page_directory[pdi] & PAGE_PRESENT))
        return 0xFFFFFFFFu;
    uint32_t *pt = (uint32_t *)(page_directory[pdi] & ADDR_MASK);
    if (!(pt[pti] & PAGE_PRESENT))
        return 0xFFFFFFFFu;
    return (pt[pti] & ADDR_MASK) | (virt & 0xFFF);
}

int paging_is_enabled(void) {
    return enabled;
}

void paging_init(void) {
    uint32_t pd_phys = pmm_alloc_frame();
    page_directory = (uint32_t *)pd_phys;
    zero_table(page_directory);

    /* Identity-map [0, top): every page of usable RAM, capped at IDENTITY_CAP.
     * Page tables allocated below are themselves within this range, so they
     * stay reachable after paging is on. */
    uint64_t top64 = (uint64_t)pmm_total_frames() << 12;
    if (top64 > IDENTITY_CAP)
        top64 = IDENTITY_CAP;
    uint32_t top = (uint32_t)top64;

    for (uint32_t addr = 0; addr < top; addr += PAGE_SIZE)
        paging_map(addr, addr, PAGE_PRESENT | PAGE_WRITE);

    load_cr3(pd_phys);
    set_paging_bit();
    enabled = 1;
}

/* ---- page-fault reporter (vector 14) -------------------------------------- */
void paging_fault(struct registers *r) {
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    console_set_color(VGA_WHITE, VGA_RED);
    console_write("\n\n *** PAGE FAULT *** faulting address ");
    console_write_hex(cr2);
    console_write("\n  cause: ");
    console_write((r->err_code & 0x1) ? "protection violation" : "page not present");
    console_write((r->err_code & 0x2) ? ", on write"  : ", on read");
    console_write((r->err_code & 0x4) ? ", user mode" : ", kernel mode");
    if (r->err_code & 0x10)
        console_write(", instruction fetch");
    console_write("\n  eip = ");
    console_write_hex(r->eip);
    console_write("\n PumpkinOS halted.\n");

    for (;;)
        __asm__ volatile("cli; hlt");
}

/* ===========================================================================
 * PumpkinOS - Global Descriptor Table + Task State Segment
 * ========================================================================= */
#include "gdt.h"
#include "string.h"
#include <stdint.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;          /* limit bits 16-19 + granularity flags */
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* 32-bit Task State Segment. We only really use ss0/esp0. */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0, ss0;
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[8];
static struct gdt_ptr   gdtp;
static struct tss_entry tss;

static void set_gdt(int i, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t gran) {
    gdt[i].limit_low = limit & 0xFFFF;
    gdt[i].base_low  = base & 0xFFFF;
    gdt[i].base_mid  = (base >> 16) & 0xFF;
    gdt[i].access    = access;
    gdt[i].gran      = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].base_high = (base >> 24) & 0xFF;
}

void tss_set_esp0(uint32_t esp0) {
    tss.esp0 = esp0;
}

void gdt_init(void) {
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint32_t)&gdt;

    set_gdt(0, 0, 0, 0, 0);                       /* null                    */
    set_gdt(1, 0, 0xFFFFF, 0x9A, 0xC0);           /* kernel code (ring 0)    */
    set_gdt(2, 0, 0xFFFFF, 0x92, 0xC0);           /* kernel data (ring 0)    */
    set_gdt(3, 0, 0xFFFFF, 0xFA, 0xC0);           /* user code   (ring 3)    */
    set_gdt(4, 0, 0xFFFFF, 0xF2, 0xC0);           /* user data   (ring 3)    */

    /* TSS descriptor: byte granularity, type 0x89 = present 32-bit TSS. */
    memset(&tss, 0, sizeof(tss));
    tss.ss0        = KERNEL_DATA_SEL;             /* ring-0 stack segment    */
    tss.esp0       = 0x90000;                     /* placeholder; updated per task */
    tss.iomap_base = sizeof(tss);                 /* no I/O permission bitmap */
    set_gdt(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    /* 16-bit code/data (base 0, 64 KiB, byte granularity, D bit clear). The
     * real-mode BIOS thunk uses these to step down PM -> 16-bit PM -> real. */
    set_gdt(6, 0, 0xFFFF, 0x9A, 0x00);            /* 16-bit code (0x30)      */
    set_gdt(7, 0, 0xFFFF, 0x92, 0x00);            /* 16-bit data (0x38)      */

    /* Load the GDT, reload the segment registers, then load the TSS. */
    __asm__ volatile(
        "lgdt %0\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "ljmp $0x08, $1f\n\t"                     /* far jump reloads CS      */
        "1:\n\t"
        : : "m"(gdtp) : "ax", "memory");

    __asm__ volatile("ltr %0" : : "r"((uint16_t)TSS_SEL));
}

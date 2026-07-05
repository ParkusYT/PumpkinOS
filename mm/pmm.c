/* ===========================================================================
 * PumpkinOS - physical memory manager
 * ---------------------------------------------------------------------------
 * A bitmap allocator over 4 KiB physical frames. One bit per frame: 1 = used,
 * 0 = free. The set of frames that exist and are usable comes from the BIOS
 * E820 map, which the bootloader gathered in real mode and left at a fixed
 * low address for us to read here in protected mode.
 *
 * Note: everything works in 32-bit quantities (frame numbers, KiB counts) and
 * uses shifts rather than 64-bit division, because we link without libgcc and
 * so have no __udivdi3 helper.
 * ========================================================================= */
#include "pmm.h"
#include "console.h"
#include <stdint.h>

/* Must match boot/boot.asm. */
#define E820_COUNT_ADDR   0x0500u
#define E820_ENTRIES_ADDR 0x0504u
#define E820_USABLE       1u

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attr;
} __attribute__((packed));

#define FOUR_GIB 0x100000000ULL

static uint32_t *bitmap;          /* one bit per frame, stored in real RAM */
static uint32_t  total_frames;    /* frames covering [0, highest usable addr) */
static uint32_t  used_frames;

/* Turn a fixed physical address into a pointer without letting the compiler
 * reason about it as a small constant (which would trip -Warray-bounds when we
 * index the E820 array living at that address). */
static void *phys_ptr(uint32_t addr) {
    void *p;
    __asm__("" : "=r"(p) : "0"(addr));
    return p;
}

static uint32_t map_count(void) {
    return *(volatile uint32_t *)phys_ptr(E820_COUNT_ADDR);
}
static const struct e820_entry *map_entries(void) {
    return (const struct e820_entry *)phys_ptr(E820_ENTRIES_ADDR);
}

/* ---- bitmap primitives (guard against out-of-range frames) ---------------- */
static void frame_mark_used(uint32_t f) {
    if (f >= total_frames) return;
    if (!(bitmap[f >> 5] & (1u << (f & 31)))) {
        bitmap[f >> 5] |= (1u << (f & 31));
        used_frames++;
    }
}
static void frame_mark_free(uint32_t f) {
    if (f >= total_frames) return;
    if (bitmap[f >> 5] & (1u << (f & 31))) {
        bitmap[f >> 5] &= ~(1u << (f & 31));
        used_frames--;
    }
}

/* Reserve every frame that touches [start, end). */
static void reserve_range(uint64_t start, uint64_t end) {
    if (end > FOUR_GIB) end = FOUR_GIB;
    if (start >= end) return;
    uint32_t f0 = (uint32_t)(start >> 12);            /* floor */
    uint32_t f1 = (uint32_t)((end + 0xFFF) >> 12);    /* ceil  */
    for (uint32_t f = f0; f < f1; f++)
        frame_mark_used(f);
}

/* Free every whole frame fully inside [start, end). */
static void free_range(uint64_t start, uint64_t end) {
    if (end > FOUR_GIB) end = FOUR_GIB;
    if (start >= end) return;
    uint32_t f0 = (uint32_t)((start + 0xFFF) >> 12);  /* ceil  */
    uint32_t f1 = (uint32_t)(end >> 12);              /* floor */
    for (uint32_t f = f0; f < f1; f++)
        frame_mark_free(f);
}

void pmm_init(void) {
    uint32_t count = map_count();
    const struct e820_entry *e = map_entries();

    total_frames = 0;
    used_frames  = 0;
    bitmap       = 0;

    /* Pass 1: highest usable address (capped at 4 GiB) sizes the bitmap. */
    uint64_t max_addr = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (e[i].type != E820_USABLE) continue;
        if (e[i].base >> 32) continue;                /* region above 4 GiB */
        uint64_t end = e[i].base + e[i].length;
        if (end > FOUR_GIB) end = FOUR_GIB;
        if (end > max_addr) max_addr = end;
    }
    if (max_addr == 0)
        return;                                       /* no usable RAM found */

    total_frames = (uint32_t)(max_addr >> 12);
    uint32_t bitmap_bytes = ((total_frames + 7) >> 3);
    bitmap_bytes = (bitmap_bytes + 3) & ~3u;          /* round up to dword */

    /* Pass 2: find a usable spot >= 1 MiB big enough to hold the bitmap. */
    uint32_t bitmap_addr = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (e[i].type != E820_USABLE) continue;
        if (e[i].base >> 32) continue;
        uint64_t base = e[i].base;
        uint64_t end  = e[i].base + e[i].length;
        if (end > FOUR_GIB) end = FOUR_GIB;
        if (base < 0x100000ULL) base = 0x100000ULL;   /* stay above 1 MiB */
        if (base >= end) continue;
        if (end - base >= bitmap_bytes) {
            bitmap_addr = (uint32_t)base;
            break;
        }
    }
    if (bitmap_addr == 0) {                           /* nowhere to live */
        total_frames = 0;
        return;
    }
    bitmap = (uint32_t *)bitmap_addr;

    /* Start with everything used, then free the usable regions. */
    for (uint32_t i = 0; i < (bitmap_bytes >> 2); i++)
        bitmap[i] = 0xFFFFFFFFu;
    used_frames = total_frames;

    for (uint32_t i = 0; i < count; i++) {
        if (e[i].type != E820_USABLE) continue;
        if (e[i].base >> 32) continue;
        free_range(e[i].base, e[i].base + e[i].length);
    }

    /* Re-reserve the things we must never hand out:
     *  - the entire first 1 MiB (BIOS, IVT/BDA, video, our real-mode E820
     *    buffer, and the loaded kernel all live there), and
     *  - the bitmap's own storage. */
    reserve_range(0, 0x100000ULL);
    reserve_range(bitmap_addr, (uint64_t)bitmap_addr + bitmap_bytes);
}

uint32_t pmm_alloc_frame(void) {
    if (total_frames == 0)
        return 0;
    uint32_t words = (total_frames + 31) >> 5;
    for (uint32_t i = 0; i < words; i++) {
        if (bitmap[i] == 0xFFFFFFFFu)
            continue;
        for (uint32_t b = 0; b < 32; b++) {
            uint32_t f = (i << 5) + b;
            if (f >= total_frames)
                break;
            if (!(bitmap[i] & (1u << b))) {
                frame_mark_used(f);
                return f << 12;                       /* physical address */
            }
        }
    }
    return 0;                                         /* out of memory */
}

void pmm_free_frame(uint32_t phys_addr) {
    frame_mark_free(phys_addr >> 12);
}

void pmm_reserve(uint32_t phys, uint32_t bytes) {
    reserve_range(phys, (uint64_t)phys + bytes);
}

uint32_t pmm_total_frames(void) { return total_frames; }
uint32_t pmm_used_frames(void)  { return used_frames; }
uint32_t pmm_free_frames(void)  { return total_frames - used_frames; }

/* ---- reporting ------------------------------------------------------------ */
static const char *type_name(uint32_t t) {
    switch (t) {
    case 1:  return "usable";
    case 2:  return "reserved";
    case 3:  return "ACPI reclaim";
    case 4:  return "ACPI NVS";
    case 5:  return "bad memory";
    default: return "other";
    }
}

/* Print a KiB count as MiB when it is large, otherwise as KiB. */
static void print_size_kb(uint32_t kb) {
    if (kb >= 1024) {
        console_write_dec(kb >> 10);                  /* / 1024 */
        console_write(" MB");
    } else {
        console_write_dec(kb);
        console_write(" KB");
    }
}

void pmm_report(void) {
    uint32_t count = map_count();
    const struct e820_entry *e = map_entries();

    if (count == 0) {
        console_write("  BIOS E820 memory map: unavailable\n");
    } else {
        console_write("  BIOS E820 memory map (");
        console_write_dec(count);
        console_write(" entries):\n");
        for (uint32_t i = 0; i < count; i++) {
            console_write("    ");
            console_write_hex((uint32_t)e[i].base);
            console_write("  ");
            print_size_kb((uint32_t)(e[i].length >> 10));
            console_write("\t");
            console_write(type_name(e[i].type));
            console_putc('\n');
        }
    }

    console_write("  frame size .. 4 KB\n");

    console_write("  total ....... ");
    console_write_dec(pmm_total_frames());
    console_write(" frames, ");
    print_size_kb(pmm_total_frames() * 4);            /* 4 KiB per frame */
    console_putc('\n');

    console_write("  used ........ ");
    console_write_dec(pmm_used_frames());
    console_write(" frames, ");
    print_size_kb(pmm_used_frames() * 4);
    console_putc('\n');

    console_write("  free ........ ");
    console_write_dec(pmm_free_frames());
    console_write(" frames, ");
    print_size_kb(pmm_free_frames() * 4);
    console_putc('\n');
}

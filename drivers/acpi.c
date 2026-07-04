/* ===========================================================================
 * PumpkinOS - minimal ACPI: locate the tables and perform a soft power-off
 * ---------------------------------------------------------------------------
 * The chain we walk:
 *   RSDP  ("RSD PTR ")  -> physical pointer to the RSDT
 *   RSDT  ("RSDT")      -> array of pointers to other tables
 *   FADT  ("FACP")      -> PM1a/PM1b control ports, SMI_CMD, DSDT pointer
 *   DSDT  ("DSDT")      -> AML bytecode; we scan it for the \_S5_ package to
 *                          pull out the SLP_TYPa/SLP_TYPb values.
 *
 * Powering off then means writing (SLP_TYPx << 10) | SLP_EN to the PM1 control
 * registers. This is the classic OSDev shutdown recipe; it is enough for QEMU
 * and for real ACPI PCs of the era.
 * ========================================================================= */
#include "acpi.h"
#include "io.h"
#include "paging.h"
#include "console.h"
#include "string.h"
#include <stdint.h>

#define SLP_EN  (1 << 13)          /* sleep-enable bit in PM1_CNT   */
#define SCI_EN  1                  /* "ACPI mode on" bit in PM1_CNT */

/* Cached results of acpi_init(). */
static int      available;
static uint16_t pm1a_cnt, pm1b_cnt;
static uint8_t  slp_typa, slp_typb;
static uint16_t smi_cmd;
static uint8_t  acpi_enable;

/* Turn a physical address into a pointer without GCC folding the constant into
 * an out-of-bounds warning (the tables live at fixed firmware addresses). */
static inline uint8_t *phys_ptr(uint32_t addr) {
    uint8_t *p = (uint8_t *)addr;
    __asm__("" : "+r"(p));
    return p;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ACPI tables sit in reserved memory above usable RAM, which the identity map
 * doesn't cover. Identity-map the pages spanning [addr, addr+len) so we can
 * read them. (No-op before paging is on, when memory is already flat.) */
static void map_phys(uint32_t addr, uint32_t len) {
    if (!paging_is_enabled())
        return;
    uint32_t start = addr & ~0xFFFu;
    uint32_t stop  = (addr + len + 0xFFFu) & ~0xFFFu;
    for (uint32_t a = start; a != stop; a += 0x1000)
        if (paging_get_phys(a) == 0xFFFFFFFFu)
            paging_map(a, a, PAGE_PRESENT | PAGE_WRITE);
}

/* Map an ACPI table's header, then its full length (from the length field at
 * offset 4), and return a pointer to it. */
static uint8_t *map_table(uint32_t phys) {
    map_phys(phys, 64);
    uint8_t *p = phys_ptr(phys);
    uint32_t len = rd32(p + 4);
    if (len > 64 && len < 0x100000)
        map_phys(phys, len);
    return p;
}

/* Sum of 'len' bytes is zero (mod 256) for a valid ACPI table/RSDP. */
static int checksum_ok(const uint8_t *p, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

/* Search a physical range for the 8-byte "RSD PTR " signature (16-aligned). */
static uint8_t *scan_rsdp(uint32_t start, uint32_t end) {
    for (uint32_t a = start; a < end; a += 16) {
        uint8_t *p = phys_ptr(a);
        if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
            p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ' &&
            checksum_ok(p, 20))
            return p;
    }
    return NULL;
}

static uint8_t *find_rsdp(void) {
    /* The Extended BIOS Data Area, whose segment lives at 0x40E. */
    uint16_t ebda_seg = (uint16_t)(phys_ptr(0x40E)[0] | (phys_ptr(0x40E)[1] << 8));
    uint32_t ebda = (uint32_t)ebda_seg << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        uint8_t *p = scan_rsdp(ebda, ebda + 1024);
        if (p) return p;
    }
    /* The main BIOS ROM area. */
    return scan_rsdp(0xE0000, 0x100000);
}

/* Pull SLP_TYPa / SLP_TYPb out of the DSDT's \_S5_ package. Returns 0 on ok. */
static int parse_s5(const uint8_t *dsdt) {
    uint32_t len = rd32(dsdt + 4);
    if (len < 36 || len > 0x100000) return -1;

    const uint8_t *p   = dsdt + 36;      /* AML starts after the 36-byte header */
    const uint8_t *end = dsdt + len;
    while (p < end - 4) {
        if (p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_')
            break;
        p++;
    }
    if (p >= end - 4)
        return -1;

    /* Must be a real NameOp: 0x08 '_S5_' or 0x08 '\' '_S5_'. */
    if (!(p[-1] == 0x08 || (p[-2] == 0x08 && p[-1] == '\\')))
        return -1;
    if (p[4] != 0x12)                    /* PackageOp */
        return -1;

    const uint8_t *pkg = p + 5;          /* -> PkgLength lead byte */
    pkg += ((*pkg & 0xC0) >> 6) + 1;     /* skip PkgLength (lead + extra bytes) */
    pkg += 1;                            /* skip NumElements */

    if (*pkg == 0x0A) pkg++;             /* optional BytePrefix */
    slp_typa = (uint8_t)(*pkg++ & 0x07);
    if (*pkg == 0x0A) pkg++;
    slp_typb = (uint8_t)(*pkg & 0x07);
    return 0;
}

void acpi_init(void) {
    available = 0;

    uint8_t *rsdp = find_rsdp();
    if (!rsdp)
        return;

    uint8_t *rsdt = map_table(rd32(rsdp + 16));
    if (rsdt[0] != 'R' || rsdt[1] != 'S' || rsdt[2] != 'D' || rsdt[3] != 'T')
        return;
    uint32_t rsdt_len = rd32(rsdt + 4);
    if (!checksum_ok(rsdt, rsdt_len))
        return;

    /* Walk the RSDT's table pointers looking for the FADT ("FACP"). */
    const uint8_t *fadt = NULL;
    uint32_t entries = (rsdt_len - 36) / 4;
    for (uint32_t i = 0; i < entries; i++) {
        uint8_t *t = map_table(rd32(rsdt + 36 + i * 4));
        if (t[0] == 'F' && t[1] == 'A' && t[2] == 'C' && t[3] == 'P') {
            fadt = t;
            break;
        }
    }
    if (!fadt)
        return;

    smi_cmd     = (uint16_t)rd32(fadt + 48);
    acpi_enable = fadt[52];
    pm1a_cnt    = (uint16_t)rd32(fadt + 64);
    pm1b_cnt    = (uint16_t)rd32(fadt + 68);

    uint8_t *dsdt = map_table(rd32(fadt + 40));
    if (dsdt[0] != 'D' || dsdt[1] != 'S' || dsdt[2] != 'D' || dsdt[3] != 'T')
        return;
    if (parse_s5(dsdt) != 0)
        return;

    available = (pm1a_cnt != 0);
}

int acpi_available(void) {
    return available;
}

int acpi_poweroff(void) {
    if (!available)
        return -1;

    /* Switch the chipset into ACPI mode if the firmware left it in legacy mode
     * (SCI_EN clear) and gave us a way to ask (SMI_CMD / ACPI_ENABLE). */
    if (!(inw(pm1a_cnt) & SCI_EN) && smi_cmd && acpi_enable) {
        outb(smi_cmd, acpi_enable);
        for (int i = 0; i < 300 && !(inw(pm1a_cnt) & SCI_EN); i++)
            io_wait();
    }

    outw(pm1a_cnt, (uint16_t)((slp_typa << 10) | SLP_EN));
    if (pm1b_cnt)
        outw(pm1b_cnt, (uint16_t)((slp_typb << 10) | SLP_EN));

    return -1;                           /* only reached if power-off failed */
}

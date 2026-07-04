/* ===========================================================================
 * PumpkinOS - ELF32 program loader
 * ---------------------------------------------------------------------------
 * elf_exec() reads the file into a kernel buffer, sanity-checks the ELF header,
 * and spawns a kernel thread. That thread walks the program headers, maps each
 * PT_LOAD segment as user pages at its virtual address, copies the file bytes
 * in (zeroing the rest, so .bss works), frees the file buffer, and drops to
 * ring 3 at the entry point exactly like the built-in user demo does.
 * ========================================================================= */
#include "elf.h"
#include "fat12.h"
#include "kheap.h"
#include "pmm.h"
#include "paging.h"
#include "sched.h"
#include "gdt.h"
#include "console.h"
#include "string.h"
#include <stdint.h>

extern void enter_user_mode(uint32_t entry, uint32_t user_esp);

#define MAX_ELF     (128 * 1024)         /* largest program we will load */
#define USTACK_TOP  0x50000000u          /* user stack top (well above code) */
#define PAGE_SIZE   4096u

/* ELF32 header / program-header field offsets we care about. */
#define E_TYPE      16   /* half: 2 = ET_EXEC                */
#define E_MACHINE   18   /* half: 3 = EM_386                 */
#define E_ENTRY     24   /* word: entry virtual address      */
#define E_PHOFF     28   /* word: program-header table offset */
#define E_PHENTSIZE 42   /* half: bytes per program header   */
#define E_PHNUM     44   /* half: number of program headers  */

#define P_TYPE      0    /* word: 1 = PT_LOAD */
#define P_OFFSET    4    /* word */
#define P_VADDR     8    /* word */
#define P_FILESZ    16   /* word */
#define P_MEMSZ     20   /* word */

struct elf_job {
    uint8_t *buf;
    uint32_t len;
};

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Ensure a user page covering 'va' is mapped (allocating + zeroing a frame the
 * first time). Reuses an existing mapping so overlapping segments don't clash.
 * Returns 0 on success. */
static int ensure_page(uint32_t va) {
    uint32_t page = va & ~(PAGE_SIZE - 1);
    if (paging_get_phys(page) != 0xFFFFFFFFu)
        return 0;                        /* already mapped */
    uint32_t frame = pmm_alloc_frame();
    if (frame == 0)
        return -1;
    if (paging_map(page, frame, PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
        pmm_free_frame(frame);
        return -1;
    }
    memset((void *)page, 0, PAGE_SIZE);  /* zero-fill (covers .bss) */
    return 0;
}

/* Map every page a segment spans, then place its file bytes at the exact vaddr.
 * Returns 0 on success. */
static int load_segment(const uint8_t *elf, uint32_t vaddr,
                        uint32_t off, uint32_t filesz, uint32_t memsz) {
    for (uint32_t a = vaddr; a < vaddr + memsz; a += PAGE_SIZE)
        if (ensure_page(a) != 0)
            return -1;
    if (memsz > 0)                       /* also map the final partial page */
        if (ensure_page(vaddr + memsz - 1) != 0)
            return -1;

    memcpy((void *)vaddr, elf + off, filesz);
    return 0;
}

/* The ring-3 launcher thread. */
static void elf_task(void *arg) {
    struct elf_job *job = (struct elf_job *)arg;
    uint8_t *elf = job->buf;

    uint32_t entry    = rd32(elf + E_ENTRY);
    uint32_t phoff    = rd32(elf + E_PHOFF);
    uint16_t phentsz  = rd16(elf + E_PHENTSIZE);
    uint16_t phnum    = rd16(elf + E_PHNUM);

    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + (uint32_t)i * phentsz;
        if (rd32(ph + P_TYPE) != 1)      /* PT_LOAD only */
            continue;
        uint32_t vaddr  = rd32(ph + P_VADDR);
        uint32_t off    = rd32(ph + P_OFFSET);
        uint32_t filesz = rd32(ph + P_FILESZ);
        uint32_t memsz  = rd32(ph + P_MEMSZ);
        if (load_segment(elf, vaddr, off, filesz, memsz) != 0) {
            console_write("elf: out of memory loading segment\n");
            kfree(job->buf);
            kfree(job);
            return;
        }
    }

    kfree(job->buf);                     /* image is now resident in user pages */
    kfree(job);

    /* One page of user stack, just below USTACK_TOP. */
    if (ensure_page(USTACK_TOP - PAGE_SIZE) != 0) {
        console_write("elf: out of memory for user stack\n");
        return;
    }

    tss_set_esp0(sched_current()->kstack_top);
    enter_user_mode(entry, USTACK_TOP);  /* never returns */
}

int elf_exec(const char *path) {
    uint8_t *buf = (uint8_t *)kmalloc(MAX_ELF);
    if (!buf)
        return -1;

    int len = fs_read(path, buf, MAX_ELF);
    if (len < 52) {                      /* smaller than an ELF header */
        kfree(buf);
        return -1;
    }

    /* \x7f E L F, 32-bit (class 1), little-endian (data 1). */
    if (buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F' ||
        buf[4] != 1 || buf[5] != 1) {
        kfree(buf);
        return -1;
    }
    if (rd16(buf + E_TYPE) != 2 || rd16(buf + E_MACHINE) != 3) {  /* ET_EXEC, EM_386 */
        kfree(buf);
        return -1;
    }

    struct elf_job *job = (struct elf_job *)kmalloc(sizeof(struct elf_job));
    if (!job) {
        kfree(buf);
        return -1;
    }
    job->buf = buf;
    job->len = (uint32_t)len;

    task_spawn(elf_task, job, "elfprog");
    return 0;
}

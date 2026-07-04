/* ===========================================================================
 * PumpkinOS - PumpkinShell (PSH)
 * ---------------------------------------------------------------------------
 * A tiny read-eval-print loop. It reads a line from the keyboard (with echo
 * and backspace editing), splits off the first word as the command, and runs
 * the matching built-in. Everything after the command is passed along as the
 * argument string.
 * ========================================================================= */
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "timer.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "string.h"
#include "io.h"

#define PROMPT      "psh> "
#define LINE_MAX    128

/* ---- the pumpkin banner (shared with the boot screen) --------------------- */
void shell_banner(void) {
    console_set_color(VGA_YELLOW, VGA_BLACK);
    console_write("\n");
    console_write("   ____                   _    _        ___  ____\n");
    console_write("  |  _ \\ _   _ _ __ ___  | | _(_)_ __  / _ \\/ ___|\n");
    console_write("  | |_) | | | | '_ ` _ \\ | |/ / | '_ \\| | | \\___ \\\n");
    console_write("  |  __/| |_| | | | | | ||   <| | | | | |_| |___) |\n");
    console_write("  |_|    \\__,_|_| |_| |_||_|\\_\\_|_| |_|\\___/|____/\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ---- built-in commands ---------------------------------------------------- */
static void cmd_help(void) {
    console_write("PumpkinShell (PSH) built-in commands:\n");
    console_write("  help          show this help\n");
    console_write("  clear, cls    clear the screen\n");
    console_write("  echo <text>   print <text>\n");
    console_write("  banner        draw the PumpkinOS banner\n");
    console_write("  about         about PumpkinOS\n");
    console_write("  colors        show the VGA colour palette\n");
    console_write("  uptime        time since boot\n");
    console_write("  sleep <sec>   pause for <sec> seconds\n");
    console_write("  meminfo       show the physical memory map\n");
    console_write("  memtest       allocate/free some frames\n");
    console_write("  pgtest        map a frame and prove the translation\n");
    console_write("  pgfault       trigger a page fault (halts - demo)\n");
    console_write("  heap          show kernel heap statistics\n");
    console_write("  htest         exercise kmalloc/kfree\n");
    console_write("  reboot        restart the machine\n");
    console_write("  halt          stop the CPU\n");
}

static void cmd_about(void) {
    shell_banner();
    console_write("\n");
    console_write("  PumpkinOS - a 32-bit, BIOS-only hobby OS.\n");
    console_write("  Protected mode, VGA + serial console, PS/2 keyboard,\n");
    console_write("  and this very shell: PumpkinShell (PSH).\n\n");
}

static void cmd_colors(void) {
    for (int i = 0; i < 16; i++) {
        console_set_color((enum vga_color)i, VGA_BLACK);
        console_write(" #");
        console_write_dec((uint32_t)i);
    }
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("\n");
}

/* Print a value as at least two digits (zero-padded), for HH:MM:SS. */
static void print_2d(uint32_t v) {
    if (v < 10)
        console_putc('0');
    console_write_dec(v);
}

static void cmd_uptime(void) {
    uint32_t t  = timer_ticks();
    uint32_t hz = timer_hz();
    if (hz == 0) {
        console_write("uptime: timer is not running\n");
        return;
    }

    uint32_t total = t / hz;                 /* whole seconds since boot */
    uint32_t days  = total / 86400;
    uint32_t hours = (total / 3600) % 24;
    uint32_t mins  = (total / 60) % 60;
    uint32_t secs  = total % 60;

    console_write("up ");
    if (days) {
        console_write_dec(days);
        console_write("d ");
    }
    print_2d(hours);
    console_putc(':');
    print_2d(mins);
    console_putc(':');
    print_2d(secs);
    console_write("   (");
    console_write_dec(t);
    console_write(" ticks @ ");
    console_write_dec(hz);
    console_write(" Hz)\n");
}

/* Parse a non-negative decimal integer; stops at the first non-digit. */
static uint32_t parse_uint(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static void cmd_sleep(const char *args) {
    uint32_t secs = parse_uint(args);
    if (secs == 0) {
        console_write("usage: sleep <seconds>\n");
        return;
    }
    if (secs > 3600)
        secs = 3600;                         /* keep the shell responsive */

    console_write("sleeping for ");
    console_write_dec(secs);
    console_write("s ...\n");
    timer_sleep(secs * 1000);
    console_write("awake!\n");
}

static void cmd_meminfo(void) {
    pmm_report();
}

/* Allocate a few frames, show their (distinct, page-aligned) addresses, then
 * free them and confirm the free count is restored. Exercises the allocator. */
static void cmd_memtest(void) {
    uint32_t start_free = pmm_free_frames();
    console_write("free frames before: ");
    console_write_dec(start_free);
    console_putc('\n');

    uint32_t frames[4];
    for (int i = 0; i < 4; i++) {
        frames[i] = pmm_alloc_frame();
        console_write("  alloc frame ");
        console_write_dec((uint32_t)i);
        console_write(" -> ");
        console_write_hex(frames[i]);
        console_putc('\n');
    }
    console_write("free frames after 4 allocs: ");
    console_write_dec(pmm_free_frames());
    console_putc('\n');

    for (int i = 0; i < 4; i++)
        pmm_free_frame(frames[i]);
    console_write("free frames after freeing: ");
    console_write_dec(pmm_free_frames());
    if (pmm_free_frames() == start_free)
        console_write("  (restored OK)\n");
    else
        console_write("  (MISMATCH!)\n");
}

/* Demonstrate paging: map a fresh physical frame at a high virtual address,
 * write through the virtual pointer, and confirm the same bytes appear at the
 * frame's identity-mapped physical address (i.e. the translation works). */
static void cmd_pgtest(void) {
    console_write("paging enabled: ");
    console_write(paging_is_enabled() ? "yes\n" : "no\n");

    console_write("  identity check: virt 0x000B8000 -> phys ");
    console_write_hex(paging_get_phys(0xB8000));
    console_putc('\n');

    uint32_t phys = pmm_alloc_frame();
    if (phys == 0) {
        console_write("  out of memory\n");
        return;
    }
    uint32_t virt = 0xD0000000;
    if (paging_map(virt, phys, PAGE_PRESENT | PAGE_WRITE) != 0) {
        console_write("  paging_map failed\n");
        pmm_free_frame(phys);
        return;
    }
    console_write("  mapped virt ");
    console_write_hex(virt);
    console_write(" -> phys ");
    console_write_hex(phys);
    console_putc('\n');

    volatile uint32_t *vptr = (volatile uint32_t *)virt;
    volatile uint32_t *pptr = (volatile uint32_t *)phys;   /* identity-mapped */
    *vptr = 0xCAFEBABE;

    console_write("  wrote 0xCAFEBABE via virtual; read back ");
    console_write_hex(*vptr);
    console_write(", via physical ");
    console_write_hex(*pptr);
    console_putc('\n');

    if (*vptr == 0xCAFEBABE && *pptr == 0xCAFEBABE)
        console_write("  translation verified OK\n");
    else
        console_write("  MISMATCH!\n");

    console_write("  paging_get_phys(0xD0000000) = ");
    console_write_hex(paging_get_phys(virt));
    console_putc('\n');

    paging_unmap(virt);          /* tidy up so repeated runs don't leak/alias */
    pmm_free_frame(phys);
}

/* Deliberately touch an unmapped address to show the page-fault handler.
 * This halts the machine (by design) - it is a demonstration. */
static void cmd_pgfault(void) {
    uint32_t addr = 0xDEADB000;              /* far above the identity map */
    __asm__("" : "+r"(addr));                /* hide the constant from GCC */
    console_write("touching unmapped ");
    console_write_hex(addr);
    console_write(" (expect a page fault)...\n");
    volatile uint32_t *bad = (volatile uint32_t *)addr;
    uint32_t v = *bad;                        /* triggers #PF - never returns */
    console_write("  unexpectedly read ");
    console_write_hex(v);
    console_putc('\n');
}

static void cmd_heap(void) {
    kheap_report();
}

/* Exercise the allocator: allocate blocks, prove the memory is real by writing
 * and reading a pattern, then free one and show the space gets reused. */
static void cmd_htest(void) {
    console_write("initial heap:\n");
    kheap_report();

    void *a = kmalloc(100);
    void *b = kmalloc(2000);
    void *c = kmalloc(50);
    console_write("  kmalloc(100)  -> ");
    console_write_hex((uint32_t)a);
    console_write("\n  kmalloc(2000) -> ");
    console_write_hex((uint32_t)b);
    console_write("\n  kmalloc(50)   -> ");
    console_write_hex((uint32_t)c);
    console_putc('\n');

    /* Write a pattern through 'a' and read it back - proves the pages are
     * really mapped and writable. */
    uint32_t *arr = (uint32_t *)a;
    int ok = 1;
    for (int i = 0; i < 25; i++)
        arr[i] = (uint32_t)(i * 2654435761u);
    for (int i = 0; i < 25; i++)
        if (arr[i] != (uint32_t)(i * 2654435761u))
            ok = 0;
    console_write("  write/read pattern: ");
    console_write(ok ? "OK\n" : "FAILED\n");

    console_write("after 3 allocs:\n");
    kheap_report();

    kfree(b);
    console_write("freed the 2000-byte block; reallocating 1000...\n");
    void *d = kmalloc(1000);
    console_write("  kmalloc(1000) -> ");
    console_write_hex((uint32_t)d);
    console_write((d == b) ? "  (reused the freed block)\n" : "\n");

    kfree(a);
    kfree(c);
    kfree(d);
    console_write("after freeing everything:\n");
    kheap_report();
}

static void cmd_reboot(void) {
    console_write("Rebooting...\n");
    /* Pulse the CPU reset line via the 8042 keyboard controller. */
    uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);
    /* If that somehow didn't take, fall through to a halt. */
    for (;;)
        __asm__ volatile("cli; hlt");
}

static void cmd_halt(void) {
    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_write("System halted. It is now safe to turn off your computer.\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* ---- command dispatch ----------------------------------------------------- */

/* Skip leading spaces/tabs. */
static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static void shell_execute(char *line) {
    char *cmd = skip_spaces(line);

    /* Split the command word from its argument string. */
    char *args = cmd;
    while (*args && *args != ' ' && *args != '\t')
        args++;
    if (*args) {
        *args = '\0';         /* terminate the command word */
        args = skip_spaces(args + 1);
    }

    if (cmd[0] == '\0')
        return;                                  /* empty line */
    else if (strcmp(cmd, "help") == 0)
        cmd_help();
    else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0)
        console_clear();
    else if (strcmp(cmd, "echo") == 0) {
        console_write(args);
        console_putc('\n');
    }
    else if (strcmp(cmd, "banner") == 0) {
        shell_banner();
        console_putc('\n');
    }
    else if (strcmp(cmd, "about") == 0)
        cmd_about();
    else if (strcmp(cmd, "colors") == 0 || strcmp(cmd, "color") == 0)
        cmd_colors();
    else if (strcmp(cmd, "uptime") == 0)
        cmd_uptime();
    else if (strcmp(cmd, "sleep") == 0)
        cmd_sleep(args);
    else if (strcmp(cmd, "meminfo") == 0)
        cmd_meminfo();
    else if (strcmp(cmd, "memtest") == 0)
        cmd_memtest();
    else if (strcmp(cmd, "pgtest") == 0)
        cmd_pgtest();
    else if (strcmp(cmd, "pgfault") == 0)
        cmd_pgfault();
    else if (strcmp(cmd, "heap") == 0)
        cmd_heap();
    else if (strcmp(cmd, "htest") == 0)
        cmd_htest();
    else if (strcmp(cmd, "reboot") == 0)
        cmd_reboot();
    else if (strcmp(cmd, "halt") == 0)
        cmd_halt();
    else {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_write("psh: command not found: ");
        console_write(cmd);
        console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* ---- the read-eval-print loop --------------------------------------------- */
void shell_run(void) {
    static char line[LINE_MAX];
    size_t len = 0;

    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write(PROMPT);
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n') {
            console_putc('\n');
            line[len] = '\0';
            shell_execute(line);
            len = 0;
            console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            console_write(PROMPT);
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                console_putc('\b');   /* console erases the glyph for us */
            }
        } else if (c == '\t') {
            /* treat tab as a single space for simplicity */
            if (len < LINE_MAX - 1) {
                line[len++] = ' ';
                console_putc(' ');
            }
        } else if ((unsigned char)c >= ' ' && (unsigned char)c < 127) {
            if (len < LINE_MAX - 1) {
                line[len++] = c;
                console_putc(c);
            }
        }
        /* anything else (Esc, unmapped keys) is ignored */
    }
}
